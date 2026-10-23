// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Checkpoint and rewind, in-process, for recreating a bug exactly (off by default; Windows).
//   BOTW_RECOMP_CHECKPOINT=1                 enables it (host objects then come from the arena: runtime/arena.c)
//   BOTW_RECOMP_CHECKPOINT_AT=<frame>        take a checkpoint at that presented frame
//   BOTW_RECOMP_REWIND_AT=<frame>[,<frame>]  rewind to the checkpoint at those frames (the same moment is replayed)
//   F5 / F8 (keyboard, while playing)     take a checkpoint / rewind to it
// Design and measurements: coding_workflow knowledge/switch-decompile-and-port/14-checkpoint-and-rewind.md.
//
// Taking one: every guest thread parks at a safe point (rt_unwind_point: SVC entry/exit and HLE calls; kwait and
// SleepThread for blocked threads, which are woken for it) and saves its full register context (RtlCaptureContext)
// and its host stack; the thread that presents coordinates and saves itself last. Then the guest memory (committed
// pages; the non-zero ones copied), the extra host ranges (NVN command shadows), the arena and the registered statics
// are copied, and the save folder.
// Rewinding: every thread parks again; threads made since the checkpoint end; the GPU goes idle; the renderer drops
// its caches keyed by guest addresses; memory, arena and statics are written back (pages only where they differ: read-
// only ones never do); texture objects keep their GPU images; the save folder goes back; then each thread writes its
// old stack back from below it and resumes its saved context (RtlRestoreContext without an exception record: no
// unwind over the rewritten frames).
// Not exact (by design): render targets hold what was drawn last, not their checkpoint contents; the audio output and
// the host's own threads (vsync, audio, shader compilers) run on. A rewind is refused when a thread of the checkpoint
// has ended since. (BotW, the Shrine of Resurrection: 41 threads park in ~30 ms; 1.75 GB copied in ~1.4 s; a rewind
// writes ~5,000 pages back in ~0.7 s, and 100 frames later the picture matches the first pass to within 10/255.)
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "checkpoint.h"
#include "kernel.h"
#include "rt.h"
#include "sched.h"

volatile int g_ck_park;  // threads: park at the next safe point

#ifdef _WIN32
#include <windows.h>

KThread *kernel_current_thread(void);
int kernel_thread_list(KThread **out, int max);
void kernel_wake_all_waiters(void);
#define t_cur kernel_current_thread()

#define MAXT 512
typedef struct {
    KThread *t;
    CONTEXT ctx;
    volatile int rewinding;
    uint8_t *stack;
    uintptr_t lo, hi;
    bool saved, in_ck;      // saved in the current checkpoint
    volatile bool leave;    // (rewind) made since the checkpoint: end it
} ThreadSave;

static void save_folder_copy(void);
static ThreadSave g_ts[MAXT];
static int g_nts;
static pthread_mutex_t pk_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pk_cond = PTHREAD_COND_INITIALIZER, pk_done = PTHREAD_COND_INITIALIZER;
static int g_parked;
static volatile int g_capture, g_rewind;

// ---- registered memory: statics (fixed) and host ranges (NVN command shadows; reserved, committed page by page)
typedef struct { void *p; size_t n; uint8_t *copy; } Static;
static Static g_statics[64];
static int g_nstatics;
typedef struct { uint64_t lo, hi; } Range;
static Range g_ranges[256];
static int g_nranges;

void rt_checkpoint_static(void *p, size_t n) {
    if (g_nstatics < 64) g_statics[g_nstatics++] = (Static){p, n, NULL};
}
void rt_checkpoint_range(uint64_t lo, uint64_t size) {
    if (!rt_arena_active()) return;
    pthread_mutex_lock(&pk_lock);
    if (g_nranges < 256) g_ranges[g_nranges++] = (Range){lo, lo + size};
    pthread_mutex_unlock(&pk_lock);
}

static ThreadSave *slot_of(KThread *t) {  // pk_lock held
    for (int i = 0; i < g_nts; i++) if (g_ts[i].t == t) return &g_ts[i];
    if (g_nts == MAXT) return NULL;
    memset(&g_ts[g_nts], 0, sizeof g_ts[g_nts]);
    g_ts[g_nts].t = t;
    return &g_ts[g_nts++];
}

// ---- host stacks and contexts
static void __attribute__((noinline)) save_thread(ThreadSave *s) {
    ULONG_PTR low, high;
    GetCurrentThreadStackLimits(&low, &high);
    s->lo = (uintptr_t)__builtin_frame_address(0) - 4096;  // (this frame and a margin under it)
    s->hi = (uintptr_t)high;
    free(s->stack);
    s->stack = malloc(s->hi - s->lo);
    memcpy(s->stack, (void *)s->lo, s->hi - s->lo);
    s->saved = s->in_ck = true;
}

static void __attribute__((noinline)) restore_from_below(ThreadSave *s) {
    memcpy((void *)s->lo, s->stack, s->hi - s->lo);
    s->rewinding = 1;
    RtlRestoreContext(&s->ctx, NULL);
}

static void __attribute__((noinline)) rewind_thread(ThreadSave *s) {
    volatile uint8_t *pad = __builtin_alloca(s->hi - s->lo + 65536);  // this frame below the saved range
    pad[0] = 0;
    restore_from_below(s);
}

// Capture this thread: returns false when taken, true when it resumes here after a rewind.
static bool __attribute__((noinline)) capture(ThreadSave *s) {
    s->rewinding = 0;
    RtlCaptureContext(&s->ctx);
    if (s->rewinding) { s->rewinding = 0; return true; }
    save_thread(s);
    return false;
}

// A guest thread at a safe point while g_ck_park is set. core: it holds its emulated core (gives it up meanwhile).
void checkpoint_park(bool core) {
    KThread *t = t_cur;
    if (!t) return;
    if (core) sched_leave(t);
    pthread_mutex_lock(&pk_lock);
    ThreadSave *s = slot_of(t);
    bool cap = g_capture;
    pthread_mutex_unlock(&pk_lock);
    if (s && cap && capture(s)) {  // (resumed by a rewind: memory is already back, the others resume too)
        if (core) sched_enter(t);
        return;
    }
    pthread_mutex_lock(&pk_lock);
    g_parked++;
    pthread_cond_broadcast(&pk_done);
    while (g_ck_park && !(s && s->leave)) pthread_cond_wait(&pk_cond, &pk_lock);
    g_parked--;
    bool rewind = g_rewind && s && s->in_ck, leave = s && s->leave;
    pthread_mutex_unlock(&pk_lock);
    if (leave) {  // made after the checkpoint: ends now, as at a reboot (thread_main)
        s->leave = false;
        if (core) sched_enter(t);
        longjmp(t->unwind, 1);
    }
    if (rewind) rewind_thread(s);
    if (core) sched_enter(t);
}

// ---- guest memory and host ranges: committed pages; non-zero pages copied
typedef struct { uint64_t addr; int64_t off; } Page;  // off < 0: all zeros
static Page *g_pages;
static uint64_t g_npages, g_cap_pages;
static uint8_t *g_data;
static uint64_t g_data_len, g_data_cap;

static bool page_zero(const uint64_t *q) {
    for (int k = 0; k < 512; k++) if (q[k]) return false;
    return true;
}

static void snap_range(uint64_t lo, uint64_t hi) {
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (end > hi) end = hi;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = a; p < end; p += 4096) {
                if (g_npages == g_cap_pages) {
                    g_cap_pages = g_cap_pages ? g_cap_pages * 2 : 1 << 20;
                    g_pages = realloc(g_pages, g_cap_pages * sizeof *g_pages);
                }
                const uint64_t *q = (const uint64_t *)(uintptr_t)p;
                int64_t off = -1;
                if (!page_zero(q)) {
                    if (g_data_len + 4096 > g_data_cap) {
                        g_data_cap = g_data_cap ? g_data_cap * 2 : 1ull << 30;
                        g_data = realloc(g_data, g_data_cap);
                        if (!g_data) FATAL("checkpoint: out of memory for the memory copy\n");
                    }
                    memcpy(g_data + g_data_len, q, 4096);
                    off = (int64_t)g_data_len;
                    g_data_len += 4096;
                }
                g_pages[g_npages++] = (Page){p, off};
            }
        a = end;
    }
}

// One page back to its checkpoint contents (data NULL: zeros), written only when it differs: read-only pages (module
// code) never differ, and writing one would fault.
static uint64_t g_pages_written;
static void put_page(uint64_t p, const uint8_t *data) {
    const uint64_t *q = (const uint64_t *)(uintptr_t)p;
    if (data ? !memcmp(q, data, 4096) : page_zero(q)) return;
    if (data) memcpy((void *)(uintptr_t)p, data, 4096);
    else memset((void *)(uintptr_t)p, 0, 4096);
    g_pages_written++;
}

// Every page committed now: its checkpoint contents, or zeros when it wasn't committed then (g_pages is sorted).
static uint64_t restore_range(uint64_t lo, uint64_t hi, uint64_t *k) {
    uint64_t zeroed = 0;
    for (uint64_t a = lo; a < hi;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (end > hi) end = hi;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = a; p < end; p += 4096) {
                while (*k < g_npages && g_pages[*k].addr < p) {  // (committed then, not now: commit, write back)
                    plat_commit(g_pages[*k].addr, 4096);
                    put_page(g_pages[*k].addr, g_pages[*k].off >= 0 ? g_data + g_pages[*k].off : NULL);
                    (*k)++;
                }
                if (*k < g_npages && g_pages[*k].addr == p) {
                    put_page(p, g_pages[*k].off >= 0 ? g_data + g_pages[*k].off : NULL);
                    (*k)++;
                } else {
                    if (!page_zero((const uint64_t *)(uintptr_t)p)) put_page(p, NULL), zeroed++;
                }
            }
        a = end;
    }
    while (*k < g_npages && g_pages[*k].addr < hi) {
        plat_commit(g_pages[*k].addr, 4096);
        put_page(g_pages[*k].addr, g_pages[*k].off >= 0 ? g_data + g_pages[*k].off : NULL);
        (*k)++;
    }
    return zeroed;
}

static Range g_snap_ranges[256];
static int g_nsnap_ranges;
static uint8_t *g_arena_copy;
static uint64_t g_arena_len;
static bool g_have;
static uint64_t g_frame;

static int cmp_range(const void *a, const void *b) {
    const Range *x = a, *y = b;
    return x->lo < y->lo ? -1 : x->lo > y->lo;
}

static void snapshot_memory(void) {
    g_npages = g_data_len = 0;
    snap_range(GUEST_LO, GUEST_HI);
    pthread_mutex_lock(&pk_lock);
    g_nsnap_ranges = g_nranges;
    memcpy(g_snap_ranges, g_ranges, sizeof g_ranges);
    pthread_mutex_unlock(&pk_lock);
    qsort(g_snap_ranges, (size_t)g_nsnap_ranges, sizeof *g_snap_ranges, cmp_range);
    for (int i = 0; i < g_nsnap_ranges; i++) snap_range(g_snap_ranges[i].lo, g_snap_ranges[i].hi);
    uint8_t *base;
    rt_arena_span(&base, &g_arena_len);
    free(g_arena_copy);
    g_arena_copy = malloc(g_arena_len);
    memcpy(g_arena_copy, base, g_arena_len);
    for (int i = 0; i < g_nstatics; i++) {
        free(g_statics[i].copy);
        g_statics[i].copy = malloc(g_statics[i].n);
        memcpy(g_statics[i].copy, g_statics[i].p, g_statics[i].n);
    }
}

static uint64_t restore_memory(void) {
    g_pages_written = 0;
    uint64_t k = 0, zeroed = restore_range(GUEST_LO, GUEST_HI, &k);
    for (int i = 0; i < g_nsnap_ranges; i++) zeroed += restore_range(g_snap_ranges[i].lo, g_snap_ranges[i].hi, &k);
    uint8_t *base;
    uint64_t now;
    rt_arena_span(&base, &now);
    memcpy(base, g_arena_copy, g_arena_len);
    if (now > g_arena_len) memset(base + g_arena_len, 0, now - g_arena_len);
    for (int i = 0; i < g_nstatics; i++) memcpy(g_statics[i].p, g_statics[i].copy, g_statics[i].n);
    pthread_mutex_lock(&pk_lock);
    g_nranges = g_nsnap_ranges;
    memcpy(g_ranges, g_snap_ranges, sizeof g_ranges);
    pthread_mutex_unlock(&pk_lock);
    return zeroed;
}

// ---- coordination (on the presenting thread, after a present: an HLE call, a safe point itself)
static int park_all(double timeout_s) {
    KThread *th[MAXT];
    uint64_t t0 = plat_now_ns();
    sched_leave(t_cur);
    pthread_mutex_lock(&pk_lock);
    g_ck_park = 1;
    pthread_mutex_unlock(&pk_lock);
    int missing = 0, n = 0;
    for (int round = 0;; round++) {
        if (round % 50 == 0) kernel_wake_all_waiters();  // (blocked threads park in kwait; a wake is a re-check)
        n = kernel_thread_list(th, MAXT);
        pthread_mutex_lock(&pk_lock);
        missing = n - 1 - g_parked;
        pthread_mutex_unlock(&pk_lock);
        if (missing <= 0 || plat_now_ns() - t0 > (uint64_t)(timeout_s * 1e9)) break;
        Sleep(1);
    }
    if (missing > 0) LOG("[checkpoint] %d of %d threads did not reach a safe point in %.0f s\n", missing, n - 1, timeout_s);
    return missing;
}

static void release_all(void) {
    pthread_mutex_lock(&pk_lock);
    g_ck_park = 0;
    pthread_cond_broadcast(&pk_cond);
    pthread_mutex_unlock(&pk_lock);
}

static void take(uint64_t frame) {
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < g_nts; i++) g_ts[i].in_ck = false;
    g_capture = 1;
    if (park_all(10) > 0) { g_capture = 0; release_all(); sched_enter(t_cur); return; }
    pthread_mutex_lock(&pk_lock);
    ThreadSave *self = slot_of(t_cur);
    pthread_mutex_unlock(&pk_lock);
    if (capture(self)) {  // resumed by a rewind
        sched_enter(t_cur);
        LOG("[checkpoint] rewound to frame %llu\n", (unsigned long long)g_frame);
        return;
    }
    g_capture = 0;
    uint64_t t1 = plat_now_ns();
    snapshot_memory();
    save_folder_copy();
    g_have = true;
    g_frame = frame;
    int nthreads = 0;
    for (int i = 0; i < g_nts; i++) nthreads += g_ts[i].in_ck;
    release_all();
    sched_enter(t_cur);
    LOG("[checkpoint] taken at frame %llu: %d threads, %.0f MB of memory (%llu pages), arena %.1f MB; park %.0f ms, "
        "copy %.0f ms\n", (unsigned long long)frame, nthreads, g_data_len / 1048576.0, (unsigned long long)g_npages,
        g_arena_len / 1048576.0, (t1 - t0) / 1e6, (plat_now_ns() - t1) / 1e6);
}

static void rewind_to(void) {
    if (!g_have) { LOG("[checkpoint] rewind: no checkpoint taken\n"); return; }
    uint64_t t0 = plat_now_ns();
    if (park_all(10) > 0) { release_all(); sched_enter(t_cur); return; }
    // every thread of the checkpoint must still exist; threads made since then end
    KThread *th[MAXT];
    int n = kernel_thread_list(th, MAXT), gone = 0, leaving = 0;
    pthread_mutex_lock(&pk_lock);
    for (int i = 0; i < g_nts; i++) {
        if (!g_ts[i].in_ck) continue;
        bool alive = false;
        for (int k = 0; k < n; k++) alive |= th[k] == g_ts[i].t;
        gone += !alive;
    }
    if (gone) {
        pthread_mutex_unlock(&pk_lock);
        LOG("[checkpoint] rewind refused: %d thread(s) of the checkpoint have ended since\n", gone);
        release_all();
        sched_enter(t_cur);
        return;
    }
    for (int k = 0; k < n; k++) {
        ThreadSave *s = slot_of(th[k]);
        if (s && !s->in_ck && th[k] != t_cur) { s->leave = true; leaving++; }
    }
    pthread_cond_broadcast(&pk_cond);
    pthread_mutex_unlock(&pk_lock);
    for (int k = 0; k < n; k++)  // (they end through thread_main, which still touches today's kernel objects)
        for (int i = 0; i < g_nts; i++)
            if (g_ts[i].t == th[k] && !g_ts[i].in_ck && th[k] != t_cur) pthread_join(th[k]->host, NULL);
    extern void renderer_sync(void);
    renderer_sync();  // GPU idle: nothing in flight reads guest memory
    // the renderer drops what it built from guest memory first: its lists point at today's texture objects
    extern void checkpoint_host_reset(void), checkpoint_host_rewound(void);
    checkpoint_host_reset();
    pthread_mutex_lock(&g_klock);
    rt_arena_lock(true);
    uint64_t zeroed = restore_memory();
    rt_arena_lock(false);
    pthread_mutex_unlock(&g_klock);
    checkpoint_host_rewound();  // (texture objects of the checkpoint: no image yet; the save folder)
    LOG("[checkpoint] rewinding to frame %llu: %d thread(s) ended, %llu pages written back (%llu zeroed), %.0f ms\n",
        (unsigned long long)g_frame, leaving, (unsigned long long)g_pages_written, (unsigned long long)zeroed,
        (plat_now_ns() - t0) / 1e6);
    pthread_mutex_lock(&pk_lock);
    g_rewind = 1;
    g_ck_park = 0;
    pthread_cond_broadcast(&pk_cond);
    ThreadSave *self = slot_of(t_cur);
    pthread_mutex_unlock(&pk_lock);
    if (self && self->in_ck) rewind_thread(self);  // (the coordinator was in the checkpoint: back to its point)
    sched_enter(t_cur);  // (a coordinator made after the checkpoint: it can't go back; it runs on)
}

// Around the write-back (GPU idle): before, the renderer drops its caches keyed by guest addresses (programs,
// pipelines, vertex data) and its state, and each texture object's GPU image is noted; after, the checkpoint's texture
// objects get their images again (nvn/objects.c: render targets keep what was drawn last) and the save folder is
// written back. Guest-backed textures upload again by themselves: their pages changed.
void nvn_checkpoint_rewound(void);
extern const char *g_save_dir;
int rt_copy_tree(const char *from, const char *to);
void rt_remove_tree(const char *p);
static char g_save_copy[1100];
void checkpoint_host_reset(void) {  // before the memory goes back (GPU idle)
    extern void vk_exec_reset(void), vk_alias_reset(void), nvn_checkpoint_keep_images(void);
    vk_alias_reset();  // (copy aliases between textures)
    vk_exec_reset();   // the renderer's caches keyed by guest addresses (programs, pipelines, vertex data) and its state
    nvn_checkpoint_keep_images();
}
void checkpoint_host_rewound(void) {  // after
    nvn_checkpoint_rewound();
    if (g_save_copy[0]) {
        rt_remove_tree(g_save_dir);
        rt_copy_tree(g_save_copy, g_save_dir);
    }
}
static void save_folder_copy(void) {  // the save files as of the checkpoint (they may be written after it)
    snprintf(g_save_copy, sizeof g_save_copy, "%s.checkpoint", g_save_dir);
    rt_remove_tree(g_save_copy);
    if (rt_copy_tree(g_save_dir, g_save_copy) < 0) g_save_copy[0] = 0;
}

static volatile int g_key_take, g_key_rewind;
void checkpoint_key(int which) {  // runtime/hle/input.c: F5 = 1, F8 = 2
    if (!rt_arena_active()) return;
    if (which == 1) g_key_take = 1;
    else g_key_rewind = 1;
}

// runtime/nvn/vulkan/device.c, after each present
void checkpoint_frame(uint64_t frame) {
    static int init;
    static uint64_t at;
    static char rewinds[256];
    if (!init) {
        init = 1;
        if (rt_arena_active()) {
            extern void kernel_checkpoint_statics(void), dispatch_checkpoint_statics(void), nvn_checkpoint_statics(void),
                cmdbuf_checkpoint_statics(void);
            kernel_checkpoint_statics();
            dispatch_checkpoint_statics();
            nvn_checkpoint_statics();
            cmdbuf_checkpoint_statics();
            LOG("[checkpoint] on: F5 takes a checkpoint, F8 rewinds to it\n");
        }
        at = getenv("BOTW_RECOMP_CHECKPOINT_AT") ? strtoull(getenv("BOTW_RECOMP_CHECKPOINT_AT"), NULL, 0) : 0;
        snprintf(rewinds, sizeof rewinds, "%s", getenv("BOTW_RECOMP_REWIND_AT") ? getenv("BOTW_RECOMP_REWIND_AT") : "");
    }
    if (!rt_arena_active() || !t_cur) return;
    g_rewind = 0;
    if ((at && frame == at) || g_key_take) { g_key_take = 0; take(frame); return; }
    bool rw = g_key_rewind;
    for (char *p = rewinds; *p && !rw;) {  // (each listed frame once)
        char *end;
        uint64_t f = strtoull(p, &end, 0);
        if (f == frame) { rw = true; *p = '-'; }
        p = *end ? end + 1 : end;
    }
    if (rw) { g_key_rewind = 0; rewind_to(); }
}
#else
void checkpoint_park(bool core) { (void)core; }
void checkpoint_frame(uint64_t frame) { (void)frame; }
void checkpoint_key(int which) { (void)which; }
void rt_checkpoint_static(void *p, size_t n) { (void)p; (void)n; }
void rt_checkpoint_range(uint64_t lo, uint64_t size) { (void)lo; (void)size; }
#endif
