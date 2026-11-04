/* wasm: no statfs; callers fall back to their default free space */
#pragma once
#include <stdint.h>
struct statfs { uint64_t f_bsize, f_blocks, f_bfree, f_bavail; };
static inline int statfs(const char *p, struct statfs *s){ (void)p; (void)s; return -1; }
