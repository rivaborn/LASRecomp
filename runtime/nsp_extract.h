// Game files from the player's own .nsp: ExeFS + RomFS image (the update applied over the base), via hactool.
// runtime/nsp_extract.c has the steps; the start screen's Game Files page runs it (games other than Link's Awakening).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *nsp;       // the package
    const char *keys_dir;  // holds prod.keys (read by hactool only)
    const char *hactool;   // hactool executable
    const char *out_dir;   // gets exefs/, romfs.bin, .source; <out_dir>.work is the temporary folder
} NspExtractOpts;

// frac 0..1; step: a new step's description, or NULL (same step, progress only). Called on the extracting thread.
typedef void (*NspProgress)(double frac, const char *step);

// false: why says what went wrong, in words for the player (never key material).
bool nsp_extract(const NspExtractOpts *o, NspProgress progress, char *why, size_t why_size);
// The NSO build id (32 bytes, as 64 hex digits) of an ExeFS main.
bool nsp_main_build_id(const char *exefs_main, char out[65]);
// The output in out_dir was extracted from this .nsp, unchanged since (path, size, modification time).
bool nsp_source_matches(const char *out_dir, const char *nsp);

#ifdef __cplusplus
}
#endif
