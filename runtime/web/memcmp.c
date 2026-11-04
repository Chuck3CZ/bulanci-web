/* memcmp and bcmp for the browser build, 16 bytes at a time.
 *
 * wasi-libc's memcmp is musl's byte loop.  In V8 an equal 512-byte compare
 * takes ~300 ns with it and ~24 ns with wasm SIMD (node 23, this Mac).  The
 * runtime compares constant blocks on every draw and every redundant-state
 * check (dxrec_call_redundant, raw_rec, the D3DX memos), mostly blocks that
 * are equal, i.e. compared to the end.  An object file is linked before
 * libc.a, so these definitions replace libc's for the whole module. */
#include <stddef.h>
#include <stdint.h>
#include <wasm_simd128.h>

int memcmp(const void *va, const void *vb, size_t n){
    const unsigned char *a = va, *b = vb;
    size_t i = 0;
    for (; i + 16 <= n; i += 16){
        uint32_t ne = ~(uint32_t)wasm_i8x16_bitmask(wasm_i8x16_eq(wasm_v128_load(a + i), wasm_v128_load(b + i))) & 0xFFFFu;
        if (ne){ i += (size_t)__builtin_ctz(ne); return a[i] - b[i]; }
    }
    for (; i < n; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

/* clang turns memcmp() == 0 into bcmp() where the C library has one */
int bcmp(const void *a, const void *b, size_t n){ return memcmp(a, b, n); }
