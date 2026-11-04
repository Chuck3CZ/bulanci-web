/* TEMPORARY: neutral stand-ins for the nfsu2-web functions the imported
 * runtime still calls but whose files (NFSU2-only: widescreen, graphics
 * enhancements, DirectInput pad/keyboard, D3DX HLE, autotest, ...) were left
 * out of this port (tools/import_nfsu2.sh).  Each returns what the left-out
 * code returned with its feature off.  Every entry here is a call site still
 * to remove from the runtime (docs/PLAN.md, krok 6); this file goes when the
 * list is empty.  All call sites are in com.c. */
#include "cpu.h"
#include "enhance.h"
#include "widescreen.h"
#include <stdint.h>
#include <stdio.h>

/* widescreen.c: no Hor+ correction, the game's own aspect */
float  ws_ui_x(void){ return 1.f; }
float  ws_ui_y(void){ return 1.f; }
double ws_render_factor(void){ return 1.0; }
double ws_max_pixels(void){ return WS_MAX_PIXELS_DEFAULT; }
float  ws_persp_x(void){ return 1.f; }
float  ws_persp_y(void){ return 1.f; }
void   ws_set_mapping(float k){ (void)k; }
int    ws_fill_target(void){ return 0; }
int    ws_take_border_quad(void){ return 0; }
void   ws_report(void){ }

/* framerate.c: NFSU2's frame-rate modes and host pacer.  Off: the game's
 * Present waits for the page's display slot (gpu_web.c, FIFO, no mailbox).
 * WC3 gets a pacer of its own later (docs/PLAN.md). */
int  hfr_mode_off(void){ return 1; }
int  hfr_mailbox(void){ return 0; }
int  hfr_display_live(void){ return 1; }          /* the page draws: sleeps may spin at their end (win32_ext.c) */
void hfr_note_vsync(double ts, double dur){ (void)ts; (void)dur; }

/* headlights.c: NFSU2's other cars' headlights on the road */
void headlights_vs_ramp(float a, float b){ (void)a; (void)b; }
void headlights_report(void){ }

/* enhance.c: render targets at the size the game asks for */
int enh_rt_size(uint32_t ret, uint32_t bb_w, uint32_t bb_h, uint32_t *w, uint32_t *h){
    (void)ret; (void)bb_w; (void)bb_h; (void)w; (void)h; return ENH_NONE; }

/* d3dx_hle.c / autotest.c / gcpu.c (NFSU2's D3DX effect passes, its front
 * end's package manager, the test scripts' per-Present hook) */
int g_dx_in_pass = 0;
uint32_t g_fe_mgr = 0;
void autotest_on_present(CPU *c){ (void)c; }
void guest_cpu_mark(void){ }

/* dinput_pad.c / dinput_kbd.c: WC3 imports no DirectInput (DirectInput8Create
 * is never called), so no device of these kinds is ever created */
int      dipad_guid(uint32_t guid){ (void)guid; return 0; }
void     dipad_created(uint32_t dev, uint32_t guid){ (void)dev; (void)guid; }
int      dipad_enum(CPU *c, uint32_t devtype, uint32_t cb, uint32_t ref, uint32_t flags){
    (void)c; (void)devtype; (void)cb; (void)ref; (void)flags; return 0; }
uint32_t dipad_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *args){
    (void)c; (void)self; (void)nargs; (void)args; fprintf(stderr, "[compat] DirectInput pad %s: no pad\n", nm); return 0x80004005u; }
uint32_t dipad_effect_call(CPU *c, uint32_t self, const char *nm, int nargs, const uint32_t *args){
    (void)c; (void)self; (void)nm; (void)nargs; (void)args; return 0x80004005u; }
void     dipad_effect_free(uint32_t obj){ (void)obj; }
/* the DirectInput keyboard: runtime/dinput_kbd.c */
