/* d3d8.c -- Direct3D 8 on the D3D9 layer (com.c).
 *
 * WC3 1.26 renders through Direct3D 8 (Game.dll: LoadLibrary("d3d8.dll"),
 * Direct3DCreate8).  The D3D8 objects ARE com.c's D3D9 objects: the same
 * guest object, with a D3D8 vtable (d3d8_tables.h) instead of the D3D9 one.
 * A call through it lands here (com.c com_call: an interface from IF_D3D8
 * up) and becomes the D3D9 call:
 *   - same name, same arguments (most of IDirect3DDevice8: render states,
 *     transforms, lights, draws, Lock/Unlock...): re-dispatched as the D3D9
 *     method, the stack as it is;
 *   - otherwise the arguments are rewritten into a D3D9 call made below the
 *     caller's stack (com_call pops it as usual), and the caller's D3D8
 *     arguments are popped afterwards: Create* without pSharedHandle,
 *     SetStreamSource without the offset, DrawIndexedPrimitive with the base
 *     vertex SetIndices gave, SetRenderTarget(rt, z), CopyRects,
 *     SetVertexShader(FVF), the sampler states of SetTextureStageState,
 *     state block tokens, the D3D8 structures (D3DPRESENT_PARAMETERS,
 *     D3DSURFACE_DESC, D3DCAPS8, D3DADAPTER_IDENTIFIER8).
 * Every object a D3D8 call hands out gets the D3D8 vtable of its kind. */
#include "cpu.h"
#include "d3d8_tables.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* com.c */
int      com_call(CPU *c, uint32_t magic);
uint32_t com_scratch(uint32_t n);
uint32_t com_vtab_for(uint32_t iface);
uint32_t com_obj_iface(uint32_t obj);       /* 0: not one of com.c's objects */
int      com_obj_is_cube(uint32_t obj);
int      com_method_index(uint32_t iface, const char *name, int *nargs);
const char *com_method_name(uint32_t iface, uint32_t m, int *nargs);
uint32_t com_direct3dcreate9(void);
#define COM_MAGIC 0xE0000000u

/* the interface numbers of com.c's g_if (keep in step with com.c) */
enum { IF_D3D9 = 1, IF_DEV, IF_TEX, IF_VB, IF_IB, IF_SURF, IF_SIMPLE,
       IF_VSHADER, IF_VDECL, IF_STATEBLOCK, IF_SWAPCHAIN, IF_QUERY,
       IF_DI8, IF_DIDEV, IF_DS, IF_DSBUF, IF_DIEFF,
       IF_D3D8, IF_DEV8, IF_TEX8, IF_CUBE8, IF_VB8, IF_IB8, IF_SURF8, IF_END8 };

static uint32_t base9(uint32_t if8){
    switch (if8){
    case IF_D3D8: return IF_D3D9;  case IF_DEV8: return IF_DEV;
    case IF_TEX8: case IF_CUBE8: return IF_TEX;
    case IF_VB8: return IF_VB;     case IF_IB8: return IF_IB;   case IF_SURF8: return IF_SURF;
    }
    return 0;
}
static uint32_t iface8_of(uint32_t obj){
    switch (com_obj_iface(obj)){
    case IF_D3D9: return IF_D3D8;  case IF_DEV: return IF_DEV8;
    case IF_TEX:  return com_obj_is_cube(obj) ? IF_CUBE8 : IF_TEX8;
    case IF_VB:   return IF_VB8;   case IF_IB: return IF_IB8;  case IF_SURF: return IF_SURF8;
    }
    return 0;
}
/* the object at *pp (an out parameter) is handed to D3D8 code: its D3D8 vtable */
static void wrap(uint32_t pp){
    if (!pp || !mem_is_committed(pp)) return;
    uint32_t o = MR32(pp), i8 = o ? iface8_of(o) : 0;
    if (i8) MR32(o) = com_vtab_for(i8);
    else if (o){ static int said; if (said++ < 8) fprintf(stderr, "[d3d8] object 0x%08x (interface %u) has no D3D8 vtable\n", o, com_obj_iface(o)); }
}
uint32_t d3d8_create(void){
    uint32_t d = com_direct3dcreate9();
    if (d) MR32(d) = com_vtab_for(IF_D3D8);
    fprintf(stderr, "[d3d8] Direct3DCreate8 -> 0x%08x (IDirect3D8 on the D3D9 layer)\n", d);
    return d;
}

#define ARG8(i) MR32(c->esp + 4u + 4u*(uint32_t)(i))   /* before the return address is popped: 0 = this */
static int d3d8_call_(CPU *c, uint32_t if8, uint32_t m);

/* Call D3D9 method `name` of iface9 with args a[0..n-1] (a[0] = this),
 * made below the caller's stack; returns eax.  The caller's own frame is
 * left as it was (the D3D8 arguments are popped by the dispatcher). */
static uint32_t call9(CPU *c, uint32_t iface9, const char *name, int n, const uint32_t *a){
    int n9 = 0, m9 = com_method_index(iface9, name, &n9);
    if (m9 < 0 || n9 != n){
        fprintf(stderr, "[d3d8] internal: D3D9 %u.%s takes %d arguments, not %d\n", iface9, name, n9, n);
        return 0x8876086Cu;                                  /* D3DERR_INVALIDCALL */
    }
    uint32_t esp0 = c->esp, eip0 = c->eip;
    uint32_t sp = esp0 - 64u - 4u * (uint32_t)n;            /* leave the red zone below esp alone */
    for (int i = 0; i < n; i++) MR32(sp + 4u + 4u*(uint32_t)i) = a[i];
    MR32(sp) = 0xD3D8D3D8u;                                  /* return address: popped by com_call */
    c->esp = sp;
    com_call(c, COM_MAGIC | (iface9 << 16) | (uint32_t)m9);
    c->esp = esp0; c->eip = eip0;
    return c->eax;
}

/* ---- per (interface, method): passed through, or one of ours ---- */
enum { K_UNSET = 0, K_PASS, K_SPECIAL, K_NONE };
static uint8_t  g_kind[IF_END8][128];
static int16_t  g_m9[IF_END8][128];
static uint8_t  g_wrap[IF_END8][128];          /* out-object argument index + 1 */

/* methods whose out-object argument (index, this = 0) gets a D3D8 vtable when passed through */
static int out_object_arg(const char *nm, uint32_t if8){
    if (!strcmp(nm, "QueryInterface")) return 2;
    if (!strcmp(nm, "GetDevice")) return 1;
    if (!strcmp(nm, "GetTexture")) return 2;
    if (!strcmp(nm, "GetSurfaceLevel")) return 2;
    if (!strcmp(nm, "GetContainer")) return 2;
    if (!strcmp(nm, "GetDepthStencilSurface")) return 1;
    (void)if8;
    return 0;
}
static const char *const SPECIAL[] = {
    /* IDirect3D8 */
    "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "CheckDeviceMultiSampleType",
    "GetDeviceCaps", "CreateDevice",
    /* IDirect3DDevice8 */
    "ResourceManagerDiscardBytes", "GetDirect3D", "GetDisplayMode", "Reset", "GetBackBuffer",
    "GetRasterStatus", "SetGammaRamp", "GetGammaRamp", "CreateTexture", "CreateVolumeTexture",
    "CreateCubeTexture", "CreateVertexBuffer", "CreateIndexBuffer", "CreateRenderTarget",
    "CreateDepthStencilSurface", "CreateImageSurface", "CopyRects", "GetFrontBuffer",
    "SetRenderTarget", "GetRenderTarget", "EndStateBlock", "ApplyStateBlock", "CaptureStateBlock",
    "DeleteStateBlock", "CreateStateBlock", "GetTextureStageState", "SetTextureStageState", "GetInfo",
    "DrawIndexedPrimitive", "ProcessVertices", "CreateVertexShader", "SetVertexShader", "GetVertexShader",
    "DeleteVertexShader", "SetVertexShaderConstant", "GetVertexShaderConstant",
    "GetVertexShaderDeclaration", "GetVertexShaderFunction", "SetStreamSource", "GetStreamSource",
    "SetIndices", "GetIndices", "CreatePixelShader", "DeletePixelShader", "SetPixelShaderConstant",
    "GetPixelShaderConstant", "GetPixelShaderFunction", "CreateAdditionalSwapChain",
    /* resources */
    "GetLevelDesc", "GetDesc",
    0 };
static int is_special(const char *nm, uint32_t if8){
    if (!strcmp(nm, "GetDesc") && (if8 == IF_VB8 || if8 == IF_IB8)) return 0;   /* same struct in D3D9 */
    if (!strcmp(nm, "GetDeviceCaps") && if8 != IF_D3D8 && if8 != IF_DEV8) return 0;
    for (int i = 0; SPECIAL[i]; i++) if (!strcmp(nm, SPECIAL[i])) return 1;
    return 0;
}
static void classify(uint32_t if8, uint32_t m, const char *nm, int n8){
    g_kind[if8][m] = K_NONE;
    if (is_special(nm, if8)){ g_kind[if8][m] = K_SPECIAL; return; }
    int n9 = 0, wrap_arg = out_object_arg(nm, if8);
    const char *nm9 = nm;
    /* a cube texture's methods sit in the 2D slots with the face first: com.c
     * adds that argument itself for a cube texture (is_cube_tex) */
    if (if8 == IF_CUBE8 && !strcmp(nm, "GetCubeMapSurface")){ nm9 = "GetSurfaceLevel"; n8--; wrap_arg = 3; }
    else if (if8 == IF_CUBE8 && (!strcmp(nm, "LockRect") || !strcmp(nm, "UnlockRect") || !strcmp(nm, "AddDirtyRect"))) n8--;
    int m9 = com_method_index(base9(if8), nm9, &n9);
    if (m9 >= 0 && n9 == n8){ g_kind[if8][m] = K_PASS; g_m9[if8][m] = (int16_t)m9; g_wrap[if8][m] = (uint8_t)wrap_arg; }
}

/* ---- D3D8 state that D3D9 keeps elsewhere ---- */
static uint32_t g_base_vertex;                  /* SetIndices' BaseVertexIndex */
static uint32_t g_cur_vs8;                      /* SetVertexShader's handle (FVF or ours) */
static uint32_t g_stream_vb[16], g_stream_stride[16];
static uint64_t g_calls8, g_pass8, g_special8, g_unimpl8;
static uint32_t g_unimpl_seen[64]; static int g_unimpl_n;

/* D3D8 vertex shader handles: an FVF (bit 0 clear), or one of ours (bit 0 set):
 * a scratch block {magic, vs9, decl9} */
#define VS8_MAGIC 0x38535644u                   /* "DVS8" */

/* D3DVSD declaration (D3D8) -> D3DVERTEXELEMENT9[] in scratch; 0 if unsupported */
static uint32_t decl9_from8(CPU *c, uint32_t dev, uint32_t tok){
    static const uint8_t vsdt_size[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
    /* the fixed-function register meanings (D3DVSDE_*) -> (usage, index) */
    static const uint8_t usage[17][2] = { {0,0},{1,0},{2,0},{3,0},{4,0},{10,0},{10,1},
        {5,0},{5,1},{5,2},{5,3},{5,4},{5,5},{5,6},{5,7},{0,1},{3,1} };
    uint32_t el = com_scratch(8u * 65u), n = 0, stream = 0, off = 0;
    for (int k = 0; k < 256 && mem_is_committed(tok); k++, tok += 4){
        uint32_t t = MR32(tok), type = t >> 29;
        if (t == 0xFFFFFFFFu) break;                         /* D3DVSD_END */
        if (type == 0) continue;                             /* NOP */
        if (type == 1){ stream = t & 0xFFFFu; off = 0; continue; }     /* STREAM */
        if (type == 2){
            if (t & 0x10000000u){ off += 4u * ((t >> 16) & 0xFu); continue; }   /* SKIP: n dwords */
            uint32_t dt = (t >> 16) & 0xFu, reg = t & 0x1Fu;
            if (dt > 7 || reg > 16 || n >= 64){ fprintf(stderr, "[d3d8] vertex declaration: register %u type %u not supported\n", reg, dt); return 0; }
            uint32_t e = el + 8u * n++;
            MR16(e) = (uint16_t)stream; MR16(e+2) = (uint16_t)off; MR8(e+4) = (uint8_t)dt; MR8(e+5) = 0;
            MR8(e+6) = usage[reg][0]; MR8(e+7) = usage[reg][1];
            off += vsdt_size[dt]; continue;
        }
        if (type == 4){ tok += 4u * ((t >> 25) & 0xFu) * 4u; continue; }   /* CONST: count*4 dwords follow (not kept) */
        fprintf(stderr, "[d3d8] vertex declaration token 0x%08x not supported\n", t);
        return 0;
    }
    uint32_t e = el + 8u * n;                                /* D3DDECL_END */
    MR16(e) = 0xFFu; MR16(e+2) = 0; MR8(e+4) = 17; MR8(e+5) = 0; MR8(e+6) = 0; MR8(e+7) = 0;
    uint32_t pp = com_scratch(4); MR32(pp) = 0;
    uint32_t a[3] = { dev, el, pp };
    if (call9(c, IF_DEV, "CreateVertexDeclaration", 3, a) != 0) return 0;
    return MR32(pp);
}

/* D3DCAPS8 from com.c's D3DCAPS9: the same first 212 bytes, with what a
 * D3D8 driver of WC3's time reports: shader models 1.1 / 1.4 (D3D8 has no
 * 2.0; com.c says 2.0 for NFSU2), 4 textures at once over 8 stages. */
static void caps8(uint32_t p8, uint32_t c9){
    mem_commit(p8, 212); memcpy(g_mem + p8, g_mem + c9, 212);
    MR32(p8 + 0x98) = 4u;                   /* MaxSimultaneousTextures */
    MR32(p8 + 0xC4) = 0xFFFE0101u;          /* VertexShaderVersion 1.1 */
    MR32(p8 + 0xC8) = 96u;                  /* MaxVertexShaderConst */
    MR32(p8 + 0xCC) = 0xFFFF0104u;          /* PixelShaderVersion 1.4 */
}

static uint32_t d3d_special(CPU *c, uint32_t if8, const char *nm, uint32_t *ok){
    uint32_t t = ARG8(0);
    *ok = 1;
#define IS(x) (!strcmp(nm, x))
    if (if8 == IF_D3D8){
        if (IS("GetAdapterIdentifier")){                     /* (adapter, flags, D3DADAPTER_IDENTIFIER8*) */
            uint32_t tmp = com_scratch(1100), p8 = ARG8(3);
            uint32_t a[4] = { t, ARG8(1), ARG8(2), tmp }; uint32_t r = call9(c, IF_D3D9, "GetAdapterIdentifier", 4, a);
            if (!r && p8){ mem_commit(p8, 1068);
                memcpy(g_mem + p8, g_mem + tmp, 1024);                       /* Driver, Description */
                memcpy(g_mem + p8 + 1024, g_mem + tmp + 1056, 44); }         /* DriverVersion .. WHQLLevel */
            return r; }
        if (IS("GetAdapterModeCount")){ uint32_t a[3] = { t, ARG8(1), 22u }; return call9(c, IF_D3D9, "GetAdapterModeCount", 3, a); }
        if (IS("EnumAdapterModes")){ uint32_t a[5] = { t, ARG8(1), 22u, ARG8(2), ARG8(3) }; return call9(c, IF_D3D9, "EnumAdapterModes", 5, a); }
        if (IS("CheckDeviceMultiSampleType")){ uint32_t a[7] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), ARG8(5), 0 };
            return call9(c, IF_D3D9, "CheckDeviceMultiSampleType", 7, a); }
        if (IS("GetDeviceCaps")){                            /* D3DCAPS8 = the first 212 bytes of D3DCAPS9 */
            uint32_t tmp = com_scratch(0x150), p8 = ARG8(3);
            uint32_t a[4] = { t, ARG8(1), ARG8(2), tmp }; uint32_t r = call9(c, IF_D3D9, "GetDeviceCaps", 4, a);
            if (!r && p8) caps8(p8, tmp);
            return r; }
        if (IS("CreateDevice")){                             /* (adapter, type, hwnd, flags, D3DPRESENT_PARAMETERS8*, pp) */
            uint32_t p8 = ARG8(5), pp = ARG8(6), p9 = com_scratch(56);
            for (int k = 0; k < 14; k++) MR32(p9 + 4u*k) = 0;
            if (p8){ for (int k = 0; k < 5; k++) MR32(p9 + 4u*k) = MR32(p8 + 4u*k);   /* w, h, format, count, multisample */
                     for (int k = 5; k < 13; k++) MR32(p9 + 4u*(k+1)) = MR32(p8 + 4u*k); }  /* swap effect .. interval */
            fprintf(stderr, "[d3d8] CreateDevice %ux%u format %u, %s, flags 0x%x\n", MR32(p9), MR32(p9+4), MR32(p9+8),
                    MR32(p9+32) ? "windowed" : "full screen", ARG8(4));
            uint32_t a[7] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), p9, pp };
            uint32_t r = call9(c, IF_D3D9, "CreateDevice", 7, a);
            if (p8){ for (int k = 0; k < 5; k++) MR32(p8 + 4u*k) = MR32(p9 + 4u*k);
                     for (int k = 5; k < 13; k++) MR32(p8 + 4u*k) = MR32(p9 + 4u*(k+1)); }
            if (!r) wrap(pp);
            return r; }
    }
    if (if8 == IF_DEV8){
        if (IS("ResourceManagerDiscardBytes")){ uint32_t a[1] = { t }; return call9(c, IF_DEV, "EvictManagedResources", 1, a); }
        if (IS("GetDirect3D")){ uint32_t a[2] = { t, ARG8(1) }; uint32_t r = call9(c, IF_DEV, "GetDirect3D", 2, a); if (!r) wrap(ARG8(1)); return r; }
        if (IS("GetDisplayMode")){ uint32_t a[3] = { t, 0, ARG8(1) }; return call9(c, IF_DEV, "GetDisplayMode", 3, a); }
        if (IS("Reset")){
            uint32_t p8 = ARG8(1), p9 = com_scratch(56);
            for (int k = 0; k < 14; k++) MR32(p9 + 4u*k) = 0;
            if (p8){ for (int k = 0; k < 5; k++) MR32(p9 + 4u*k) = MR32(p8 + 4u*k);
                     for (int k = 5; k < 13; k++) MR32(p9 + 4u*(k+1)) = MR32(p8 + 4u*k); }
            uint32_t a[2] = { t, p9 }; return call9(c, IF_DEV, "Reset", 2, a); }
        if (IS("GetBackBuffer")){ uint32_t a[5] = { t, 0, ARG8(1), ARG8(2), ARG8(3) }; uint32_t r = call9(c, IF_DEV, "GetBackBuffer", 5, a); if (!r) wrap(ARG8(3)); return r; }
        if (IS("GetRasterStatus")){ uint32_t a[3] = { t, 0, ARG8(1) }; return call9(c, IF_DEV, "GetRasterStatus", 3, a); }
        if (IS("SetGammaRamp")){ uint32_t a[4] = { t, 0, ARG8(1), ARG8(2) }; return call9(c, IF_DEV, "SetGammaRamp", 4, a); }
        if (IS("GetGammaRamp")){ uint32_t a[3] = { t, 0, ARG8(1) }; return call9(c, IF_DEV, "GetGammaRamp", 3, a); }
        if (IS("CreateTexture")){ uint32_t a[9] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), ARG8(5), ARG8(6), ARG8(7), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateTexture", 9, a); if (!r) wrap(ARG8(7)); return r; }
        if (IS("CreateVolumeTexture")){ *ok = 0; return 0x8876086Cu; }
        if (IS("CreateCubeTexture")){ uint32_t a[8] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), ARG8(5), ARG8(6), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateCubeTexture", 8, a); if (!r) wrap(ARG8(6)); return r; }
        if (IS("CreateVertexBuffer")){ uint32_t a[7] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), ARG8(5), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateVertexBuffer", 7, a); if (!r) wrap(ARG8(5)); return r; }
        if (IS("CreateIndexBuffer")){ uint32_t a[7] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), ARG8(5), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateIndexBuffer", 7, a); if (!r) wrap(ARG8(5)); return r; }
        if (IS("CreateRenderTarget")){   /* (w, h, fmt, ms, lockable, pp) */
            uint32_t a[9] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), 0, ARG8(5), ARG8(6), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateRenderTarget", 9, a); if (!r) wrap(ARG8(6)); return r; }
        if (IS("CreateDepthStencilSurface")){   /* (w, h, fmt, ms, pp) */
            uint32_t a[9] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), 0, 0, ARG8(5), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateDepthStencilSurface", 9, a); if (!r) wrap(ARG8(5)); return r; }
        if (IS("CreateImageSurface")){   /* (w, h, fmt, pp): a system-memory surface */
            uint32_t a[7] = { t, ARG8(1), ARG8(2), ARG8(3), 2u /* D3DPOOL_SYSTEMMEM */, ARG8(4), 0 };
            uint32_t r = call9(c, IF_DEV, "CreateOffscreenPlainSurface", 7, a); if (!r) wrap(ARG8(4)); return r; }
        if (IS("CopyRects")){            /* (src, RECT* rects, n, dst, POINT* points) */
            uint32_t src = ARG8(1), rects = ARG8(2), n = ARG8(3), dst = ARG8(4), pts = ARG8(5), r = 0;
            if (!rects || !n){ uint32_t a[5] = { t, src, 0, dst, 0 }; return call9(c, IF_DEV, "UpdateSurface", 5, a); }
            for (uint32_t k = 0; k < n && !r; k++){
                uint32_t a[5] = { t, src, rects + 16u*k, dst, pts ? pts + 8u*k : 0 };
                r = call9(c, IF_DEV, "UpdateSurface", 5, a);
            }
            return r; }
        if (IS("GetFrontBuffer")){ uint32_t a[3] = { t, 0, ARG8(1) }; return call9(c, IF_DEV, "GetFrontBufferData", 3, a); }
        if (IS("SetRenderTarget")){      /* (rt, zs): NULL rt keeps the current one */
            uint32_t r = 0;
            if (ARG8(1)){ uint32_t a[3] = { t, 0, ARG8(1) }; r = call9(c, IF_DEV, "SetRenderTarget", 3, a); }
            if (!r){ uint32_t a[2] = { t, ARG8(2) }; r = call9(c, IF_DEV, "SetDepthStencilSurface", 2, a); }
            return r; }
        if (IS("GetRenderTarget")){ uint32_t a[3] = { t, 0, ARG8(1) }; uint32_t r = call9(c, IF_DEV, "GetRenderTarget", 3, a); if (!r) wrap(ARG8(1)); return r; }
        /* state blocks: the D3D8 token is the IDirect3DStateBlock9 object */
        if (IS("CreateStateBlock")){ uint32_t a[3] = { t, ARG8(1), ARG8(2) }; return call9(c, IF_DEV, "CreateStateBlock", 3, a); }
        if (IS("EndStateBlock")){ uint32_t a[2] = { t, ARG8(1) }; return call9(c, IF_DEV, "EndStateBlock", 2, a); }
        if (IS("ApplyStateBlock") || IS("CaptureStateBlock") || IS("DeleteStateBlock")){
            uint32_t sb = ARG8(1); if (!sb) return 0x8876086Cu;
            uint32_t a[1] = { sb };
            uint32_t r = call9(c, IF_STATEBLOCK, IS("ApplyStateBlock") ? "Apply" : IS("CaptureStateBlock") ? "Capture" : "Release", 1, a);
            return IS("DeleteStateBlock") ? 0u : r; }
        /* D3D8 texture stage states 12..21 and 25 are D3D9 sampler states */
        if (IS("SetTextureStageState") || IS("GetTextureStageState")){
            uint32_t st = ARG8(1), ty = ARG8(2), v = ARG8(3), smp = 0;
            switch (ty){ case 13: smp = 1; break; case 14: smp = 2; break; case 25: smp = 3; break; case 15: smp = 4; break;
                         case 16: smp = 5; break; case 17: smp = 6; break; case 18: smp = 7; break; case 19: smp = 8; break;
                         case 20: smp = 9; break; case 21: smp = 10; break; }
            int set = IS("SetTextureStageState");
            if (smp){
                if (smp >= 5 && smp <= 7 && set && v == 4u) v = 3u;    /* D3DTEXF_FLATCUBIC/GAUSSIANCUBIC (D3D8 4,5): linear-ish */
                uint32_t a[4] = { t, st, smp, v };
                return call9(c, IF_DEV, set ? "SetSamplerState" : "GetSamplerState", 4, a); }
            uint32_t a[4] = { t, st, ty, v };
            return call9(c, IF_DEV, set ? "SetTextureStageState" : "GetTextureStageState", 4, a); }
        if (IS("GetInfo")) return 1u;                         /* S_FALSE: no info */
        if (IS("DrawIndexedPrimitive")){  /* (type, minIndex, numVertices, startIndex, primCount) + SetIndices' base */
            uint32_t a[7] = { t, ARG8(1), g_base_vertex, ARG8(2), ARG8(3), ARG8(4), ARG8(5) };
            return call9(c, IF_DEV, "DrawIndexedPrimitive", 7, a); }
        if (IS("ProcessVertices")){ uint32_t a[7] = { t, ARG8(1), ARG8(2), ARG8(3), ARG8(4), 0, ARG8(5) };
            return call9(c, IF_DEV, "ProcessVertices", 7, a); }
        if (IS("CreateVertexShader")){   /* (D3DVSD declaration, function, DWORD* handle, usage) */
            uint32_t decl = ARG8(1), fn = ARG8(2), ph = ARG8(3);
            uint32_t d9 = decl ? decl9_from8(c, t, decl) : 0, vs9 = 0;
            if (fn){
                uint32_t pp = com_scratch(4); MR32(pp) = 0;
                uint32_t a[3] = { t, fn, pp };
                if (call9(c, IF_DEV, "CreateVertexShader", 3, a) == 0) vs9 = MR32(pp);
                static int said; if (said++ < 4) fprintf(stderr, "[d3d8] programmable vertex shader (version 0x%08x) -> 0x%08x\n", MR32(fn), vs9);
            }
            if ((fn && !vs9) || (decl && !d9)) return 0x8876086Cu;
            uint32_t h = com_scratch(16);
            MR32(h) = VS8_MAGIC; MR32(h + 4) = vs9; MR32(h + 8) = d9;
            if (ph) MR32(ph) = h | 1u;
            return 0; }
        if (IS("SetVertexShader")){
            uint32_t h = ARG8(1), r;
            g_cur_vs8 = h;
            if (h & 1u){
                uint32_t b = h & ~1u;
                if (!mem_is_committed(b) || MR32(b) != VS8_MAGIC) return 0x8876086Cu;
                uint32_t a1[2] = { t, MR32(b + 8) }; call9(c, IF_DEV, "SetVertexDeclaration", 2, a1);
                uint32_t a2[2] = { t, MR32(b + 4) }; r = call9(c, IF_DEV, "SetVertexShader", 2, a2);
            } else {
                uint32_t a1[2] = { t, 0 }; call9(c, IF_DEV, "SetVertexShader", 2, a1);
                uint32_t a2[2] = { t, h }; r = call9(c, IF_DEV, "SetFVF", 2, a2);
            }
            return r; }
        if (IS("GetVertexShader")){ if (ARG8(1)) MR32(ARG8(1)) = g_cur_vs8; return 0; }
        if (IS("DeleteVertexShader")) return 0;
        if (IS("SetVertexShaderConstant") || IS("GetVertexShaderConstant")){
            uint32_t a[4] = { t, ARG8(1), ARG8(2), ARG8(3) };
            return call9(c, IF_DEV, IS("SetVertexShaderConstant") ? "SetVertexShaderConstantF" : "GetVertexShaderConstantF", 4, a); }
        if (IS("SetPixelShaderConstant") || IS("GetPixelShaderConstant")){
            uint32_t a[4] = { t, ARG8(1), ARG8(2), ARG8(3) };
            return call9(c, IF_DEV, IS("SetPixelShaderConstant") ? "SetPixelShaderConstantF" : "GetPixelShaderConstantF", 4, a); }
        if (IS("CreatePixelShader")){    /* (function, DWORD* handle): the handle is the IDirect3DPixelShader9 */
            uint32_t a[3] = { t, ARG8(1), ARG8(2) }; return call9(c, IF_DEV, "CreatePixelShader", 3, a); }
        if (IS("DeletePixelShader")) return 0;
        if (IS("SetStreamSource")){      /* (stream, vb, stride) */
            uint32_t n = ARG8(1); if (n < 16){ g_stream_vb[n] = ARG8(2); g_stream_stride[n] = ARG8(3); }
            uint32_t a[5] = { t, n, ARG8(2), 0, ARG8(3) }; return call9(c, IF_DEV, "SetStreamSource", 5, a); }
        if (IS("GetStreamSource")){      /* (stream, IDirect3DVertexBuffer8**, UINT* stride) */
            uint32_t off = com_scratch(4);
            uint32_t a[5] = { t, ARG8(1), ARG8(2), off, ARG8(3) }; uint32_t r = call9(c, IF_DEV, "GetStreamSource", 5, a);
            if (!r) wrap(ARG8(2)); return r; }
        if (IS("SetIndices")){ g_base_vertex = ARG8(2); uint32_t a[2] = { t, ARG8(1) }; return call9(c, IF_DEV, "SetIndices", 2, a); }
        if (IS("GetIndices")){ uint32_t a[2] = { t, ARG8(1) }; uint32_t r = call9(c, IF_DEV, "GetIndices", 2, a);
            if (!r) wrap(ARG8(1));
            if (ARG8(2)) MR32(ARG8(2)) = g_base_vertex; return r; }
        if (IS("GetDeviceCaps")){
            uint32_t tmp = com_scratch(0x150), p8 = ARG8(1);
            uint32_t a[2] = { t, tmp }; uint32_t r = call9(c, IF_DEV, "GetDeviceCaps", 2, a);
            if (!r && p8) caps8(p8, tmp);
            return r; }
    }
    /* D3DSURFACE_DESC8: Format Type Usage Pool Size MultiSampleType Width Height;
     * D3D9: Format Type Usage Pool MultiSampleType MultiSampleQuality Width Height */
    if ((if8 == IF_TEX8 || if8 == IF_CUBE8) && IS("GetLevelDesc")){
        uint32_t d9 = com_scratch(32), p8 = ARG8(2);
        uint32_t a[3] = { t, ARG8(1), d9 }; uint32_t r = call9(c, IF_TEX, "GetLevelDesc", 3, a);
        if (!r && p8){ for (int k = 0; k < 4; k++) MR32(p8 + 4u*k) = MR32(d9 + 4u*k);
            MR32(p8 + 20) = MR32(d9 + 16); MR32(p8 + 24) = MR32(d9 + 24); MR32(p8 + 28) = MR32(d9 + 28);
            MR32(p8 + 16) = MR32(d9 + 24) * MR32(d9 + 28) * 4u; }          /* Size: approximate (32-bit texels) */
        return r; }
    if (if8 == IF_SURF8 && IS("GetDesc")){
        uint32_t d9 = com_scratch(32), p8 = ARG8(1);
        uint32_t a[2] = { t, d9 }; uint32_t r = call9(c, IF_SURF, "GetDesc", 2, a);
        if (!r && p8){ for (int k = 0; k < 4; k++) MR32(p8 + 4u*k) = MR32(d9 + 4u*k);
            MR32(p8 + 20) = MR32(d9 + 16); MR32(p8 + 24) = MR32(d9 + 24); MR32(p8 + 28) = MR32(d9 + 28);
            MR32(p8 + 16) = MR32(d9 + 24) * MR32(d9 + 28) * 4u; }
        return r; }
#undef IS
    *ok = 0;
    return 0x8876086Cu;
}

static void check_saved(CPU *c, const uint32_t *before, const char *nm){
    const uint32_t now[4] = { c->ebx, c->esi, c->edi, c->ebp };
    for (int k = 0; k < 4; k++) if (now[k] != before[k]){
        static int said; if (said++ < 8)
            fprintf(stderr, "[d3d8] %s changed a callee-saved register (%s %08x -> %08x)\n", nm,
                    k == 0 ? "ebx" : k == 1 ? "esi" : k == 2 ? "edi" : "ebp", before[k], now[k]);
    }
}
/* per method, for the watchdog: what the renderer calls between two reports */
static uint32_t g_hot8[32][128], g_hot8_seen[32][128];
void d3d8_hot_report(void){
    int any = 0;
    for (int k = 0; k < 48; k++){
        uint32_t best = 0; int bi = -1, bm = -1;
        for (int i = 0; i < 32; i++) for (int m = 0; m < 128; m++){
            uint32_t d = g_hot8[i][m] - g_hot8_seen[i][m];
            if (d > best){ best = d; bi = i; bm = m; } }
        if (!best) break;
        if (!any++) fprintf(stderr, "  D3D8 calls since the last report:\n");
        int na = 0; const char *nm = com_method_name((uint32_t)bi, (uint32_t)bm, &na);
        fprintf(stderr, "    if%-3d %-28s %8u\n", bi, nm ? nm : "?", best);
        g_hot8_seen[bi][bm] = g_hot8[bi][bm];
    }
    if (!any) fprintf(stderr, "  D3D8 calls since the last report: none\n");
    for (int i = 0; i < 32; i++) for (int m = 0; m < 128; m++) g_hot8_seen[i][m] = g_hot8[i][m];
}

int d3d8_call(CPU *c, uint32_t if8, uint32_t m){
    if (if8 < 32 && m < 128) g_hot8[if8][m]++;
    const uint32_t saved[4] = { c->ebx, c->esi, c->edi, c->ebp };
    int r_ = d3d8_call_(c, if8, m);
    int n_ = 0; const char *nm_ = com_method_name(if8, m, &n_);
    check_saved(c, saved, nm_ ? nm_ : "?");
    return r_;
}
static int d3d8_call_(CPU *c, uint32_t if8, uint32_t m){
    int n8 = 0;
    const char *nm = com_method_name(if8, m, &n8);
    g_calls8++;
    if (!nm || if8 >= IF_END8 || m >= 128){
        fprintf(stderr, "[d3d8] invalid call %u:%u\n", if8, m);
        POP32(c->eip); c->eax = 0x80004001u; return 1;
    }
    if (g_kind[if8][m] == K_UNSET) classify(if8, m, nm, n8);
    if (g_kind[if8][m] == K_PASS){
        g_pass8++;
        uint32_t w = g_wrap[if8][m] ? ARG8(g_wrap[if8][m]) : 0;
        int r = com_call(c, COM_MAGIC | (base9(if8) << 16) | (uint32_t)g_m9[if8][m]);
        if (w && !c->eax) wrap(w);
        return r;
    }
    uint32_t ret = MR32(c->esp), ok = 0, r = 0;
    if (g_kind[if8][m] == K_SPECIAL){ g_special8++; r = d3d_special(c, if8, nm, &ok); }
    if (!ok){
        g_unimpl8++;
        int seen = 0; for (int i = 0; i < g_unimpl_n; i++) if (g_unimpl_seen[i] == (if8 << 16 | m)) seen = 1;
        if (!seen && g_unimpl_n < 64){ g_unimpl_seen[g_unimpl_n++] = if8 << 16 | m;
            fprintf(stderr, "[d3d8] NOT IMPLEMENTED: %s::%s (%d args) from 0x%08x\n",
                    if8 == IF_D3D8 ? "IDirect3D8" : if8 == IF_DEV8 ? "IDirect3DDevice8" : if8 == IF_SURF8 ? "IDirect3DSurface8"
                    : if8 == IF_TEX8 ? "IDirect3DTexture8" : if8 == IF_CUBE8 ? "IDirect3DCubeTexture8" : "IDirect3DResource8",
                    nm, n8, ret); }
        r = 0x8876086Cu;
    }
    c->esp += 4u + 4u * (uint32_t)n8;          /* return address + the D3D8 arguments (stdcall) */
    c->eip = ret;
    c->eax = r;
    return 1;
}

void d3d8_report(void){
    if (!g_calls8) return;
    fprintf(stderr, "  D3D8 calls        : %llu (%llu passed to D3D9 as they are, %llu translated, %llu not implemented)\n",
            (unsigned long long)g_calls8, (unsigned long long)g_pass8, (unsigned long long)g_special8, (unsigned long long)g_unimpl8);
}
