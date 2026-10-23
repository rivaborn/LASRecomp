// New in BotW Recomp (based on LAS Recomp by Peiikai, MIT).
// Game files from the player's own .nsp (games other than Link's Awakening; the start screen's Game Files page).
//
// The same steps as the nxdecomp pipeline's romfs stage (coding_workflow pipelines/switch): read the package's PFS0
// table; open each meta NCA (hactool, section 0) and parse its CNMT for the titles (the game and its update), their
// program NCAs and tickets; copy the program NCAs out next to the output; then hactool writes the update's ExeFS and
// the RomFS with the update applied over the base (--basenca + --section1: hactool skips --romfs for a patch section).
// Output: <out>/exefs/, <out>/romfs.bin, <out>/.source (which .nsp it came from). The work folder is deleted after.
//
// Keys: prod.keys stays where the player keeps it (passed to hactool by path). A title key goes to hactool on its
// command line, and the base game's (a second one) in a title.keys under a temporary home folder that is deleted when
// hactool returns. hactool's output goes to a log in the work folder, deleted with it (it is never shown: it can
// name keys).
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/statvfs.h>
#include <unistd.h>
#endif
#include <dirent.h>
#include <strings.h>

#include "nsp_extract.h"
#include "platform.h"
#include "rt.h"

typedef struct {
    char name[256];
    uint64_t offset, size;  // offset from the start of the file
} PfsEntry;

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint8_t type;           // 0x80 application, 0x81 patch
    char program_nca[64];   // "<content id>.nca"
    char rights_id[33];     // from its ticket ("" = none)
    char title_key[33];
} Title;

static void env_set(const char *k, const char *v) {  // v NULL: remove
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

// Runs hactool with HOME / USERPROFILE pointing at `home` (where it reads .switch/title.keys; NULL: unchanged).
static PlatProc spawn_hactool(const NspExtractOpts *o, const char *log, const char *home, const char *const *args) {
    const char *argv[24];
    char keys[1100];
    snprintf(keys, sizeof keys, "%s/prod.keys", o->keys_dir);
    int n = 0;
    argv[n++] = o->hactool;
    argv[n++] = "--disablekeywarns";
    argv[n++] = "-k";
    argv[n++] = keys;
    for (int i = 0; args[i] && n < 23; i++) argv[n++] = args[i];
    argv[n] = NULL;
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    char *old_profile = getenv("USERPROFILE") ? strdup(getenv("USERPROFILE")) : NULL;
    if (home) env_set("HOME", home), env_set("USERPROFILE", home);
    PlatProc p = plat_spawn(argv, log, true);  // (the child takes the environment as it is now)
    if (home) env_set("HOME", old_home), env_set("USERPROFILE", old_profile);
    free(old_home);
    free(old_profile);
    return p;
}

static void fail(char *why, size_t n, const char *fmt, const char *arg) {
    snprintf(why, n, fmt, arg ? arg : "");
}

static int read_pfs0(const char *path, PfsEntry **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[16];
    int n = -1;
    if (fread(h, 1, 16, f) == 16 && !memcmp(h, "PFS0", 4)) {
        uint32_t count, strsize;
        memcpy(&count, h + 4, 4);
        memcpy(&strsize, h + 8, 4);
        uint8_t *tab = malloc((size_t)count * 0x18 + strsize);
        if (tab && count < 100000 && fread(tab, 1, (size_t)count * 0x18 + strsize, f) == (size_t)count * 0x18 + strsize) {
            const uint64_t data = 0x10 + (uint64_t)count * 0x18 + strsize;
            const char *names = (const char *)tab + (size_t)count * 0x18;
            PfsEntry *e = calloc(count ? count : 1, sizeof *e);
            for (uint32_t i = 0; i < count; i++) {
                uint64_t off, size;
                uint32_t name;
                memcpy(&off, tab + i * 0x18, 8);
                memcpy(&size, tab + i * 0x18 + 8, 8);
                memcpy(&name, tab + i * 0x18 + 16, 4);
                e[i].offset = data + off;
                e[i].size = size;
                snprintf(e[i].name, sizeof e[i].name, "%s", name < strsize ? names + name : "");
            }
            *out = e;
            n = (int)count;
        }
        free(tab);
    }
    fclose(f);
    return n;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && !strcmp(s + a - b, suffix);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static uint64_t file_size(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static uint64_t free_bytes(const char *dir) {
#ifdef _WIN32
    ULARGE_INTEGER avail;
    return GetDiskFreeSpaceExA(dir, &avail, NULL, NULL) ? avail.QuadPart : UINT64_MAX;
#else
    struct statvfs s;
    return statvfs(dir, &s) == 0 ? (uint64_t)s.f_bavail * s.f_frsize : UINT64_MAX;
#endif
}

// Copies one package entry out; progress in bytes through *done
static bool copy_entry(const char *nsp, const PfsEntry *e, const char *dest, volatile uint64_t *done) {
    FILE *in = fopen(nsp, "rb"), *out = in ? fopen(dest, "wb") : NULL;
    bool ok = in && out;
#ifdef _WIN32
    if (ok) ok = _fseeki64(in, (long long)e->offset, SEEK_SET) == 0;
#else
    if (ok) ok = fseeko(in, (off_t)e->offset, SEEK_SET) == 0;
#endif
    const size_t CH = 1 << 22;
    char *buf = ok ? malloc(CH) : NULL;
    for (uint64_t left = e->size; ok && left;) {
        size_t k = left < CH ? (size_t)left : CH;
        ok = buf && fread(buf, 1, k, in) == k && fwrite(buf, 1, k, out) == k;
        left -= k;
        if (done) *done += k;
    }
    free(buf);
    if (in) fclose(in);
    if (out && fclose(out)) ok = false;
    return ok;
}

// The ticket of a title: "<rights id>.tik", whose rights id starts with the title id. Rights id and the encrypted title
// key, as hex (memory only).
static bool read_ticket(const char *nsp, const PfsEntry *e, char rights[33], char key[33]) {
    FILE *f = fopen(nsp, "rb");
    if (!f) return false;
    uint8_t *t = malloc(e->size);
    bool ok = t && e->size >= 0x2C0;
#ifdef _WIN32
    if (ok) ok = _fseeki64(f, (long long)e->offset, SEEK_SET) == 0;
#else
    if (ok) ok = fseeko(f, (off_t)e->offset, SEEK_SET) == 0;
#endif
    if (ok) ok = fread(t, 1, e->size, f) == e->size;
    fclose(f);
    if (ok) {
        uint32_t sig;
        memcpy(&sig, t, 4);
        uint64_t body = sig == 0x10000 || sig == 0x10003 ? 0x240 : sig == 0x10001 || sig == 0x10004 ? 0x140
                      : sig == 0x10002 || sig == 0x10005 ? 0x80 : 0;
        ok = body && body + 0x170 <= e->size;
        for (int i = 0; ok && i < 16; i++) {
            snprintf(rights + i * 2, 3, "%02x", t[body + 0x160 + i]);
            snprintf(key + i * 2, 3, "%02x", t[body + 0x40 + i]);
        }
    }
    if (t) memset(t, 0, e->size);
    free(t);
    return ok;
}

// hactool, waited for: its exit code (-1: didn't start)
static int hactool(const NspExtractOpts *o, const char *log, const char *home, const char *const *args) {
    PlatProc p = spawn_hactool(o, log, home, args);
    return p ? plat_proc_wait(p, true) : -1;
}

static bool find_file_with_suffix(const char *dir, const char *suffix, char *out, size_t n) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));)
        if (ends_with(e->d_name, suffix)) {
            snprintf(out, n, "%s/%s", dir, e->d_name);
            found = true;
        }
    closedir(d);
    return found;
}

static bool parse_cnmt(const char *path, Title *t) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[0x20];
    bool ok = fread(h, 1, 0x20, f) == 0x20;
    if (ok) {
        memcpy(&t->title_id, h, 8);
        memcpy(&t->version, h + 8, 4);
        t->type = h[0xC];
        uint16_t ext, count;
        memcpy(&ext, h + 0xE, 2);
        memcpy(&count, h + 0x10, 2);
        fseek(f, 0x20 + ext, SEEK_SET);
        for (int i = 0; i < count; i++) {
            uint8_t r[0x38];
            if (fread(r, 1, 0x38, f) != 0x38) { ok = false; break; }
            if (r[0x36] == 1) {  // Program
                char *p = t->program_nca;
                for (int k = 0; k < 16; k++) p += sprintf(p, "%02x", r[0x20 + k]);
                strcpy(p, ".nca");
            }
        }
    }
    fclose(f);
    return ok && t->program_nca[0];
}

bool nsp_main_build_id(const char *exefs_main, char out[65]) {
    FILE *f = fopen(exefs_main, "rb");
    if (!f) return false;
    uint8_t id[32];
    bool ok = fseek(f, 0x40, SEEK_SET) == 0 && fread(id, 1, 32, f) == 32;
    fclose(f);
    for (int i = 0; ok && i < 32; i++) snprintf(out + i * 2, 3, "%02x", id[i]);
    return ok;
}

bool nsp_extract(const NspExtractOpts *o, NspProgress progress, char *why, size_t why_n) {
    char work[1100], log[1200], path[1300], cnmt_dir[1300];
    snprintf(work, sizeof work, "%s.work", o->out_dir);
    snprintf(log, sizeof log, "%s/hactool.log", work);
    if (!is_file(o->nsp)) { fail(why, why_n, "The .nsp file wasn't found: %s", o->nsp); return false; }
    snprintf(path, sizeof path, "%s/prod.keys", o->keys_dir);
    if (!is_file(path)) { fail(why, why_n, "prod.keys wasn't found in the keys folder (%s)", o->keys_dir); return false; }
    if (!is_file(o->hactool)) { fail(why, why_n, "hactool wasn't found: %s", o->hactool); return false; }
    PfsEntry *e = NULL;
    int n = read_pfs0(o->nsp, &e);
    if (n <= 0) { fail(why, why_n, "Not an .nsp package (PFS0): %s", o->nsp); free(e); return false; }
    plat_remove_tree(work);
    mkdir(o->out_dir, 0755);
    mkdir(work, 0755);
    bool ok = false;
    Title titles[8];
    int nt = 0;
    progress(0, "Reading the package");
    // 1. titles: each meta NCA's CNMT
    for (int i = 0; i < n && nt < 8; i++) {
        if (!ends_with(e[i].name, ".cnmt.nca")) continue;
        snprintf(path, sizeof path, "%s/%s", work, e[i].name);
        snprintf(cnmt_dir, sizeof cnmt_dir, "%s/meta%d", work, nt);
        char sect[1400];
        snprintf(sect, sizeof sect, "--section0dir=%s", cnmt_dir);
        const char *args[] = {"-t", "nca", sect, path, NULL};
        char cnmt[1400];
        memset(&titles[nt], 0, sizeof titles[nt]);
        if (!copy_entry(o->nsp, &e[i], path, NULL) || hactool(o, log, NULL, args) != 0 ||
            !find_file_with_suffix(cnmt_dir, ".cnmt", cnmt, sizeof cnmt) || !parse_cnmt(cnmt, &titles[nt]))
            continue;  // (meta of an add-on without a program, or unreadable: not a title we need)
        if (titles[nt].type != 0x80 && titles[nt].type != 0x81) continue;
        char tid[17];
        snprintf(tid, sizeof tid, "%016llx", (unsigned long long)titles[nt].title_id);
        for (int k = 0; k < n; k++)
            if (ends_with(e[k].name, ".tik") && !strncasecmp(e[k].name, tid, 16))
                read_ticket(o->nsp, &e[k], titles[nt].rights_id, titles[nt].title_key);
        nt++;
    }
    Title *base = NULL, *update = NULL;
    for (int i = 0; i < nt; i++) {
        if (titles[i].type == 0x80 && !base) base = &titles[i];
        if (titles[i].type == 0x81 && (!update || titles[i].version > update->version)) update = &titles[i];
    }
    Title *prog = update ? update : base;
    const PfsEntry *base_e = NULL, *prog_e = NULL;
    for (int i = 0; i < n && prog; i++) {
        if (base && !strcasecmp(e[i].name, base->program_nca)) base_e = &e[i];
        if (!strcasecmp(e[i].name, prog->program_nca)) prog_e = &e[i];
    }
    if (!prog || !prog_e) {
        fail(why, why_n, "%s", nt ? "The package has no game program (an update needs its base game in the same .nsp)"
                                  : "No title could be read from the package (are the keys right for it?)");
        goto out;
    }
    if (update && !base_e) {
        fail(why, why_n, "%s", "This .nsp holds an update without its base game: use a package with both");
        goto out;
    }
    {
        // 2. space: the program NCA copies, then the RomFS image beside them
        const uint64_t copy_total = prog_e->size + (update && base_e ? base_e->size : 0);
        const uint64_t need = copy_total + (base_e ? base_e->size : prog_e->size) + (1ull << 30);
        if (free_bytes(o->out_dir) < need) {
            char gb[64];
            snprintf(gb, sizeof gb, "%.1f", need / 1e9);
            fail(why, why_n, "Not enough free disk space: about %s GB are needed while extracting", gb);
            goto out;
        }
        // 3. program NCAs
        volatile uint64_t done = 0;
        const double est_romfs = (double)(base_e ? base_e->size : prog_e->size);
        const double total = (double)copy_total + est_romfs;
        char prog_nca[1300], base_nca[1300];
        snprintf(prog_nca, sizeof prog_nca, "%s/%s", work, prog_e->name);
        progress(0.01, "Copying the game program out of the package");
        if (!copy_entry(o->nsp, prog_e, prog_nca, &done)) { fail(why, why_n, "%s", "Copying from the .nsp failed"); goto out; }
        if (update && base_e) {
            snprintf(base_nca, sizeof base_nca, "%s/%s", work, base_e->name);
            progress(done / total, "Copying the base game out of the package");
            if (!copy_entry(o->nsp, base_e, base_nca, &done)) { fail(why, why_n, "%s", "Copying from the .nsp failed"); goto out; }
        }
        // 4. ExeFS (the newest program's)
        char exefs[1200], tk[64], out_arg[1300];
        snprintf(exefs, sizeof exefs, "%s/exefs", o->out_dir);
        plat_remove_tree(exefs);
        snprintf(out_arg, sizeof out_arg, "--exefsdir=%s", exefs);
        snprintf(tk, sizeof tk, "--titlekey=%s", prog->title_key);
        progress(done / total, "Extracting the game code (ExeFS)");
        {
            const char *args[8];
            int k = 0;
            if (prog->title_key[0]) args[k++] = tk;
            args[k++] = "-t";
            args[k++] = "nca";
            args[k++] = out_arg;
            args[k++] = prog_nca;
            args[k] = NULL;
            snprintf(path, sizeof path, "%s/main", exefs);
            if (hactool(o, log, NULL, args) != 0 || !is_file(path)) {
                fail(why, why_n, "%s", "hactool couldn't extract the ExeFS (are the keys right for this game?)");
                goto out;
            }
        }
        // 5. RomFS: one image, the update applied over the base
        char romfs_tmp[1200], romfs[1200], home[1200], arg2[1400];
        snprintf(romfs_tmp, sizeof romfs_tmp, "%s/romfs.bin.tmp", o->out_dir);
        snprintf(romfs, sizeof romfs, "%s/romfs.bin", o->out_dir);
        remove(romfs_tmp);
        int code;
        progress(done / total, "Extracting the game data (RomFS); this takes a while");
        {
            // (hactool runs in a child: the progress is the image's size while it grows)
            const char *home_arg = NULL;
            if (update && base_e) {
                snprintf(home, sizeof home, "%s/home", work);
                char sw[1300];
                snprintf(sw, sizeof sw, "%s/.switch", home);
                mkdir(home, 0755);
                mkdir(sw, 0755);
                snprintf(path, sizeof path, "%s/title.keys", sw);
                FILE *f = base->rights_id[0] ? fopen(path, "w") : NULL;
                if (f) {
                    fprintf(f, "%s = %s\n", base->rights_id, base->title_key);
                    fclose(f);
                }
                home_arg = home;
                snprintf(arg2, sizeof arg2, "--basenca=%s", base_nca);
                snprintf(out_arg, sizeof out_arg, "--section1=%s", romfs_tmp);
            } else {
                snprintf(out_arg, sizeof out_arg, "--romfs=%s", romfs_tmp);
            }
            const char *args[8];
            int k = 0;
            if (prog->title_key[0]) args[k++] = tk;
            if (update && base_e) args[k++] = arg2;
            args[k++] = out_arg;
            args[k++] = prog_nca;
            args[k] = NULL;
            PlatProc p = spawn_hactool(o, log, home_arg, args);  // (in the background, polled)
            code = -1;
            if (p)
                while ((code = plat_proc_wait(p, false)) < 0) {
                    double f = ((double)done + (double)file_size(romfs_tmp)) / total;
                    progress(f > 0.99 ? 0.99 : f, NULL);
#ifdef _WIN32
                    Sleep(250);
#else
                    usleep(250000);
#endif
                }
            if (home_arg) {
                snprintf(path, sizeof path, "%s/.switch/title.keys", home_arg);
                FILE *f = fopen(path, "r+");  // (overwritten before it goes)
                if (f) {
                    fprintf(f, "%080d", 0);
                    fclose(f);
                }
                plat_remove_tree(home_arg);
            }
        }
        memset(tk, 0, sizeof tk);  // (the title key: last used above)
        if (code != 0 || file_size(romfs_tmp) == 0) {  // (hactool can exit 0 without writing a section it couldn't read)
            remove(romfs_tmp);
            fail(why, why_n, "%s", "hactool couldn't extract the RomFS (are the keys right for this game and its update?)");
            goto out;
        }
        remove(romfs);
        if (rename(romfs_tmp, romfs)) { fail(why, why_n, "%s", "Couldn't write romfs.bin"); goto out; }
        // 6. where it came from (the start screen notices a different .nsp)
        snprintf(path, sizeof path, "%s/.source", o->out_dir);
        FILE *f = fopen(path, "w");
        if (f) {
            struct stat st;
            stat(o->nsp, &st);
            fprintf(f, "%s\n%llu %lld\n", o->nsp, (unsigned long long)st.st_size, (long long)st.st_mtime);
            fclose(f);
        }
        progress(1, "Done");
        ok = true;
    }
out:
    for (int i = 0; i < nt; i++) memset(titles[i].title_key, 0, sizeof titles[i].title_key);
    plat_remove_tree(work);  // (NCA copies, meta sections, the hactool log)
    free(e);
    return ok;
}

bool nsp_source_matches(const char *out_dir, const char *nsp) {
    char path[1100], line[1100];
    snprintf(path, sizeof path, "%s/.source", out_dir);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof line, f) != NULL;
    if (ok) {
        line[strcspn(line, "\r\n")] = 0;
        unsigned long long size = 0;
        long long mtime = 0;
        struct stat st;
        ok = !strcmp(line, nsp) && fscanf(f, "%llu %lld", &size, &mtime) == 2 && stat(nsp, &st) == 0 &&
             (unsigned long long)st.st_size == size && (long long)st.st_mtime == mtime;
    }
    fclose(f);
    return ok;
}
