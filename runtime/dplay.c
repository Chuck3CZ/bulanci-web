/* dplay.c -- DirectPlay 4 (IDirectPlay4A, IDirectPlayLobby3A) over the virtual LAN.
 *
 * Bulanci runs every game through DirectPlay: its CDSDirectPlay (0x43ae70)
 * does CoCreateInstance(CLSID_DirectPlay, IID_IDirectPlay4A) and
 * CoCreateInstance(CLSID_DirectPlayLobby, IID_IDirectPlayLobby3A), builds a
 * TCP/IP address with IDirectPlayLobby2::CreateCompoundAddress (0x43b360) or
 * picks the service provider from EnumConnections (0x43b620),
 * InitializeConnection, EnumSessions (async, the join screen's list), Open
 * (DPOPEN_CREATE 0x43afa0, DPOPEN_JOIN 0x43af00), CreatePlayer, and from then
 * on Send (its sender thread, 0x43ad20) and Receive (polled, 0x43ab70).  Of
 * the system messages its handler (0x415290) reads only DESTROYPLAYERORGROUP,
 * SESSIONLOST and HOST (host migration: it gives up); the joining itself is
 * the game's own messages.
 *
 * The network is the runtime's virtual LAN (net_lan.c in the browser, the
 * server's /lan switch; net_vlan.c on the Mac), one UDP socket on DirectPlay's
 * port 47624.  The switch relays over a WebSocket, so datagrams arrive in
 * order and are not lost while the receiving queue has room; every call here
 * empties the socket into our own queue first.  A session is a star: the host
 * knows every machine in it, a client knows the host and the owner of every
 * player (players' ids are made by their own machine: address and a count).
 * Messages bigger than one datagram travel in pieces.
 *
 *   ENUMREQ  broadcast   guidApplication             -> the hosts answer ENUMREP
 *   ENUMREP  unicast     the session's description and name
 *   JOIN     to host     guidInstance                -> JOINACK: description + the players
 *   PADD     player created (client -> host, host -> every other machine)
 *   PDEL     player destroyed (same way); the others get DPSYS_DESTROYPLAYERORGROUP
 *   DATA     a message: to a player's machine, or (DPID_ALLPLAYERS) client -> host,
 *            host -> every other machine
 *   LEAVE    Close: from a client, the host drops its players; from the host,
 *            the clients get DPSYS_SESSIONLOST
 *
 * Without the virtual LAN (no ?net= on the page) a session still works inside
 * the browser: EnumSessions finds nothing and Open(JOIN) fails.
 *
 * Objects are guest memory, as in ddraw.c: [obj] = vtable of COM magic
 * numbers (interfaces 0x104 and 0x105), [obj+4] = interface, [obj+8] = refs.
 * DPLOG=1 prints every call and the network events, DPLOG=2 the empty Receives too. */
#include "cpu.h"
#include "winsock.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

uint32_t com_scratch(uint32_t n);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
int winsock_dplay_up(uint32_t *ip);

#define COM_MAGIC 0xE0000000u
enum { DP_IF_DP = 0x104, DP_IF_LOBBY, DP_IF_END };

#define DP_OK                 0x00000000u
#define DPERR_BUFFERTOOSMALL  0x8877001Eu
#define DPERR_INVALIDPARAMS   0x80070057u
#define DPERR_INVALIDPLAYER   0x88770096u
#define DPERR_NOMESSAGES      0x887700BEu
#define DPERR_NOSESSIONS      0x88770104u
#define DPERR_TIMEOUT         0x887700F0u
#define DPERR_UNSUPPORTED     0x80004001u
#define DPERR_NOTLOBBIED      0x887703FEu

#define DPOPEN_JOIN           0x1u
#define DPOPEN_CREATE         0x2u
#define DPRECEIVE_TOPLAYER    0x1u
#define DPRECEIVE_FROMPLAYER  0x2u
#define DPRECEIVE_PEEK        0x8u
#define DPID_SYSMSG           0u
#define DPID_ALLPLAYERS       0u
#define DPESC_TIMEDOUT        0x1u
#define DPENUMSESSIONS_ASYNC     0x10u
#define DPENUMSESSIONS_STOPASYNC 0x20u
#define DPSYS_DESTROYPLAYERORGROUP 0x0005u
#define DPSYS_SESSIONLOST          0x0031u

typedef struct { const char *name; int nargs; } DpM;
static const DpM M_DP[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"AddPlayerToGroup",3},{"Close",1},{"CreateGroup",6},
    {"CreatePlayer",7},{"DeletePlayerFromGroup",3},{"DestroyGroup",2},{"DestroyPlayer",2},{"EnumGroupPlayers",6},
    {"EnumGroups",5},{"EnumPlayers",5},{"EnumSessions",6},{"GetCaps",3},{"GetGroupData",5},{"GetGroupName",4},
    {"GetMessageCount",3},{"GetPlayerAddress",4},{"GetPlayerCaps",4},{"GetPlayerData",5},{"GetPlayerName",4},
    {"GetSessionDesc",3},{"Initialize",2},{"Open",3},{"Receive",6},{"Send",6},{"SetGroupData",5},
    {"SetGroupName",4},{"SetPlayerData",5},{"SetPlayerName",4},{"SetSessionDesc",3},
    /* IDirectPlay3 */
    {"AddGroupToGroup",3},{"CreateGroupInGroup",7},{"DeleteGroupFromGroup",3},{"EnumConnections",5},
    {"EnumGroupsInGroup",6},{"GetGroupConnectionSettings",5},{"InitializeConnection",3},{"SecureOpen",5},
    {"SendChatMessage",5},{"SetGroupConnectionSettings",4},{"StartSession",3},{"GetGroupFlags",3},
    {"GetGroupParent",3},{"GetPlayerAccount",5},{"GetPlayerFlags",3},
    /* IDirectPlay4 */
    {"GetGroupOwner",3},{"SetGroupOwner",3},{"SendEx",10},{"GetMessageQueue",6},{"CancelMessage",3},
    {"CancelPriority",4} };
static const DpM M_LOBBY[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"Connect",4},{"CreateAddress",7},{"EnumAddress",5},
    {"EnumAddressTypes",5},{"EnumLocalApplications",4},{"GetConnectionSettings",4},{"ReceiveLobbyMessage",6},
    {"RunApplication",5},{"SendLobbyMessage",5},{"SetConnectionSettings",4},{"SetLobbyMessageEvent",4},
    /* IDirectPlayLobby2 */
    {"CreateCompoundAddress",5},
    /* IDirectPlayLobby3 */
    {"ConnectEx",6},{"RegisterApplication",3},{"UnregisterApplication",3},{"WaitForConnectionSettings",2} };
static const struct { const DpM *m; unsigned n; const char *name; } g_dpif[] = {
    { M_DP, sizeof M_DP / sizeof *M_DP, "IDirectPlay4A" },
    { M_LOBBY, sizeof M_LOBBY / sizeof *M_LOBBY, "IDirectPlayLobby3A" } };

/* the GUIDs, first dword enough to tell them apart */
#define GUID_CLSID_DirectPlay       0xD1EB6D20u
#define GUID_CLSID_DirectPlayLobby  0x2FE8F810u
static const uint8_t DPAID_TotalSize[16]       = { 0x60,0xF5,0x18,0x13, 0x2C,0x91, 0xD0,0x11, 0x9D,0xAA,0x00,0xA0,0xC9,0x0A,0x43,0xCB };
static const uint8_t DPAID_ServiceProvider[16] = { 0xC0,0x16,0xD9,0x07, 0xAF,0xE0, 0xCF,0x11, 0x9C,0x4E,0x00,0xA0,0xC9,0x05,0x42,0x5E };
static const uint8_t DPSPGUID_TCPIP[16]        = { 0xE0,0x5E,0xE9,0x36, 0x77,0x85, 0xCF,0x11, 0x96,0x0C,0x00,0x80,0xC7,0x53,0x4E,0x82 };
static const uint8_t DPSPGUID_IPX[16]          = { 0x00,0xC4,0x5B,0x68, 0x2C,0x9D, 0xCF,0x11, 0xA9,0xCD,0x00,0xAA,0x00,0x68,0x86,0xE3 };

static int dplog(void){ static int on = -1; if (on < 0){ const char *e = getenv("DPLOG"); on = e ? atoi(e) : 0; } return on; }
#define NLOG(...) do { if (dplog()) fprintf(stderr, "[dplay] " __VA_ARGS__); } while (0)
static void trace(const char *what, uint32_t from, uint32_t to, const uint8_t *d, uint32_t size);
static const char *ipstr(uint32_t n, char *b){   /* network order */
    const uint8_t *p = (const uint8_t *)&n; snprintf(b, 16, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]); return b;
}

/* --- the session and its players ------------------------------------------ */
#define NPLAYER 32
#define NMEMBER 16
#define NFOUND  16
typedef struct { uint32_t id; int live; uint32_t flags; uint32_t owner; /* machine, network order */
                 char name[32]; uint8_t *data; uint32_t datasize; } Player;
typedef struct Msg { struct Msg *next; uint32_t from, to, size; uint8_t data[]; } Msg;
static struct {
    int open;                    /* Open done, Close not yet */
    int host;                    /* this machine created the session */
    uint32_t host_ip;            /* a client's host */
    uint32_t member[NMEMBER]; int nmember;   /* the host: the other machines */
    uint8_t desc[0x50];          /* DPSESSIONDESC2, pointers zero */
    char sname[64];
    Player p[NPLAYER];
    uint32_t next_n;
    Msg *head, *tail; uint32_t nmsg;
} S;
static struct { uint32_t ip; uint8_t desc[0x50]; char name[64]; uint64_t seen_ms; uint32_t guest; } g_found[NFOUND];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_vt[2];

static uint32_t new_obj(int iface){
    int k = iface - DP_IF_DP;
    if (!g_vt[k]){
        g_vt[k] = com_scratch(4u * g_dpif[k].n);
        for (unsigned i = 0; i < g_dpif[k].n; i++) MR32(g_vt[k] + 4u*i) = COM_MAGIC | ((uint32_t)iface << 16) | i;
    }
    uint32_t o = com_scratch(16);
    MR32(o) = g_vt[k]; MR32(o + 4) = (uint32_t)iface; MR32(o + 8) = 1;
    return o;
}

static Player *player(uint32_t id){
    for (int i = 0; i < NPLAYER; i++) if (S.p[i].live && S.p[i].id == id) return &S.p[i];
    return 0;
}
static Player *player_add(uint32_t id, uint32_t flags, uint32_t owner, const char *name){
    Player *p = player(id);
    for (int i = 0; i < NPLAYER && !p; i++) if (!S.p[i].live) p = &S.p[i];
    if (!p) return 0;
    if (!p->live) MR32(S.desc + 0x2c) = MR32(S.desc + 0x2c) + 1;          /* dwCurrentPlayers */
    free(p->data);
    memset(p, 0, sizeof *p);
    p->live = 1; p->id = id; p->flags = flags; p->owner = owner;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "");
    return p;
}
static void player_del(Player *p){
    free(p->data); memset(p, 0, sizeof *p);
    if (MR32(S.desc + 0x2c)) MR32(S.desc + 0x2c) = MR32(S.desc + 0x2c) - 1;
}
static void gstr(char *dst, size_t n, uint32_t p){
    size_t k = 0;
    if (p) for (; k + 1 < n && MR8(p + k); k++) dst[k] = (char)MR8(p + k);
    dst[k] = 0;
}
/* DPNAME { dwSize, dwFlags, lpszShortNameA, lpszLongNameA } */
static void name_of(char *dst, size_t n, uint32_t dpname){
    dst[0] = 0;
    if (!dpname) return;
    uint32_t s = MR32(dpname + 8); if (!s) s = MR32(dpname + 12);
    gstr(dst, n, s);
}

static void queue_bytes(uint32_t from, uint32_t to, const uint8_t *data, uint32_t size){
    Msg *m = malloc(sizeof *m + size);
    if (!m) return;
    m->next = 0; m->from = from; m->to = to; m->size = size;
    if (size) memcpy(m->data, data, size);
    if (S.tail) S.tail->next = m; else S.head = m;
    S.tail = m; S.nmsg++;
}
static void drop_all(void){
    for (Msg *m = S.head, *n; m; m = n){ n = m->next; free(m); }
    S.head = S.tail = 0; S.nmsg = 0;
}
/* a message for the players on this machine: to one of them, or (DPID_ALLPLAYERS) to each but `from` */
static void deliver_local(uint32_t from, uint32_t to, const uint8_t *data, uint32_t size){
    for (int i = 0; i < NPLAYER; i++){
        Player *p = &S.p[i];
        if (!p->live || p->owner) continue;
        if (to == DPID_ALLPLAYERS ? p->id != from : p->id == to) queue_bytes(from, p->id, data, size);
    }
}
static void sysmsg_local(const uint8_t *msg, uint32_t size){
    for (int i = 0; i < NPLAYER; i++) if (S.p[i].live && !S.p[i].owner) queue_bytes(DPID_SYSMSG, S.p[i].id, msg, size);
}
static void sys_destroyed(uint32_t id){
    uint8_t m[52]; memset(m, 0, sizeof m);
    uint32_t v[3] = { DPSYS_DESTROYPLAYERORGROUP, 1u /* DPPLAYERTYPE_PLAYER */, id };
    memcpy(m, v, sizeof v);
    sysmsg_local(m, sizeof m);
}
static void session_reset(void){
    drop_all();
    for (int i = 0; i < NPLAYER; i++){ free(S.p[i].data); memset(&S.p[i], 0, sizeof S.p[i]); }
    S.open = S.host = 0; S.host_ip = 0; S.nmember = 0;
}

/* --- the network ------------------------------------------------------------ */
#define DP_PORT   47624
#define PKT_MAX   1400                  /* the virtual LAN carries 1464-byte datagrams */
enum { P_ENUMREQ = 1, P_ENUMREP, P_JOIN, P_JOINACK, P_PADD, P_PDEL, P_DATA, P_LEAVE };
static WsSock g_ws;
static int g_net;                       /* 1 socket up, -1 no virtual LAN */
static uint32_t g_me;                   /* own address, network order */
static uint32_t g_msgid;
static int g_joinack;                   /* Open(JOIN) waits for it: 1 accepted */
static struct { uint32_t ip, msgid, total, got, from, to; uint8_t *buf; } g_asm[NMEMBER + 1];

static uint64_t now_ms(void){ return ws_now_us() / 1000u; }
static int net_up(void){
    if (g_net) return g_net > 0;
    uint32_t ip;
    if (winsock_dplay_up(&ip)){ g_net = -1; NLOG("no virtual LAN (the page has no ?net=): sessions stay in this browser\n"); return 0; }
    memset(&g_ws, 0, sizeof g_ws);
    g_ws.used = 1; g_ws.fd = -1; g_ws.type = 2; g_ws.proto = 17; g_ws.so_reuse = 1; g_ws.so_broadcast = 1;
    if (vlan_open(&g_ws) || vlan_bind(&g_ws, 0, DP_PORT)){ g_net = -1; NLOG("UDP %u: no socket\n", DP_PORT); return 0; }
    g_ws.bound = 1; g_ws.state = ST_BOUND;
    g_me = ip; g_net = 1;
    char b[16]; NLOG("virtual LAN %s, UDP %u\n", ipstr(g_me, b), DP_PORT);
    return 1;
}
static void put32(uint8_t *p, uint32_t v){ memcpy(p, &v, 4); }
static uint32_t get32(const uint8_t *p){ uint32_t v; memcpy(&v, p, 4); return v; }
static void tx(uint32_t ip, const uint8_t *b, uint32_t n){
    if (g_net <= 0) return;
    int sent; vlan_sendto(&g_ws, b, (int)n, ip, DP_PORT, &sent);
}
/* the description as it travels: pointers zero, then the name */
static uint32_t put_desc(uint8_t *b){
    memcpy(b, S.desc, 0x50); put32(b + 0x30, 0); put32(b + 0x34, 0);
    size_t l = strlen(S.sname) + 1; memcpy(b + 0x50, S.sname, l);
    return 0x50 + (uint32_t)l;
}
/* PADD / PDEL: one player */
static uint32_t put_player(uint8_t *b, const Player *p){
    put32(b, p->id); put32(b + 4, p->flags); put32(b + 8, p->owner ? p->owner : g_me);
    memcpy(b + 12, p->name, 32);
    return 44;
}
static void tx_members(const uint8_t *b, uint32_t n, uint32_t except){
    for (int i = 0; i < S.nmember; i++) if (S.member[i] != except) tx(S.member[i], b, n);
}
/* a player change from this machine: client -> host, host -> every other machine */
static void tx_player(int type, const Player *p){
    if (g_net <= 0 || !S.open) return;
    uint8_t b[64]; b[0] = 'B'; b[1] = (uint8_t)type;
    uint32_t n = 2 + put_player(b + 2, p);
    if (S.host) tx_members(b, n, 0); else if (S.host_ip) tx(S.host_ip, b, n);
}
/* a game message to other machines, in pieces of PKT_MAX */
static void tx_data(uint32_t ip, int relay_all, uint32_t except, uint32_t from, uint32_t to, const uint8_t *d, uint32_t size){
    uint32_t id = ++g_msgid, off = 0;
    uint8_t b[24 + PKT_MAX];
    do {
        uint32_t k = size - off < PKT_MAX ? size - off : PKT_MAX;
        b[0] = 'B'; b[1] = P_DATA; put32(b + 2, from); put32(b + 6, to); put32(b + 10, id);
        put32(b + 14, size); put32(b + 18, off);
        memcpy(b + 22, d + off, k);
        if (relay_all) tx_members(b, 22 + k, except); else tx(ip, b, 22 + k);
        off += k;
    } while (off < size);
}
static void rx_data(uint32_t src, const uint8_t *b, uint32_t n){
    if (n < 22) return;
    uint32_t from = get32(b + 2), to = get32(b + 6), id = get32(b + 10), total = get32(b + 14), off = get32(b + 18);
    uint32_t k = n - 22;
    const uint8_t *whole = 0;
    int slot = -1;
    if (off == 0 && k == total) whole = b + 22;
    else {
        for (int i = 0; i <= NMEMBER; i++) if (g_asm[i].buf && g_asm[i].ip == src){ slot = i; break; }
        if (off == 0){
            if (slot < 0) for (int i = 0; i <= NMEMBER; i++) if (!g_asm[i].buf){ slot = i; break; }
            if (slot < 0 || total > (16u << 20)) return;
            free(g_asm[slot].buf);
            g_asm[slot].buf = malloc(total ? total : 1);
            if (!g_asm[slot].buf) return;
            g_asm[slot].ip = src; g_asm[slot].msgid = id; g_asm[slot].total = total; g_asm[slot].got = 0;
            g_asm[slot].from = from; g_asm[slot].to = to;
        }
        if (slot < 0 || g_asm[slot].msgid != id || off != g_asm[slot].got || off + k > g_asm[slot].total) return;
        memcpy(g_asm[slot].buf + off, b + 22, k); g_asm[slot].got += k;
        if (g_asm[slot].got < g_asm[slot].total) return;
        whole = g_asm[slot].buf;
    }
    trace("recv", from, to, whole, total);
    deliver_local(from, to, whole, total);
    /* the host passes a client's message for everyone on to the other machines */
    if (S.host && to == DPID_ALLPLAYERS) tx_data(0, 1, src, from, to, whole, total);
    if (slot >= 0){ free(g_asm[slot].buf); g_asm[slot].buf = 0; }
}
static void rx(uint32_t src, const uint8_t *b, uint32_t n){
    char ib[16];
    if (n < 2 || b[0] != 'B') return;
    switch (b[1]){
    case P_ENUMREQ:   /* a host answers if the application matches */
        if (S.open && S.host && n >= 18 && !memcmp(b + 2, S.desc + 0x18, 16)){
            uint8_t r[2 + 0x50 + 64]; r[0] = 'B'; r[1] = P_ENUMREP;
            tx(src, r, 2 + put_desc(r + 2));
        }
        break;
    case P_ENUMREP: {
        if (n < 2 + 0x50 || src == g_me) break;
        int k = -1;
        for (int i = 0; i < NFOUND && k < 0; i++) if (g_found[i].seen_ms && !memcmp(g_found[i].desc + 8, b + 2 + 8, 16)) k = i;
        for (int i = 0; i < NFOUND && k < 0; i++) if (!g_found[i].seen_ms) k = i;
        if (k < 0) break;
        if (!g_found[k].seen_ms) NLOG("found session \"%.40s\" at %s\n", (const char *)b + 2 + 0x50, ipstr(src, ib));
        g_found[k].ip = src; memcpy(g_found[k].desc, b + 2, 0x50);
        snprintf(g_found[k].name, sizeof g_found[k].name, "%.*s", (int)(n - 2 - 0x50), (const char *)b + 2 + 0x50);
        g_found[k].seen_ms = now_ms();
        break; }
    case P_JOIN: {
        if (!S.open || !S.host || n < 18 || memcmp(b + 2, S.desc + 8, 16)) break;
        int known = 0;
        for (int i = 0; i < S.nmember; i++) known |= S.member[i] == src;
        if (!known && S.nmember < NMEMBER) S.member[S.nmember++] = src;
        uint8_t r[2 + 4 + 0x50 + 64 + 4 + NPLAYER * 44]; r[0] = 'B'; r[1] = P_JOINACK;
        uint32_t o = 2; put32(r + o, 0); o += 4;
        o += put_desc(r + o);
        uint32_t np = 0, npo = o; o += 4;
        for (int i = 0; i < NPLAYER; i++) if (S.p[i].live){ o += put_player(r + o, &S.p[i]); np++; }
        put32(r + npo, np);
        tx(src, r, o);
        NLOG("%s joined (%d machines)\n", ipstr(src, ib), S.nmember + 1);
        break; }
    case P_JOINACK: {
        if (S.open || !S.host_ip || src != S.host_ip || n < 6 + 0x50) break;
        memcpy(S.desc, b + 6, 0x50);
        const char *nm = (const char *)b + 6 + 0x50; size_t l = strnlen(nm, n - 6 - 0x50);
        snprintf(S.sname, sizeof S.sname, "%.*s", (int)l, nm);
        uint32_t o = 6 + 0x50 + (uint32_t)l + 1;
        MR32(S.desc + 0x2c) = 0;
        if (o + 4 <= n){
            uint32_t np = get32(b + o); o += 4;
            for (uint32_t i = 0; i < np && o + 44 <= n; i++, o += 44){
                char pn[33]; memcpy(pn, b + o + 12, 32); pn[32] = 0;
                player_add(get32(b + o), get32(b + o + 4), get32(b + o + 8), pn);
            }
        }
        g_joinack = 1;
        break; }
    case P_PADD:
        if (S.open && n >= 46){
            uint32_t owner = get32(b + 10);
            if (owner == g_me) break;
            char pn[33]; memcpy(pn, b + 14, 32); pn[32] = 0;
            player_add(get32(b + 2), get32(b + 6), owner, pn);
            if (S.host) tx_members(b, n, src);
            NLOG("player 0x%x \"%s\" at %s\n", get32(b + 2), pn, ipstr(owner, ib));
        }
        break;
    case P_PDEL:
        if (S.open && n >= 46){
            Player *p = player(get32(b + 2));
            if (p && p->owner){ player_del(p); sys_destroyed(get32(b + 2)); }
            if (S.host) tx_members(b, n, src);
        }
        break;
    case P_DATA:
        if (S.open) rx_data(src, b, n);
        break;
    case P_LEAVE:
        if (!S.open) break;
        if (S.host){
            for (int i = 0; i < NPLAYER; i++){
                Player *p = &S.p[i];
                if (!p->live || p->owner != src) continue;
                uint8_t d[64]; d[0] = 'B'; d[1] = P_PDEL; uint32_t k = 2 + put_player(d + 2, p);
                uint32_t id = p->id;
                player_del(p); sys_destroyed(id);
                tx_members(d, k, src);
            }
            for (int i = 0; i < S.nmember; i++) if (S.member[i] == src){ S.member[i] = S.member[--S.nmember]; break; }
            NLOG("%s left\n", ipstr(src, ib));
        } else if (src == S.host_ip){
            uint8_t m[4]; put32(m, DPSYS_SESSIONLOST);
            sysmsg_local(m, sizeof m);
            NLOG("the host left: session lost\n");
        }
        break;
    }
}
/* everything waiting on the socket */
static void pump(void){
    if (g_net <= 0) return;
    static uint8_t b[2048];
    for (int k = 0; k < 4096; k++){
        uint32_t ip; uint16_t port; int got;
        if (vlan_recvfrom(&g_ws, b, sizeof b, 0, &ip, &port, &got) || got <= 0) break;
        rx(ip, b, (uint32_t)got);
    }
}

/* Send: DPID_ALLPLAYERS goes to every player except the sender, a player id to that player */
static int g_traced;                    /* DPLOG=1: the first messages each way */
static void trace(const char *what, uint32_t from, uint32_t to, const uint8_t *d, uint32_t size){
    if (!dplog() || (dplog() < 2 && g_traced >= 60)) return;
    g_traced++;
    fprintf(stderr, "[dplay] %s 0x%x -> 0x%x, %u bytes:", what, from, to, size);
    for (uint32_t i = 0; i < size && i < 300; i++) fprintf(stderr, i >= 0x2a && d[i] >= 0x20 && d[i] < 0x7f ? "%c" : " %02x", d[i]);
    fprintf(stderr, "\n");
}
static uint32_t dp_send(uint32_t from, uint32_t to, uint32_t data, uint32_t size){
    if (!S.open) return DPERR_INVALIDPARAMS;
    const uint8_t *d = g_mem + data;
    trace("send", from, to, d, size);
    if (from && !player(from)){ NLOG("send from 0x%x: no such player\n", from); return DPERR_INVALIDPLAYER; }
    pump();
    if (to == DPID_ALLPLAYERS){
        deliver_local(from, to, d, size);
        if (g_net > 0){
            if (S.host) tx_data(0, 1, 0, from, to, d, size);
            else if (S.host_ip) tx_data(S.host_ip, 0, 0, from, to, d, size);
        }
        return DP_OK;
    }
    Player *p = player(to);
    if (!p){ NLOG("send to 0x%x: no such player\n", to); return DPERR_INVALIDPLAYER; }
    if (p->owner) tx_data(p->owner, 0, 0, from, to, d, size);
    else queue_bytes(from, to, d, size);
    return DP_OK;
}
/* Receive(lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize) */
static uint32_t dp_receive(uint32_t pfrom, uint32_t pto, uint32_t flags, uint32_t data, uint32_t psize){
    if (!psize) return DPERR_INVALIDPARAMS;
    pump();
    uint32_t want_from = (flags & DPRECEIVE_FROMPLAYER) && pfrom ? MR32(pfrom) : 0;
    uint32_t want_to   = (flags & DPRECEIVE_TOPLAYER)   && pto   ? MR32(pto)   : 0;
    Msg *prev = 0, *m = S.head;
    for (; m; prev = m, m = m->next){
        if ((flags & DPRECEIVE_FROMPLAYER) && m->from != want_from) continue;
        if ((flags & DPRECEIVE_TOPLAYER) && m->to != want_to) continue;
        break;
    }
    if (!m) return DPERR_NOMESSAGES;
    uint32_t have = MR32(psize);
    MR32(psize) = m->size;
    if (!data || have < m->size) return DPERR_BUFFERTOOSMALL;
    if (pfrom) MR32(pfrom) = m->from;
    if (pto) MR32(pto) = m->to;
    if (m->size) memcpy(g_mem + data, m->data, m->size);
    if (!(flags & DPRECEIVE_PEEK)){
        if (prev) prev->next = m->next; else S.head = m->next;
        if (S.tail == m) S.tail = prev;
        S.nmsg--; free(m);
    }
    return DP_OK;
}

/* a compound address: DPAID_TotalSize, then the elements, each
 * { GUID guidDataType; DWORD dwDataSize; BYTE data[] } */
static uint32_t compound_size(uint32_t el, uint32_t n){
    uint32_t sz = 16 + 4 + 4;
    for (uint32_t i = 0; i < n; i++) sz += 16 + 4 + MR32(el + 24u*i + 16);
    return sz;
}
static void compound_write(uint32_t out, uint32_t el, uint32_t n, uint32_t total){
    memcpy(g_mem + out, DPAID_TotalSize, 16); MR32(out + 16) = 4; MR32(out + 20) = total;
    uint32_t p = out + 24;
    for (uint32_t i = 0; i < n; i++){
        uint32_t e = el + 24u*i, sz = MR32(e + 16), d = MR32(e + 20);
        memcpy(g_mem + p, g_mem + e, 16); MR32(p + 16) = sz;
        if (sz && d) memcpy(g_mem + p + 20, g_mem + d, sz);
        p += 20 + sz;
    }
}
/* a connection for one service provider, as EnumConnections hands it out */
static uint32_t connection_for(const uint8_t *sp, uint32_t *size){
    uint32_t total = 24 + 20 + 16;
    uint32_t p = com_scratch(total);
    memcpy(g_mem + p, DPAID_TotalSize, 16); MR32(p + 16) = 4; MR32(p + 20) = total;
    memcpy(g_mem + p + 24, DPAID_ServiceProvider, 16); MR32(p + 40) = 16;
    memcpy(g_mem + p + 44, sp, 16);
    *size = total;
    return p;
}

static uint32_t A_(CPU *c, int i){ return MR32(c->esp + 4u*(uint32_t)i); }

/* EnumSessions(lpsd, dwTimeout, cb, ctx, flags): ask the LAN, list what answered
 * lately; cb(lpThisSD, lpdwTimeOut, dwFlags, ctx), without the lock */
static uint32_t enum_sessions(CPU *c){
    uint32_t sd = A_(c, 1), cb = A_(c, 3), ctx = A_(c, 4), flags = A_(c, 5);
    uint32_t list[NFOUND]; int n = 0;
    pthread_mutex_lock(&g_lock);
    if (net_up()){
        pump();
        uint8_t q[18]; q[0] = 'B'; q[1] = P_ENUMREQ;
        memcpy(q + 2, sd ? g_mem + sd + 0x18 : (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16);
        if (!(flags & DPENUMSESSIONS_STOPASYNC)) tx(0xFFFFFFFFu, q, sizeof q);
        uint64_t t = now_ms();
        for (int i = 0; i < NFOUND; i++){
            if (!g_found[i].seen_ms) continue;
            if (t - g_found[i].seen_ms > 5000){ g_found[i].seen_ms = 0; continue; }
            if (!g_found[i].guest) g_found[i].guest = com_scratch(0x50 + 64);
            uint32_t gd = g_found[i].guest;
            memcpy(g_mem + gd, g_found[i].desc, 0x50);
            memcpy(g_mem + gd + 0x50, g_found[i].name, 64);
            MR32(gd + 0x30) = gd + 0x50; MR32(gd + 0x34) = 0;
            list[n++] = gd;
        }
    }
    pthread_mutex_unlock(&g_lock);
    uint32_t to = com_scratch(4);
    for (int i = 0; i < n && cb; i++){
        MR32(to) = 0;
        uint32_t a[4] = { list[i], to, 0, ctx };
        if (!call_guest(c, cb, 4, a)) return DP_OK;
    }
    if (!(flags & (DPENUMSESSIONS_ASYNC | DPENUMSESSIONS_STOPASYNC)) && cb){
        MR32(to) = 0;
        uint32_t a[4] = { 0, to, DPESC_TIMEDOUT, ctx };
        call_guest(c, cb, 4, a);
    }
    return DP_OK;
}

/* Open(lpsd, dwFlags) */
static uint32_t dp_open(uint32_t sd, uint32_t flags){
    if (!sd) return DPERR_INVALIDPARAMS;
    net_up();
    if (flags & DPOPEN_CREATE){
        session_reset();
        memcpy(S.desc, g_mem + sd, 0x50);
        MR32(S.desc) = 0x50;
        static uint32_t k; k++;                                         /* guidInstance: this machine, this time */
        put32(S.desc + 8, g_me ? g_me : 0x7F000001u); put32(S.desc + 12, (uint32_t)now_ms());
        put32(S.desc + 16, k); put32(S.desc + 20, 0x42554C41u);
        MR32(S.desc + 0x2c) = 0;
        gstr(S.sname, sizeof S.sname, MR32(sd + 0x30));
        MR32(S.desc + 0x30) = 0; MR32(S.desc + 0x34) = 0;
        S.open = 1; S.host = 1;
        return DP_OK;
    }
    if (!(flags & DPOPEN_JOIN) || g_net <= 0) return DPERR_NOSESSIONS;
    int k = -1;
    for (int i = 0; i < NFOUND; i++) if (g_found[i].seen_ms && !memcmp(g_found[i].desc + 8, g_mem + sd + 8, 16)) k = i;
    if (k < 0) return DPERR_NOSESSIONS;
    session_reset();
    S.host_ip = g_found[k].ip; g_joinack = 0;
    uint8_t q[18]; q[0] = 'B'; q[1] = P_JOIN; memcpy(q + 2, g_found[k].desc + 8, 16);
    /* the host answers within milliseconds; ask again every half second, 5 s in all */
    for (int t = 0; t < 2500 && !g_joinack; t++){
        if (t % 250 == 0) tx(S.host_ip, q, sizeof q);
        usleep(2000);
        pump();
    }
    char b[16];
    if (!g_joinack){ NLOG("join %s: no answer\n", ipstr(S.host_ip, b)); S.host_ip = 0; return DPERR_TIMEOUT; }
    S.open = 1;
    NLOG("joined \"%s\" at %s\n", S.sname, ipstr(S.host_ip, b));
    return DP_OK;
}

static void dp_close(void){
    if (S.open && g_net > 0){
        uint8_t q[2] = { 'B', P_LEAVE };
        if (S.host) tx_members(q, 2, 0); else if (S.host_ip) tx(S.host_ip, q, 2);
    }
    session_reset();
}

static uint32_t dp_method(CPU *c, uint32_t m, uint32_t self){
    switch (m){
    case 0: {   /* QueryInterface(riid, ppv): the DirectPlay interfaces are all this object */
        uint32_t ppv = A_(c, 2);
        if (!ppv) return DPERR_INVALIDPARAMS;
        MR32(ppv) = self; MR32(self + 8)++;
        return DP_OK; }
    case 1: return ++MR32(self + 8);
    case 2: { uint32_t r = MR32(self + 8); if (r) r = --MR32(self + 8); if (!r && S.open) dp_close(); return r; }
    case 4: dp_close(); return DP_OK;   /* Close */
    case 6: {   /* CreatePlayer(lpidPlayer, lpPlayerName, hEvent, lpData, dwDataSize, dwFlags) */
        uint32_t pid = A_(c, 1);
        if (!pid || !S.open) return DPERR_INVALIDPARAMS;
        char nm[32]; name_of(nm, sizeof nm, A_(c, 2));
        /* the id: this machine's address (last two bytes) and a count, never 0 or 1 */
        const uint8_t *ip = (const uint8_t *)&g_me;
        uint32_t id = ((uint32_t)ip[2] << 24 | (uint32_t)ip[3] << 16 | (++S.next_n & 0xFFFFu)) | 0x100u;
        Player *p = player_add(id, A_(c, 6), 0, nm);
        if (!p) return DPERR_INVALIDPARAMS;
        uint32_t d = A_(c, 4), n = A_(c, 5);
        if (d && n && (p->data = malloc(n))){ memcpy(p->data, g_mem + d, n); p->datasize = n; }
        tx_player(P_PADD, p);
        MR32(pid) = p->id;
        return DP_OK; }
    case 9: {   /* DestroyPlayer(id) */
        Player *p = player(A_(c, 1));
        if (!p || p->owner) return DPERR_INVALIDPLAYER;
        tx_player(P_PDEL, p);
        player_del(p);
        return DP_OK; }
    case 14: {  /* GetCaps(lpDPCaps, flags) */
        uint32_t p = A_(c, 1); if (!p) return DPERR_INVALIDPARAMS;
        uint32_t sz = MR32(p); if (sz < 40) sz = 40;
        memset(g_mem + p, 0, sz); MR32(p) = sz;
        MR32(p + 8) = 0x10000u; MR32(p + 16) = NPLAYER; MR32(p + 20) = 100000; MR32(p + 24) = 1;
        MR32(p + 28) = NPLAYER; MR32(p + 32) = 20; MR32(p + 36) = 1000;
        return DP_OK; }
    case 17: {  /* GetMessageCount(idPlayer, lpdwCount) */
        uint32_t id = A_(c, 1), pc = A_(c, 2), n = 0;
        if (!pc) return DPERR_INVALIDPARAMS;
        pump();
        for (Msg *q = S.head; q; q = q->next) if (q->to == id) n++;
        MR32(pc) = n; return DP_OK; }
    case 20: {  /* GetPlayerData(id, lpData, lpdwDataSize, flags) */
        Player *p = player(A_(c, 1)); uint32_t d = A_(c, 2), ps = A_(c, 3);
        if (!p) return DPERR_INVALIDPLAYER;
        if (!ps) return DPERR_INVALIDPARAMS;
        uint32_t have = MR32(ps); MR32(ps) = p->datasize;
        if (!d || have < p->datasize) return DPERR_BUFFERTOOSMALL;
        if (p->datasize) memcpy(g_mem + d, p->data, p->datasize);
        return DP_OK; }
    case 21: {  /* GetPlayerName(id, lpData, lpdwDataSize): a DPNAME and its strings */
        Player *p = player(A_(c, 1)); uint32_t d = A_(c, 2), ps = A_(c, 3);
        if (!p) return DPERR_INVALIDPLAYER;
        if (!ps) return DPERR_INVALIDPARAMS;
        uint32_t len = (uint32_t)strlen(p->name) + 1, need = 16 + len;
        uint32_t have = MR32(ps); MR32(ps) = need;
        if (!d || have < need) return DPERR_BUFFERTOOSMALL;
        memset(g_mem + d, 0, 16); MR32(d) = 16; MR32(d + 8) = d + 16;
        memcpy(g_mem + d + 16, p->name, len);
        return DP_OK; }
    case 22: {  /* GetSessionDesc(lpData, lpdwDataSize) */
        uint32_t d = A_(c, 1), ps = A_(c, 2);
        if (!S.open) return DPERR_NOSESSIONS;
        if (!ps) return DPERR_INVALIDPARAMS;
        uint32_t len = (uint32_t)strlen(S.sname) + 1, need = 0x50 + len;
        uint32_t have = MR32(ps); MR32(ps) = need;
        if (!d || have < need) return DPERR_BUFFERTOOSMALL;
        memcpy(g_mem + d, S.desc, 0x50);
        memcpy(g_mem + d + 0x50, S.sname, len);
        MR32(d + 0x30) = d + 0x50; MR32(d + 0x34) = 0;
        return DP_OK; }
    case 23: return DP_OK;   /* Initialize */
    case 24: return dp_open(A_(c, 1), A_(c, 2));
    case 25: return dp_receive(A_(c, 1), A_(c, 2), A_(c, 3), A_(c, 4), A_(c, 5));
    case 26: return dp_send(A_(c, 1), A_(c, 2), A_(c, 4), A_(c, 5));   /* Send(from, to, flags, data, size) */
    case 29: {  /* SetPlayerData(id, lpData, dwDataSize, flags): kept on this machine */
        Player *p = player(A_(c, 1)); uint32_t d = A_(c, 2), n = A_(c, 3);
        if (!p) return DPERR_INVALIDPLAYER;
        free(p->data); p->data = 0; p->datasize = 0;
        if (d && n && (p->data = malloc(n))){ memcpy(p->data, g_mem + d, n); p->datasize = n; }
        return DP_OK; }
    case 30: {  /* SetPlayerName(id, lpName, flags) */
        Player *p = player(A_(c, 1));
        if (!p) return DPERR_INVALIDPLAYER;
        name_of(p->name, sizeof p->name, A_(c, 2));
        return DP_OK; }
    case 31: {  /* SetSessionDesc(lpsd, flags) */
        uint32_t sd = A_(c, 1);
        if (!S.open) return DPERR_NOSESSIONS;
        if (!sd) return DPERR_INVALIDPARAMS;
        uint32_t cur = MR32(S.desc + 0x2c); uint8_t inst[16]; memcpy(inst, S.desc + 8, 16);
        memcpy(S.desc, g_mem + sd, 0x50);
        MR32(S.desc) = 0x50; memcpy(S.desc + 8, inst, 16); MR32(S.desc + 0x2c) = cur;
        gstr(S.sname, sizeof S.sname, MR32(sd + 0x30));
        MR32(S.desc + 0x30) = 0; MR32(S.desc + 0x34) = 0;
        return DP_OK; }
    case 38: net_up(); return DP_OK;   /* InitializeConnection(lpConnection, flags) */
    case 46: {  /* GetPlayerFlags(id, lpdwFlags) */
        Player *p = player(A_(c, 1)); uint32_t pf = A_(c, 2);
        if (!p) return DPERR_INVALIDPLAYER;
        if (pf) MR32(pf) = p->flags | (p->owner ? 0u : 0x8u /* DPPLAYER_LOCAL */);
        return DP_OK; }
    case 49:    /* SendEx(from, to, flags, data, size, priority, timeout, context, lpdwMsgID) */
        if (A_(c, 9)) MR32(A_(c, 9)) = 0;
        return dp_send(A_(c, 1), A_(c, 2), A_(c, 4), A_(c, 5));
    case 50: {  /* GetMessageQueue(from, to, flags, lpdwNumMsgs, lpdwNumBytes): nothing waits to go out */
        if (A_(c, 4)) MR32(A_(c, 4)) = 0;
        if (A_(c, 5)) MR32(A_(c, 5)) = 0;
        return DP_OK; }
    default:
        fprintf(stderr, "[dplay] IDirectPlay4A::%s NOT IMPLEMENTED\n", M_DP[m].name);
        return DPERR_UNSUPPORTED;
    }
}

/* the methods that call back into the game: no lock held across the callbacks */
static uint32_t dp_callback_method(CPU *c, uint32_t m){
    if (m == 13) return enum_sessions(c);
    if (m == 12){   /* EnumPlayers(lpguidInstance, cb, ctx, flags): cb(id, type, lpName, flags, ctx) */
        uint32_t cb = A_(c, 2), ctx = A_(c, 3);
        uint32_t ids[NPLAYER], fl[NPLAYER], nm[NPLAYER]; int n = 0;
        pthread_mutex_lock(&g_lock);
        if (S.open) for (int i = 0; i < NPLAYER; i++){
            if (!S.p[i].live) continue;
            uint32_t g = com_scratch(16 + 32);
            memset(g_mem + g, 0, 16); MR32(g) = 16; MR32(g + 8) = g + 16;
            memcpy(g_mem + g + 16, S.p[i].name, 32);
            ids[n] = S.p[i].id; fl[n] = S.p[i].flags | (S.p[i].owner ? 0u : 0x8u); nm[n++] = g;
        }
        int open = S.open;
        pthread_mutex_unlock(&g_lock);
        if (!open) return DPERR_NOSESSIONS;
        for (int i = 0; i < n; i++){
            uint32_t a[5] = { ids[i], 1u /* DPPLAYERTYPE_PLAYER */, nm[i], fl[i], ctx };
            if (!call_guest(c, cb, 5, a)) break;
        }
        return DP_OK;
    }
    /* 35: EnumConnections(lpguidApplication, cb, ctx, flags):
     *     cb(lpguidSP, lpConnection, dwConnectionSize, lpName, dwFlags, ctx) */
    uint32_t cb = A_(c, 2), ctx = A_(c, 3);
    if (!cb) return DPERR_INVALIDPARAMS;
    static const struct { const uint8_t *g; const char *name; } sp[2] = {
        { DPSPGUID_TCPIP, "Internet TCP/IP Connection For DirectPlay" },
        { DPSPGUID_IPX,   "IPX Connection For DirectPlay" } };
    for (int i = 0; i < 2; i++){
        uint32_t g = com_scratch(16); memcpy(g_mem + g, sp[i].g, 16);
        uint32_t size, conn = connection_for(sp[i].g, &size);
        uint32_t len = (uint32_t)strlen(sp[i].name) + 1, nm = com_scratch(16 + len);
        memset(g_mem + nm, 0, 16); MR32(nm) = 16; MR32(nm + 8) = nm + 16;
        memcpy(g_mem + nm + 16, sp[i].name, len);
        uint32_t a[6] = { g, conn, size, nm, 0, ctx };
        if (!call_guest(c, cb, 6, a)) break;
    }
    return DP_OK;
}

static uint32_t lobby_method(CPU *c, uint32_t m, uint32_t self){
    switch (m){
    case 0: { uint32_t ppv = A_(c, 2); if (!ppv) return DPERR_INVALIDPARAMS; MR32(ppv) = self; MR32(self + 8)++; return DP_OK; }
    case 1: return ++MR32(self + 8);
    case 2: { uint32_t r = MR32(self + 8); if (r) r = --MR32(self + 8); return r; }
    case 8: return DPERR_NOTLOBBIED;   /* GetConnectionSettings: not started from a lobby */
    case 4: {   /* CreateAddress(guidSP, guidDataType, lpData, dwDataSize, lpAddress, lpdwAddressSize) */
        uint32_t el = com_scratch(48);
        memcpy(g_mem + el, DPAID_ServiceProvider, 16); MR32(el + 16) = 16; MR32(el + 20) = A_(c, 1);
        memcpy(g_mem + el + 24, g_mem + A_(c, 2), 16); MR32(el + 40) = A_(c, 4); MR32(el + 44) = A_(c, 3);
        uint32_t out = A_(c, 5), ps = A_(c, 6);
        if (!ps) return DPERR_INVALIDPARAMS;
        uint32_t need = compound_size(el, 2), have = MR32(ps); MR32(ps) = need;
        if (!out || have < need) return DPERR_BUFFERTOOSMALL;
        compound_write(out, el, 2, need); return DP_OK; }
    case 14: {  /* CreateCompoundAddress(lpElements, dwElementCount, lpAddress, lpdwAddressSize) */
        uint32_t el = A_(c, 1), n = A_(c, 2), out = A_(c, 3), ps = A_(c, 4);
        if (!el || !n || !ps) return DPERR_INVALIDPARAMS;
        uint32_t need = compound_size(el, n), have = MR32(ps); MR32(ps) = need;
        if (!out || have < need) return DPERR_BUFFERTOOSMALL;
        compound_write(out, el, n, need); return DP_OK; }
    default:
        fprintf(stderr, "[dplay] IDirectPlayLobby3A::%s NOT IMPLEMENTED\n", M_LOBBY[m].name);
        return DPERR_UNSUPPORTED;
    }
}

/* com.c com_call: a method of IDirectPlay4A or IDirectPlayLobby3A */
int dplay_call(CPU *c, uint32_t iface, uint32_t m){
    POP32(c->eip);
    int k = (int)iface - DP_IF_DP;
    if (k < 0 || k >= 2 || m >= g_dpif[k].n){ c->eax = DPERR_UNSUPPORTED; return 1; }
    const DpM *M = &g_dpif[k].m[m];
    uint32_t self = MR32(c->esp), r;
    if (iface == DP_IF_DP && (m == 12 || m == 13 || m == 35)) r = dp_callback_method(c, m);
    else {
        pthread_mutex_lock(&g_lock);
        r = iface == DP_IF_DP ? dp_method(c, m, self) : lobby_method(c, m, self);
        pthread_mutex_unlock(&g_lock);
    }
    /* Receive is polled every frame, EnumSessions every few: quiet unless DPLOG=2 */
    int quiet = iface == DP_IF_DP && ((m == 25 && r == DPERR_NOMESSAGES) || m == 13 || m == 26 || m == 25);
    if (dplog() > 1 || (dplog() && !quiet))
        fprintf(stderr, "[dplay] %s::%s(0x%08x, %x, %x, %x, %x, %x) -> 0x%x\n", g_dpif[k].name, M->name, self,
                A_(c, 1), A_(c, 2), A_(c, 3), A_(c, 4), A_(c, 5), r);
    c->eax = r;
    c->esp += 4u * (uint32_t)M->nargs;
    return 1;
}

/* win32.c CoCreateInstance(rclsid, outer, ctx, riid, ppv): 1 if the class is ours */
int dplay_cocreate(uint32_t clsid, uint32_t ppv, uint32_t *hr){
    if (!clsid) return 0;
    uint32_t d1 = MR32(clsid);
    if (d1 != GUID_CLSID_DirectPlay && d1 != GUID_CLSID_DirectPlayLobby) return 0;
    uint32_t o = new_obj(d1 == GUID_CLSID_DirectPlay ? DP_IF_DP : DP_IF_LOBBY);
    if (ppv) MR32(ppv) = o;
    *hr = DP_OK;
    if (dplog()) fprintf(stderr, "[dplay] %s object 0x%08x\n", d1 == GUID_CLSID_DirectPlay ? "IDirectPlay4A" : "IDirectPlayLobby3A", o);
    return 1;
}
