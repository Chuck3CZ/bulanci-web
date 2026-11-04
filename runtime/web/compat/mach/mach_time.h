/* wasm: mach_absolute_time in nanoseconds (timebase 1/1) */
#pragma once
#include <stdint.h>
#include <time.h>
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
typedef mach_timebase_info_data_t *mach_timebase_info_t;
static inline int mach_timebase_info(mach_timebase_info_t i){ i->numer = 1; i->denom = 1; return 0; }
static inline uint64_t mach_absolute_time(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static inline uint64_t mach_continuous_time(void){ return mach_absolute_time(); }
