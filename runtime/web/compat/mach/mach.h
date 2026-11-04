/* wasm: Mach task queries are unavailable; callers treat failure as "unknown" */
#pragma once
#include <stdint.h>
typedef int kern_return_t;
typedef unsigned mach_msg_type_number_t;
typedef int *task_info_t;
typedef unsigned task_t;
typedef struct { uint64_t virtual_size, resident_size, phys_footprint, resident_size_peak; } task_vm_info_data_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define TASK_VM_INFO 22
#define TASK_POWER_INFO_V2 26
struct task_power_info_v2 { uint64_t gpu_energy, pad[16]; };
#define TASK_POWER_INFO_V2_COUNT ((mach_msg_type_number_t)(sizeof(struct task_power_info_v2) / sizeof(int)))
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(int)))
static inline task_t mach_task_self(void){ return 0; }
static inline kern_return_t task_info(task_t t, int flavor, task_info_t out, mach_msg_type_number_t *n){
    (void)t; (void)flavor; (void)out; (void)n; return KERN_FAILURE; }
