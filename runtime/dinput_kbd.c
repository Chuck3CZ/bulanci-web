/* The DirectInput 8 keyboard device (com.c hands it every call on a device
 * of kind 1 but AddRef/Release/QueryInterface): immediate state
 * (GetDeviceState, 256 bytes of DIK_* 0x80 = down) and buffered data
 * (GetDeviceData) from kbd.h, which the browser's key events feed
 * (runtime/web/input_web.c, KSRC_HOST).  CMR5 reads its keys here; it took
 * the place of nfsu2_compat's stand-in, which failed every call. */
#include "cpu.h"
#include "kbd.h"
#include <stdio.h>
#include <string.h>

void mem_commit(uint32_t, uint32_t);

#define DI_OK            0u
#define DI_BUFFEROVERFLOW 1u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIGDD_PEEK       1u

/* per device: the next event GetDeviceData returns */
#define KDEV_MAX 8
static struct { uint32_t self, seq; int acquired; } g_kd[KDEV_MAX];
static uint64_t g_states, g_events;

static int kd_slot(uint32_t self){
    int f = -1;
    for (int k = 0; k < KDEV_MAX; k++){
        if (g_kd[k].self == self) return k;
        if (!g_kd[k].self && f < 0) f = k;
    }
    if (f < 0) f = 0;
    g_kd[f].self = self; g_kd[f].seq = kbd_ev_head(); g_kd[f].acquired = 0;
    return f;
}

void dikbd_created(uint32_t dev){ (void)kd_slot(dev); }

void dikbd_report(void){
    fprintf(stderr, "  DirectInput keyboard: %llu state reads, %llu buffered events\n",
            (unsigned long long)g_states, (unsigned long long)g_events);
}

uint32_t dikbd_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *a){
    (void)c;
    int k = kd_slot(self);
    if (!strcmp(nm, "GetDeviceState")){                    /* (self, cbData, lpvData) */
        uint32_t cb = nargs > 1 ? a[1] : 0, p = nargs > 2 ? a[2] : 0;
        if (!p || !cb || cb > 4096u) return DIERR_INVALIDPARAM;
        uint8_t st[256]; kbd_state(st, 256);
        mem_commit(p, cb); memset(g_mem + p, 0, cb);
        memcpy(g_mem + p, st, cb < 256u ? cb : 256u);
        g_states++;
        return DI_OK;
    }
    if (!strcmp(nm, "GetDeviceData")){                     /* (self, cbObjectData, rgdod, pdwInOut, dwFlags) */
        uint32_t cbo = a[1], rg = a[2], pio = a[3], fl = nargs > 4 ? a[4] : 0;
        if (!pio || (cbo != 16u && cbo != 20u)) return DIERR_INVALIDPARAM;
        uint32_t want = MR32(pio), n = 0, seq = g_kd[k].seq, r = DI_OK;
        KbdEv e; int got;
        while ((got = kbd_ev_get(seq, &e)) != 0){
            if (got < 0){ seq = kbd_ev_head(); r = DI_BUFFEROVERFLOW; break; }   /* overwritten */
            if (rg){
                if (n >= want) break;
                uint32_t d = rg + n * cbo;
                mem_commit(d, cbo); memset(g_mem + d, 0, cbo);
                MR32(d) = e.dik; MR32(d + 4) = e.down ? 0x80u : 0u;
                MR32(d + 8) = e.ms; MR32(d + 12) = e.seq;
            }
            n++; seq++;
            if (!rg && want != 0xFFFFFFFFu && n >= want) break;
        }
        if (!(fl & DIGDD_PEEK)) g_kd[k].seq = seq;
        MR32(pio) = n; g_events += n;
        return r;
    }
    if (!strcmp(nm, "Acquire")){
        if (!g_kd[k].acquired){ g_kd[k].acquired = 1; g_kd[k].seq = kbd_ev_head(); }
        return DI_OK;
    }
    if (!strcmp(nm, "Unacquire")){ g_kd[k].acquired = 0; return DI_OK; }
    if (!strcmp(nm, "GetCapabilities")){                   /* DIDEVCAPS */
        uint32_t p = a[1]; if (!p) return DIERR_INVALIDPARAM;
        uint32_t sz = MR32(p); if (!sz || sz > 256u) sz = 44u;
        mem_commit(p, sz); memset(g_mem + p, 0, sz); MR32(p) = sz;
        MR32(p + 4) = 0x1u;                                  /* DIDC_ATTACHED */
        MR32(p + 8) = 0x413u;                                /* DI8DEVTYPE_KEYBOARD, PCENH */
        MR32(p + 16) = 128u;                                 /* dwButtons */
        return DI_OK;
    }
    if (!strcmp(nm, "GetDeviceInfo")){                     /* DIDEVICEINSTANCEA: size kept, the rest zero */
        uint32_t p = a[1]; if (!p) return DIERR_INVALIDPARAM;
        uint32_t sz = MR32(p); if (!sz || sz > 1024u) sz = 580u;
        mem_commit(p, sz); memset(g_mem + p, 0, sz); MR32(p) = sz;
        MR32(p + 36) = 0x413u;                               /* dwDevType */
        snprintf((char *)(g_mem + p + 40), 260, "Keyboard");
        snprintf((char *)(g_mem + p + 300), 260, "Keyboard");
        return DI_OK;
    }
    if (!strcmp(nm, "GetProperty")){
        uint32_t p = nargs > 2 ? a[2] : 0;
        if (p){ uint32_t sz = MR32(p); if (sz >= 20u && sz <= 256u){ mem_commit(p, sz); memset(g_mem + p + 16, 0, sz - 16); } }
        return DI_OK;
    }
    /* SetDataFormat, SetCooperativeLevel, SetProperty, Poll, SetEventNotification, EnumObjects, ... */
    return DI_OK;
}
