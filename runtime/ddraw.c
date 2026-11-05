/* ddraw.c -- DirectDraw (the version 1 interfaces) for a 2D game.
 *
 * Bulanci draws with IDirectDraw / IDirectDrawSurface (DirectDrawCreate, no
 * QueryInterface to the later versions): a primary surface with a back buffer
 * (or a primary it blits to), off-screen surfaces with the sprites, a source
 * colour key, Blt / BltFast and Lock.  Here every surface is plain guest
 * memory (the game writes it through Lock), the blits are done on the CPU,
 * and showing the primary (Flip, or a Blt onto it) uploads it as a texture
 * and blits that to the screen (gpu_web.c: metal_*).
 *
 * The objects are guest memory too: vtable, then the surface's index here.
 * Their vtables hold COM magic numbers (com.c's scheme) of interfaces 0x100..;
 * com_call hands those to ddraw_call. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t com_scratch(uint32_t n);
int  metal_available(void);
unsigned metal_create_texture(unsigned w, unsigned h, unsigned levels, unsigned f);
void metal_upload_texture(unsigned id, unsigned level, const void *data, unsigned pitch, unsigned w, unsigned h, unsigned f);
void metal_free_texture(unsigned id);
void metal_begin_frame(void);
void metal_blit(unsigned src, unsigned dst);
void metal_present(void);
void metal_want_size(unsigned w, unsigned h);

#define COM_MAGIC 0xE0000000u
enum { DD_IF_DD = 0x100, DD_IF_SURF, DD_IF_PAL, DD_IF_CLIP, DD_IF_END };

typedef struct { const char *name; int nargs; } DdM;
static const DdM M_DD[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"Compact",1},{"CreateClipper",4},{"CreatePalette",5},
    {"CreateSurface",4},{"DuplicateSurface",3},{"EnumDisplayModes",5},{"EnumSurfaces",5},{"FlipToGDISurface",1},
    {"GetCaps",3},{"GetDisplayMode",2},{"GetFourCCCodes",3},{"GetGDISurface",2},{"GetMonitorFrequency",2},
    {"GetScanLine",2},{"GetVerticalBlankStatus",2},{"Initialize",2},{"RestoreDisplayMode",1},
    {"SetCooperativeLevel",3},{"SetDisplayMode",4},{"WaitForVerticalBlank",3} };
static const DdM M_SURF[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"AddAttachedSurface",2},{"AddOverlayDirtyRect",2},
    {"Blt",6},{"BltBatch",4},{"BltFast",6},{"DeleteAttachedSurface",3},{"EnumAttachedSurfaces",3},
    {"EnumOverlayZOrders",4},{"Flip",3},{"GetAttachedSurface",3},{"GetBltStatus",2},{"GetCaps",2},
    {"GetClipper",2},{"GetColorKey",3},{"GetDC",2},{"GetFlipStatus",2},{"GetOverlayPosition",3},
    {"GetPalette",2},{"GetPixelFormat",2},{"GetSurfaceDesc",2},{"Initialize",3},{"IsLost",1},{"Lock",5},
    {"ReleaseDC",2},{"Restore",1},{"SetClipper",2},{"SetColorKey",3},{"SetOverlayPosition",3},
    {"SetPalette",2},{"Unlock",2},{"UpdateOverlay",6},{"UpdateOverlayDisplay",2},{"UpdateOverlayZOrder",3} };
static const DdM M_PAL[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetCaps",2},{"GetEntries",5},{"Initialize",4},{"SetEntries",5} };
static const DdM M_CLIP[] = {
    {"QueryInterface",3},{"AddRef",1},{"Release",1},{"GetClipList",4},{"GetHWnd",2},{"Initialize",3},
    {"IsClipListChanged",2},{"SetClipList",3},{"SetHWnd",3} };
static const struct { const DdM *m; unsigned n; const char *name; } g_ddif[] = {
    { M_DD, sizeof M_DD / sizeof *M_DD, "IDirectDraw" }, { M_SURF, sizeof M_SURF / sizeof *M_SURF, "IDirectDrawSurface" },
    { M_PAL, sizeof M_PAL / sizeof *M_PAL, "IDirectDrawPalette" }, { M_CLIP, sizeof M_CLIP / sizeof *M_CLIP, "IDirectDrawClipper" } };

#define NSURF 4096
typedef struct {
    uint32_t obj, mem, w, h, pitch, bpp, caps;
    uint32_t ck_lo, ck_hi; int has_ck;
    int back;            /* attached back buffer (flip chain), 0 none */
    int pal;             /* palette object index */
    int ref;
} Surf;
static Surf g_s[NSURF]; static int g_ns = 1;
static uint32_t g_pal_rgb[64][256]; static int g_npal = 1;   /* palettes: 0x00RRGGBB */
static uint32_t g_vt[4], g_dd;
static uint32_t g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static int g_primary;
static unsigned g_tex, g_tex_w, g_tex_h, g_tex_f;
static uint32_t *g_conv;
static long g_frames;
static int g_dirty;

static int ddlog(void){ static int on = -1; if (on < 0) on = getenv("DDLOG") ? atoi(getenv("DDLOG")) : 0; return on; }

static uint32_t new_obj(int iface, uint32_t idx){
    int k = iface - DD_IF_DD;
    if (!g_vt[k]){
        g_vt[k] = com_scratch(4u * g_ddif[k].n);
        for (unsigned i = 0; i < g_ddif[k].n; i++) MR32(g_vt[k] + 4u*i) = COM_MAGIC | ((uint32_t)iface << 16) | i;
    }
    uint32_t o = com_scratch(16);
    MR32(o) = g_vt[k]; MR32(o + 4) = idx; MR32(o + 8) = 1;
    return o;
}

/* DDPIXELFORMAT (32 bytes) of a bit depth */
static void put_pf(uint32_t p, uint32_t bpp){
    memset(g_mem + p, 0, 32);
    MR32(p) = 32; MR32(p + 12) = bpp;
    if (bpp == 8){ MR32(p + 4) = 0x60u; }                         /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    else { MR32(p + 4) = 0x40u;                                   /* DDPF_RGB */
        if (bpp == 16){ MR32(p + 16) = 0xF800u; MR32(p + 20) = 0x07E0u; MR32(p + 24) = 0x001Fu; }
        else { MR32(p + 16) = 0xFF0000u; MR32(p + 20) = 0x00FF00u; MR32(p + 24) = 0x0000FFu; } }
}
/* DDSURFACEDESC (108 bytes) of a surface */
static void put_desc(uint32_t d, const Surf *s){
    uint32_t sz = MR32(d); if (sz < 108) sz = 108;
    memset(g_mem + d, 0, sz); MR32(d) = sz;
    MR32(d + 4) = 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u;          /* CAPS HEIGHT WIDTH PITCH PIXELFORMAT */
    MR32(d + 8) = s->h; MR32(d + 12) = s->w; MR32(d + 16) = s->pitch;
    if (s->has_ck){ MR32(d + 4) |= 0x10000u; MR32(d + 64) = s->ck_lo; MR32(d + 68) = s->ck_hi; }
    put_pf(d + 72, s->bpp);
    MR32(d + 104) = s->caps;
}

static int new_surf(uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps){
    if (g_ns >= NSURF) return 0;
    int i = g_ns++; Surf *s = &g_s[i];
    memset(s, 0, sizeof *s);
    s->w = w; s->h = h; s->bpp = bpp; s->caps = caps; s->ref = 1;
    s->pitch = (w * (bpp / 8) + 3u) & ~3u;
    s->mem = com_scratch(s->pitch * h + 64);
    memset(g_mem + s->mem, 0, s->pitch * h);
    s->obj = new_obj(DD_IF_SURF, (uint32_t)i);
    if (ddlog()) fprintf(stderr, "[ddraw] surface %d: %ux%u %u bpp caps 0x%x -> 0x%08x (pixels 0x%08x)\n", i, w, h, bpp, caps, s->obj, s->mem);
    return i;
}
static int surf_of(uint32_t obj){
    if (!obj || !mem_is_committed(obj + 7) || MR32(obj) != g_vt[1]) return 0;
    uint32_t i = MR32(obj + 4); return i > 0 && i < (uint32_t)g_ns ? (int)i : 0;
}

/* ---- pixels ---- */
static inline uint32_t px_get(const Surf *s, uint32_t x, uint32_t y){
    uint32_t a = s->mem + y * s->pitch;
    switch (s->bpp){ case 8: return MR8(a + x); case 16: return MR16(a + 2*x); default: return MR32(a + 4*x); }
}
typedef struct { int32_t l, t, r, b; } Rect;
static int get_rect(uint32_t p, const Surf *s, Rect *r){
    if (!p){ r->l = 0; r->t = 0; r->r = (int32_t)s->w; r->b = (int32_t)s->h; return 1; }
    r->l = (int32_t)MR32(p); r->t = (int32_t)MR32(p + 4); r->r = (int32_t)MR32(p + 8); r->b = (int32_t)MR32(p + 12);
    return r->r > r->l && r->b > r->t;
}
static void fill(Surf *d, Rect r, uint32_t col){
    if (r.l < 0) r.l = 0; if (r.t < 0) r.t = 0;
    if (r.r > (int32_t)d->w) r.r = (int32_t)d->w; if (r.b > (int32_t)d->h) r.b = (int32_t)d->h;
    uint32_t bp = d->bpp / 8;
    for (int32_t y = r.t; y < r.b; y++){
        uint8_t *row = g_mem + d->mem + (uint32_t)y * d->pitch + (uint32_t)r.l * bp;
        int32_t n = r.r - r.l;
        if (bp == 1) memset(row, (int)col, (size_t)n);
        else if (bp == 2){ uint16_t v = (uint16_t)col; for (int32_t x = 0; x < n; x++) memcpy(row + 2*x, &v, 2); }
        else { for (int32_t x = 0; x < n; x++) memcpy(row + 4*x, &col, 4); }
    }
}
/* copy (with stretching when the sizes differ), optionally keyed */
static void blit(Surf *d, Rect dr, Surf *s, Rect sr, int key){
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || d->bpp != s->bpp) return;
    uint32_t bp = d->bpp / 8, lo = s->ck_lo, hi = s->ck_hi;
    int same = dw == sw && dh == sh;
    for (int32_t y = 0; y < dh; y++){
        int32_t dy = dr.t + y; if (dy < 0 || dy >= (int32_t)d->h) continue;
        int32_t sy = sr.t + (same ? y : (int32_t)((int64_t)y * sh / dh)); if (sy < 0 || sy >= (int32_t)s->h) continue;
        uint8_t *drow = g_mem + d->mem + (uint32_t)dy * d->pitch;
        const uint8_t *srow = g_mem + s->mem + (uint32_t)sy * s->pitch;
        if (same && !key){
            int32_t x0 = 0, x1 = dw;
            if (dr.l + x0 < 0) x0 = -dr.l; if (dr.l + x1 > (int32_t)d->w) x1 = (int32_t)d->w - dr.l;
            if (sr.l + x0 < 0) x0 = -sr.l; if (sr.l + x1 > (int32_t)s->w) x1 = (int32_t)s->w - sr.l;
            if (x1 > x0) memmove(drow + (uint32_t)(dr.l + x0) * bp, srow + (uint32_t)(sr.l + x0) * bp, (size_t)(x1 - x0) * bp);
            continue;
        }
        for (int32_t x = 0; x < dw; x++){
            int32_t dx = dr.l + x; if (dx < 0 || dx >= (int32_t)d->w) continue;
            int32_t sx = sr.l + (same ? x : (int32_t)((int64_t)x * sw / dw)); if (sx < 0 || sx >= (int32_t)s->w) continue;
            uint32_t v = 0; memcpy(&v, srow + (uint32_t)sx * bp, bp);
            if (key && v >= lo && v <= hi) continue;
            memcpy(drow + (uint32_t)dx * bp, &v, bp);
        }
    }
}

/* ---- the screen ---- */
static void present(const Surf *s){
    g_frames++;
    if (!metal_available()) return;
    unsigned f = s->bpp == 16 ? 23u : 22u;
    if (!g_tex || g_tex_w != s->w || g_tex_h != s->h || g_tex_f != f){
        if (g_tex) metal_free_texture(g_tex);
        g_tex = metal_create_texture(s->w, s->h, 1, f); g_tex_w = s->w; g_tex_h = s->h; g_tex_f = f;
    }
    if (s->bpp == 16) metal_upload_texture(g_tex, 0, g_mem + s->mem, s->pitch, s->w, s->h, f);
    else {
        if (!g_conv) g_conv = malloc(4u * 2048u * 2048u);
        const uint32_t *pal = g_pal_rgb[s->pal > 0 && s->pal < g_npal ? s->pal : (g_primary ? g_s[g_primary].pal : 0)];
        for (uint32_t y = 0; y < s->h; y++){
            uint32_t *o = g_conv + y * s->w; const uint8_t *r = g_mem + s->mem + y * s->pitch;
            if (s->bpp == 8) for (uint32_t x = 0; x < s->w; x++) o[x] = 0xFF000000u | pal[r[x]];
            else memcpy(o, r, 4u * s->w);
        }
        metal_upload_texture(g_tex, 0, g_conv, 4u * s->w, s->w, s->h, f);
    }
    metal_begin_frame();
    metal_blit(g_tex, 0);
    metal_present();
}

/* IDirectDraw::EnumDisplayModes: the modes a game may ask for */
static uint32_t call_guest_cb(CPU *c, uint32_t fn, uint32_t a0, uint32_t a1);

static uint32_t dd_method(CPU *c, int iface, unsigned m, const char *nm, uint32_t self){
    #define AR(i) MR32(c->esp + 4u*(i))
    if (iface == DD_IF_DD){
        switch (m){
        case 0: if (AR(2)) MR32(AR(2)) = 0; return 0x80004002u;     /* E_NOINTERFACE: version 1 only */
        case 1: case 2: return 1;
        case 4: { uint32_t pp = AR(2); if (pp) MR32(pp) = new_obj(DD_IF_CLIP, 0); return 0; }
        case 5: { uint32_t ent = AR(2), pp = AR(3);                  /* CreatePalette(flags, entries, pp, outer) */
            int k = g_npal < 64 ? g_npal++ : 63;
            for (int i = 0; i < 256; i++) if (ent){ uint32_t e = MR32(ent + 4u*i); g_pal_rgb[k][i] = (e & 0xFFu) << 16 | (e & 0xFF00u) | (e >> 16 & 0xFFu); }
            if (pp) MR32(pp) = new_obj(DD_IF_PAL, (uint32_t)k); return 0; }
        case 6: {                                                    /* CreateSurface(desc, pp, outer) */
            uint32_t d = AR(1), pp = AR(2), fl = MR32(d + 4), caps = (fl & 1u) ? MR32(d + 104) : 0;
            uint32_t w = (fl & 4u) ? MR32(d + 12) : g_mode_w, h = (fl & 2u) ? MR32(d + 8) : g_mode_h;
            uint32_t bpp = (fl & 0x1000u) ? MR32(d + 84) : g_mode_bpp;
            if (caps & 0x200u){ w = g_mode_w; h = g_mode_h; bpp = g_mode_bpp; }    /* the primary is the screen */
            int i = new_surf(w, h, bpp, caps | ((caps & 0x200u) ? 0x20u : 0u));
            if (!i){ if (pp) MR32(pp) = 0; return 0x8876017Cu; }      /* DDERR_OUTOFMEMORY */
            if (fl & 0x10000u){ g_s[i].has_ck = 1; g_s[i].ck_lo = MR32(d + 64); g_s[i].ck_hi = MR32(d + 68); }
            if (caps & 0x200u){
                g_primary = i;
                if ((fl & 0x20u) && MR32(d + 20)) g_s[i].back = new_surf(w, h, bpp, 0x4u | 0x10u);
            }
            if (pp) MR32(pp) = g_s[i].obj; return 0; }
        case 8: {                                                    /* EnumDisplayModes(flags, desc, ctx, cb) */
            static const uint32_t modes[][3] = { {640,480,8},{640,480,16},{640,480,32},{800,600,8},{800,600,16},{800,600,32},{1024,768,16},{1024,768,32} };
            uint32_t ctx = AR(3), cb = AR(4), d = com_scratch(108);
            for (unsigned k = 0; k < sizeof modes / sizeof *modes; k++){
                Surf t = { .w = modes[k][0], .h = modes[k][1], .bpp = modes[k][2] }; t.pitch = t.w * t.bpp / 8;
                MR32(d) = 108; put_desc(d, &t);
                if (!call_guest_cb(c, cb, d, ctx)) break;
            }
            return 0; }
        case 11: {                                                   /* GetCaps(driver, hel): blits in hardware, nothing else */
            for (int k = 1; k <= 2; k++){ uint32_t p = AR(k); if (!p) continue; uint32_t sz = MR32(p); if (sz < 4 || sz > 400) sz = 316;
                memset(g_mem + p + 4, 0, sz - 4);
                MR32(p + 4) = 0x40u | 0x200u;                          /* DDCAPS_BLT | DDCAPS_COLORKEY */
                MR32(p + 12) = 0x40u; }                                /* dwCKeyCaps: DDCKEYCAPS_SRCBLT */
            return 0; }
        case 12: { uint32_t d = AR(1); Surf t = { .w = g_mode_w, .h = g_mode_h, .bpp = g_mode_bpp }; t.pitch = t.w * t.bpp / 8; put_desc(d, &t); return 0; }
        case 15: if (AR(1)) MR32(AR(1)) = 60; return 0;
        case 17: if (AR(1)) MR32(AR(1)) = 1; return 0;
        case 20: return 0;
        case 21: g_mode_w = AR(1); g_mode_h = AR(2); g_mode_bpp = AR(3);
            fprintf(stderr, "[ddraw] SetDisplayMode %ux%u %u bpp\n", g_mode_w, g_mode_h, g_mode_bpp);
            metal_want_size(g_mode_w, g_mode_h); return 0;
        case 3: case 10: case 18: case 19: case 22: return 0;
        }
        return 0x80004001u;
    }
    if (iface == DD_IF_SURF){
        int si = surf_of(self); Surf *s = si ? &g_s[si] : 0;
        if (!s && m > 2) return 0x88760082u;                        /* DDERR_INVALIDOBJECT */
        switch (m){
        case 0: if (AR(2)) MR32(AR(2)) = 0; return 0x80004002u;
        case 1: return s ? (uint32_t)++s->ref : 1u;
        case 2: return s && s->ref > 0 ? (uint32_t)--s->ref : 0;
        case 5: {                                                    /* Blt(dstRect, src, srcRect, flags, fx) */
            uint32_t fl = AR(4), fx = AR(5); Rect dr;
            if (!get_rect(AR(1), s, &dr)) return 0;
            if (fl & 0x400u){ fill(s, dr, fx ? MR32(fx + 80) : 0); }                 /* DDBLT_COLORFILL */
            else { int sj = surf_of(AR(2)); Rect sr; if (!sj || !get_rect(AR(3), &g_s[sj], &sr)) return 0;
                   int key = (fl & 0x8000u) && g_s[sj].has_ck;                         /* DDBLT_KEYSRC */
                   if (fl & 0x10000u){ key = 1; g_s[sj].ck_lo = MR32(fx + 92); g_s[sj].ck_hi = MR32(fx + 96); }
                   blit(s, dr, &g_s[sj], sr, key); }
            if (si == g_primary && !s->back) g_dirty = 1;               /* shown at the game's next PeekMessage (ddraw_flush) */
            return 0; }
        case 7: {                                                    /* BltFast(x, y, src, srcRect, trans) */
            int sj = surf_of(AR(3)); Rect sr; if (!sj || !get_rect(AR(4), &g_s[sj], &sr)) return 0;
            Rect dr = { (int32_t)AR(1), (int32_t)AR(2), 0, 0 }; dr.r = dr.l + (sr.r - sr.l); dr.b = dr.t + (sr.b - sr.t);
            blit(s, dr, &g_s[sj], sr, (AR(5) & 1u) && g_s[sj].has_ck);
            if (si == g_primary && !s->back) g_dirty = 1;               /* shown at the game's next PeekMessage (ddraw_flush) */
            return 0; }
        case 11: {                                                   /* Flip: the back buffer's pixels become the front's */
            if (s->back){ Surf *b = &g_s[s->back]; uint32_t t = s->mem; s->mem = b->mem; b->mem = t; }
            present(s);
            return 0; }
        case 12: {                                                   /* GetAttachedSurface(caps, pp) */
            uint32_t pp = AR(2);
            if (!s->back){ if (pp) MR32(pp) = 0; return 0x887600FFu; }   /* DDERR_NOTFOUND */
            g_s[s->back].ref++; if (pp) MR32(pp) = g_s[s->back].obj; return 0; }
        case 13: case 18: return 0;                                  /* Get{Blt,Flip}Status: done */
        case 14: if (AR(1)) MR32(AR(1)) = s->caps; return 0;
        case 16: { uint32_t ck = AR(2); if (!s->has_ck) return 0x887600BBu; if (ck){ MR32(ck) = s->ck_lo; MR32(ck + 4) = s->ck_hi; } return 0; }
        case 21: put_pf(AR(1), s->bpp); return 0;
        case 22: put_desc(AR(1), s); return 0;
        case 24: case 27: return 0;                                  /* IsLost, Restore: never lost */
        case 25: {                                                   /* Lock(rect, desc, flags, event) */
            uint32_t d = AR(2); put_desc(d, s);
            uint32_t p = s->mem; Rect r;
            if (AR(1) && get_rect(AR(1), s, &r)) p += (uint32_t)r.t * s->pitch + (uint32_t)r.l * (s->bpp / 8);
            MR32(d + 4) |= 0x800u; MR32(d + 36) = p; return 0; }
        case 28: case 32: return 0;                                  /* SetClipper, Unlock */
        case 29: {                                                   /* SetColorKey(flags, ck) */
            uint32_t ck = AR(2);
            if (!ck){ s->has_ck = 0; return 0; }
            if (AR(1) & 0x8u){ s->has_ck = 1; s->ck_lo = MR32(ck); s->ck_hi = MR32(ck + 4); }
            return 0; }
        case 31: { uint32_t po = AR(1); s->pal = po && mem_is_committed(po + 7) ? (int)MR32(po + 4) : 0; return 0; }
        case 20: if (AR(1)) MR32(AR(1)) = 0; return 0x88760083u;    /* DDERR_NOPALETTEATTACHED */
        case 17: return 0x80004001u;                                 /* GetDC: no GDI */
        }
        return 0;
    }
    if (iface == DD_IF_PAL){
        int k = (int)MR32(self + 4); if (k <= 0 || k >= 64) k = 0;
        switch (m){
        case 0: return 0x80004002u; case 1: case 2: return 1;
        case 4: case 6: {                                            /* Get/SetEntries(flags, start, n, entries) */
            uint32_t st = AR(2), n = AR(3), e = AR(4);
            for (uint32_t i = 0; i < n && st + i < 256; i++){
                if (m == 6){ uint32_t v = MR32(e + 4u*i); g_pal_rgb[k][st + i] = (v & 0xFFu) << 16 | (v & 0xFF00u) | (v >> 16 & 0xFFu); }
                else { uint32_t v = g_pal_rgb[k][st + i]; MR32(e + 4u*i) = (v >> 16 & 0xFFu) | (v & 0xFF00u) | (v & 0xFFu) << 16; }
            }
            return 0; }
        }
        return 0;
    }
    /* IDirectDrawClipper: accepted, nothing to clip against */
    if (m == 0) return 0x80004002u;
    return m <= 2 ? 1u : 0u;
    #undef AR
}

static uint32_t call_guest_cb(CPU *c, uint32_t fn, uint32_t a0, uint32_t a1){
    uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
    uint32_t a[2] = { a0, a1 };
    return call_guest(c, fn, 2, a);
}

/* com.c com_call: a method of one of the interfaces here */
int ddraw_call(CPU *c, uint32_t iface, uint32_t m){
    POP32(c->eip);
    int k = (int)iface - DD_IF_DD;
    if (k < 0 || k >= 4 || m >= g_ddif[k].n){ c->eax = 0x80004001u; return 1; }
    const DdM *M = &g_ddif[k].m[m];
    uint32_t self = MR32(c->esp);
    uint32_t r = dd_method(c, (int)iface, m, M->name, self);
    if (ddlog() > 1 || (ddlog() && !(iface == DD_IF_SURF && (m == 5 || m == 7 || m == 25 || m == 32))))
        fprintf(stderr, "[ddraw] %s::%s(0x%08x, %x, %x, %x) -> 0x%x\n", g_ddif[k].name, M->name, self,
                MR32(c->esp + 4), MR32(c->esp + 8), MR32(c->esp + 12), r);
    c->eax = r;
    c->esp += 4u * (uint32_t)M->nargs;
    return 1;
}

/* DirectDrawCreate(guid, pp, outer) */
uint32_t ddraw_create(uint32_t pp){
    if (!g_dd){ g_dd = new_obj(DD_IF_DD, 0); fprintf(stderr, "[ddraw] IDirectDraw object 0x%08x\n", g_dd); }
    if (pp) MR32(pp) = g_dd;
    return 0;
}
long ddraw_frames(void){ return g_frames; }
/* win32.c, at PeekMessage: a primary the game blitted onto since (in pieces:
 * Bulanci blits its dirty rectangles) goes to the screen once */
void ddraw_flush(void){ if (g_dirty && g_primary){ g_dirty = 0; present(&g_s[g_primary]); } }
