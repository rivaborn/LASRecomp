// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Turbo, for debugging runs: BOTW_RECOMP_TURBO_UNTIL=<frame> [BOTW_RECOMP_TURBO=<factor>|max]. Until that presented
// frame the emulated vsync (hle/vi.c) and the system tick (kernel.c rt_ticks) run <factor> times faster than real
// time (default 4, max = 1000), and timed kernel waits are shortened to match, so a run reaches a late point in
// seconds instead of minutes. From that frame on everything is real time again. Guest time stays continuous: it is
// accumulated, so it never jumps back when the factor changes. BOTW_RECOMP_TURBO_NO_PRESENT=1 also skips showing the
// frames until then (vulkan/device.c), when presenting is what limits the speed.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "rt.h"

uint64_t renderer_present_count(void);

static double factor = 1;
static uint64_t until;
static uint64_t t_start;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void init(void) {
    const char *u = getenv("BOTW_RECOMP_TURBO_UNTIL"), *f = getenv("BOTW_RECOMP_TURBO");
    if (!u || atoll(u) <= 0) return;
    until = (uint64_t)atoll(u);
    factor = !f ? 4 : !strcmp(f, "max") ? 1000 : atof(f);
    if (factor <= 1) { factor = 1; until = 0; return; }
    t_start = plat_now_ns();
    LOG("[turbo] x%g until frame %llu\n", factor, (unsigned long long)until);
}

bool turbo_configured(void) {
    pthread_once(&once, init);
    return until != 0;
}

double turbo_factor(void) {
    if (!turbo_configured()) return 1;
    if (renderer_present_count() < until) return factor;
    static int done;
    if (!__atomic_exchange_n(&done, 1, __ATOMIC_RELAXED))
        LOG("[turbo] frame %llu reached after %.1f s; real time from here\n", (unsigned long long)until,
            (plat_now_ns() - t_start) / 1e9);
    return 1;
}

bool turbo_hide_frames(void) {
    static int on = -1;
    if (on < 0) on = getenv("BOTW_RECOMP_TURBO_NO_PRESENT") != NULL;
    return on && turbo_factor() > 1;
}

// Guest nanoseconds: real time, stretched by the factor while turbo is on. Without turbo it is plat_now_ns() itself.
uint64_t turbo_guest_ns(void) {
    uint64_t now = plat_now_ns();
    if (!turbo_configured()) return now;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_real, last_guest;
    const double f = turbo_factor();
    pthread_mutex_lock(&m);
    if (!last_real) last_real = last_guest = now;
    if (now > last_real) {
        last_guest += (uint64_t)((double)(now - last_real) * f);
        last_real = now;
    }
    const uint64_t g = last_guest;
    pthread_mutex_unlock(&m);
    return g;
}
