/* X87PC fast path check (web/tools/x87pctest.sh): x87_pcround, the inlined
 * PC=24 rounding every lifted x87 result goes through, against the exact
 * out-of-line x87_round24 for the game thread's control word (PC=24,
 * RC=nearest) and the CRT's (PC=53/64, returned as is).  Inputs: zeros,
 * subnormals, the float normal boundary and its neighbours, float max and
 * overflow, infinities, NaNs, random doubles over the whole exponent range and
 * random bit patterns.  Exit 0 = every result bit-identical. */
#include "cpu.h"
#include <stdio.h>
#include <string.h>

static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void){ s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static double from(uint64_t b){ double d; memcpy(&d, &b, 8); return d; }
static uint64_t bits(double d){ uint64_t b; memcpy(&b, &d, 8); return b; }

int main(void){
    static const double fixed[] = { 0.0, -0.0, 0x1p-126, -0x1p-126, 0x1p-126 - 0x1p-150, 0x1p-126 - 0x1p-149, 0x1p-126 + 0x1p-150,
        0x1p-149, 0x1p-150, 0x1p-151, -0x1p-150, 0x1p-200, 0x1.fffffep127, 0x1.fffffefp127, 0x1.ffffffp127, 0x1p128, -0x1p128,
        1e300, 1.0 / 0.0, -1.0 / 0.0, 1.0 / 3.0, 0.1, 16777217.0, 4.9e-324 };
    long n = 0, bad = 0;
    for (int pass = 0; pass < 2; pass++){
        uint16_t fcw = pass ? 0x027F : 0x007F;           /* PC=53: unchanged; PC=24: rounded */
        for (long i = 0; i < 4000000; i++){
            double v;
            if (i < (long)(sizeof fixed / sizeof *fixed)) v = fixed[i];
            else if (i < 2000000){                       /* random significand, exponent around the float range */
                uint64_t b = rnd(); int e = (int)(rnd() % 400) - 200 + 1023;
                v = from((b & 0x800FFFFFFFFFFFFFull) | ((uint64_t)e << 52));
                if ((i & 15) == 0) v = 0.0 * v;          /* signed zeros */
            } else v = from(rnd());                      /* any bit pattern: NaNs, infinities, huge, tiny */
            double want = pass ? v : x87_round24(v, 0);
            double got = x87_pcround(fcw, v);
            n++;
            if (bits(want) != bits(got) && !(want != want && got != got)){
                if (bad++ < 10) printf("  differs: fcw %04x v %a: want %a got %a\n", fcw, v, want, got);
            }
        }
    }
    printf("x87_pcround vs x87_round24: %ld inputs, %ld differ\n", n, bad);
    return bad != 0;
}
