/* wasm: no per-process resource info (diagnostics only) */
#pragma once
#include <stdint.h>
#include <string.h>
#define RUSAGE_INFO_V6 6
typedef void *rusage_info_t;
struct rusage_info_v6 { uint64_t ri_user_time, ri_system_time, ri_resident_size, ri_phys_footprint, ri_lifetime_max_phys_footprint,
                        ri_instructions, ri_cycles, ri_pinstructions, ri_pcycles, ri_user_ptime, ri_system_ptime,
                        ri_energy_nj, ri_penergy_nj; };
static inline int proc_pid_rusage(int pid, int flavor, rusage_info_t *buf){ (void)pid; (void)flavor; (void)buf; return -1; }
