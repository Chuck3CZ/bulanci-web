/* Guest memory is untyped (web/tools/aliastest.sh).
 * x86 code moves floats through integer registers all the time; MSVC returns
 * a float vector by value like this:
 *     sub  esp,10h ; ... ; fstp dword [esp+8] ; mov edx,[esp+8] ; mov [ecx+8],edx
 * The lifted C for that is an MRF32 store and an MR32 load of one guest
 * address.  If the compiler may assume float and uint32_t accesses never
 * alias, it can move the load above the store and copy a stale value.
 * clang 23's wasm backend did that in the game's vector normalize (0x4870B0)
 * until runtime/cpu.h gave the guest accesses may_alias types.
 * vnorm() is a vector normalize written the same way (lifted style, built with
 * web/build.sh's flags); main() checks its result.  It also builds natively
 * (clang -O3 -mcpu=apple-m1 -Iruntime tests/web/aliastest.c): Apple clang's
 * arm64 code passes with the Mac's plain-typed macros too. */
#include "cpu.h"
#include <stdio.h>

#define ONE 0x00100200u                      /* a guest constant 1.0f */
/* this = ecx (x, y, z), [esp+4] = out; returns out in eax; ret 4 */
__attribute__((noinline)) void vnorm(CPU *restrict c){
    { uint32_t _a=c->esp,_b=0x10u,_r=_a-_b; FLAGS_SUB_M(_a,_b,_r,32,0x00u); c->esp = (uint32_t)(_r); }
    FPUSH((double)MRF32(c->ecx+0x8u));
    c->eax = MR32(c->esp+0x14u);
    FPUSH((double)MRF32(c->ecx+0x4u));
    FPUSH((double)MRF32(c->ecx));
    FPUSH(ST(0));
    { double _d=ST(0), _s=ST(1); ST(0)=X87R(x87_r2(_d,_s,_d * _s)); }
    FPUSH(ST(2));
    { double _d=ST(0), _s=ST(3); ST(0)=X87R(x87_r2(_d,_s,_d * _s)); }
    { double _d=ST(1), _s=ST(0); ST(1)=X87R(x87_r2(_d,_s,_d + _s)); }
    FPOP();
    FPUSH(ST(3));
    { double _d=ST(0), _s=ST(4); ST(0)=X87R(x87_r2(_d,_s,_d * _s)); }
    { double _d=ST(1), _s=ST(0); ST(1)=X87R(x87_r2(_d,_s,_d + _s)); }
    FPOP();
    { double _a=ST(0); ST(0)=X87R(x87_r1(_a, sqrt(_a))); }
    ST(3) = ST(0);
    FPOP(); FPOP(); FPOP();
    { double _d=ST(0), _s=((double)MRF32(ONE)); ST(0)=X87R(x87_r2(_d,_s,_s / _d)); }
    FPUSH(ST(0));
    { double _d=ST(0), _s=(double)MRF32(c->ecx); ST(0)=X87R(x87_r2(_d,_s,_d * _s)); }
    MRF32(c->esp) = (float)ST(0);
    FPOP();
    c->edx = MR32(c->esp);
    FPUSH(ST(0));
    { double _d=ST(0), _s=(double)MRF32(c->ecx+0x4u); ST(0)=X87R(x87_r2(_d,_s,_d * _s)); }
    MRF32(c->esp+0x4u) = (float)ST(0);
    FPOP();
    { double _d=ST(0), _s=(double)MRF32(c->ecx+0x8u); ST(0)=X87R(x87_r2(_d,_s,_d * _s)); }
    c->ecx = c->eax;
    MR32(c->ecx) = c->edx;
    c->edx = MR32(c->esp+0x4u);
    MR32(c->ecx+0x4u) = c->edx;
    MRF32(c->esp+0x8u) = (float)ST(0);      /* fstp dword [esp+8] */
    FPOP();
    c->edx = MR32(c->esp+0x8u);             /* mov edx,[esp+8]    */
    MR32(c->ecx+0x8u) = c->edx;
    c->edx = MR32(c->esp+0xcu);
    MR32(c->ecx+0xcu) = c->edx;
    { uint32_t _a=c->esp,_b=0x10u,_r=_a+_b; FLAGS_ADD_M(_a,_b,_r,32,0x3fu); c->esp = (uint32_t)(_r); }
    POP32(c->eip); c->esp += 4u;
}

#ifndef __wasm__
uint8_t *g_mem;                              /* the Mac: guest base pointer */
#include <stdlib.h>
#endif
/* what cpu.h declares for the lifted code; vnorm needs none of it */
void dispatch_indirect(CPU *c, uint32_t a){ (void)c; (void)a; }
void dispatch_tail(CPU *c){ (void)c; }

int main(void){
    static CPU cpu; CPU *c = &cpu;
#ifndef __wasm__
    g_mem = calloc(1, 0x00300000u);
#endif
    const uint32_t V = 0x00100000u, OUT = 0x00100100u, SP = 0x00200000u;
    int fail = 0;
    static const float in[][3] = { {3, 0, 4}, {0, 0, 2}, {1, 2, 2}, {-6, 8, 0}, {0.5f, -0.5f, 0.5f} };
    for (unsigned k = 0; k < sizeof in / sizeof in[0]; k++){
        memset(c, 0, sizeof *c);
        c->fcw = 0x027F;                         /* PC=53, RC=nearest */
        MRF32(ONE) = 1.0f;
        MRF32(V) = in[k][0]; MRF32(V+4) = in[k][1]; MRF32(V+8) = in[k][2];
        for (uint32_t a = SP - 0x40u; a < SP + 0x10u; a += 4) MR32(a) = 0x7F7F7F7Fu;  /* stale stack */
        c->esp = SP - 8u;                        /* the call: out pushed, then the return address */
        MR32(SP - 4u) = OUT; MR32(SP - 8u) = 0x00401000u;
        c->ecx = V;
        vnorm(c);
        double n = sqrt((double)in[k][0]*in[k][0] + (double)in[k][1]*in[k][1] + (double)in[k][2]*in[k][2]);
        for (int i = 0; i < 3; i++){
            float want = (float)(in[k][i] / n), got = MRF32(OUT + 4u*(uint32_t)i);
            if (!(fabsf(got - want) <= 1e-6f)){
                printf("FAIL vector %u component %d: got %g (0x%08x), want %g\n", k, i, got, MR32(OUT + 4u*(uint32_t)i), want);
                fail++;
            }
        }
        if (c->eax != OUT || c->esp != SP || c->eip != 0x00401000u){ printf("FAIL vector %u: registers\n", k); fail++; }
    }
    printf("alias test: %s (%d failures)\n", fail ? "FAILED" : "passed", fail);
    return fail != 0;
}
