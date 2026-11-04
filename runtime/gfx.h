/* Vertex format shared by com.c (guest side) and metal.m (draw side).
 * Because the vertex shader runs on the CPU, positions arriving here are
 * in CLIP SPACE; the rasterizer does the perspective divide. */
#ifndef GFX_H
#define GFX_H
/* NOTE: texture coordinates carry FOUR components per stage.
 * Previously only .xy was carried, but the game's vertex shaders produce
 * PROJECTED coordinates (measured: oT1.x/y/z = dot(v0, c4..c6),
 * oT1.w = r0.x).  With z/w dropped, the road/car reflections came out
 * streaky and dark. */
typedef struct {
    float x, y, z, w;      /* clip space */
    float r, g, b, a;      /* oD0 - diffuse */
    float r1,g1,b1,a1;     /* oD1 - specular */
    float t[4][4];         /* oT0..oT3, all four components */
} TVert;

void metal_draw_tv(const TVert *tv, unsigned nverts, unsigned prim, unsigned tex);
#endif
