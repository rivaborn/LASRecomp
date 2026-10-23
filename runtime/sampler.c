// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Sampling profiler for the guest threads (Windows): BOTW_RECOMP_SAMPLE=<interval ms> [BOTW_RECOMP_SAMPLE_REPORT=<s>].
// Every interval each guest thread is suspended for a moment: its instruction pointer and the top of its stack are
// copied (nothing is allocated meanwhile: the thread may hold the heap lock), then it runs on. Every report period
// (default 10 s) the most frequent places are printed:
//   f_<guest addr>                    inside recompiled game code
//   botw_recomp+<off> / <dll>+<off>     the runtime, the CRT, a driver; with the nearest game function on the stack
//                                     ("<- f_..."). ntdll / KERNELBASE leaves are mostly waits (kernel waits, locks).
// Runtime offsets are named with addr2line -f -e build-<game>/botw_recomp.exe 0x140000000+<off>.
#ifdef _WIN32
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "kernel.h"
#include "rt.h"
#include "gamelib.h"

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);
int kernel_thread_list(KThread **out, int max);

typedef struct {
    uint64_t leaf, game;  // leaf: game function host address or module bucket; game: nearest game frame (host)
    uint32_t count;
} Sample;

#define TAB 65536
static Sample tab[TAB];
static uint64_t game_lo, game_hi, gen_end, total;  // gen_end: end of the generated code (gamelib_tables)

static void count(uint64_t leaf, uint64_t game) {
    uint64_t h = (leaf * 0x9E3779B97F4A7C15ull) ^ (game * 0xC2B2AE3D27D4EB4Full);
    for (uint64_t i = h & (TAB - 1);; i = (i + 1) & (TAB - 1)) {
        if (!tab[i].count) { tab[i].leaf = leaf; tab[i].game = game; }
        if (tab[i].leaf == leaf && tab[i].game == game) { tab[i].count++; break; }
    }
    total++;
}

static void name_of(uint64_t pc, char *out, size_t n) {
    uint64_t off = 0;
    const char *f = pc >= game_lo && pc < gen_end ? rt_host_fn_name(pc, &off) : NULL;
    if (f) { snprintf(out, n, "%s", f); return; }
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)pc, &m) && m) {
        GetModuleFileNameA(m, path, sizeof path);
        const char *b = strrchr(path, '\\');
        char base[64];
        snprintf(base, sizeof base, "%s", b ? b + 1 : path);
        char *dot = strrchr(base, '.');
        if (dot && !_stricmp(dot, ".exe")) *dot = 0;
        snprintf(out, n, "%s+%llx", base, (unsigned long long)(pc - (uint64_t)(uintptr_t)m));
    } else {
        snprintf(out, n, "%llx", (unsigned long long)pc);
    }
}

static int cmp_count(const void *a, const void *b) {
    return (int)((const Sample *)b)->count - (int)((const Sample *)a)->count;
}

static void report(double secs) {
    static Sample s[TAB];
    int n = 0;
    for (int i = 0; i < TAB; i++) if (tab[i].count) s[n++] = tab[i];
    qsort(s, (size_t)n, sizeof *s, cmp_count);
    LOG("[sample] %.0f s, %llu samples; top places (leaf <- nearest game function):\n", secs, (unsigned long long)total);
    for (int i = 0; i < n && i < 25; i++) {
        char a[160], b[64] = "";
        name_of(s[i].leaf, a, sizeof a);
        if (s[i].game && s[i].game != s[i].leaf) {
            char g[64];
            name_of(s[i].game, g, sizeof g);
            snprintf(b, sizeof b, "  <- %s", g);
        }
        LOG("[sample] %5.1f%%  %s%s\n", 100.0 * s[i].count / (double)total, a, b);
    }
    memset(tab, 0, sizeof tab);
    total = 0;
}

static uint64_t leaf_key(uint64_t pc) {  // a game function's host start, or a 64-byte bucket elsewhere
    uint64_t off = 0;
    if (pc >= game_lo && pc < gen_end && rt_host_fn_name(pc, &off)) return pc - off;
    return pc & ~0x3Full;
}

static void *sampler(void *arg) {
    (void)arg;
    const long ms = atol(getenv("BOTW_RECOMP_SAMPLE"));
    const double every = getenv("BOTW_RECOMP_SAMPLE_REPORT") ? atof(getenv("BOTW_RECOMP_SAMPLE_REPORT")) : 10;
    uint64_t t0 = GetTickCount64(), last = t0;
    for (;;) {
        Sleep((DWORD)(ms > 0 ? ms : 1));
        HMODULE g;
        if (!game_lo && (g = GetModuleHandleA(GAMELIB_NAME))) {  // (loaded by the boot)
            IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g;
            IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)g + dos->e_lfanew);
            game_hi = (uint64_t)(uintptr_t)g + nt->OptionalHeader.SizeOfImage;
            game_lo = (uint64_t)(uintptr_t)g;
            FARPROC tables = GetProcAddress(g, GAMELIB_ENTRY);  // (the C runtime is linked in after the generated code)
            gen_end = tables ? (uint64_t)(uintptr_t)tables : game_hi;
        }
        KThread *th[512];
        int n = kernel_thread_list(th, 512);
        for (int i = 0; i < n; i++) {
            HANDLE h = pthread_gethandle(th[i]->host);
            if (!h || SuspendThread(h) == (DWORD)-1) continue;
            CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            uint64_t stack[128];
            int ns = 0;
            if (GetThreadContext(h, &ctx)) {
                MEMORY_BASIC_INFORMATION mi;  // (copy what is readable of the top 1 KB)
                if (VirtualQuery((void *)(uintptr_t)ctx.Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                    uint64_t avail = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize - ctx.Rsp;
                    ns = (int)(avail / 8 < 128 ? avail / 8 : 128);
                    memcpy(stack, (void *)(uintptr_t)ctx.Rsp, (size_t)ns * 8);
                }
            }
            ResumeThread(h);
            if (!ctx.Rip) continue;
            uint64_t leaf = leaf_key(ctx.Rip), game = 0;
            for (int k = 0; k < ns && !game; k++)  // the nearest game function on the stack other than the leaf's
                if (stack[k] >= game_lo && stack[k] < game_hi && leaf_key(stack[k]) != leaf) game = leaf_key(stack[k]);
            count(leaf, game);
        }
        uint64_t now = GetTickCount64();
        if ((now - last) / 1000.0 >= every) {
            report((now - t0) / 1000.0);
            last = now;
        }
    }
    return NULL;
}

void sampler_start(void) {
    const char *e = getenv("BOTW_RECOMP_SAMPLE");
    if (!e || atol(e) <= 0) return;
    pthread_t t;
    pthread_create(&t, NULL, sampler, NULL);
    LOG("[sample] every %ld ms\n", atol(e));
}
#else
void sampler_start(void) {}
#endif
