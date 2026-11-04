/* net_vlan.c -- the "vlan" network backend: a client of tools/vlanhub.py.
 *
 * The hub emulates one IPv4 /24 broadcast domain (10.77.0.0/24 by default)
 * for several game instances on one Mac.  Nothing here touches an AF_INET
 * socket: every virtual socket is a real AF_UNIX fd, so poll/select, EOF,
 * back-pressure and non-blocking behaviour stay kernel-real, and the Winsock
 * front end (winsock.c) treats it like any other fd.
 *
 *   control   one AF_UNIX stream to the hub: framed requests and replies
 *             (HELLO, OPEN, BIND, CONNECT, LISTEN, CLOSE, STATUS, NOTE), the
 *             hub's end of a new socket travels with the request (SCM_RIGHTS)
 *   UDP/ICMP  an AF_UNIX datagram pair per socket; each datagram carries an
 *             8-byte header {ip[4] (network order), port (LE), flags (LE)}:
 *             the destination on the way out, the source on the way in
 *   TCP       an AF_UNIX stream pair per connection; the hub relays the bytes
 *             to the peer's stream (so it can delay them and write a pcap).
 *             A listener gets an AF_UNIX datagram channel on which the hub
 *             delivers each accepted connection's fd with its addresses.
 * All integers in the control protocol are little endian; addresses are the
 * 4 bytes in network order.  The protocol is documented in tools/vlanhub.py.
 *
 * Requests are made with the guest lock held (they take well under a
 * millisecond); a hub that does not answer within 5 s is declared dead and
 * every later call fails with WSAENETDOWN. */
#include "cpu.h"
#include "winsock.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

enum { M_HELLO = 1, M_OPEN, M_BIND, M_CONNECT, M_LISTEN, M_CLOSE, M_STATUS, M_NOTE };
static int g_ctl = -1, g_dead;
static uint16_t g_seq;
static uint32_t g_next_sid = 1;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;

static int vdbg(void){ static int v = -1; if (v < 0){ const char *e = getenv("NFSU2_VLAN_DEBUG"); v = e && *e && *e != '0'; } return v; }
static void put32(uint8_t *p, uint32_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v){ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }

/* send a whole buffer (with an fd on the first piece) before the deadline */
static int xsend(int fd, const uint8_t *b, size_t n, int pass_fd, uint64_t dl){
    size_t off = 0; int first = 1;
    while (off < n){
        struct iovec io = { (void *)(b + off), n - off };
        struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = &io; m.msg_iovlen = 1;
        char cb[CMSG_SPACE(sizeof(int))];
        if (first && pass_fd >= 0){
            memset(cb, 0, sizeof cb);
            m.msg_control = cb; m.msg_controllen = sizeof cb;
            struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
            cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(cm), &pass_fd, sizeof(int));
        }
        ssize_t k = sendmsg(fd, &m, MSG_NOSIGNAL);
        if (k > 0){ off += (size_t)k; first = 0; continue; }
        if (k < 0 && errno != EAGAIN && errno != EINTR) return -1;
        uint64_t now = ws_now_us(); if (now >= dl) return -1;
        struct pollfd p = { fd, POLLOUT, 0 }; poll(&p, 1, (int)((dl - now) / 1000 + 1));
    }
    return 0;
}
static int xrecv(int fd, uint8_t *b, size_t n, uint64_t dl){
    size_t off = 0;
    while (off < n){
        ssize_t k = recv(fd, b + off, n - off, 0);
        if (k > 0){ off += (size_t)k; continue; }
        if (k == 0) return -1;
        if (errno != EAGAIN && errno != EINTR) return -1;
        uint64_t now = ws_now_us(); if (now >= dl) return -1;
        struct pollfd p = { fd, POLLIN, 0 }; poll(&p, 1, (int)((dl - now) / 1000 + 1));
    }
    return 0;
}
/* One request/reply.  Returns the reply's WSA error (0 = success) and the
 * reply payload after the error field in rep (rcap bytes at most). */
static int ctl_call(uint16_t type, const uint8_t *pl, uint32_t n, int pass_fd, uint8_t *rep, uint32_t rcap, uint32_t *rlen){
    if (g_ctl < 0 || g_dead) return WSAENETDOWN;
    pthread_mutex_lock(&g_mx);
    uint8_t hdr[8 + 256]; uint16_t seq = ++g_seq;
    if (n > 256){ pthread_mutex_unlock(&g_mx); return WSAEINVAL; }
    put32(hdr, 4 + n); put16(hdr + 4, type); put16(hdr + 6, seq); memcpy(hdr + 8, pl, n);
    uint64_t dl = ws_now_us() + 5000000ull;
    int err = WSAENETDOWN;
    if (xsend(g_ctl, hdr, 8 + n, pass_fd, dl) == 0){
        uint8_t rh[8];
        for (;;){
            if (xrecv(g_ctl, rh, 8, dl) != 0) break;
            uint32_t len = get32(rh); uint16_t rt = get16(rh + 4), rs = get16(rh + 6);
            if (len < 8 || len > 4096) break;
            uint8_t body[4096];
            if (xrecv(g_ctl, body, len - 4, dl) != 0) break;
            if (rt != (type | 0x8000) || rs != seq) continue;          /* stale reply */
            err = (int)get32(body);
            uint32_t pn = len - 8; if (pn > rcap) pn = rcap;
            if (rep && pn) memcpy(rep, body + 4, pn);
            if (rlen) *rlen = pn;
            pthread_mutex_unlock(&g_mx);
            return err;
        }
    }
    g_dead = 1;
    pthread_mutex_unlock(&g_mx);
    ws_log("vlan: lost the hub (request %u): every socket call now fails with WSAENETDOWN", type);
    return WSAENETDOWN;
}

int vlan_alive(void){ return g_ctl >= 0 && !g_dead; }
void vlan_session_end(void){}                           /* the hub link stays: it holds no server's place */

int vlan_hello(const char *path, NetIdent *id){
    if (g_ctl >= 0 && !g_dead) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return WSAENETDOWN;
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path){ close(fd); ws_log("vlan: hub path too long: %s", path); return WSAENETDOWN; }
    strcpy(a.sun_path, path);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0){ close(fd); return WSAENETDOWN; }
    ws_fd_setup(fd);
    g_ctl = fd; g_dead = 0;
    uint8_t pl[40]; memset(pl, 0, sizeof pl);
    const char *wa = getenv("NFSU2_NET_ADDR"), *wn = getenv("NFSU2_NET_NAME");
    if (wa && *wa){ unsigned b[4]; if (sscanf(wa, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) == 4)
        for (int i = 0; i < 4; i++) pl[i] = (uint8_t)b[i]; }
    if (wn && *wn) snprintf((char *)pl + 4, 32, "%s", wn);
    put32(pl + 36, (uint32_t)getpid());
    uint8_t r[48]; uint32_t rl = 0;
    int e = ctl_call(M_HELLO, pl, 40, -1, r, sizeof r, &rl);
    if (e || rl < 48){ close(fd); g_ctl = -1; return e ? e : WSAENETDOWN; }
    memcpy(&id->ip, r, 4); memcpy(&id->mask, r + 4, 4); memcpy(id->mac, r + 8, 6);
    memcpy(id->name, r + 16, 32); id->name[31] = 0;
    snprintf(id->ifname, sizeof id->ifname, "vlan0");
    { const char *em = getenv("NFSU2_NET_MAC"); unsigned v[6];
      if (em && sscanf(em, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
          for (int i = 0; i < 6; i++) id->mac[i] = (uint8_t)v[i]; }
    return 0;
}

static void bufsizes(int fd){
    int sz = 262144;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz) != 0){ sz = 131072; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz); }
    sz = 262144;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz) != 0){ sz = 131072; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz); }
}

int vlan_open(WsSock *s){
    s->vid = g_next_sid++;
    uint8_t pl[8]; put32(pl, s->vid); put16(pl + 4, (uint16_t)s->type); put16(pl + 6, 0);
    if (s->type == 1){
        int e = ctl_call(M_OPEN, pl, 8, -1, 0, 0, 0);
        s->fd = -1;
        return e;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return ws_errno_to_wsa(errno);
    bufsizes(sv[0]); bufsizes(sv[1]);
    ws_fd_setup(sv[0]); ws_fd_setup(sv[1]);
    int e = ctl_call(M_OPEN, pl, 8, sv[1], 0, 0, 0);
    close(sv[1]);
    if (e){ close(sv[0]); return e; }
    s->fd = sv[0];
    return 0;
}

int vlan_bind(WsSock *s, uint32_t ip, uint16_t port){
    uint8_t pl[12]; put32(pl, s->vid); memcpy(pl + 4, &ip, 4); put16(pl + 8, port); put16(pl + 10, (uint16_t)(s->so_reuse ? 1 : 0));
    uint8_t r[8]; uint32_t rl = 0;
    int e = ctl_call(M_BIND, pl, 12, -1, r, sizeof r, &rl);
    if (e) return e;
    if (rl >= 6){ memcpy(&s->laddr, r, 4); s->lport = get16(r + 4); }
    return 0;
}

int vlan_connect(WsSock *s, uint32_t ip, uint16_t port){
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return ws_errno_to_wsa(errno);
    ws_fd_setup(sv[0]); ws_fd_setup(sv[1]);
    uint8_t pl[12]; put32(pl, s->vid); memcpy(pl + 4, &ip, 4); put16(pl + 8, port); put16(pl + 10, 0);
    uint8_t r[16]; uint32_t rl = 0;
    int e = ctl_call(M_CONNECT, pl, 12, sv[1], r, sizeof r, &rl);
    close(sv[1]);
    uint32_t delay = rl >= 16 ? get32(r + 12) : 0;
    s->conn_ready_us = ws_now_us() + delay;
    if (rl >= 6){ memcpy(&s->laddr, r, 4); s->lport = get16(r + 4); }
    if (e){ close(sv[0]); if (s->fd >= 0 && s->fd != sv[0]) {} return e; }
    s->conn_id = rl >= 12 ? get32(r + 8) : 0;
    if (s->fd >= 0) close(s->fd);
    s->fd = sv[0];
    if (vdbg()) ws_log("vlan: connect -> fd %d, conn #%u", s->fd, s->conn_id);
    return 0;
}

int vlan_listen(WsSock *s, int backlog){
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return ws_errno_to_wsa(errno);
    ws_fd_setup(sv[0]); ws_fd_setup(sv[1]);
    uint8_t pl[8]; put32(pl, s->vid); put32(pl + 4, (uint32_t)backlog);
    int e = ctl_call(M_LISTEN, pl, 8, sv[1], 0, 0, 0);
    close(sv[1]);
    if (e){ close(sv[0]); return e; }
    if (s->fd >= 0) close(s->fd);
    s->fd = sv[0];
    return 0;
}

int vlan_accept(WsSock *ls, WsSock *ns){
    uint8_t h[20];
    struct iovec io = { h, sizeof h };
    char cb[CMSG_SPACE(sizeof(int))];
    struct msghdr m; memset(&m, 0, sizeof m);
    m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = cb; m.msg_controllen = sizeof cb;
    ssize_t k = recvmsg(ls->fd, &m, 0);
    if (k < 0) return ws_errno_to_wsa(errno);
    int nfd = -1;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm; cm = CMSG_NXTHDR(&m, cm))
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) memcpy(&nfd, CMSG_DATA(cm), sizeof(int));
    if (k < 20 || nfd < 0){ if (nfd >= 0) close(nfd); return WSAEWOULDBLOCK; }
    ws_fd_setup(nfd);
    ns->fd = nfd;
    memcpy(&ns->raddr, h, 4); ns->rport = get16(h + 4);
    memcpy(&ns->laddr, h + 8, 4); ns->lport = get16(h + 12);
    ns->conn_id = get32(h + 16);
    ns->vid = g_next_sid++;
    if (vdbg()){ int av = -1; ioctl(nfd, FIONREAD, &av);
                 struct pollfd q = { nfd, POLLIN, 0 }; poll(&q, 1, 0);
                 ws_log("vlan: accept on listener fd %d -> fd %d, conn #%u, %d bytes pending, revents 0x%x", ls->fd, nfd, ns->conn_id, av, q.revents); }
    return 0;
}

int vlan_sendto(WsSock *s, const uint8_t *buf, int len, uint32_t ip, uint16_t port, int *sent){
    uint8_t h[8]; memcpy(h, &ip, 4); put16(h + 4, port); put16(h + 6, 0);
    struct iovec io[2] = { { h, 8 }, { (void *)buf, (size_t)len } };
    struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = io; m.msg_iovlen = 2;
    ssize_t k = sendmsg(s->fd, &m, MSG_NOSIGNAL);
    if (k < 0){
        int e = errno;
        if (e == ENOBUFS){ *sent = len; return 0; }      /* the hub is behind: dropped, as UDP may be */
        return ws_errno_to_wsa(e);
    }
    *sent = len;
    return 0;
}

int vlan_recvfrom(WsSock *s, uint8_t *buf, int len, int peek, uint32_t *ip, uint16_t *port, int *got){
    for (;;){
        uint8_t h[8];
        uint8_t tmp[65536 + 64];
        int icmp = s->type == 3;
        struct iovec io[2] = { { h, 8 }, { icmp ? tmp : buf, icmp ? sizeof tmp : (size_t)len } };
        struct msghdr m; memset(&m, 0, sizeof m); m.msg_iov = io; m.msg_iovlen = 2;
        ssize_t k = recvmsg(s->fd, &m, peek ? MSG_PEEK : 0);
        if (k < 0) return ws_errno_to_wsa(errno);
        if (k < 8){ if (peek) recv(s->fd, h, 1, 0); continue; }   /* malformed: drop */
        uint16_t fl = get16(h + 6);
        if (fl & 1){                                      /* ICMP port unreachable for this socket */
            if (peek) recv(s->fd, h, 8, 0);
            if (s->has_peer) return WSAECONNRESET;
            continue;
        }
        memcpy(ip, h, 4); *port = get16(h + 4);
        int n = (int)k - 8;
        if (icmp){
            /* Windows raw sockets deliver the IPv4 header in front of the ICMP message */
            uint8_t ih[20]; memset(ih, 0, sizeof ih);
            int tot = n + 20;
            ih[0] = 0x45; ih[2] = (uint8_t)(tot >> 8); ih[3] = (uint8_t)tot; ih[8] = 64; ih[9] = 1;
            memcpy(ih + 12, ip, 4); memcpy(ih + 16, &g_ws_id.ip, 4);
            uint32_t sum = 0; for (int i = 0; i < 20; i += 2) sum += (uint32_t)(ih[i] << 8 | ih[i + 1]);
            while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
            sum = ~sum & 0xFFFF; ih[10] = (uint8_t)(sum >> 8); ih[11] = (uint8_t)sum;
            int c1 = len < 20 ? len : 20; memcpy(buf, ih, (size_t)c1);
            int c2 = len - c1 < n ? len - c1 : n; if (c2 > 0) memcpy(buf + c1, tmp, (size_t)c2);
            if (tot > len){ *got = len; return WSAEMSGSIZE; }
            *got = tot; return 0;
        }
        if (m.msg_flags & MSG_TRUNC){ *got = len; return WSAEMSGSIZE; }
        *got = n;
        return 0;
    }
}

void vlan_close(WsSock *s, int abortive){
    if (vdbg()) ws_log("vlan: close fd %d conn #%u%s", s->fd, s->conn_id, abortive ? " (abortive)" : "");
    if (g_ctl < 0 || g_dead) return;
    uint8_t pl[12]; put32(pl, s->vid); put32(pl + 4, abortive ? 1u : 0u); put32(pl + 8, s->conn_id);
    ctl_call(M_CLOSE, pl, 12, -1, 0, 0, 0);
}

/* After EOF on a stream: did the peer reset the connection (linger 0) or
 * close it normally?  Asked only when the fd reports a hang-up. */
int vlan_conn_was_reset(WsSock *s){
    if (!s->conn_id || s->fd < 0) return 0;
    struct pollfd p = { s->fd, POLLIN, 0 };
    poll(&p, 1, 0);
    if (!(p.revents & (POLLHUP | POLLIN))) return 0;
    if ((p.revents & POLLIN) && !(p.revents & POLLHUP)){
        char ch; ssize_t k = recv(s->fd, &ch, 1, MSG_PEEK);
        if (k != 0) return 0;                            /* data pending, not EOF */
    }
    uint8_t pl[4]; put32(pl, s->conn_id);
    uint8_t r[4]; uint32_t rl = 0;
    if (ctl_call(M_STATUS, pl, 4, -1, r, sizeof r, &rl) || rl < 4) return 0;
    return get32(r) == 2;
}

void vlan_note(const char *text){
    uint8_t pl[200]; size_t n = strlen(text); if (n > 199) n = 199;
    memcpy(pl, text, n);
    ctl_call(M_NOTE, pl, (uint32_t)n, -1, 0, 0, 0);
}
