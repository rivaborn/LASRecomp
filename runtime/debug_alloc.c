// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Guard-page allocator for host memory errors (build with GUARD_ALLOC=1; Windows). Every malloc / calloc / realloc
// / free of the runtime's own code (linked with --wrap) gets its own pages: the block ends right at an inaccessible
// page, so writing past it faults at the guilty instruction (the crash report names it), and a freed block's pages
// become inaccessible, so a later use faults too. Allocations made elsewhere (the CRT's strdup, SDL, ...) are told
// apart by a header and go to the real free / realloc.
#ifdef GUARD_ALLOC
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define PAGE 4096ull
#define MAGIC 0x6775617264616c6cull  // "guardall"

// The CRT's own functions, from ucrtbase.dll directly (lld's --wrap sends __real_* back to the wrappers with MinGW's
// import stubs).
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void (*real_free)(void *);
static size_t guard_min;  // BOTW_RECOMP_GUARD_MIN: smaller blocks go to the CRT (default 1024: few, large buffers)

static void real_init(void) {
    if (real_free) return;
    HMODULE m = GetModuleHandleA("ucrtbase.dll");
    if (!m) m = LoadLibraryA("ucrtbase.dll");
    real_malloc = (void *(*)(size_t))(void *)GetProcAddress(m, "malloc");
    real_calloc = (void *(*)(size_t, size_t))(void *)GetProcAddress(m, "calloc");
    real_realloc = (void *(*)(void *, size_t))(void *)GetProcAddress(m, "realloc");
    char e[32];
    guard_min = GetEnvironmentVariableA("BOTW_RECOMP_GUARD_MIN", e, sizeof e) ? (size_t)strtoull(e, NULL, 0) : 1024;
    real_free = (void (*)(void *))(void *)GetProcAddress(m, "free");
}

typedef struct { uint64_t magic, size, base, pages; } Hdr;  // just before the block (on the first page)

static Hdr *hdr_of(void *p) {
    if (!p || ((uintptr_t)p & (PAGE - 1)) < sizeof(Hdr)) return NULL;  // ours never start within a page's first bytes
    Hdr *h = (Hdr *)p - 1;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(h, &mi, sizeof mi) || mi.State != MEM_COMMIT || !(mi.Protect & PAGE_READWRITE)) return NULL;
    return h->magic == MAGIC ? h : NULL;
}

static void *guarded(size_t n);

void *__wrap_malloc(size_t n) {
    real_init();
    return n < guard_min ? real_malloc(n) : guarded(n);
}

static void *guarded(size_t n) {
    // [header + block, ending at a page boundary][guard page]; 16-byte alignment kept by rounding the size
    const uint64_t size = (n + 15) & ~15ull;
    const uint64_t data_pages = (size + sizeof(Hdr) + PAGE - 1) / PAGE;
    uint8_t *base = VirtualAlloc(NULL, (data_pages + 1) * PAGE, MEM_RESERVE, PAGE_NOACCESS);
    if (!base || !VirtualAlloc(base, data_pages * PAGE, MEM_COMMIT, PAGE_READWRITE)) return NULL;
    uint8_t *p = base + data_pages * PAGE - size;
    Hdr *h = (Hdr *)p - 1;
    h->magic = MAGIC;
    h->size = n;
    h->base = (uint64_t)(uintptr_t)base;
    h->pages = data_pages + 1;
    return p;
}

void *__wrap_calloc(size_t n, size_t m) {
    real_init();
    return n * m < guard_min ? real_calloc(n, m) : guarded(n * m);  // (VirtualAlloc'd pages are zeroed)
}

void __wrap_free(void *p) {
    if (!p) return;
    Hdr *h = hdr_of(p);
    if (!h) { real_init(); real_free(p); return; }
    // keep the address space reserved and make it inaccessible: a use after free faults
    const uint64_t base = h->base, pages = h->pages;
    VirtualFree((void *)(uintptr_t)base, (pages - 1) * PAGE, MEM_DECOMMIT);
}

void *__wrap_realloc(void *p, size_t n) {
    if (!p) return __wrap_malloc(n);
    Hdr *h = hdr_of(p);
    if (!h) { real_init(); return real_realloc(p, n); }
    void *q = __wrap_malloc(n);
    if (q) memcpy(q, p, h->size < n ? h->size : n);
    __wrap_free(p);
    return q;
}
#endif

// Heap checkpoints (BOTW_RECOMP_HEAPCHECK=1, Windows): rt_heap_check(where) validates the CRT heap; the first failing
// checkpoint is reported with the last one that passed, which brackets the code that corrupted it.
#ifdef _WIN32
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
void rt_fatal_trace(void) __attribute__((noreturn));
void rt_heap_check(const char *where) {
    static int on = -1;
    static const char *volatile last_ok = "start";
    if (on < 0) on = getenv("BOTW_RECOMP_HEAPCHECK") != NULL;
    if (!on) return;
    if (HeapValidate((HANDLE)_get_heap_handle(), 0, NULL)) {
        last_ok = where;
        return;
    }
    fprintf(stderr, "FATAL: CRT heap corrupt at checkpoint \"%s\" (last good: \"%s\")\n", where, last_ok);
    rt_fatal_trace();
}
#else
void rt_heap_check(const char *where) { (void)where; }
#endif
