// Based on LAS Recomp by Peiikai (MIT); modified for BotW Recomp.
// Fuzz test for the Maxwell shader translator (runtime/nvn/maxwell/translate.c).
//
//  1. MSL must not change: every program is translated by the current translate.c (key.glsl = 0) and by the
//     reference MSL-only translator (an older translate.c compiled with old_* symbol names, see
//     tools/test_translate.sh); the MSL text and all ShaderInfo fields must be identical.
//  2. GLSL must compile: the same program with key.glsl = 1 is compiled in-process with shaderc (Vulkan 1.3) and
//     the SPIR-V validated with SPIRV-Tools.
//
// Programs are synthesized from the opcode patterns translate.c recognizes (every OPM(value, mask) and the
// half-precision `(op & mask) == value` tests, read from the source file): the unmasked bits are random, with
// register / predicate / cbuf / texture / attribute / branch fields biased towards meaningful values.
//
// usage: test_translate <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/nvn/maxwell/shader.h"

#ifdef WITH_GLSL
#include <shaderc/shaderc.h>
#include <spirv-tools/libspirv.h>
#endif

// the reference translator (same ShaderKey prefix and ShaderInfo layout)
ShaderInfo *old_maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
void old_maxwell_free(ShaderInfo *s);
extern uint64_t old_maxwell_gmem_base, old_maxwell_gmem_size;

static uint64_t rng_state, g_seed;
static uint64_t rnd64(void) {  // splitmix64
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
static uint32_t rnd(uint32_t n) { return (uint32_t)(rnd64() % n); }

typedef struct { uint64_t val, mask; } Pat;
static Pat pats[512];
static int npats;

static void load_patterns(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    fread(s, 1, n, f);
    s[n] = 0;
    fclose(f);
    for (char *p = s; (p = strstr(p, "OPM(0x")); p += 4) {
        unsigned long long v, m;
        if (sscanf(p, "OPM(0x%llxull, 0x%llxull)", &v, &m) == 2 && npats < 512) {
            bool dup = false;
            for (int i = 0; i < npats; i++) dup |= pats[i].val == v && pats[i].mask == m;
            if (!dup) pats[npats++] = (Pat){v, m};
        }
    }
    for (char *p = s; (p = strstr(p, "(op & 0x")); p += 4) {  // emit_half: 16-bit opcode tests
        unsigned m, v;
        if (sscanf(p, "(op & 0x%x) == 0x%x", &m, &v) == 2 && npats < 512) pats[npats++] = (Pat){(uint64_t)v << 48, (uint64_t)m << 48};
    }
    free(s);
    if (npats < 50) { fprintf(stderr, "only %d patterns found in %s\n", npats, path); exit(1); }
}

static uint64_t setf(uint64_t w, int lo, int n, uint64_t v) {
    uint64_t m = ((1ull << n) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}
static uint64_t getf(uint64_t w, int lo, int n) { return (w >> lo) & ((1ull << n) - 1); }
static bool is(uint64_t w, uint64_t v, uint64_t m) { return (w & m) == v; }

static uint32_t rreg(void) { uint32_t x = rnd(100); return x < 8 ? 255 : x < 12 ? 250 + rnd(5) : rnd(24); }

static const uint32_t attr_addrs[] = {0x70, 0x74, 0x78, 0x7c, 0x80, 0x84, 0x88, 0x8c, 0x90, 0x94, 0x98, 0x9c, 0xa0,
                                      0xb0, 0xc0, 0x100, 0x270, 0x27c, 0x2f8, 0x2fc, 0x3fc, 0x6c, 0x2e0, 0x2e4, 0x40};

// one random instruction at byte offset pc of a program of `total` bytes
static uint64_t gen_insn(uint32_t pc, uint32_t total) {
    if (rnd(400) == 0) {  // unknown instruction (but no cbuf index >= 17: the translators index arrays of 18)
        uint64_t w = rnd64();
        w = setf(w, 34, 5, rnd(17));
        return setf(w, 36, 5, rnd(17)) & ~(1ull << 38);
    }
    const Pat *p = &pats[rnd(npats)];
    uint64_t w = rnd64();
    if (rnd(100) < 90) w = setf(w, 0, 8, rreg());
    if (rnd(100) < 90) w = setf(w, 8, 8, rreg());
    if (rnd(100) < 70) w = setf(w, 20, 8, rreg());
    if (rnd(100) < 80) w = setf(w, 39, 8, rreg());
    w = setf(w, 16, 4, rnd(100) < 70 ? 7 : rnd(16));  // predicate guard (mostly PT)
    w = (w & ~p->mask) | p->val;
    // cbuf index fields: hardware has 18 buffers; 17 clashes with the gmem binding (both translators reject it)
    if (is(w, 0xef90000000000000ull, 0xfff8000000000000ull)) w = setf(w, 36, 5, rnd(17));  // LDC
    else if (getf(w, 34, 5) >= 17) w = setf(w, 34, 5, rnd(17));
    // texture index: few textures so that slots get reused
    if (is(w, 0xd000000000000000ull, 0xf400000000000000ull) || is(w, 0xc038000000000000ull, 0xfc38000000000000ull) ||
        is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) || is(w, 0xdeb8000000000000ull, 0xfff8000000000000ull))
        w = setf(w, 36, 13, rnd(100) < 90 ? rnd(4) : rnd(8192));
    if (is(w, 0xdf48000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) w = setf(w, 22, 6, 1);  // TXQ dimension
    // attribute addresses (LD / ST a[], IPA)
    uint32_t aa = attr_addrs[rnd(sizeof attr_addrs / sizeof attr_addrs[0])] + (rnd(100) < 30 ? 4 * rnd(4) : 0);
    if (is(w, 0xefd8000000000000ull, 0xfff8000000000000ull) || is(w, 0xeff0000000000000ull, 0xfff8000000000000ull)) {
        w = setf(w, 20, 10, aa);
        if (rnd(100) < 85) w = setf(w, 8, 8, 255);  // not indexed
    }
    if (is(w, 0xe000000000000000ull, 0xff00000000000000ull)) {
        w = setf(w, 28, 10, aa);
        if (rnd(100) < 90) w = setf(w, 38, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    if (is(w, 0xf0c8000000000000ull, 0xfff8000000000000ull)) {  // S2R
        static const uint32_t srs[] = {0x00, 0x12, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27, 0x50};
        w = setf(w, 20, 8, srs[rnd(sizeof srs / sizeof srs[0])]);
    }
    if (is(w, 0x5080000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) {  // MUFU: valid functions
        static const uint32_t fns[] = {0, 1, 2, 3, 4, 5, 8};
        w = setf(w, 20, 4, fns[rnd(7)]);
    }
    if (is(w, 0x5bc0000000000000ull, 0xffe0000000000000ull) || is(w, 0x4bc0000000000000ull, 0xffe0000000000000ull) ||
        is(w, 0x36c0000000000000ull, 0xfee0000000000000ull) || is(w, 0x53c0000000000000ull, 0xffe0000000000000ull))
        if (rnd(100) < 90) w = setf(w, 48, 4, 0);  // PRMT default mode
    // P2R / R2P .CC and BFE .BREV are unsupported: mostly off
    static const uint64_t bit40_ops[] = {0x38e8, 0x5ce8, 0x4ce8, 0x38f0, 0x5cf0, 0x4cf0, 0x5c00, 0x4c00, 0x3800};
    for (int i = 0; i < 9; i++)
        if ((w >> 48 & (bit40_ops[i] & 0xff00 ? (bit40_ops[i] >> 8 == 0x38 ? 0xfef8 : 0xfff8) : 0)) == bit40_ops[i] && rnd(100) < 90)
            w = setf(w, 40, 1, 0);
    if (is(w, 0x5098000000000000ull, 0xfff8000000000000ull) && rnd(100) < 90) return gen_insn(pc, total);  // CSET fails
    // branch targets inside the program
    uint64_t top12 = w & 0xfff0000000000000ull;
    if (top12 == 0xe240000000000000ull || top12 == 0xe290000000000000ull || top12 == 0xe2a0000000000000ull ||
        top12 == 0xe2b0000000000000ull || top12 == 0xe250000000000000ull) {
        if (rnd(100) < 90) {
            int64_t target = (int64_t)rnd(total / 8) * 8;
            int64_t off = target - (int64_t)pc - 8;
            if (off == -8) off = 8;  // not a self-branch (that ends the program)
            w = setf(w, 20, 24, (uint64_t)off & 0xFFFFFF);
        }
        w = setf(w, 5, 1, 0);
        w = (w & ~p->mask) | p->val;
    }
    return w;
}

static size_t gen_program(uint8_t *buf, size_t cap, int stage) {
    memset(buf, 0, cap);
    size_t hdr = stage == STAGE_COMPUTE ? 0 : 0x80;
    if (stage == STAGE_FRAGMENT) {
        uint32_t omap = rnd(100) < 40 ? 0xF : rnd(100) < 50 ? (uint32_t)rnd64() : (uint32_t)rnd64() & 0xFF;
        memcpy(buf + 0x30 + 0x48, &omap, 4);
        uint32_t dw = rnd(4) == 0 ? 2 : 0;
        memcpy(buf + 0x30 + 0x4C, &dw, 4);
    }
    int groups = 1 + rnd(rnd(100) < 70 ? 3 : 10);
    uint32_t total = (uint32_t)(groups + 1) * 32;
    uint64_t *code = (uint64_t *)(buf + hdr);
    int i = 0;
    for (int g = 0; g < groups; g++) {
        code[i++] = rnd64();  // scheduling word
        for (int k = 0; k < 3; k++, i++) code[i] = gen_insn((uint32_t)i * 8, total);
    }
    code[i++] = 0x001f8000fc0007e0ull;
    code[i++] = 0xe30000000007000full;  // EXIT
    code[i++] = 0xe2400fffff87000full;  // BRA self (end of program)
    code[i++] = 0xe2400fffff87000full;
    return hdr + (size_t)i * 8 + 64;
}

static void gen_key(ShaderKey *k, int stage) {
    memset(k, 0, sizeof *k);
    for (int i = 0; i < 32; i++) k->attr_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    for (int i = 0; i < 8; i++) k->rt_kind[i] = rnd(100) < 80 ? 0 : rnd(3);
    k->depth_minus_one = rnd(2);
    k->flip_y = rnd(2);
    if (stage == STAGE_FRAGMENT && rnd(2)) k->vs_out_mask = (uint32_t)rnd64();
}

static bool same_info(const ShaderInfo *a, const ShaderInfo *b) {
    if ((a->msl == NULL) != (b->msl == NULL)) return false;
    if (a->msl && strcmp(a->msl, b->msl)) return false;
    ShaderInfo x = *a, y = *b;
    x.msl = y.msl = NULL;
    return !memcmp(&x, &y, sizeof x);
}

static void dump(const char *dir, const char *tag, long n, const char *src, const char *err, const uint8_t *prog, size_t len) {
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s_%llu_%ld.txt", dir, tag, (unsigned long long)g_seed, n);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "// %s\n", err ? err : "");
    if (prog) {
        fprintf(f, "// program:");
        for (size_t i = 0; i < len; i += 8) fprintf(f, " %016llx", (unsigned long long)*(const uint64_t *)(prog + i));
        fprintf(f, "\n");
    }
    fputs(src ? src : "(null)", f);
    fclose(f);
}

#ifdef WITH_GLSL
// Directed cases: instruction words from Breath of the Wild's shaders (TLD4, TLD4S, TMML.B, STS) and the other
// forms of the same decoders, translated to GLSL, compiled and validated; `expect` is whether the translation
// should succeed. usage: test_translate cases [-v]
#define MOVC(d, idx, off) (0x4c98000000000000ull | (uint64_t)(idx) << 34 | (uint64_t)((off) / 4) << 20 | 0xfull << 39 | 7ull << 16 | (d))
#define LDC32(d, idx, off) (0xef94000000000000ull | (uint64_t)(idx) << 36 | (uint64_t)(off) << 20 | 0xffull << 8 | 7ull << 16 | (d))
#define LDS32(d, a, off) (0xef4c000000000000ull | (uint64_t)(off) << 20 | (uint64_t)(a) << 8 | 7ull << 16 | (d))
#define ATOMS(op, sz, d, a, b, woff) (0xec00000000000000ull | (uint64_t)(op) << 52 | (uint64_t)(woff) << 30 | (uint64_t)(sz) << 28 | \
                                      (uint64_t)(b) << 20 | (uint64_t)(a) << 8 | 7ull << 16 | (d))
#define S2R_TIDX(d) (0xf0c8000000000000ull | 0x21ull << 20 | 0xffull << 8 | 7ull << 16 | (d))
#define SHL_IMM(d, a, n) (0x3848000000000000ull | (uint64_t)(n) << 20 | (uint64_t)(a) << 8 | 7ull << 16 | (d))
#define BAR_SYNC 0xf0a8000000000000ull | 0x8000000000ull | 7ull << 16
#define SET(w, lo, n, v) (((w) & ~(((1ull << (n)) - 1) << (lo))) | ((uint64_t)(v) << (lo)))
#define TLD4_W 0xc83801e6aff70c00ull
#define IADD_RR(d, a, b) (0x5c10000000000000ull | (uint64_t)(b) << 20 | (uint64_t)(a) << 8 | 7ull << 16 | (d))
typedef struct { const char *name; int stage; bool expect; uint64_t insn[9]; } Case;
static const Case cases[] = {
    {"TLD4 2D, mask rba", STAGE_FRAGMENT, true, {0xc83801e6aff70c00ull}},
    {"TLD4 2D array", STAGE_FRAGMENT, true, {0xc83a0087bff70400ull}},
    {"TLD4 2D, dest r28", STAGE_FRAGMENT, true, {0xc8380146aff70a1cull}},
    {"TLD4 2D, component G, AOFFI", STAGE_FRAGMENT, true, {SET(SET(SET(TLD4_W, 56, 2, 1), 54, 2, 1), 20, 8, 4)}},
    {"TLD4 2D, DC + AOFFI (shadow)", STAGE_FRAGMENT, true, {SET(SET(SET(TLD4_W, 54, 2, 1), 50, 1, 1), 20, 8, 4)}},
    {"TLD4 cube", STAGE_FRAGMENT, true, {SET(TLD4_W, 28, 3, 6)}},
    {"TLD4 compute (base level)", STAGE_COMPUTE, true, {TLD4_W}},
    {"TLD4.B 2D array, handle c[2][0x40], AOFFI", STAGE_FRAGMENT, true, {MOVC(4, 2, 0x40), SET(SET(SET(0xdec0000000000000ull | 0xfull << 31 | 7ull << 16, 28, 3, 3), 36, 2, 1), 20, 8, 4) | 8ull << 8}},
    {"TLD4 PTP (unsupported)", STAGE_FRAGMENT, false, {SET(SET(TLD4_W, 54, 2, 2), 20, 8, 4)}},
    {"TLD4 3D (no gather)", STAGE_FRAGMENT, false, {SET(TLD4_W, 28, 3, 4)}},
    {"TLD4S F32", STAGE_FRAGMENT, true, {0xdf0000c020470500ull}},
    {"TLD4S F32 b", STAGE_FRAGMENT, true, {0xdf00024000870d02ull}},
    {"TLD4S F32 c", STAGE_FRAGMENT, true, {0xdf00024020670b00ull}},
    {"TLD4S F16, component A, AOFFI + DC", STAGE_FRAGMENT, true, {SET(SET(0xdf0000c020470500ull | 1ull << 55 | 3ull << 52 | 1ull << 51 | 1ull << 50, 8, 8, 4), 20, 8, 6)}},
    {"TLD4S DC", STAGE_FRAGMENT, true, {0xdf0000c020470500ull | 1ull << 50}},
    {"TMML.B, handle MOV c[2][0x30] (bound)", STAGE_FRAGMENT, true, {MOVC(2, 2, 0x30), 0xdf60000120270402ull}},
    {"TMML.B, handle LDC c[2][0x38] (bound)", STAGE_FRAGMENT, true, {LDC32(3, 2, 0x38), 0xdf60000120370403ull}},
    {"TMML.B, handle LDC c[3][0x40] (uniform buffer)", STAGE_FRAGMENT, true, {LDC32(7, 3, 0x40), 0xdf60000120770407ull}},
    {"TMML.B, handle c[1] (compiler consts)", STAGE_FRAGMENT, true, {MOVC(11, 1, 0x8), 0xdf60000120b7080bull}},
    {"TMML.B, handle across a later write", STAGE_FRAGMENT, true, {MOVC(2, 2, 0x30), MOVC(3, 2, 0x38), 0xdf60000120270402ull}},
    {"TMML.B, handle computed (unsupported)", STAGE_FRAGMENT, false, {IADD_RR(2, 4, 5), 0xdf60000120270402ull}},
    {"TMML.B, handle from c[0] (unsupported)", STAGE_FRAGMENT, false, {MOVC(2, 0, 0x30), 0xdf60000120270402ull}},
    {"TMML.B, handle never set (unsupported)", STAGE_FRAGMENT, false, {0xdf60000120270402ull}},
    {"TMML 2D, x and y", STAGE_FRAGMENT, true, {(0xdf58000000000000ull | 3ull << 31 | 2ull << 28 | 4ull << 8 | 5ull << 36 | 7ull << 16 | 2)}},
    {"TMML cube array", STAGE_FRAGMENT, true, {(0xdf58000000000000ull | 2ull << 31 | 7ull << 28 | 4ull << 8 | 6ull << 36 | 7ull << 16 | 2)}},
    {"TMML BA (unsupported)", STAGE_FRAGMENT, false, {(0xdf58000000000000ull | 4ull << 31 | 2ull << 28 | 4ull << 8 | 5ull << 36 | 7ull << 16 | 2)}},
    {"TMML.B then TLD4 on the same texture", STAGE_FRAGMENT, true, {MOVC(2, 2, 0x78), 0xdf60000120270402ull, 0xc83801e6aff70c00ull}},
    {"TEX.B, handle c[3][0x10]", STAGE_FRAGMENT, true, {LDC32(6, 3, 0x10), 0xdeb8000000670400ull | 2ull << 28 | 0xfull << 31 | 7ull << 16}},
    {"STS (BotW) + BAR + LDS", STAGE_COMPUTE, true, {S2R_TIDX(1), SHL_IMM(1, 1, 2), 0xef5c000000870102ull, 0xef5c000000070108ull, BAR_SYNC, LDS32(4, 1, 8), LDS32(5, 0xff, 0x40)}},
    {"STS.U8 / LDS.S16 / STS.128", STAGE_COMPUTE, true, {0xef58000000070102ull, (0xef49000000070104ull & ~(7ull << 48)) | 3ull << 48, (0xef58000000870108ull & ~(7ull << 48)) | 6ull << 48}},
    {"ATOMS add / min.s32 / inc / exch", STAGE_COMPUTE, true, {ATOMS(0, 0, 2, 1, 3, 4), ATOMS(1, 1, 4, 0xff, 3, 8), ATOMS(3, 0, 5, 1, 3, 1), ATOMS(8, 0, 6, 1, 3, 0)}},
    {"ATOMS 64-bit (unsupported)", STAGE_COMPUTE, false, {ATOMS(8, 2, 6, 1, 3, 0)}},
    {"STS in a fragment shader (unsupported)", STAGE_FRAGMENT, false, {0xef5c000000870102ull}},
};

static size_t case_program(uint8_t *buf, size_t cap, const Case *k) {
    memset(buf, 0, cap);
    size_t hdr = k->stage == STAGE_COMPUTE ? 0 : 0x80;
    if (k->stage == STAGE_FRAGMENT) { uint32_t omap = 0xF; memcpy(buf + 0x30 + 0x48, &omap, 4); }
    uint64_t *code = (uint64_t *)(buf + hdr);
    int i = 0, n = 0;
    while (n < 9 && k->insn[n]) n++;
    for (int j = 0; j < n; j++) {
        if (i % 4 == 0) code[i++] = 0x001f8000fc0007e0ull;  // scheduling word
        code[i++] = k->insn[j];
    }
    while (i % 4 != 0 || i == 0) { if (i % 4 == 0) code[i++] = 0x001f8000fc0007e0ull; else code[i++] = 0x50b0000000070f00ull; }  // NOP
    code[i++] = 0x001f8000fc0007e0ull;
    code[i++] = 0xe30000000007000full;  // EXIT
    code[i++] = 0xe2400fffff87000full;  // BRA self
    code[i++] = 0xe2400fffff87000full;
    return hdr + (size_t)i * 8 + 64;
}

static int run_cases(bool verbose) {
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    spv_context spv = spvContextCreate(SPV_ENV_VULKAN_1_3);
    static uint8_t prog[0x1000];
    int bad = 0;
    for (size_t ci = 0; ci < sizeof cases / sizeof cases[0]; ci++) {
        const Case *k = &cases[ci];
        size_t len = case_program(prog, sizeof prog, k);
        ShaderKey key = {0};
        key.glsl = 1;
        if (k->stage == STAGE_FRAGMENT) key.vs_out_mask = ~0u;
        ShaderInfo *g = maxwell_translate(prog, len, k->stage, &key);
        const char *status;
        char err[512] = "";
        if (!g->msl) {
            status = k->expect ? "FAIL (translation)" : "ok (fails as expected)";
            snprintf(err, sizeof err, "%s", g->error);
        } else {
            shaderc_shader_kind kind = k->stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
            shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), kind, "case", "main", opt);
            if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                status = "FAIL (glsl compile)";
                snprintf(err, sizeof err, "%s", shaderc_result_get_error_message(res));
            } else {
                spv_diagnostic diag = NULL;
                spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                spv_validator_options vo = spvValidatorOptionsCreate();
                bool valid = spvValidateWithOptions(spv, vo, &bin, &diag) == SPV_SUCCESS;
                spvValidatorOptionsDestroy(vo);
                if (!valid) { status = "FAIL (spir-v validation)"; snprintf(err, sizeof err, "%s", diag ? diag->error : "?"); }
                else status = k->expect ? "ok" : "FAIL (translated, expected a failure)";
                spvDiagnosticDestroy(diag);
            }
            shaderc_result_release(res);
        }
        if (strncmp(status, "ok", 2)) bad++;
        printf("%-48s %s%s%s\n", k->name, status, err[0] ? ": " : "", err);
        for (int t = 0; g->msl && t < g->ntex; t++)
            if (verbose || g->tex[t].bindless)
                printf("    tex %d: type %d shadow %d array %d %s c[%u][%#x] handle_offset %#x\n", t, g->tex[t].type, g->tex[t].shadow,
                       g->tex[t].is_array, g->tex[t].bindless ? "bindless" : "bound", g->tex[t].bindless_cbuf, g->tex[t].bindless_offset,
                       g->tex[t].handle_offset);
        if (g->msl && verbose) {  // the body lines of interest
            for (const char *p = g->msl; *p;) {
                const char *e = strchr(p, '\n');
                if (!e) e = p + strlen(p);
                static const char *const keys[] = {"textureGather", "lq", "smem", "barrier", "uniform sampler", "texture(", "atomic"};
                bool show = false;
                for (int q = 0; q < 7; q++) { const char *h = strstr(p, keys[q]); show |= h && h < e; }
                if (show)
                    printf("      %.*s\n", (int)(e - p), p);
                p = *e ? e + 1 : e;
            }
        }
        maxwell_free(g);
    }
    printf("%d case(s) wrong\n", bad);
    return bad ? 1 : 0;
}
#endif

int main(int argc, char **argv) {
#ifdef WITH_GLSL
    if (argc >= 2 && !strcmp(argv[1], "cases")) return run_cases(argc >= 3 && !strcmp(argv[2], "-v"));
#endif
    if (argc < 4) { fprintf(stderr, "usage: %s <translate.c> <count> <seed> [msl|glsl|both] [dump_dir]\n", argv[0]); return 2; }
    load_patterns(argv[1]);
    long count = atol(argv[2]);
    rng_state = g_seed = strtoull(argv[3], NULL, 0);
    const char *mode = argc > 4 ? argv[4] : "both";
    const char *dir = argc > 5 ? argv[5] : NULL;
    bool do_msl = strcmp(mode, "glsl") != 0, do_glsl = strcmp(mode, "msl") != 0;
    maxwell_gmem_base = old_maxwell_gmem_base = 0x80000000ull;
    maxwell_gmem_size = old_maxwell_gmem_size = 0x10000000ull;
#ifdef WITH_GLSL
    shaderc_compiler_t comp = shaderc_compiler_initialize();
    shaderc_compile_options_t opt = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opt, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_source_language(opt, shaderc_source_language_glsl);
    spv_context spv = spvContextCreate(SPV_ENV_VULKAN_1_3);
#else
    if (do_glsl) { fprintf(stderr, "built without WITH_GLSL: msl mode only\n"); do_glsl = false; }
#endif
    static uint8_t prog[0x80 + 64 * 32 + 128];
    long msl_ok = 0, msl_fail = 0, msl_diff = 0, gl_ok = 0, gl_tfail = 0, gl_tfail_only = 0, gl_cfail = 0, gl_vfail = 0;
    long by_stage[3] = {0};
    char reasons[64][96];
    long reason_n[64];
    int nreasons = 0;
    for (long n = 0; n < count; n++) {
        int stage = rnd(3) == 0 ? STAGE_VERTEX : rnd(2) ? STAGE_FRAGMENT : STAGE_COMPUTE;
        size_t len = gen_program(prog, sizeof prog, stage);
        ShaderKey key;
        gen_key(&key, stage);
        bool msl_good = true;
        if (do_msl) {
            ShaderInfo *a = maxwell_translate(prog, len, stage, &key);
            ShaderInfo *b = old_maxwell_translate(prog, len, stage, &key);
            if (!same_info(a, b)) {
                if (msl_diff < 20) dump(dir, "msl_diff", n, a->msl, b->msl, prog, len);
                msl_diff++;
            }
            if (a->msl) msl_ok++; else msl_fail++;
            msl_good = a->msl != NULL;
            maxwell_free(a);
            old_maxwell_free(b);
        }
#ifdef WITH_GLSL
        if (do_glsl) {
            key.glsl = 1;
            ShaderInfo *g = maxwell_translate(prog, len, stage, &key);
            if (!g->msl) {
                gl_tfail++;
                if (msl_good && do_msl) gl_tfail_only++;
                {  // tally the reasons (GLSL-only: the MSL translation succeeded)
                    char r[96];
                    snprintf(r, sizeof r, "%s%.24s", msl_good && do_msl ? "GLSL-only " : "", g->error);
                    for (char *q = r; *q; q++) if (*q >= '0' && *q <= '9') *q = '#';
                    int i = 0;
                    while (i < nreasons && strcmp(reasons[i], r)) i++;
                    if (i == nreasons && nreasons < 64) { strcpy(reasons[nreasons], r); reason_n[nreasons++] = 0; }
                    if (i < 64) reason_n[i]++;
                }
            } else {
                shaderc_shader_kind kind = stage == STAGE_VERTEX ? shaderc_vertex_shader
                                         : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
                shaderc_compilation_result_t res = shaderc_compile_into_spv(comp, g->msl, strlen(g->msl), kind, "fuzz", "main", opt);
                if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
                    if (gl_cfail < 50) dump(dir, "glsl_compile", n, g->msl, shaderc_result_get_error_message(res), prog, len);
                    gl_cfail++;
                } else {
                    spv_diagnostic diag = NULL;
                    spv_const_binary_t bin = {(const uint32_t *)shaderc_result_get_bytes(res), shaderc_result_get_length(res) / 4};
                    spv_validator_options vo = spvValidatorOptionsCreate();
                    spv_result_t vr = spvValidateWithOptions(spv, vo, &bin, &diag);
                    spvValidatorOptionsDestroy(vo);
                    if (vr != SPV_SUCCESS) {
                        if (gl_vfail < 50) dump(dir, "glsl_validate", n, g->msl, diag ? diag->error : "?", prog, len);
                        gl_vfail++;
                    } else {
                        gl_ok++;
                        by_stage[stage == STAGE_VERTEX ? 0 : stage == STAGE_FRAGMENT ? 1 : 2]++;
                        if (dir && gl_ok <= (getenv("TT_DUMP_OK") ? atol(getenv("TT_DUMP_OK")) : 40)) dump(dir, "glsl_ok", n, g->msl, NULL, prog, len);
                    }
                    spvDiagnosticDestroy(diag);
                }
                shaderc_result_release(res);
            }
            maxwell_free(g);
        }
#endif
    }
    if (do_msl) printf("MSL: %ld programs, %ld translated, %ld failed in both, %ld DIFFERENCES\n", count, msl_ok, msl_fail, msl_diff);
    if (do_glsl) {
        printf("GLSL: %ld compiled+validated (vs %ld / fs %ld / cs %ld), %ld compile failures, %ld validation failures, "
               "%ld translation failures (%ld where MSL translated)\n",
               gl_ok, by_stage[0], by_stage[1], by_stage[2], gl_cfail, gl_vfail, gl_tfail, gl_tfail_only);
        for (int i = 0; i < nreasons; i++) printf("  translation failure: %-36s %ld\n", reasons[i], reason_n[i]);
    }
    return (msl_diff || gl_cfail || gl_vfail) ? 1 : 0;
}
