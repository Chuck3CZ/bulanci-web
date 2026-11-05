/* dplay.c -- DirectPlay 4 (IDirectPlay4A, IDirectPlayLobby3A) for one machine.
 *
 * Bulanci runs every game through DirectPlay, the one-machine game too: its
 * CDSDirectPlay (0x43ae70) does CoCreateInstance(CLSID_DirectPlay,
 * IID_IDirectPlay4A) and CoCreateInstance(CLSID_DirectPlayLobby,
 * IID_IDirectPlayLobby3A), builds a TCP/IP address with
 * IDirectPlayLobby2::CreateCompoundAddress (0x43b360) or picks the service
 * provider from EnumConnections (0x43b620), InitializeConnection, Open
 * (DPOPEN_CREATE 0x43afa0, DPOPEN_JOIN 0x43af00), CreatePlayer, and from then
 * on Send (its sender thread, 0x43ad20) and Receive (polled, 0x43ab70).  With
 * no class registered it went on with a NULL IDirectPlay4A, and "Hrat" ended
 * in garbage: a stream overflow or a 1 GB allocation.
 *
 * Here a session lives inside this process: Open(CREATE) makes it, the
 * players are local, Send queues the message for its addressees and Receive
 * hands it out, with DirectPlay's rules (DPID_ALLPLAYERS is everyone but the
 * sender; a message that does not fit stays queued and the size comes back
 * with DPERR_BUFFERTOOSMALL).  Joining finds no session (EnumSessions lists
 * none, Open(JOIN) fails with DPERR_NOSESSIONS): the network game is a later
 * step (docs/PLAN.md).
 *
 * Objects are guest memory, as in ddraw.c: [obj] = vtable of COM magic
 * numbers (interfaces 0x104 and 0x105), [obj+4] = interface, [obj+8] = refs.
 * DPLOG=1 prints every call. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

uint32_t com_scratch(uint32_t n);
uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);

#define COM_MAGIC 0xE0000000u
enum { DP_IF_DP = 0x104, DP_IF_LOBBY, DP_IF_END };

#define DP_OK                 0x00000000u
#define DPERR_BUFFERTOOSMALL  0x8877001Eu
#define DPERR_INVALIDPARAMS   0x80070057u
#define DPERR_INVALIDPLAYER   0x88770096u
#define DPERR_NOMESSAGES      0x887700BEu
#define DPERR_NOSESSIONS      0x88770104u
#define DPERR_UNSUPPORTED     0x80004001u
#define DPERR_NOTLOBBIED      0x887703FEu
#define E_NOINTERFACE         0x80004002u

#define DPOPEN_JOIN           0x1u
#define DPOPEN_CREATE         0x2u
#define DPRECEIVE_TOPLAYER    0x1u
#define DPRECEIVE_FROMPLAYER  0x2u
#define DPRECEIVE_PEEK        0x8u
#define DPID_ALLPLAYERS       0u
#define DPESC_TIMEDOUT        0x1u
#define DPENUMSESSIONS_ASYNC     0x10u
#define DPENUMSESSIONS_STOPASYNC 0x20u
#define DPPLAYER_SERVERPLAYER 0x100u

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
#define GUID_DPSP_TCPIP             0x36E95EE0u
#define GUID_DPSP_IPX               0x685BC400u
static const uint8_t DPAID_TotalSize[16]       = { 0x60,0xF5,0x18,0x13, 0x2C,0x91, 0xD0,0x11, 0x9D,0xAA,0x00,0xA0,0xC9,0x0A,0x43,0xCB };
static const uint8_t DPAID_ServiceProvider[16] = { 0xC0,0x16,0xD9,0x07, 0xAF,0xE0, 0xCF,0x11, 0x9C,0x4E,0x00,0xA0,0xC9,0x05,0x42,0x5E };
static const uint8_t DPSPGUID_TCPIP[16]        = { 0xE0,0x5E,0xE9,0x36, 0x77,0x85, 0xCF,0x11, 0x96,0x0C,0x00,0x80,0xC7,0x53,0x4E,0x82 };
static const uint8_t DPSPGUID_IPX[16]          = { 0x00,0xC4,0x5B,0x68, 0x2C,0x9D, 0xCF,0x11, 0xA9,0xCD,0x00,0xAA,0x00,0x68,0x86,0xE3 };

static int dplog(void){ static int on = -1; if (on < 0){ const char *e = getenv("DPLOG"); on = e ? atoi(e) : 0; } return on; }

/* --- the session and its players ------------------------------------------ */
#define NPLAYER 32
typedef struct { uint32_t id; int live; uint32_t flags; char name[64]; uint8_t *data; uint32_t datasize; } Player;
typedef struct Msg { struct Msg *next; uint32_t from, to, size; uint8_t data[]; } Msg;
static struct {
    int open;                    /* Open(CREATE) done, Close not yet */
    uint8_t desc[0x50];          /* DPSESSIONDESC2 as given, guidInstance filled */
    char sname[64];
    Player p[NPLAYER];
    uint32_t next_id;
    Msg *head, *tail; uint32_t nmsg;
} S;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_vt[2];
static uint32_t g_inited;        /* InitializeConnection done (on any object) */

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

static void queue(uint32_t from, uint32_t to, uint32_t data, uint32_t size){
    Msg *m = malloc(sizeof *m + size);
    if (!m) return;
    m->next = 0; m->from = from; m->to = to; m->size = size;
    if (size) memcpy(m->data, g_mem + data, size);
    if (S.tail) S.tail->next = m; else S.head = m;
    S.tail = m; S.nmsg++;
}
static void drop_all(void){
    for (Msg *m = S.head, *n; m; m = n){ n = m->next; free(m); }
    S.head = S.tail = 0; S.nmsg = 0;
}
/* Send: DPID_ALLPLAYERS goes to every local player except the sender, a
 * player id to that player (itself included). */
static uint32_t dp_send(uint32_t from, uint32_t to, uint32_t data, uint32_t size){
    if (!S.open) return DPERR_INVALIDPARAMS;
    if (from && !player(from)) return DPERR_INVALIDPLAYER;
    if (to == DPID_ALLPLAYERS){
        for (int i = 0; i < NPLAYER; i++) if (S.p[i].live && S.p[i].id != from) queue(from, S.p[i].id, data, size);
        return DP_OK;
    }
    if (!player(to)) return DPERR_INVALIDPLAYER;
    queue(from, to, data, size);
    return DP_OK;
}
/* Receive(lpidFrom, lpidTo, dwFlags, lpData, lpdwDataSize) */
static uint32_t dp_receive(uint32_t pfrom, uint32_t pto, uint32_t flags, uint32_t data, uint32_t psize){
    if (!psize) return DPERR_INVALIDPARAMS;
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

static uint32_t dp_method(CPU *c, uint32_t m, uint32_t self){
    switch (m){
    case 0: {   /* QueryInterface(riid, ppv): the DirectPlay interfaces are all this object */
        uint32_t riid = A_(c, 1), ppv = A_(c, 2);
        if (!ppv) return DPERR_INVALIDPARAMS;
        MR32(ppv) = self; MR32(self + 8)++;
        (void)riid; return DP_OK; }
    case 1: return ++MR32(self + 8);
    case 2: { uint32_t r = MR32(self + 8); if (r) r = --MR32(self + 8); return r; }
    case 4:     /* Close */
        S.open = 0; drop_all();
        for (int i = 0; i < NPLAYER; i++){ free(S.p[i].data); memset(&S.p[i], 0, sizeof S.p[i]); }
        return DP_OK;
    case 6: {   /* CreatePlayer(lpidPlayer, lpPlayerName, hEvent, lpData, dwDataSize, dwFlags) */
        uint32_t pid = A_(c, 1);
        if (!pid) return DPERR_INVALIDPARAMS;
        if (!S.open) return DPERR_INVALIDPARAMS;
        Player *p = 0;
        for (int i = 0; i < NPLAYER && !p; i++) if (!S.p[i].live) p = &S.p[i];
        if (!p) return DPERR_INVALIDPARAMS;
        memset(p, 0, sizeof *p);
        p->live = 1; p->id = S.next_id++; p->flags = A_(c, 6);
        name_of(p->name, sizeof p->name, A_(c, 2));
        uint32_t d = A_(c, 4), n = A_(c, 5);
        if (d && n && (p->data = malloc(n))){ memcpy(p->data, g_mem + d, n); p->datasize = n; }
        MR32(S.desc + 0x2c)++;                          /* dwCurrentPlayers */
        MR32(pid) = p->id;
        return DP_OK; }
    case 9: {   /* DestroyPlayer(id) */
        Player *p = player(A_(c, 1));
        if (!p) return DPERR_INVALIDPLAYER;
        free(p->data); memset(p, 0, sizeof *p);
        if (MR32(S.desc + 0x2c)) MR32(S.desc + 0x2c)--;
        return DP_OK; }
    case 12: {  /* EnumPlayers(lpguidInstance, cb, ctx, flags): cb(id, type, lpName, flags, ctx) */
        uint32_t cb = A_(c, 2), ctx = A_(c, 3);
        if (!S.open) return DPERR_NOSESSIONS;
        for (int i = 0; i < NPLAYER; i++){
            if (!S.p[i].live) continue;
            uint32_t nm = com_scratch(16 + 64), s = nm + 16;
            memset(g_mem + nm, 0, 16 + 64); MR32(nm) = 16;
            memcpy(g_mem + s, S.p[i].name, strlen(S.p[i].name) + 1);
            MR32(nm + 8) = s;
            uint32_t a[5] = { S.p[i].id, 1u /* DPPLAYERTYPE_PLAYER */, nm, S.p[i].flags, ctx };
            if (!call_guest(c, cb, 5, a)) break;
        }
        return DP_OK; }
    case 13: {  /* EnumSessions(lpsd, dwTimeout, cb, ctx, flags): no session to find on this machine */
        uint32_t cb = A_(c, 3), ctx = A_(c, 4), flags = A_(c, 5);
        if (!(flags & (DPENUMSESSIONS_ASYNC | DPENUMSESSIONS_STOPASYNC)) && cb){
            uint32_t to = com_scratch(4); MR32(to) = 0;
            uint32_t a[4] = { 0, to, DPESC_TIMEDOUT, ctx };
            call_guest(c, cb, 4, a);
        }
        return DP_OK; }
    case 14: {  /* GetCaps(lpDPCaps, flags) */
        uint32_t p = A_(c, 1); if (!p) return DPERR_INVALIDPARAMS;
        uint32_t sz = MR32(p); if (sz < 40) sz = 40;
        memset(g_mem + p, 0, sz); MR32(p) = sz;
        MR32(p + 8) = 0x10000u;          /* dwMaxBufferSize */
        MR32(p + 16) = NPLAYER;          /* dwMaxPlayers */
        MR32(p + 20) = 100000;           /* dwHundredBaud */
        MR32(p + 24) = 1;                /* dwLatency */
        MR32(p + 28) = NPLAYER;          /* dwMaxLocalPlayers */
        MR32(p + 32) = 20;               /* dwHeaderLength */
        MR32(p + 36) = 1000;             /* dwTimeout */
        return DP_OK; }
    case 17: {  /* GetMessageCount(idPlayer, lpdwCount) */
        uint32_t id = A_(c, 1), pc = A_(c, 2), n = 0;
        if (!pc) return DPERR_INVALIDPARAMS;
        for (Msg *m = S.head; m; m = m->next) if (m->to == id) n++;
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
    case 24: {  /* Open(lpsd, dwFlags) */
        uint32_t sd = A_(c, 1), flags = A_(c, 2);
        if (!sd) return DPERR_INVALIDPARAMS;
        if (flags & DPOPEN_CREATE){
            drop_all();
            for (int i = 0; i < NPLAYER; i++){ free(S.p[i].data); memset(&S.p[i], 0, sizeof S.p[i]); }
            memcpy(S.desc, g_mem + sd, 0x50);
            MR32(S.desc) = 0x50;
            for (int i = 0; i < 16; i++) S.desc[8 + i] = (uint8_t)(0xB0 + i);   /* guidInstance */
            MR32(S.desc + 0x2c) = 0;
            gstr(S.sname, sizeof S.sname, MR32(sd + 0x30));
            MR32(S.desc + 0x30) = 0; MR32(S.desc + 0x34) = 0;
            S.next_id = 0x1000u;
            S.open = 1;
            return DP_OK;
        }
        return DPERR_NOSESSIONS; }
    case 25: return dp_receive(A_(c, 1), A_(c, 2), A_(c, 3), A_(c, 4), A_(c, 5));
    case 26: return dp_send(A_(c, 1), A_(c, 2), A_(c, 4), A_(c, 5));   /* Send(from, to, flags, data, size) */
    case 29: {  /* SetPlayerData(id, lpData, dwDataSize, flags) */
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
    case 35: {  /* EnumConnections(lpguidApplication, cb, ctx, flags):
                 * cb(lpguidSP, lpConnection, dwConnectionSize, lpName, dwFlags, ctx) */
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
        return DP_OK; }
    case 38: g_inited = 1; return DP_OK;   /* InitializeConnection(lpConnection, flags) */
    case 46: {  /* GetPlayerFlags(id, lpdwFlags) */
        Player *p = player(A_(c, 1)); uint32_t pf = A_(c, 2);
        if (!p) return DPERR_INVALIDPLAYER;
        if (pf) MR32(pf) = p->flags | 0x8u /* DPPLAYER_LOCAL */;
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
    uint32_t self = MR32(c->esp);
    /* the callbacks (EnumConnections, EnumPlayers, EnumSessions) run guest
     * code, which may come back here: no lock held across them */
    int cb = iface == DP_IF_DP && (m == 12 || m == 13 || m == 35);
    if (!cb) pthread_mutex_lock(&g_lock);
    uint32_t r = iface == DP_IF_DP ? dp_method(c, m, self) : lobby_method(c, m, self);
    if (!cb) pthread_mutex_unlock(&g_lock);
    int quiet = iface == DP_IF_DP && r == DPERR_NOMESSAGES && m == 25;   /* Receive is polled every frame */
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
    fprintf(stderr, "[dplay] %s object 0x%08x\n", d1 == GUID_CLSID_DirectPlay ? "IDirectPlay4A" : "IDirectPlayLobby3A", o);
    return 1;
}
