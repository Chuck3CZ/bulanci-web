/* The Direct3D fixed-function vertex pipeline, on the CPU (WC3 draws with it:
 * SetTransform, lights, materials, FVF vertices; NFSU2 used vertex shaders
 * only, so com.c had no such stage and took FVF positions for clip space).
 *
 * com.c hands over the state as the game sets it (ffp_set_*) and, for a draw
 * without a vertex shader and with an FVF, asks ffp_vertex for each vertex:
 * position by World x View x Projection (XYZRHW: screen pixels through the
 * viewport), the diffuse and specular colours (vertex colours, or the
 * lighting equation with the material and the enabled lights), and the
 * texture coordinates each stage reads (TEXCOORDINDEX, texture transforms).
 * The pixel stage stays the renderer's.
 *
 * D3D conventions: row vectors (p' = p * M), clip z in 0..w, colours as
 * floats 0..1. */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "gfx.h"

extern uint32_t g_rs[256];
extern uint32_t g_tss_raw[8][40];

/* D3DRENDERSTATETYPE */
#define RS_LIGHTING        137
#define RS_AMBIENT         139
#define RS_COLORVERTEX     141
#define RS_LOCALVIEWER     142
#define RS_NORMALIZE       143
#define RS_DIFFUSEMATSRC   145
#define RS_SPECULARMATSRC  146
#define RS_AMBIENTMATSRC   147
#define RS_EMISSIVEMATSRC  148
#define RS_SPECULARENABLE   29

static float g_m[3][16] = {           /* world, view, projection: identity */
    {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1} };
static float g_tm[8][16];             /* D3DTS_TEXTURE0..7 */
static float g_wvp[16], g_wv[16];
static int   g_dirty = 1;
typedef struct { uint32_t type; float dif[4], spe[4], amb[4], pos[3], dir[3], range, falloff, a0, a1, a2, theta, phi; } Light;
static Light g_lt[8];
static int   g_lten[8];
static struct { float dif[4], amb[4], spe[4], emi[4], pow; } g_mat = { {1,1,1,1}, {0,0,0,0}, {0,0,0,0}, {0,0,0,0}, 0 };
static float g_vp[6] = { 0, 0, 1280, 720, 0, 1 };
static float g_rtw = 1280, g_rth = 720;   /* the render target's size (com.c SetRenderTarget) */
/* the renderer draws to the whole target: D3D's viewport (the unit portrait's
 * box, ...) is applied here, clip space of the viewport -> of the target */
void ffp_set_target_size(unsigned w, unsigned h){ if (w && h){ g_rtw = (float)w; g_rth = (float)h; } }
/* a new device: its viewport is the whole back buffer, as Direct3D sets it */
void ffp_device_size(unsigned w, unsigned h){
    if (!w || !h) return;
    ffp_set_target_size(w, h);
    g_vp[0] = g_vp[1] = 0.f; g_vp[2] = (float)w; g_vp[3] = (float)h; g_vp[4] = 0.f; g_vp[5] = 1.f;
}
int ffp_viewport_rect(int *r){
    if (g_vp[0] == 0.f && g_vp[1] == 0.f && g_vp[2] >= g_rtw && g_vp[3] >= g_rth) return 0;
    r[0] = (int)g_vp[0]; r[1] = (int)g_vp[1]; r[2] = (int)(g_vp[0] + g_vp[2]); r[3] = (int)(g_vp[1] + g_vp[3]);
    return 1;
}
static void vp_apply(TVert *o){
    float W = g_rtw, H = g_rth;
    if (g_vp[0] == 0.f && g_vp[1] == 0.f && g_vp[2] == W && g_vp[3] == H && g_vp[4] == 0.f && g_vp[5] == 1.f) return;
    float sx = g_vp[2] / W, sy = g_vp[3] / H;
    float ox = (2.f * g_vp[0] + g_vp[2]) / W - 1.f, oy = 1.f - (2.f * g_vp[1] + g_vp[3]) / H;
    o->x = o->x * sx + o->w * ox;
    o->y = o->y * sy + o->w * oy;
    o->z = o->z * (g_vp[5] - g_vp[4]) + o->w * g_vp[4];
}
/* com.c: a vertex shader's output (clip space of the viewport) gets the same
 * viewport transform (CMR5 draws its pace-note arrows into a small viewport) */
void ffp_vp_apply(TVert *o){ vp_apply(o); }
static int   g_rs_init = 0;

static void rs_defaults(void){
    if (g_rs_init) return;
    g_rs_init = 1;
    /* D3D's defaults for the states the game may never set; com.c's g_rs
     * starts at 0, which is wrong for these */
    if (!g_rs[RS_LIGHTING])      g_rs[RS_LIGHTING] = 1;      /* TRUE: zeroed by a game that turns it off */
    if (!g_rs[RS_COLORVERTEX])   g_rs[RS_COLORVERTEX] = 1;
    if (!g_rs[RS_DIFFUSEMATSRC]) g_rs[RS_DIFFUSEMATSRC] = 1; /* D3DMCS_COLOR1 */
    if (!g_rs[RS_SPECULARMATSRC])g_rs[RS_SPECULARMATSRC] = 2;/* D3DMCS_COLOR2 */
}
/* com.c calls this once, before the game sets any state */
void ffp_init(void){ for (int i = 0; i < 8; i++){ memset(g_tm[i], 0, sizeof g_tm[i]); g_tm[i][0] = g_tm[i][5] = g_tm[i][10] = g_tm[i][15] = 1; }
                     rs_defaults(); }

static void mul(float *o, const float *a, const float *b){
    float t[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)
        t[r*4+c] = a[r*4]*b[c] + a[r*4+1]*b[4+c] + a[r*4+2]*b[8+c] + a[r*4+3]*b[12+c];
    memcpy(o, t, sizeof t);
}
static void read_mat(float *m, uint32_t p){ for (int i = 0; i < 16; i++) m[i] = MRF32(p + 4u*(uint32_t)i); }

void ffp_set_transform(uint32_t type, uint32_t p){
    if (!p || !mem_is_committed(p + 63)) return;
    if (type == 256u) read_mat(g_m[0], p);
    else if (type == 2u) read_mat(g_m[1], p);
    else if (type == 3u) read_mat(g_m[2], p);
    else if (type >= 16u && type < 24u) read_mat(g_tm[type - 16u], p);
    else return;
    g_dirty = 1;
}
/* MultiplyTransform: M = m * M */
void ffp_multiply_transform(uint32_t type, uint32_t p){
    if (!p || !mem_is_committed(p + 63)) return;
    float m[16]; read_mat(m, p);
    float *d = type == 256u ? g_m[0] : type == 2u ? g_m[1] : type == 3u ? g_m[2] : (type >= 16u && type < 24u) ? g_tm[type - 16u] : 0;
    if (!d) return;
    mul(d, m, d); g_dirty = 1;
}
int ffp_get_transform(uint32_t type, uint32_t p){
    const float *s = type == 256u ? g_m[0] : type == 2u ? g_m[1] : type == 3u ? g_m[2] : (type >= 16u && type < 24u) ? g_tm[type - 16u] : 0;
    if (!s || !p) return 0;
    mem_commit(p, 64); for (int i = 0; i < 16; i++) MRF32(p + 4u*(uint32_t)i) = s[i];
    return 1;
}
void ffp_set_light(uint32_t i, uint32_t p){
    if (i >= 8u || !p || !mem_is_committed(p + 103)) return;
    Light *l = &g_lt[i];
    l->type = MR32(p);
    for (int k = 0; k < 4; k++){ l->dif[k] = MRF32(p + 4 + 4*k); l->spe[k] = MRF32(p + 20 + 4*k); l->amb[k] = MRF32(p + 36 + 4*k); }
    for (int k = 0; k < 3; k++){ l->pos[k] = MRF32(p + 52 + 4*k); l->dir[k] = MRF32(p + 64 + 4*k); }
    l->range = MRF32(p + 76); l->falloff = MRF32(p + 80); l->a0 = MRF32(p + 84); l->a1 = MRF32(p + 88); l->a2 = MRF32(p + 92);
    l->theta = MRF32(p + 96); l->phi = MRF32(p + 100);
    { static int n = 0; const char *e = getenv("WC3_FFLOG");
      if (e && *e && n++ < 12){
        fprintf(stderr, "[ff] SetLight(%u): type %08x", i, MR32(p));
        for (int k = 1; k < 26; k++) fprintf(stderr, " %.3g", (double)MRF32(p + 4u*(uint32_t)k));
        fprintf(stderr, "\n"); } }
}
int ffp_get_light(uint32_t i, uint32_t p){
    if (i >= 8u || !p) return 0;
    const Light *l = &g_lt[i]; mem_commit(p, 104);
    MR32(p) = l->type;
    for (int k = 0; k < 4; k++){ MRF32(p + 4 + 4*k) = l->dif[k]; MRF32(p + 20 + 4*k) = l->spe[k]; MRF32(p + 36 + 4*k) = l->amb[k]; }
    for (int k = 0; k < 3; k++){ MRF32(p + 52 + 4*k) = l->pos[k]; MRF32(p + 64 + 4*k) = l->dir[k]; }
    MRF32(p + 76) = l->range; MRF32(p + 80) = l->falloff; MRF32(p + 84) = l->a0; MRF32(p + 88) = l->a1; MRF32(p + 92) = l->a2;
    MRF32(p + 96) = l->theta; MRF32(p + 100) = l->phi;
    return 1;
}
void ffp_light_enable(uint32_t i, uint32_t on){ if (i < 8u) g_lten[i] = on != 0; }
int  ffp_light_enabled(uint32_t i){ return i < 8u ? g_lten[i] : 0; }
void ffp_set_material(uint32_t p){
    if (!p || !mem_is_committed(p + 67)) return;
    for (int k = 0; k < 4; k++){ g_mat.dif[k] = MRF32(p + 4*k); g_mat.amb[k] = MRF32(p + 16 + 4*k);
                                 g_mat.spe[k] = MRF32(p + 32 + 4*k); g_mat.emi[k] = MRF32(p + 48 + 4*k); }
    g_mat.pow = MRF32(p + 64);
}
int ffp_get_material(uint32_t p){
    if (!p) return 0;
    mem_commit(p, 68);
    for (int k = 0; k < 4; k++){ MRF32(p + 4*k) = g_mat.dif[k]; MRF32(p + 16 + 4*k) = g_mat.amb[k];
                                 MRF32(p + 32 + 4*k) = g_mat.spe[k]; MRF32(p + 48 + 4*k) = g_mat.emi[k]; }
    MRF32(p + 64) = g_mat.pow;
    return 1;
}
int ffp_get_viewport(uint32_t p){
    if (!p) return 0;
    mem_commit(p, 24);
    MR32(p) = (uint32_t)g_vp[0]; MR32(p + 4) = (uint32_t)g_vp[1]; MR32(p + 8) = (uint32_t)g_vp[2]; MR32(p + 12) = (uint32_t)g_vp[3];
    MRF32(p + 16) = g_vp[4]; MRF32(p + 20) = g_vp[5];
    return 1;
}
void ffp_set_viewport(uint32_t p){
    if (!p || !mem_is_committed(p + 23)) return;
    g_vp[0] = (float)MR32(p); g_vp[1] = (float)MR32(p + 4); g_vp[2] = (float)MR32(p + 8); g_vp[3] = (float)MR32(p + 12);
    g_vp[4] = MRF32(p + 16); g_vp[5] = MRF32(p + 20);
    { static float seen[32][4]; static int ns = 0; int f = 0;
      for (int i = 0; i < ns; i++) if (!memcmp(seen[i], g_vp, sizeof seen[i])) f = 1;
      if (!f && ns < 32){ memcpy(seen[ns++], g_vp, sizeof seen[0]);
        fprintf(stderr, "[ff] viewport %g,%g %gx%g z %g..%g (target %gx%g)\n", g_vp[0], g_vp[1], g_vp[2], g_vp[3], g_vp[4], g_vp[5], g_rtw, g_rth); } }
}

/* ---- per vertex ---- */
static void argb(uint32_t v, float *o){ o[0] = ((v >> 16) & 255) / 255.f; o[1] = ((v >> 8) & 255) / 255.f; o[2] = (v & 255) / 255.f; o[3] = (v >> 24) / 255.f; }
static float sat(float x){ return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }

/* the FVF's layout: offsets of each part, or -1 */
typedef struct { uint32_t fvf; int pos_rhw, nblend, off_n, off_d, off_s, ntex, off_t[8], sz_t[8], ok; } Layout;
static Layout g_lay;
static const Layout *layout(uint32_t fvf){
    if (g_lay.ok && g_lay.fvf == fvf) return &g_lay;
    Layout *L = &g_lay; memset(L, 0, sizeof *L); L->fvf = fvf; L->off_n = L->off_d = L->off_s = -1;
    int off = 0;
    switch (fvf & 0x00Eu){
    case 0x002: off = 12; break;                         /* XYZ */
    case 0x004: off = 16; L->pos_rhw = 1; break;         /* XYZRHW */
    case 0x006: off = 16; L->nblend = 1; break;          /* XYZB1..B5 */
    case 0x008: off = 20; L->nblend = 2; break;
    case 0x00A: off = 24; L->nblend = 3; break;
    case 0x00C: off = 28; L->nblend = 4; break;
    case 0x00E: off = 32; L->nblend = 5; break;
    default: off = 12;
    }
    if (fvf & 0x010u){ L->off_n = off; off += 12; }       /* NORMAL */
    if (fvf & 0x020u) off += 4;                            /* PSIZE */
    if (fvf & 0x040u){ L->off_d = off; off += 4; }        /* DIFFUSE */
    if (fvf & 0x080u){ L->off_s = off; off += 4; }        /* SPECULAR */
    L->ntex = (int)((fvf >> 8) & 0xFu); if (L->ntex > 8) L->ntex = 8;
    for (int i = 0; i < L->ntex; i++){
        static const int nf[4] = { 2, 3, 4, 1 };
        L->sz_t[i] = nf[(fvf >> (16 + 2*i)) & 3u];
        L->off_t[i] = off; off += 4 * L->sz_t[i];
    }
    L->ok = 1;
    return L;
}

static void xform(float *o, const float *v, const float *m){   /* (x y z 1) * m */
    for (int c = 0; c < 4; c++) o[c] = v[0]*m[c] + v[1]*m[4+c] + v[2]*m[8+c] + m[12+c];
}

/* lighting in world space (D3D does it in camera space: the same result
 * for a rigid view matrix) */
static void light(const float *wp, const float *wn, const float *vdif, const float *vspe, float *dif, float *spe){
    float md[4], ma[4], me[4], ms[4];
    uint32_t cv = g_rs[RS_COLORVERTEX];
    const float *src[3] = { 0, vdif, vspe };
#define PICK(dst, st, mat) do { uint32_t s_ = cv ? g_rs[st] : 0u; const float *p_ = (s_ == 1u || s_ == 2u) && src[s_] ? src[s_] : (mat); memcpy(dst, p_, 16); } while (0)
    PICK(md, RS_DIFFUSEMATSRC, g_mat.dif);
    PICK(ma, RS_AMBIENTMATSRC, g_mat.amb);
    PICK(me, RS_EMISSIVEMATSRC, g_mat.emi);
    PICK(ms, RS_SPECULARMATSRC, g_mat.spe);
#undef PICK
    float ga[4]; argb(g_rs[RS_AMBIENT], ga);
    float acc_a[3] = { ga[0], ga[1], ga[2] }, acc_d[3] = { 0, 0, 0 }, acc_s[3] = { 0, 0, 0 };
    float n[3] = { wn[0], wn[1], wn[2] };
    float nl = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]); if (nl > 1e-12f){ n[0] /= nl; n[1] /= nl; n[2] /= nl; }
    int spec = g_rs[RS_SPECULARENABLE] != 0;
    /* the eye in world space: the inverse view's translation (rigid view) */
    const float *V = g_m[1];
    float eye[3] = { -(V[12]*V[0] + V[13]*V[1] + V[14]*V[2]), -(V[12]*V[4] + V[13]*V[5] + V[14]*V[6]), -(V[12]*V[8] + V[13]*V[9] + V[14]*V[10]) };
    for (int i = 0; i < 8; i++){
        if (!g_lten[i]) continue;
        const Light *l = &g_lt[i];
        float L[3], att = 1.f;
        if (l->type == 3u){ L[0] = -l->dir[0]; L[1] = -l->dir[1]; L[2] = -l->dir[2]; }   /* directional */
        else {
            L[0] = l->pos[0] - wp[0]; L[1] = l->pos[1] - wp[1]; L[2] = l->pos[2] - wp[2];
            float d = sqrtf(L[0]*L[0] + L[1]*L[1] + L[2]*L[2]);
            if (l->range > 0.f && d > l->range) continue;
            float den = l->a0 + l->a1 * d + l->a2 * d * d;
            att = den > 1e-12f ? 1.f / den : 1.f;
            if (d > 1e-12f){ L[0] /= d; L[1] /= d; L[2] /= d; }
            if (l->type == 2u){                                  /* spot */
                float dl = sqrtf(l->dir[0]*l->dir[0] + l->dir[1]*l->dir[1] + l->dir[2]*l->dir[2]);
                float rho = dl > 1e-12f ? -(L[0]*l->dir[0] + L[1]*l->dir[1] + L[2]*l->dir[2]) / dl : 1.f;
                float ct = cosf(l->theta * .5f), cp = cosf(l->phi * .5f);
                if (rho <= cp) continue;
                if (rho < ct) att *= powf((rho - cp) / (ct - cp > 1e-6f ? ct - cp : 1e-6f), l->falloff > 0.f ? l->falloff : 1.f);
            }
        }
        float l1 = l->dif[0], l2 = l->dif[1], l3 = l->dif[2];
        acc_a[0] += l->amb[0] * att; acc_a[1] += l->amb[1] * att; acc_a[2] += l->amb[2] * att;
        float nd = n[0]*L[0] + n[1]*L[1] + n[2]*L[2];
        if (nd > 0.f){ acc_d[0] += l1 * nd * att; acc_d[1] += l2 * nd * att; acc_d[2] += l3 * nd * att;
            if (spec && g_mat.pow > 0.f){
                float E[3] = { eye[0] - wp[0], eye[1] - wp[1], eye[2] - wp[2] };
                float el = sqrtf(E[0]*E[0] + E[1]*E[1] + E[2]*E[2]); if (el > 1e-12f){ E[0] /= el; E[1] /= el; E[2] /= el; }
                float H[3] = { L[0] + E[0], L[1] + E[1], L[2] + E[2] };
                float hl = sqrtf(H[0]*H[0] + H[1]*H[1] + H[2]*H[2]);
                if (hl > 1e-12f){ float nh = (n[0]*H[0] + n[1]*H[1] + n[2]*H[2]) / hl;
                    if (nh > 0.f){ float s = powf(nh, g_mat.pow) * att;
                        acc_s[0] += l->spe[0] * s; acc_s[1] += l->spe[1] * s; acc_s[2] += l->spe[2] * s; } } } }
    }
    for (int k = 0; k < 3; k++){
        dif[k] = sat(me[k] + ma[k] * acc_a[k] + md[k] * acc_d[k]);
        spe[k] = sat(ms[k] * acc_s[k]);
    }
    dif[3] = sat(md[3]); spe[3] = sat(ms[3]);
}

static uint64_t g_n_ff, g_n_rhw, g_n_lit;
/* WC3_FFLOG=N: the state and the first vertex's result of the first N
 * distinct (FVF, lighting, texture stage ops) combinations */
static void fflog(uint32_t fvf, uint32_t o, const TVert *v){
    static int max = -1, n = 0; static uint32_t seen[256][3]; static int ns = 0;
    if (max < 0){ const char *e = getenv("WC3_FFLOG"); max = e && *e ? atoi(e) : 0; }
    if (n >= max) return;
    uint32_t k1 = fvf ^ (g_rs[RS_LIGHTING] << 31) ^ (g_rs[RS_COLORVERTEX] << 30) ^ (g_rs[RS_DIFFUSEMATSRC] << 24);
    uint32_t k2 = g_tss_raw[0][1] | g_tss_raw[0][2] << 6 | g_tss_raw[0][3] << 12 | g_tss_raw[0][4] << 18 | g_tss_raw[1][1] << 24;
    uint32_t k3 = g_tss_raw[0][5] | g_tss_raw[0][6] << 8 | g_tss_raw[1][2] << 16 | g_tss_raw[1][3] << 24;
    for (int i = 0; i < 8; i++) if (g_lten[i]){ uint32_t t; memcpy(&t, &g_lt[i].dif[0], 4); k3 = k3 * 31u + t + g_lt[i].type * 7u; }
    k3 ^= g_rs[RS_AMBIENT];
    for (int i = 0; i < ns; i++) if (seen[i][0] == k1 && seen[i][1] == k2 && seen[i][2] == k3) return;
    if (ns < 256){ seen[ns][0] = k1; seen[ns][1] = k2; seen[ns][2] = k3; ns++; }
    n++;
    int nl = 0; for (int i = 0; i < 8; i++) nl += g_lten[i];
    fprintf(stderr, "[ff] fvf 0x%x lighting %u colorvertex %u difsrc %u ambsrc %u ambient 0x%08x lights %d  mat dif %.2f %.2f %.2f %.2f amb %.2f %.2f %.2f emi %.2f %.2f %.2f\n",
            fvf, g_rs[RS_LIGHTING], g_rs[RS_COLORVERTEX], g_rs[RS_DIFFUSEMATSRC], g_rs[RS_AMBIENTMATSRC], g_rs[RS_AMBIENT], nl,
            g_mat.dif[0], g_mat.dif[1], g_mat.dif[2], g_mat.dif[3], g_mat.amb[0], g_mat.amb[1], g_mat.amb[2], g_mat.emi[0], g_mat.emi[1], g_mat.emi[2]);
    for (int i = 0; i < 8; i++) if (g_lten[i])
        fprintf(stderr, "[ff]   light %d type %u dif %.2f %.2f %.2f amb %.2f %.2f %.2f dir %.2f %.2f %.2f\n", i, g_lt[i].type,
                g_lt[i].dif[0], g_lt[i].dif[1], g_lt[i].dif[2], g_lt[i].amb[0], g_lt[i].amb[1], g_lt[i].amb[2], g_lt[i].dir[0], g_lt[i].dir[1], g_lt[i].dir[2]),
        fprintf(stderr, "[ff]           pos %.1f %.1f %.1f range %.1f att %g %g %g\n", g_lt[i].pos[0], g_lt[i].pos[1], g_lt[i].pos[2], g_lt[i].range, g_lt[i].a0, g_lt[i].a1, g_lt[i].a2);
    for (int t = 0; t < 3; t++)
        fprintf(stderr, "[ff]   stage %d colorop %u arg1 0x%x arg2 0x%x  alphaop %u arg1 0x%x arg2 0x%x  texcoord 0x%x ttf 0x%x\n", t,
                g_tss_raw[t][1], g_tss_raw[t][2], g_tss_raw[t][3], g_tss_raw[t][4], g_tss_raw[t][5], g_tss_raw[t][6], g_tss_raw[t][11], g_tss_raw[t][24]);
    const Layout *L = layout(fvf);
    fprintf(stderr, "[ff]   tfactor 0x%08x  normal@%d diffuse@%d  out pos %.2f %.2f %.2f %.2f col %.2f %.2f %.2f %.2f uv %.3f %.3f",
            g_rs[60], L->off_n, L->off_d, v->x, v->y, v->z, v->w, v->r, v->g, v->b, v->a, v->t[0][0], v->t[0][1]);
    { float p0[4] = { MRF32(o), MRF32(o + 4), MRF32(o + 8), 1.f }, wp[4]; xform(wp, p0, g_m[0]);
      fprintf(stderr, "  world %.1f %.1f %.1f", wp[0], wp[1], wp[2]); }
    if (L->off_n >= 0) fprintf(stderr, "  n %.2f %.2f %.2f", MRF32(o + (uint32_t)L->off_n), MRF32(o + (uint32_t)L->off_n + 4), MRF32(o + (uint32_t)L->off_n + 8));
    fprintf(stderr, "\n");
}
/* one vertex at guest address o; 0 when the FVF is not one this handles */
int ffp_vertex(uint32_t fvf, uint32_t o, TVert *out){
    const Layout *L = layout(fvf);
    if (g_dirty){ mul(g_wv, g_m[0], g_m[1]); mul(g_wvp, g_wv, g_m[2]); g_dirty = 0; }
    float p[4] = { MRF32(o), MRF32(o + 4), MRF32(o + 8), 1.f };
    float vd[4] = { 1, 1, 1, 1 }, vs[4] = { 0, 0, 0, 0 };
    if (L->off_d >= 0) argb(MR32(o + (uint32_t)L->off_d), vd);
    if (L->off_s >= 0) argb(MR32(o + (uint32_t)L->off_s), vs);
    g_n_ff++;
    if (L->pos_rhw){
        g_n_rhw++;
        float rhw = MRF32(o + 12), w = (rhw > 1e-20f) ? 1.f / rhw : 1.f;
        float nx = (p[0] / g_rtw) * 2.f - 1.f;
        float ny = 1.f - (p[1] / g_rth) * 2.f;
        out->x = nx * w; out->y = ny * w; out->z = p[2] * w; out->w = w;
        out->r = vd[0]; out->g = vd[1]; out->b = vd[2]; out->a = vd[3];
        out->r1 = vs[0]; out->g1 = vs[1]; out->b1 = vs[2]; out->a1 = vs[3];
    } else {
        float c[4]; xform(c, p, g_wvp);
        out->x = c[0]; out->y = c[1]; out->z = c[2]; out->w = c[3];
        vp_apply(out);
        if (g_rs[RS_LIGHTING]){
            g_n_lit++;
            float wp[4]; xform(wp, p, g_m[0]);
            float wn[3] = { 0, 0, 0 };
            if (L->off_n >= 0){
                float n0 = MRF32(o + (uint32_t)L->off_n), n1 = MRF32(o + (uint32_t)L->off_n + 4), n2 = MRF32(o + (uint32_t)L->off_n + 8);
                const float *W = g_m[0];
                wn[0] = n0*W[0] + n1*W[4] + n2*W[8]; wn[1] = n0*W[1] + n1*W[5] + n2*W[9]; wn[2] = n0*W[2] + n1*W[6] + n2*W[10];
            }
            float d[4], s[4]; light(wp, wn, vd, vs, d, s);
            out->r = d[0]; out->g = d[1]; out->b = d[2]; out->a = d[3];
            out->r1 = s[0]; out->g1 = s[1]; out->b1 = s[2]; out->a1 = s[3];
        } else {
            out->r = vd[0]; out->g = vd[1]; out->b = vd[2]; out->a = vd[3];
            out->r1 = vs[0]; out->g1 = vs[1]; out->b1 = vs[2]; out->a1 = vs[3];
        }
    }
    /* texture coordinates per stage */
    for (int ts = 0; ts < 4; ts++){
        uint32_t tci = g_tss_raw[ts][11], ci = tci & 0xFFFFu, gen = tci >> 16;
        float t[4] = { 0, 0, 0, 1 };
        if (gen == 0 && (int)ci < L->ntex){
            uint32_t a = o + (uint32_t)L->off_t[ci];
            for (int k = 0; k < L->sz_t[ci] && k < 4; k++) t[k] = MRF32(a + 4u*(uint32_t)k);
            if (L->sz_t[ci] < 4) t[L->sz_t[ci]] = 1.f;      /* (u v 1 0): the next component is 1 */
        }
        uint32_t ttf = g_tss_raw[ts][24];
        if ((ttf & 7u) && gen == 0){                          /* a texture transform with a count */
            float r[4];
            for (int c = 0; c < 4; c++) r[c] = t[0]*g_tm[ts][c] + t[1]*g_tm[ts][4+c] + t[2]*g_tm[ts][8+c] + t[3]*g_tm[ts][12+c];
            memcpy(t, r, sizeof r);
            if (!(ttf & 0x100u)){ t[2] = 0.f; t[3] = 1.f; }     /* not projected: the renderer divides nothing */
        } else if (L->sz_t[ci] < 3 || gen) { t[2] = 0.f; t[3] = 1.f; }
        memcpy(out->t[ts], t, sizeof t);
    }
    fflog(fvf, o, out);
    return 1;
}
void ffp_report(void){
    fprintf(stderr, "  fixed function    : %llu vertices (%llu pretransformed, %llu lit)\n",
            (unsigned long long)g_n_ff, (unsigned long long)g_n_rhw, (unsigned long long)g_n_lit);
}

/* com.c: a vertex declaration's position under the fixed function pipeline */
void ffp_decl_pos(const float *p, int pretransformed, TVert *o){
    if (pretransformed){
        float rhw = p[3], w = (rhw > 1e-20f) ? 1.f / rhw : 1.f;
        o->x = ((p[0] / g_rtw) * 2.f - 1.f) * w; o->y = (1.f - (p[1] / g_rth) * 2.f) * w; o->z = p[2] * w; o->w = w;
        return;
    }
    if (g_dirty){ mul(g_wv, g_m[0], g_m[1]); mul(g_wvp, g_wv, g_m[2]); g_dirty = 0; }
    float in[4] = { p[0], p[1], p[2], 1.f }, c[4];
    xform(c, in, g_wvp);
    o->x = c[0]; o->y = c[1]; o->z = c[2]; o->w = c[3];
    vp_apply(o);
}
