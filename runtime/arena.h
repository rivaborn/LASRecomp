// Host objects that guest state points to: from the checkpoint arena when LARECOMP_CHECKPOINT is set, else
// calloc / realloc / free (runtime/arena.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *rt_obj_calloc(size_t n, size_t size);
void *rt_obj_realloc(void *p, size_t size);
void rt_obj_free(void *p);
bool rt_arena_active(void);
void rt_arena_span(uint8_t **base, uint64_t *used);  // (runtime/checkpoint.c)
void rt_arena_lock(bool on);

#ifdef __cplusplus
}
#endif
