// Checkpoint and rewind, in-process (runtime/checkpoint.c; off unless LARECOMP_CHECKPOINT is set).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile int g_ck_park;           // a checkpoint or rewind wants every guest thread parked
void checkpoint_park(bool holds_core);   // at a safe point while g_ck_park is set
void checkpoint_frame(uint64_t frame);   // after each present (vulkan/device.c)
void checkpoint_key(int which);          // 1: take (F5), 2: rewind (F8)
// Memory that guest state depends on, outside the guest window and the arena: copied and written back too
void rt_checkpoint_static(void *p, size_t n);
void rt_checkpoint_range(uint64_t lo, uint64_t size);

#ifdef __cplusplus
}
#endif
