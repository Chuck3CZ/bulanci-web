/* Graphics enhancements.
 *
 * Optional graphics, every build (runtime options, all off by default; the
 * default frame is the original's, bit for bit):
 *   NFSU2_GRAPHICS          original | enhanced | custom      (settings.ini graphics)
 *   NFSU2_MSAA              off | 2 | 4 | auto                (msaa)
 *   NFSU2_MIRROR            original | high | <edge px>       (mirror)
 *   NFSU2_TEXTURE_FILTERING game | anisotropic                (texture_filtering)
 *
 * The unshipped research build (./link.sh hd -> out/nfsu2_hd_boot, NFS_ENHANCED=1)
 * adds the larger road reflection (NFSU2_ROAD_REFLECTION there), upstream's SSR,
 * ray-traced reflections and the larger cube map, with upstream's defaults
 * (HD_* variables).  They were measured and rejected for
 * the shipped app.
 *
 * Principle: do not touch the game's logic.  The game keeps seeing its own
 * sizes (GetDesc says 320x240); only the real texture in Metal grows.
 * The viewport is already set to the whole target, so the scene fills the
 * large texture, and sampling is in UV space, so it is size-independent.  */
#ifndef ENHANCE_H
#define ENHANCE_H
#include <stdint.h>

#ifndef NFS_ENHANCED
#define NFS_ENHANCED 0
#endif

enum { ENH_NONE = 0, ENH_ROAD = 1, ENH_MIRROR = 2, ENH_CUBE = 3 };

/* Render target size: enlarges the (w,h) the game asked for based on the
 * CALLING address.  ret = return address of the COM call (c->eip).  Returns
 * the target kind (ENH_*); ENH_NONE if it left the size alone. */
int  enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h,
                 uint32_t *w, uint32_t *h);
/* Road reflection blur: 1 = the game's original width, 0 = sharp. */
float enh_blur(void);
/* MSAA sample count asked for (1 = off, 2, 4; 8 only in the research build);
 * *autop (optional) = 1 when the count follows the drawable (2x at >= 1400
 * rows, 4x below). */
int  enh_msaa_samples(int *autop);
/* 1 = texture_filtering = anisotropic: a linear sampler the game gives a
 * MAXANISOTROPY above 1 filters anisotropically, as the game's own
 * g_TextureFiltering = 2 does (metal.m samp_for). */
int  enh_aniso(void);
void enh_report(void);
const char *enh_build_name(void);
#if NFS_ENHANCED
/* View setup (overrides.c, sub_005c7ec0): id = eView id,
 * m = 48 floats (view, projection, product; row-vector layout),
 * cam = guest Camera* (may be 0). */
void enh_view_setup(uint32_t id, const float *m, uint32_t cam);
#else
static inline void enh_view_setup(uint32_t id, const float *m, uint32_t cam)
{ (void)id; (void)m; (void)cam; }
#endif

#endif
