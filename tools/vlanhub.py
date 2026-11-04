#!/usr/bin/env python3
"""vlanhub.py -- a virtual LAN for NFSU2 instances on one Mac (no real network).

Every game instance started with NFSU2_NET=vlan:<hub socket> (runtime/net_vlan.c)
joins one emulated IPv4 /24 broadcast domain (default 10.77.0.0/24): its own
address, MAC and host name, UDP unicast and broadcast (also to the sender, as on
Windows), TCP (relayed by the hub, with RST), ICMP echo answered by the hub.
Nothing here opens an AF_INET socket: all traffic is AF_UNIX between processes on
this Mac, so there is no packet on any network and no macOS privacy prompt.

    tools/vlanhub.py --sock <dir>/hub.sock [--pcap f.pcap] [--log f.txt]
                     [--delay MS] [--jitter MS] [--loss PCT] [--dup PCT]
                     [--reorder PCT] [--seed N] [--fake-server NAME[@IP]]
                     [--hexdump] [--quit-after S]

  --delay/--jitter/--loss/--dup/--reorder  impair traffic between instances
      (UDP datagrams; TCP data is only delayed, never lost, order kept)
  --pcap   LINKTYPE_IPV4 capture with synthesised IPv4/UDP/TCP/ICMP headers
  --log    hub log incl. the dissector: ProtoAdvt ("gEA") queries/adverts,
           lobby frames (4CC + KEY=VALUE), race datagrams on UDP 3658, pings
  --fake-server NAME[@IP]  a scripted LAN server as server.dll announces one:
           "gEA" adverts (kind NFSU2NA, note "9900|0") broadcast every 3 s and
           sent to every query, and a TCP listener on 9900 that logs the lobby
           frames it receives (it does not answer them)

Protocol (runtime/net_vlan.c is the other end; integers little endian, IPv4
addresses as their 4 bytes in network order):
  control stream frame: u32 len (bytes after this field), u16 type, u16 seq,
  payload; the reply has type|0x8000, the same seq and starts with i32 err (a
  Winsock error code, 0 = ok).  A new socket's hub end travels with the request
  as SCM_RIGHTS.
    1 HELLO   want_ip[4] name[32] pid:u32         -> ip[4] mask[4] mac[6] pad[2] name[32]
    2 OPEN    sid:u32 kind:u16 (1 tcp 2 udp 3 icmp) pad:u16  (+fd for udp/icmp)
    3 BIND    sid:u32 ip[4] port:u16 flags:u16 (1 = SO_REUSEADDR) -> ip[4] port:u16 pad:u16
    4 CONNECT sid:u32 ip[4] port:u16 pad:u16 (+fd: hub end of a stream pair)
                                                  -> lip[4] lport:u16 pad:u16 conn:u32 delay_us:u32
    5 LISTEN  sid:u32 backlog:u32 (+fd: hub end of the accept channel)
    6 CLOSE   sid:u32 flags:u32 (1 = abortive, RST) conn:u32
    7 STATUS  conn:u32                            -> state:u32 (0 open, 1 closed, 2 reset)
    8 NOTE    text
  UDP/ICMP datagram on a socket's pair: ip[4] port:u16 flags:u16 + payload
    (destination outgoing, source incoming; flags 1 = ICMP port unreachable)
  accept channel datagram: peer_ip[4] peer_port:u16 pad:u16 local_ip[4]
    local_port:u16 pad:u16 conn:u32 (+fd: the accepted stream)
"""
import argparse
import heapq
import os
import random
import selectors
import signal
import socket
import struct
import sys
import time

WSAEADDRINUSE, WSAEADDRNOTAVAIL, WSAECONNREFUSED = 10048, 10049, 10061
WSAEHOSTUNREACH, WSAEINVAL, WSAENOTSOCK = 10065, 10022, 10038

M_HELLO, M_OPEN, M_BIND, M_CONNECT, M_LISTEN, M_CLOSE, M_STATUS, M_NOTE = range(1, 9)


def ip2s(b):
    return '%d.%d.%d.%d' % tuple(b)


def s2ip(s):
    return bytes(int(x) for x in s.split('.'))


def csum(b):
    if len(b) % 2:
        b += b'\0'
    s = sum(struct.unpack('!%dH' % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


class Pcap:
    def __init__(self, path):
        self.f = open(path, 'wb')
        self.f.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 228))  # LINKTYPE_IPV4
        self.ipid = 1

    def ip(self, src, dst, proto, payload):
        self.ipid = (self.ipid + 1) & 0xffff
        hdr = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 20 + len(payload), self.ipid, 0, 64, proto, 0, src, dst)
        hdr = hdr[:10] + struct.pack('!H', csum(hdr)) + hdr[12:]
        t = time.time()
        pkt = hdr + payload
        self.f.write(struct.pack('<IIII', int(t), int((t % 1) * 1e6), len(pkt), len(pkt)) + pkt)
        self.f.flush()

    def udp(self, src, sport, dst, dport, data):
        self.ip(src, dst, 17, struct.pack('!HHHH', sport, dport, 8 + len(data), 0) + data)

    def icmp(self, src, dst, msg):
        self.ip(src, dst, 1, msg)

    def tcp(self, src, sport, dst, dport, seq, ack, flags, data=b''):
        h = struct.pack('!HHIIBBHHH', sport, dport, seq & 0xffffffff, ack & 0xffffffff, 5 << 4, flags, 65535, 0, 0)
        ph = struct.pack('!4s4sBBH', src, dst, 0, 6, len(h) + len(data))
        c = csum(ph + h + data)
        h = h[:16] + struct.pack('!H', c) + h[18:]
        self.ip(src, dst, 6, h + data)


class Inst:
    def __init__(self, hub, sock, n):
        self.hub, self.sock, self.n = hub, sock, n
        self.ip = None
        self.name = ''
        self.mac = b''
        self.rbuf = b''
        self.socks = {}
        self.pid = 0

    def label(self):
        return '%s(%s)' % (self.name, ip2s(self.ip)) if self.ip else 'inst%d' % self.n


class VSock:
    def __init__(self, inst, sid, kind, fd):
        self.inst, self.sid, self.kind, self.fd = inst, sid, kind, fd
        self.bip, self.bport, self.bound, self.reuse = b'\0\0\0\0', 0, False, False
        self.listening, self.backlog, self.pending = False, 0, 0


class Conn:
    """One relayed TCP connection: end[0] = connector's hub end, end[1] = acceptor's."""
    def __init__(self, hub, cid, a, b, caddr, saddr):
        self.hub, self.id, self.end = hub, cid, [a, b]
        self.addr = [caddr, saddr]          # (ip, port) of the connector and the acceptor
        self.out = [bytearray(), bytearray()]   # pending bytes to write INTO end[i]
        self.eof = [False, False]            # end[i] sent EOF (we read 0 from it)
        self.shut = [False, False]           # we shut down writing into end[i]
        self.last_t = [0.0, 0.0]
        self.seq = [random.randrange(1 << 32), random.randrange(1 << 32)]
        self.shut_pending = [False, False]   # EOF to pass on into end[i] once out[i] is written
        self.reset = False
        self.closed = False
        self.fake = None
        self.app_keep = None                 # our copy of the end passed to the acceptor
        self.stream = [bytearray(), bytearray()]   # dissector: bytes sent by side i


class FakeServer:
    """A scripted LAN server: ProtoAdvt adverts + a lobby listener that logs."""
    def __init__(self, hub, name, ip):
        self.hub, self.name, self.ip = hub, name, ip
        self.ident = os.urandom(4)
        self.next_adv = 0.0
        self.label = 'fake:%s(%s)' % (name, ip2s(ip))

    def advert(self):
        p = bytearray(384)
        p[0:3] = b'gEA'
        p[3] = 30                                   # announce period (s)
        p[4:8] = self.ident
        p[8:8 + 7] = b'NFSU2NA'
        nm = self.name.encode()[:31]
        p[0x28:0x28 + len(nm)] = nm
        note = b'9900|0'
        p[0x48:0x48 + len(note)] = note
        tm = b'TCP:~1:1024\tUDP:~1:1024'
        p[0x108:0x108 + len(tm)] = tm
        return bytes(p)

    def tick(self, now):
        if now >= self.next_adv:
            self.next_adv = now + 3.0
            self.hub.inject_udp(self.ip, 9999, b'\xff\xff\xff\xff', 9999, self.advert(), self.label)

    def udp_in(self, src, sport, dport, data):
        if dport == 9999 and data[:3] == b'gEA' and data[8:9] == b'?':
            self.hub.log('%s: query from %s:%d, answering' % (self.label, ip2s(src), sport))
            self.hub.inject_udp(self.ip, 9999, src, sport, self.advert(), self.label)


class Hub:
    def __init__(self, a):
        self.a = a
        self.sel = selectors.DefaultSelector()
        self.subnet = s2ip(a.subnet + '.0')
        self.mask = b'\xff\xff\xff\x00'
        self.bcast = self.subnet[:3] + b'\xff'
        self.insts = []
        self.ninst = 0
        self.conns = {}
        self.conn_state = {}       # conn id -> 0 open, 1 closed, 2 reset (kept after close)
        self.next_conn = 1
        self.timers = []
        self.tseq = 0
        self.rng = random.Random(a.seed)
        self.pcap = Pcap(a.pcap) if a.pcap else None
        self.logf = open(a.log, 'a', buffering=1) if a.log else sys.stderr
        self.t0 = time.time()
        self.fakes = []
        self.stats = dict(udp=0, bcast_deliv=0, uni_deliv=0, dropped_noport=0, lost=0, dup=0, outside=0,
                          tcp_conn=0, tcp_refused=0, tcp_bytes=0, tcp_reset=0, icmp=0)
        for spec in a.fake_server or []:
            name, _, ip = spec.partition('@')
            ip = s2ip(ip) if ip else self.subnet[:3] + bytes([50 + len(self.fakes)])
            self.fakes.append(FakeServer(self, name, ip))
            self.log('fake server %s at %s' % (name, ip2s(ip)))
        self.lsock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            os.unlink(a.sock)
        except FileNotFoundError:
            pass
        self.lsock.bind(a.sock)
        self.lsock.listen(16)
        self.lsock.setblocking(False)
        self.sel.register(self.lsock, selectors.EVENT_READ, ('listen', None))

    # ---------------------------------------------------------------- logging
    def log(self, s):
        self.logf.write('[hub %8.3f] %s\n' % (time.time() - self.t0, s))

    # ---------------------------------------------------------------- timers
    def at(self, t, fn, *args):
        self.tseq += 1
        heapq.heappush(self.timers, (t, self.tseq, fn, args))

    def impair(self, same):
        """delivery delay in seconds, or None if lost; plus duplicate flag"""
        if same:
            return 0.0, False
        if self.a.loss and self.rng.random() * 100 < self.a.loss:
            return None, False
        d = self.a.delay / 1000.0
        if self.a.jitter:
            d += self.rng.uniform(-self.a.jitter, self.a.jitter) / 1000.0
        if self.a.reorder and self.rng.random() * 100 < self.a.reorder:
            d += (2 * self.a.delay + 5) / 1000.0
        dup = bool(self.a.dup and self.rng.random() * 100 < self.a.dup)
        return max(0.0, d), dup

    # ---------------------------------------------------------------- instances
    def inst_by_ip(self, ip):
        for i in self.insts:
            if i.ip == ip:
                return i
        return None

    def fake_by_ip(self, ip):
        for f in self.fakes:
            if f.ip == ip:
                return f
        return None

    def in_subnet(self, ip):
        return ip[:3] == self.subnet[:3]

    def accept_inst(self):
        s, _ = self.lsock.accept()
        s.setblocking(False)
        self.ninst += 1
        inst = Inst(self, s, self.ninst)
        self.insts.append(inst)
        self.sel.register(s, selectors.EVENT_READ, ('ctl', inst))

    def drop_inst(self, inst):
        self.log('%s left' % inst.label())
        try:
            self.sel.unregister(inst.sock)
        except Exception:
            pass
        inst.sock.close()
        for vs in list(inst.socks.values()):
            self.close_vsock(vs)
        if inst in self.insts:
            self.insts.remove(inst)

    def reply(self, inst, typ, seq, err, payload=b''):
        body = struct.pack('<HHi', typ | 0x8000, seq, err) + payload
        try:
            inst.sock.sendall(struct.pack('<I', len(body)) + body)
        except (BlockingIOError, OSError):
            inst.sock.setblocking(True)
            try:
                inst.sock.sendall(struct.pack('<I', len(body)) + body)
            except OSError:
                pass
            inst.sock.setblocking(False)

    def ctl_readable(self, inst):
        try:
            data, fds, _, _ = socket.recv_fds(inst.sock, 65536, 4)
        except BlockingIOError:
            return
        except OSError:
            data, fds = b'', []
        if not data:
            for f in fds:
                os.close(f)
            self.drop_inst(inst)
            return
        inst.rbuf += data
        fdq = list(fds)
        while len(inst.rbuf) >= 4:
            ln = struct.unpack_from('<I', inst.rbuf)[0]
            if len(inst.rbuf) < 4 + ln:
                break
            frame = inst.rbuf[4:4 + ln]
            inst.rbuf = inst.rbuf[4 + ln:]
            typ, seq = struct.unpack_from('<HH', frame)
            fd = fdq.pop(0) if fdq and typ in (M_OPEN, M_CONNECT, M_LISTEN) else -1
            self.request(inst, typ, seq, frame[4:], fd)
        for f in fdq:
            os.close(f)

    def ephemeral(self, inst, kind):
        used = {v.bport for v in inst.socks.values() if v.bound and (v.kind == 1) == (kind == 1)}
        for _ in range(20000):
            p = self.rng.randrange(49152, 65535)
            if p not in used:
                return p
        return 0

    def request(self, inst, typ, seq, p, fd):
        if typ == M_HELLO:
            want, name, pid = p[0:4], p[4:36].split(b'\0')[0].decode(errors='replace'), struct.unpack_from('<I', p, 36)[0]
            if want != b'\0\0\0\0' and self.in_subnet(want) and not self.inst_by_ip(want):
                ip = want
            else:
                taken = {i.ip for i in self.insts if i.ip} | {f.ip for f in self.fakes}
                ip = next(self.subnet[:3] + bytes([h]) for h in range(2, 255) if self.subnet[:3] + bytes([h]) not in taken)
            inst.ip, inst.pid = ip, pid
            inst.name = name or 'VLAN-%d' % ip[3]
            inst.mac = bytes([0x02, 0x4E, 0x56, 0x4C, ip[2], ip[3]])
            self.log('%s joined (pid %d, MAC %s)' % (inst.label(), pid, ':'.join('%02x' % b for b in inst.mac)))
            self.reply(inst, typ, seq, 0, ip + self.mask + inst.mac + b'\0\0' + inst.name.encode()[:31].ljust(32, b'\0'))
        elif typ == M_OPEN:
            sid, kind = struct.unpack_from('<IH', p)
            sk = None
            if fd >= 0:
                sk = socket.socket(fileno=fd)
                sk.setblocking(False)
            vs = VSock(inst, sid, kind, sk)
            inst.socks[sid] = vs
            if sk is not None:
                self.sel.register(sk, selectors.EVENT_READ, ('dgram', vs))
            self.reply(inst, typ, seq, 0)
        elif typ == M_BIND:
            sid, ip, port, flags = struct.unpack_from('<I4sHH', p)
            vs = inst.socks.get(sid)
            if not vs:
                return self.reply(inst, typ, seq, WSAENOTSOCK)
            if ip not in (b'\0\0\0\0', inst.ip) and ip[0] != 127:
                return self.reply(inst, typ, seq, WSAEADDRNOTAVAIL)
            if vs.bound:
                return self.reply(inst, typ, seq, WSAEINVAL)
            tcp = vs.kind == 1
            if port == 0:
                port = self.ephemeral(inst, vs.kind)
            else:
                for o in inst.socks.values():
                    if o is not vs and o.bound and (o.kind == 1) == tcp and o.bport == port and \
                            (o.bip == ip or o.bip == b'\0\0\0\0' or ip == b'\0\0\0\0'):
                        if not (flags & 1):           # Windows: the second binder needs SO_REUSEADDR
                            self.log('%s bind %s %s:%d -> WSAEADDRINUSE' % (inst.label(), 'tcp' if tcp else 'udp', ip2s(ip), port))
                            return self.reply(inst, typ, seq, WSAEADDRINUSE)
            vs.bip, vs.bport, vs.bound, vs.reuse = ip, port, True, bool(flags & 1)
            self.log('%s bind %s %s:%d' % (inst.label(), 'tcp' if tcp else 'icmp' if vs.kind == 3 else 'udp', ip2s(ip), port))
            self.reply(inst, typ, seq, 0, ip + struct.pack('<HH', port, 0))
        elif typ == M_CONNECT:
            sid, ip, port = struct.unpack_from('<I4sH', p)
            self.connect(inst, typ, seq, sid, ip, port, fd)
        elif typ == M_LISTEN:
            sid, backlog = struct.unpack_from('<II', p)
            vs = inst.socks.get(sid)
            if not vs or fd < 0:
                if fd >= 0:
                    os.close(fd)
                return self.reply(inst, typ, seq, WSAEINVAL)
            vs.fd = socket.socket(fileno=fd)
            vs.fd.setblocking(False)
            vs.listening, vs.backlog = True, max(1, backlog)
            self.log('%s listen tcp %s:%d' % (inst.label(), ip2s(vs.bip), vs.bport))
            self.reply(inst, typ, seq, 0)
        elif typ == M_CLOSE:
            sid, flags, cid = struct.unpack_from('<III', p)
            if flags & 1 and cid in self.conns:
                self.rst(self.conns[cid], inst)
            vs = inst.socks.pop(sid, None)
            if vs:
                self.close_vsock(vs)
            self.reply(inst, typ, seq, 0)
        elif typ == M_STATUS:
            cid = struct.unpack_from('<I', p)[0]
            self.reply(inst, typ, seq, 0, struct.pack('<I', self.conn_state.get(cid, 1)))
        elif typ == M_NOTE:
            self.log('%s note: %s' % (inst.label(), p.decode(errors='replace')))
            self.reply(inst, typ, seq, 0)
        else:
            self.reply(inst, typ, seq, WSAEINVAL)

    def close_vsock(self, vs):
        if vs.fd is not None:
            try:
                self.sel.unregister(vs.fd)
            except Exception:
                pass
            vs.fd.close()
            vs.fd = None
        vs.inst.socks.pop(vs.sid, None)

    # ---------------------------------------------------------------- UDP / ICMP
    def src_of(self, vs, dst):
        if dst[0] == 127:
            return b'\x7f\0\0\x01'
        return vs.inst.ip

    def dgram_readable(self, vs):
        for _ in range(64):
            try:
                d = vs.fd.recv(65536 + 16)
            except BlockingIOError:
                return
            except OSError:
                return
            if len(d) < 8:
                return
            dst, dport, _fl = struct.unpack_from('<4sHH', d)
            payload = d[8:]
            if vs.kind == 3:
                self.icmp_out(vs, dst, payload)
            else:
                self.udp_out(vs, dst, dport, payload)

    def udp_targets(self, src_inst, dst, dport):
        out = []
        if dst[0] == 127:
            insts = [src_inst]
        elif dst == b'\xff\xff\xff\xff' or dst == self.bcast:
            insts = list(self.insts)
        else:
            i = self.inst_by_ip(dst)
            insts = [i] if i else []
        bc = dst == b'\xff\xff\xff\xff' or dst == self.bcast
        for i in insts:
            cands = [v for v in i.socks.values() if v.kind == 2 and v.bound and v.bport == dport and v.fd is not None
                     and (v.bip == b'\0\0\0\0' or v.bip == dst or (bc and v.bip == i.ip) or (dst[0] == 127 and v.bip[0] == 127))]
            if not cands:
                continue
            if bc:
                out.extend(cands)
            else:
                exact = [v for v in cands if v.bip == dst]
                out.append((exact or cands)[0])
        return out

    def udp_out(self, vs, dst, dport, payload, src=None, sport=None, label=None):
        src = src or self.src_of(vs, dst)
        sport = sport if sport is not None else vs.bport
        self.stats['udp'] += 1
        who = label or (vs.inst.label() if vs else '?')
        self.dissect_udp(who, src, sport, dst, dport, payload)
        if self.pcap:
            self.pcap.udp(src, sport, dst, dport, payload)
        bc = dst == b'\xff\xff\xff\xff' or dst == self.bcast
        if dst[0] != 127 and not bc and not self.in_subnet(dst):
            self.stats['outside'] += 1
            self.log('DROP %s -> %s:%d: outside the virtual LAN' % (who, ip2s(dst), dport))
            return
        src_inst = vs.inst if vs else None
        targets = self.udp_targets(src_inst, dst, dport) if (src_inst or dst[0] != 127) else []
        f = self.fake_by_ip(dst)
        if f or bc:
            for fk in ([f] if f else self.fakes):
                if fk.ip != src:
                    fk.udp_in(src, sport, dport, payload)
        if not targets and not bc and not f:
            self.stats['dropped_noport'] += 1
            di = self.inst_by_ip(dst)
            if di and vs and vs.fd is not None:           # ICMP port unreachable back to the sender
                self.deliver(vs, dst, dport, b'', 1, 0.0)
            return
        for t in targets:
            same = src_inst is t.inst
            d, dup = self.impair(same)
            if d is None:
                self.stats['lost'] += 1
                continue
            self.stats['bcast_deliv' if bc else 'uni_deliv'] += 1
            self.deliver(t, src, sport, payload, 0, d)
            if dup:
                self.stats['dup'] += 1
                self.deliver(t, src, sport, payload, 0, d + 0.001)

    def deliver(self, vs, src, sport, payload, flags, delay):
        msg = struct.pack('<4sHH', src, sport, flags) + payload
        if delay <= 0:
            self._send_dgram(vs, msg)
        else:
            self.at(time.time() + delay, self._send_dgram, vs, msg)

    def _send_dgram(self, vs, msg):
        if vs.fd is None:
            return
        try:
            vs.fd.send(msg)
        except (BlockingIOError, OSError):
            self.stats['lost'] += 1                     # receiver full: UDP drops it

    def inject_udp(self, src, sport, dst, dport, payload, label):
        """a datagram from a fake host"""
        self.udp_out(None, dst, dport, payload, src=src, sport=sport, label=label)

    def icmp_out(self, vs, dst, msg):
        self.stats['icmp'] += 1
        src = self.src_of(vs, dst)
        if self.pcap:
            self.pcap.icmp(src, dst, msg)
        if len(msg) < 8:
            return
        typ = msg[0]
        if self.a.verbose:
            self.log('%s icmp type %d -> %s id %d seq %d' % (vs.inst.label(), typ, ip2s(dst), *struct.unpack_from('!HH', msg, 4)))
        target = vs.inst if dst[0] == 127 or dst == vs.inst.ip else self.inst_by_ip(dst)
        if target is not None and target is not vs.inst:
            for o in target.socks.values():                # raw sockets see every ICMP message
                if o.kind == 3 and o.fd is not None:
                    self.deliver(o, src, 0, msg, 0, self.impair(False)[0] or 0.0)
        if typ == 8 and (target is not None or self.fake_by_ip(dst)):
            rep = bytes([0, 0, 0, 0]) + msg[4:]
            rep = rep[:2] + struct.pack('!H', csum(rep)) + rep[4:]
            d, _ = self.impair(target is vs.inst)
            if d is None:
                return
            for o in vs.inst.socks.values():
                if o.kind == 3 and o.fd is not None:
                    self.deliver(o, dst, 0, rep, 0, 2 * d)
            if self.pcap:
                self.pcap.icmp(dst, src, rep)

    # ---------------------------------------------------------------- TCP
    def connect(self, inst, typ, seq, sid, dst, dport, fd):
        vs = inst.socks.get(sid)
        if not vs or fd < 0:
            if fd >= 0:
                os.close(fd)
            return self.reply(inst, typ, seq, WSAENOTSOCK)
        loop = dst[0] == 127
        if not vs.bound:
            vs.bip, vs.bport, vs.bound = b'\0\0\0\0', self.ephemeral(inst, 1), True
        lip = b'\x7f\0\0\x01' if loop else inst.ip
        target = inst if loop or dst == inst.ip else self.inst_by_ip(dst)
        delay = 0 if target is inst else int(2 * self.a.delay * 1000)
        fake = self.fake_by_ip(dst)
        lst = None
        if target is not None:
            for o in target.socks.values():
                if o.kind == 1 and o.listening and o.bport == dport and o.fd is not None and \
                        (o.bip == b'\0\0\0\0' or o.bip == dst or (loop and o.bip[0] == 127)):
                    lst = o
                    break
        fk_ok = fake is not None and dport == 9900
        if lst is None and not fk_ok:
            os.close(fd)
            if target is None and not fake:
                self.log('%s connect %s:%d -> no such host (WSAEHOSTUNREACH)' % (inst.label(), ip2s(dst), dport))
                return self.reply(inst, typ, seq, WSAEHOSTUNREACH, lip + struct.pack('<HHII', vs.bport, 0, 0, 3000000))
            self.stats['tcp_refused'] += 1
            self.log('%s connect %s:%d -> refused' % (inst.label(), ip2s(dst), dport))
            if self.pcap:
                self.pcap.tcp(lip, vs.bport, dst, dport, 0, 0, 0x02)
                self.pcap.tcp(dst, dport, lip, vs.bport, 0, 1, 0x14)
            return self.reply(inst, typ, seq, WSAECONNREFUSED, lip + struct.pack('<HHII', vs.bport, 0, 0, delay))
        if lst is not None and lst.pending >= lst.backlog + 4:
            os.close(fd)
            self.stats['tcp_refused'] += 1
            return self.reply(inst, typ, seq, WSAECONNREFUSED, lip + struct.pack('<HHII', vs.bport, 0, 0, delay))
        cid = self.next_conn
        self.next_conn += 1
        a = socket.socket(fileno=fd)
        a.setblocking(False)
        if lst is not None:
            h, app = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
            h.setblocking(False)
            hdr = struct.pack('<4sHH4sHHI', lip, vs.bport, 0, dst if not loop else b'\x7f\0\0\x01', dport, 0, cid)
            try:
                socket.send_fds(lst.fd, [hdr], [app.fileno()])
            except OSError as e:
                self.log('accept channel of %s: %s' % (lst.inst.label(), e))
            # Keep our copy of the passed end open until the connection ends.
            # While it waits in the listener's queue, an in-flight socket whose
            # only reference is the message itself can be flushed by the
            # kernel's unix-socket garbage collector when the receiving process
            # closes some other unix socket (measured: the lobby login's bytes
            # vanished and the accepted socket read EOF).
            app_keep = app
            sname = lst.inst.label()
        else:
            h = None                                     # the fake server is the other end
            sname = fake.label
        c = Conn(self, cid, a, h, (lip, vs.bport), (dst if not loop else b'\x7f\0\0\x01', dport))
        if lst is None:
            c.fake = fake
        else:
            c.app_keep = app_keep
        self.conns[cid] = c
        self.conn_state[cid] = 0
        self.sel.register(a, selectors.EVENT_READ, ('tcp', (c, 0)))
        if h is not None:
            self.sel.register(h, selectors.EVENT_READ, ('tcp', (c, 1)))
        self.stats['tcp_conn'] += 1
        self.log('tcp #%d %s:%d -> %s:%d (%s -> %s)%s' % (cid, ip2s(lip), vs.bport, ip2s(dst), dport, inst.label(), sname,
                 ' [hub fds %d/%d]' % (a.fileno(), h.fileno() if h else -1) if self.a.verbose else ''))
        if self.pcap:
            s0, s1 = c.seq
            self.pcap.tcp(lip, vs.bport, dst, dport, s0, 0, 0x02)
            self.pcap.tcp(dst, dport, lip, vs.bport, s1, s0 + 1, 0x12)
            self.pcap.tcp(lip, vs.bport, dst, dport, s0 + 1, s1 + 1, 0x10)
            c.seq = [s0 + 1, s1 + 1]
        self.reply(inst, typ, seq, 0, lip + struct.pack('<HHII', vs.bport, 0, cid, delay))

    def tcp_readable(self, c, side):
        if c.closed:
            return
        s = c.end[side]
        try:
            d = s.recv(65536)
        except BlockingIOError:
            return
        except OSError:
            d = b''
        other = 1 - side
        if self.pcap:
            src, dst = c.addr[side], c.addr[other]
        if not d:
            c.eof[side] = True
            try:
                self.sel.unregister(s)
            except Exception:
                pass
            src_, dst_ = c.addr[side], c.addr[other]
            self.log('tcp #%d %s:%d -> %s:%d FIN' % (c.id, ip2s(src_[0]), src_[1], ip2s(dst_[0]), dst_[1]))
            if self.pcap:
                self.pcap.tcp(src[0], src[1], dst[0], dst[1], c.seq[side], c.seq[other], 0x11)
                c.seq[side] += 1
            if self.conn_state.get(c.id) == 0:
                self.conn_state[c.id] = 1
            self.schedule_tcp(c, other, None)
            return
        self.stats['tcp_bytes'] += len(d)
        if self.pcap:
            for i in range(0, len(d), 1400):
                ch = d[i:i + 1400]
                self.pcap.tcp(src[0], src[1], dst[0], dst[1], c.seq[side], c.seq[other], 0x18, ch)
                c.seq[side] += len(ch)
        self.dissect_tcp(c, side, d)
        self.schedule_tcp(c, other, d)

    def schedule_tcp(self, c, into, data):
        same = c.addr[0][0] == c.addr[1][0]
        d, _ = self.impair(same)
        d = d or 0.0                                     # TCP: never lost
        t = max(time.time() + d, c.last_t[into])
        c.last_t[into] = t
        if t <= time.time():
            self._tcp_write(c, into, data)
        else:
            self.at(t, self._tcp_write, c, into, data)

    def _tcp_write(self, c, into, data):
        if c.closed:
            return
        if c.end[into] is None:                          # the fake server's side
            if data is None:
                self.close_conn(c)
            return
        if data is None:
            c.shut_pending[into] = True
        else:
            c.out[into] += data
        self.flush(c, into)

    def flush(self, c, into):
        s = c.end[into]
        while c.out[into]:
            try:
                n = s.send(c.out[into])
            except BlockingIOError:
                self.at(time.time() + 0.005, self.flush, c, into)
                return
            except OSError as e:
                self.log('tcp #%d: write into the %s side failed (%s), %d bytes dropped' %
                         (c.id, 'connector' if into == 0 else 'acceptor', e, len(c.out[into])))
                c.out[into].clear()
                break
            if self.a.verbose:
                self.log('tcp #%d: %d bytes into the %s side' % (c.id, n, 'connector' if into == 0 else 'acceptor'))
            del c.out[into][:n]
        if c.shut_pending[into] and not c.shut[into]:
            c.shut[into] = True
            try:
                s.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        ended = [c.eof[i] or c.end[i] is None for i in (0, 1)]
        if all(ended) and not any(c.out):
            self.close_conn(c)

    def rst(self, c, by):
        if c.closed:
            return
        self.stats['tcp_reset'] += 1
        self.conn_state[c.id] = 2
        self.log('tcp #%d reset by %s' % (c.id, by.label()))
        if self.pcap:
            side = 0 if by.ip == c.addr[0][0] else 1
            src, dst = c.addr[side], c.addr[1 - side]
            self.pcap.tcp(src[0], src[1], dst[0], dst[1], c.seq[side], c.seq[1 - side], 0x14)
        self.close_conn(c)

    def close_conn(self, c):
        if c.closed:
            return
        if self.a.verbose:
            self.log('tcp #%d closed [hub fds %s]' % (c.id, '/'.join(str(x.fileno()) if x else '-' for x in c.end)))
        c.closed = True
        for s in c.end:
            if s is None:
                continue
            try:
                self.sel.unregister(s)
            except Exception:
                pass
            s.close()
        if c.app_keep is not None:
            c.app_keep.close()
            c.app_keep = None
        self.conns.pop(c.id, None)
        if self.conn_state.get(c.id) == 0:
            self.conn_state[c.id] = 1

    # ---------------------------------------------------------------- dissector
    def dissect_udp(self, who, src, sport, dst, dport, p):
        tag = '%s:%d -> %s:%d' % (ip2s(src), sport, ip2s(dst), dport)
        if p[:3] == b'gEA' and len(p) >= 8:
            kind = p[8:40].split(b'\0')[0].decode(errors='replace')
            if p[8:9] == b'?' and p[9:10] == b'\0':
                self.log('ADVT %s query (%d bytes) [%s]' % (tag, len(p), who))
            else:
                name = p[0x28:0x48].split(b'\0')[0].decode(errors='replace')
                note = p[0x48:0x108].split(b'\0')[0].decode(errors='replace')
                addr = p[0x108:0x180].split(b'\0')[0].decode(errors='replace')
                self.log('ADVT %s advert kind=%s name=%r note=%r addr=%r period=%ds ident=%s (%d bytes) [%s]' %
                         (tag, kind, name, note, addr, p[3], p[4:8].hex(), len(p), who))
        elif dport == 3658 or sport == 3658:
            if self.a.verbose:
                self.log('RACE %s %d bytes %s' % (tag, len(p), p[:24].hex()))
        elif self.a.verbose:
            self.log('UDP  %s %d bytes %s' % (tag, len(p), p[:24].hex()))
        if self.a.hexdump:
            self.log('     ' + p.hex())

    def dissect_tcp(self, c, side, d):
        buf = c.stream[side]
        buf += d
        src, dst = c.addr[side], c.addr[1 - side]
        tag = 'tcp #%d %s:%d -> %s:%d' % (c.id, ip2s(src[0]), src[1], ip2s(dst[0]), dst[1])
        while len(buf) >= 12:
            kind, code, ln = struct.unpack_from('>4sII', buf)
            if ln < 12 or ln > 0x40000 or not all(32 <= b < 127 for b in kind):
                self.log('%s %d bytes (not a lobby frame) %s' % (tag, len(buf), bytes(buf[:32]).hex()))
                buf.clear()
                break
            if len(buf) < ln:
                break
            body = bytes(buf[12:ln]).split(b'\0')[0].decode(errors='replace')
            self.log('LOBBY %s %s code=0x%x len=%d %s' % (tag, kind.decode(), code, ln, body.replace('\n', ' ').replace('\t', ' ')))
            del buf[:ln]

    # ---------------------------------------------------------------- loop
    def run(self):
        self.log('listening on %s, subnet %s/24, delay %gms jitter %gms loss %g%% dup %g%% reorder %g%%' %
                 (self.a.sock, ip2s(self.subnet), self.a.delay, self.a.jitter, self.a.loss, self.a.dup, self.a.reorder))
        end = time.time() + self.a.quit_after if self.a.quit_after else None
        self.running = True
        while self.running:
            now = time.time()
            if end and now > end:
                break
            for f in self.fakes:
                f.tick(now)
            timeout = 0.05
            if self.timers:
                timeout = max(0.0, min(timeout, self.timers[0][0] - now))
            for key, _ in self.sel.select(timeout):
                kind, obj = key.data
                if kind == 'listen':
                    self.accept_inst()
                elif kind == 'ctl':
                    self.ctl_readable(obj)
                elif kind == 'dgram':
                    self.dgram_readable(obj)
                elif kind == 'tcp':
                    self.tcp_readable(*obj)
            now = time.time()
            while self.timers and self.timers[0][0] <= now:
                _, _, fn, args = heapq.heappop(self.timers)
                fn(*args)
        self.log('stats: ' + ' '.join('%s=%d' % kv for kv in self.stats.items()))
        try:
            os.unlink(self.a.sock)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--sock', required=True)
    ap.add_argument('--subnet', default='10.77.0')
    ap.add_argument('--pcap')
    ap.add_argument('--log')
    ap.add_argument('--delay', type=float, default=0.0)
    ap.add_argument('--jitter', type=float, default=0.0)
    ap.add_argument('--loss', type=float, default=0.0)
    ap.add_argument('--dup', type=float, default=0.0)
    ap.add_argument('--reorder', type=float, default=0.0)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--fake-server', action='append')
    ap.add_argument('--hexdump', action='store_true')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--quit-after', type=float, default=0.0)
    a = ap.parse_args()
    hub = Hub(a)

    def stop(*_):
        hub.running = False
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    hub.run()


if __name__ == '__main__':
    main()
