// Start points, for debugging late parts of a game without replaying it from the boot.
//
// A start point is the game's own save data plus how to get from the boot to the point, in
// savepoints/<name>/ in the data root (runtime/paths.h; git-ignored with the rest of build-<game>/):
//   save_0/...   the save filesystem exactly as services/fs.c serves it (save_<attribute type>)
//   start.txt    optional KEY=VALUE lines (# comments) set before the boot unless the environment already has them,
//                e.g. LARECOMP_INPUT=600:A:10,900:A:10 (press Continue) or LARECOMP_TURBO_UNTIL=<frame>
//
// LARECOMP_START=<name> copies the point's files to save_start/ (recreated on each run, so the point itself is never
// written) and boots with LARECOMP_SAVE_DIR=save_start.
// LARECOMP_SAVE_SNAPSHOT=<name> makes new points from a run: each time the game commits its save filesystem, that
// save becomes savepoints/<name>-<n>/ (with the run's LARECOMP_INPUT in its start.txt).
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "paths.h"
#include "rt.h"

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void rt_remove_tree(const char *p) {  // (checkpoint.c too)
    DIR *d = opendir(p);
    if (!d) { unlink(p); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char c[2048];
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        rt_remove_tree(c);
    }
    closedir(d);
    rmdir(p);
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    bool ok = in && out;
    char buf[1 << 16];
    for (size_t n; ok && (n = fread(buf, 1, sizeof buf, in)) > 0;) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out)) ok = false;
    return ok;
}

int rt_copy_tree(const char *from, const char *to) {  // files copied, -1 on an error (checkpoint.c too)
    DIR *d = opendir(from);
    if (!d) return -1;
    mkdir(to, 0755);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n >= 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[2048], b[2048];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (is_dir(a)) {
            int k = rt_copy_tree(a, b);
            n = k < 0 ? -1 : n + k;
        } else {
            n = copy_file(a, b) ? n + 1 : -1;
        }
    }
    closedir(d);
    return n;
}

static void apply_start_txt(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        if (getenv(line)) {
            LOG("[start] %s already set: start.txt's value not used\n", line);
            continue;
        }
        setenv(line, eq + 1, 1);
        LOG("[start] %s=%s\n", line, eq + 1);
    }
    fclose(f);
}

// main.c, after rt_paths_init (the data root is the working directory). Returns true when a start point is used.
bool start_point_init(void) {
    const char *name = getenv("LARECOMP_START");
    if (!name || !*name) return false;
    char dir[1024];
    snprintf(dir, sizeof dir, "savepoints/%s", name);
    if (!is_dir(dir)) FATAL("LARECOMP_START: no start point %s/%s\n", rt_data_root(), dir);
    rt_remove_tree("save_start");
    char from[1100];
    int files = 0;
    DIR *d = opendir(dir);  // every save_<n>/ of the point
    mkdir("save_start", 0755);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "save_", 5)) continue;
        char to[1100];
        snprintf(from, sizeof from, "%s/%s", dir, e->d_name);
        snprintf(to, sizeof to, "save_start/%s", e->d_name);
        int k = rt_copy_tree(from, to);
        if (k < 0) FATAL("LARECOMP_START: copying %s failed\n", from);
        files += k;
    }
    if (d) closedir(d);
    if (!files) FATAL("LARECOMP_START: %s has no save_<n>/ files\n", dir);
    setenv("LARECOMP_SAVE_DIR", "save_start", 1);
    snprintf(from, sizeof from, "%s/start.txt", dir);
    apply_start_txt(from);
    LOG("[start] start point %s: %d save files in save_start/\n", name, files);
    return true;
}

// services/fs.c, IFileSystem Commit on a mounted save directory (root: "<save dir>/save_<n>").
void start_point_save_committed(const char *root) {
    const char *name = getenv("LARECOMP_SAVE_SNAPSHOT");
    if (!name || !*name) return;
    const char *sub = strrchr(root, '/');
    sub = sub ? sub + 1 : root;
    mkdir("savepoints", 0755);
    char dir[1024], to[1100];
    for (int n = 1;; n++) {
        snprintf(dir, sizeof dir, "savepoints/%s-%d", name, n);
        if (!is_dir(dir)) break;
    }
    mkdir(dir, 0755);
    snprintf(to, sizeof to, "%s/%s", dir, sub);
    int k = rt_copy_tree(root, to);
    snprintf(to, sizeof to, "%s/start.txt", dir);
    FILE *f = fopen(to, "w");
    if (f) {
        extern uint64_t renderer_present_count(void);
        fprintf(f, "# made by LARECOMP_SAVE_SNAPSHOT at frame %llu\n", (unsigned long long)renderer_present_count());
        if (getenv("LARECOMP_INPUT")) fprintf(f, "# the run's input was: LARECOMP_INPUT=%s\n", getenv("LARECOMP_INPUT"));
        fprintf(f, "# add the LARECOMP_INPUT that loads this save from the title screen (Continue)\n");
        fclose(f);
    }
    LOG("[start] save committed: %d files -> %s\n", k, dir);
}
