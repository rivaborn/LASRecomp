// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

// The modules compiled in: `make gen` writes gamelib_modules.h (GAMELIB_MODULES(X), one X(name) per module) into the
// generated folder; without it, Link's Awakening's two.
#if __has_include("gamelib_modules.h")
#include "gamelib_modules.h"
#else
#define GAMELIB_MODULES(X) X(main) X(sdk)
#endif

#define DECLARE(n) extern const uint64_t n##_fn_addrs[], n##_fn_count; extern const GuestFn n##_fn_ptrs[];
GAMELIB_MODULES(DECLARE)
#define ENTRY(n) {#n, n##_fn_addrs, n##_fn_ptrs, &n##_fn_count},
static const GameLibModule mods[] = {GAMELIB_MODULES(ENTRY)};

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, sizeof mods / sizeof mods[0], mods};
    return &t;
}
