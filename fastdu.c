/*
 * fastdu — A fast, TUI-based disk usage explorer (ncurses + pthreads)
 *
 * Overview
 * --------
 * fastdu provides a terminal UI to explore directory sizes quickly.
 * It scans directories in parallel using a deep work queue and caches
 * results to a TSV-like file (.fastdu_cache_v2) at the scan root to
 * avoid rescanning unchanged directories. The TUI displays a sortable
 * list (by size or name), supports filtering, incremental find, marks
 * for bulk operations, and live progress during scans.
 *
 * Architecture at a glance
 * ------------------------
 * - Util: Small helpers for strings/paths, human-readable sizes, and
 *   lightweight percent-encoding/decoding for cache safety.
 * - Cache: In-memory store (with mutex) of per-directory scan results,
 *   persisted to a per-root cache file. Entries track size, inode and
 *   mtime to detect changes and allow partial rescans.
 * - Scanner: Two implementations — a straightforward recursive walker
 *   and a parallel deep work queue backed by multiple threads. Both
 *   avoid following symlinks and skip the cache file in the root.
 * - UI/TUI: ncurses-based list view with header/footer and keyboard
 *   controls (help, sort, filter, find, mark/move/delete, rescan).
 *   Progress is rendered on the status line during long scans.
 * - Operations: File moves and deletions update the cache incrementally
 *   (delta adjustments) to keep the UI responsive without full rescans.
 *
 * Notes on portability and safety
 * -------------------------------
 * - Uses openat/fstatat/O_NOFOLLOW where possible to avoid TOCTOU races
 *   and to prevent following symlinks into unexpected trees.
 * - Uses wide-character ncurses (linked with -lncursesw) and locale.
 * - Concurrency relies on pthreads; cache is guarded by a mutex.
 * - Progress and totals are maintained with atomic counters.
 *
 * Build: see Makefile (gcc -O2 -Wall -Wextra -std=c11 -lncursesw -lpthread)
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>
#include <limits.h>
#include <wchar.h>
#include <curses.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <ctype.h>
#include <signal.h>
#include <regex.h>
#include <pwd.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Forward declaration for TUI active flag used before its definition
static volatile sig_atomic_t g_tui_active;

#define CACHE_FILENAME ".fastdu_cache_v2"
#define FASTDU_VERSION "0.33"

static void print_cli_usage(void) {
    printf("fastdu %s\n", FASTDU_VERSION);
    printf("Usage:\n");
    printf("  fastdu [options] [path]\n\n");
    printf("Options:\n");
    printf("  -h, --help           Show this help and exit\n");
    printf("  -v, --version        Show version and exit\n");
    printf("  -R, --reload         Ignore cache and perform full rescan\n");
    printf("  -H, --headless       Force headless (non-TUI) mode\n");
    printf("  -ac, --accuracy      Accurate disk usage: force deep rescan and use allocated blocks (slower)\n");
    printf("  -j N, --jobs N       Number of worker threads (default: CPUs)\n\n");
    printf("Examples:\n");
    printf("  fastdu                 # open TUI on current directory\n");
    printf("  fastdu -R /data        # full reload of /data, then open TUI\n");
    printf("  fastdu -j 8 /data      # use 8 workers\n");
    printf("  fastdu -v              # print version\n");
}

// ------------------------------
// Util
// ------------------------------
/*
 * String and path helpers:
 * - xstrdup: safe strdup that returns NULL on OOM.
 * - path_join: joins two segments with a single '/', no normalization.
 * - is_dot_or_dotdot: filters "." and ".." during directory walks.
 * - human_size: pretty-prints bytes in human units (B, KiB, MiB...).
 * - get_parent: computes the parent directory, handling the root edge-case.
 * - pct_encode/pct_decode: percent-encodes tabs/newlines/% for TSV safety.
 * - relpath_from_abs/abspath_from_rel: converts between absolute and
 *   relative paths w.r.t. the scan root for on-disk persistence.
 */
static int g_headless = 0;
static int g_accuracy_mode = 0; // when set, compute disk usage using st_blocks and force deep rescan

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    return p;
}

static char *path_join(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    int need_slash = (la > 0 && a[la-1] != '/');
    size_t n = la + need_slash + lb + 1;
    char *p = malloc(n);
    if (!p) return NULL;
    memcpy(p, a, la);
    if (need_slash) p[la++] = '/';
    memcpy(p + la, b, lb);
    p[la+lb] = '\0';
    return p;
}

static int is_dot_or_dotdot(const char *name) {
    return (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')));
}

/*
 * human_size
 * -----------
 * Convert a byte count (v) into a human-readable string (e.g. "12.3 GiB").
 * - Uses powers of 1024 (KiB, MiB, ...).
 * - For values < 1 KiB, keeps integer bytes.
 * - Writes into buf (size bufsz). Does not allocate.
 */
static void human_size(unsigned long long v, char *buf, size_t bufsz) {
    const char *units[] = {" B", "KB", "MB", "GB", "TB", "PB"};
    int i = 0;
    double d = (double)v;
    while (d >= 1024.0 && i < (int)(sizeof(units)/sizeof(units[0])) - 1) {
        d /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(buf, bufsz, "%llu %s", (unsigned long long)v, units[i]);
    else
        snprintf(buf, bufsz, "%.1f %s", d, units[i]);
}

static inline unsigned long long file_size_bytes(const struct stat *st) {
    if (!st) return 0ULL;
    if (g_accuracy_mode) {
        unsigned long long blk = (unsigned long long)st->st_blocks;
        return blk * 512ULL; // POSIX st_blocks unit is 512 bytes
    }
    return (unsigned long long)st->st_size;
}

static char *get_parent(const char *path) {
    if (!path) return NULL;
    size_t n = strlen(path);
    if (n == 0) return xstrdup("/");
    // remove trailing slash (except root)
    while (n > 1 && path[n-1] == '/') n--;
    size_t i = n;
    while (i > 1 && path[i-1] != '/') i--;
    if (i == 1) return xstrdup("/");
    char *p = malloc(i);
    if (!p) return NULL;
    memcpy(p, path, i-1);
    p[i-1] = '\0';
    return p;
}

static int starts_with(const char *s, const char *prefix) {
    size_t ls = strlen(s), lp = strlen(prefix);
    return ls >= lp && memcmp(s, prefix, lp) == 0;
}

static int strcasestr_bool(const char *hay, const char *needle) {
    if (!needle || needle[0] == '\0') return 1;
    size_t hl = strlen(hay), nl = strlen(needle);
    if (nl == 0) return 1;
    if (nl > hl) return 0;
    for (size_t i = 0; i + nl <= hl; i++) {
        size_t j = 0;
        for (; j < nl; j++) {
            unsigned char a = (unsigned char)hay[i + j];
            unsigned char b = (unsigned char)needle[j];
            if (tolower(a) != tolower(b)) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static const char *path_basename_const(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

static int path_exists(const char *p) {
    struct stat st; return lstat(p, &st) == 0;
}

static char *gen_nonconflicting_path(const char *dst) {
    // split dir and name
    const char *base = path_basename_const(dst);
    size_t dirlen = (size_t)(base - dst);
    char *dir = NULL;
    if (dirlen == 0) dir = xstrdup(".");
    else { dir = malloc(dirlen + 1); if (!dir) return NULL; memcpy(dir, dst, dirlen); dir[dirlen-1] = '\0'; }
    // split extension
    const char *dot = strrchr(base, '.');
    size_t name_len = dot ? (size_t)(dot - base) : strlen(base);
    size_t ext_len = dot ? strlen(dot) : 0;
    char namebuf[PATH_MAX];
    if (name_len >= sizeof(namebuf)) name_len = sizeof(namebuf)-1;
    memcpy(namebuf, base, name_len); namebuf[name_len] = '\0';
    const char *ext = dot ? dot : "";
    // Try suffixes: " (copy)", then " (copy N)"
    for (int i = 0; i < 1000; i++) {
        char cand[PATH_MAX];
        if (i == 0) snprintf(cand, sizeof(cand), "%s (copy)%s", namebuf, ext);
        else snprintf(cand, sizeof(cand), "%s (copy %d)%s", namebuf, i+1, ext);
        char *joined = path_join(dir, cand);
        if (!joined) { free(dir); return NULL; }
        if (!path_exists(joined)) { free(dir); return joined; }
        free(joined);
    }
    free(dir);
    return NULL;
}

static char prompt_conflict_action(const char *dst) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    char msg[PATH_MAX + 256];
snprintf(msg, sizeof(msg), "Conflict on '%s': [o]verwrite, [r]ename, [s]kip, [O] overwrite all, [R] rename all, [S] skip all ", dst);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, msg, cols-1);
    refresh();
    int ch = getch();
    if (ch=='o'||ch=='O') return (ch=='O') ? 'O' : 'o';
    if (ch=='r'||ch=='R') return (ch=='R') ? 'R' : 'r';
    if (ch=='s'||ch=='S') return (ch=='S') ? 'S' : 's';
    return 's';
}

// Percent-encode tabs/newlines/percent for safe TSV cache
static char *pct_encode(const char *s) {
    size_t cap = strlen(s) * 3 + 1; // worst case all encoded
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t' || c == '\n' || c == '\r' || c == '%') {
            if (j + 3 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) return NULL; }
            out[j++] = '%';
            const char *hex = "0123456789ABCDEF";
            out[j++] = hex[(c >> 4) & 0xF];
            out[j++] = hex[c & 0xF];
        } else {
            if (j + 1 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) return NULL; }
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return out;
}

static char from_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;
}

static char *pct_decode(const char *s) {
    size_t cap = strlen(s) + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == '%' && s[i+1] && s[i+2]) {
            unsigned char v = (from_hex(s[i+1]) << 4) | from_hex(s[i+2]);
            if (j + 1 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) return NULL; }
            out[j++] = (char)v;
            i += 2;
        } else {
            if (j + 1 >= cap) { cap *= 2; out = realloc(out, cap); if (!out) return NULL; }
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

static char *relpath_from_abs(const char *root, const char *abs) {
    size_t lr = strlen(root);
    if (strcmp(abs, root) == 0) return xstrdup(".");
    if (starts_with(abs, root) && abs[lr] == '/') {
        return xstrdup(abs + lr + 1);
    }
    // fallback: store absolute if outside (shouldn't happen)
    return xstrdup(abs);
}

static char *abspath_from_rel(const char *root, const char *rel) {
    if (strcmp(rel, ".") == 0) return xstrdup(root);
    return path_join(root, rel);
}

// ------------------------------
// Cache
// ------------------------------
// Forward declaration for global files counter used in cache I/O
static unsigned long long g_last_files;

/*
 * The in-memory cache maps absolute directories to: total size, inode and
 * mtime. It is serialized to a text file under the scan root (CACHE_FILENAME)
 * so results can be reused between runs.
 *
 * v2 format:
 *   # fastdu-cache v2
 *   root\t<abs_root>
 *   D\t<rel_path_pct>\t<size>\t<last_scan_epoch>\t<ino>\t<dir_mtime>
 *
 * - rel_path_pct: relative path, percent-encoded (tabs/newlines/% encoded).
 * - last_scan_epoch: time_t of the scan.
 * - ino/dir_mtime: allow fast invalidation checks.
 */
typedef struct {
    char *abs_path;
    char *rel_path;
    unsigned long long size;
    time_t last_scan; // when we scanned
    unsigned long long ino; // inode of directory
    time_t dir_mtime; // mtime of directory at scan time
} CacheEntry;

typedef struct {
    CacheEntry *v;
    size_t n;
    size_t cap;
    pthread_mutex_t mu;
} Cache;

static void cache_init(Cache *c) { c->v = NULL; c->n = c->cap = 0; pthread_mutex_init(&c->mu, NULL); }

static void cache_free(Cache *c) {
    pthread_mutex_lock(&c->mu);
    for (size_t i = 0; i < c->n; i++) {
        free(c->v[i].abs_path);
        free(c->v[i].rel_path);
    }
    free(c->v);
    c->v = NULL; c->n = c->cap = 0;
    pthread_mutex_unlock(&c->mu);
    pthread_mutex_destroy(&c->mu);
}

static ssize_t cache_find_index_nl(Cache *c, const char *abs_path) {
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->v[i].abs_path, abs_path) == 0) return (ssize_t)i;
    }
    return -1;
}

static CacheEntry *cache_get(Cache *c, const char *abs_path) {
    CacheEntry *ret = NULL;
    pthread_mutex_lock(&c->mu);
    ssize_t idx = cache_find_index_nl(c, abs_path);
    if (idx >= 0) ret = &c->v[idx];
    pthread_mutex_unlock(&c->mu);
    return ret;
}

static CacheEntry *cache_upsert(Cache *c, const char *root, const char *abs_path, unsigned long long size, time_t now) {
    CacheEntry *out = NULL;
    pthread_mutex_lock(&c->mu);
    ssize_t idx = cache_find_index_nl(c, abs_path);
    if (idx >= 0) {
        c->v[idx].size = size;
        c->v[idx].last_scan = now;
        out = &c->v[idx];
    } else {
        if (c->n == c->cap) {
            size_t newcap = c->cap ? c->cap * 2 : 256;
            void *nv = realloc(c->v, newcap * sizeof(CacheEntry));
            if (!nv) { pthread_mutex_unlock(&c->mu); return NULL; }
            c->v = (CacheEntry*)nv; c->cap = newcap;
        }
        CacheEntry *e = &c->v[c->n++];
        e->abs_path = xstrdup(abs_path);
        e->rel_path = relpath_from_abs(root, abs_path);
        e->size = size;
        e->last_scan = now;
        e->ino = 0ULL;
        e->dir_mtime = 0;
        out = e;
    }
    pthread_mutex_unlock(&c->mu);
    return out;
}

static void cache_upsert_with_meta(Cache *c, const char *root, const char *abs_path,
                                   unsigned long long size, time_t now,
                                   unsigned long long ino, time_t dir_mtime) {
    pthread_mutex_lock(&c->mu);
    ssize_t idx = cache_find_index_nl(c, abs_path);
    if (idx >= 0) {
        c->v[idx].size = size;
        c->v[idx].last_scan = now;
        c->v[idx].ino = ino;
        c->v[idx].dir_mtime = dir_mtime;
    } else {
        if (c->n == c->cap) {
            size_t newcap = c->cap ? c->cap * 2 : 256;
            void *nv = realloc(c->v, newcap * sizeof(CacheEntry));
            if (!nv) { pthread_mutex_unlock(&c->mu); return; }
            c->v = (CacheEntry*)nv; c->cap = newcap;
        }
        CacheEntry *e = &c->v[c->n++];
        e->abs_path = xstrdup(abs_path);
        e->rel_path = relpath_from_abs(root, abs_path);
        e->size = size;
        e->last_scan = now;
        e->ino = ino;
        e->dir_mtime = dir_mtime;
    }
    pthread_mutex_unlock(&c->mu);
}

/*
 * cache_save
 * ----------
 * Serializes all cache entries to CACHE_FILENAME under 'root'.
 * Returns 0 on success, -1 on error (OOM, I/O, etc.).
 */
static int cache_save(const char *root, const Cache *c) {
    char *cache_path = path_join(root, CACHE_FILENAME);
    if (!cache_path) return -1;
    FILE *f = fopen(cache_path, "w");
    if (!f) { free(cache_path); return -1; }
    fprintf(f, "# fastdu-cache v3\n");
    fprintf(f, "root\t%s\n", root);
    // totals: write root total bytes and global files if available
    unsigned long long total_bytes = 0ULL;
    pthread_mutex_lock((pthread_mutex_t*)&c->mu);
    for (size_t i = 0; i < c->n; i++) {
        if (c->v[i].abs_path && strcmp(c->v[i].abs_path, root) == 0) { total_bytes = c->v[i].size; break; }
    }
    pthread_mutex_unlock((pthread_mutex_t*)&c->mu);
    fprintf(f, "totals\t%llu\n", (unsigned long long)total_bytes);
    fprintf(f, "totals_files\t%llu\n", (unsigned long long)g_last_files);
    pthread_mutex_lock((pthread_mutex_t*)&c->mu);
    for (size_t i = 0; i < c->n; i++) {
        char *enc = pct_encode(c->v[i].rel_path ? c->v[i].rel_path : ".");
        if (!enc) { pthread_mutex_unlock((pthread_mutex_t*)&c->mu); fclose(f); free(cache_path); return -1; }
        fprintf(f, "D\t%s\t%llu\t%ld\t%llu\t%ld\n",
                enc,
                (unsigned long long)c->v[i].size,
                (long)c->v[i].last_scan,
                (unsigned long long)c->v[i].ino,
                (long)c->v[i].dir_mtime);
        free(enc);
    }
    pthread_mutex_unlock((pthread_mutex_t*)&c->mu);
    fclose(f);
    free(cache_path);
    return 0;
}

/*
 * cache_load
 * ----------
 * Attempts to load an existing cache from disk.
 * Returns 1 if loaded, 0 if not found, -1 on error.
 */
static int cache_load(const char *root, Cache *c) {
    char *cache_path = path_join(root, CACHE_FILENAME);
    if (!cache_path) return -1;
    FILE *f = fopen(cache_path, "r");
    int debug_cache = getenv("FASTDU_DEBUG_CACHE") ? 1 : 0;
    if (debug_cache) fprintf(stderr, "[cache] open %s\n", cache_path);
    if (!f) { free(cache_path); return 0; } // no cache file, not an error

    // Determine file size for progress
    long total_bytes = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long pos = ftell(f);
        if (pos > 0) total_bytes = pos;
        fseek(f, 0, SEEK_SET);
    }
    int ui_enabled = (g_tui_active ? 1 : 0);
    struct timespec last_draw; clock_gettime(CLOCK_MONOTONIC, &last_draw);
    int spinner = 0;

    char *line = NULL; size_t len = 0; ssize_t r;
    int header_ok = 0; int version = 1;
    unsigned long long totals_bytes = 0ULL;
    unsigned long long totals_files = 0ULL;
    unsigned long long lines_read = 0ULL;
    while ((r = getline(&line, &len, f)) != -1) {
        if (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) line[--r] = '\0';
        if (!header_ok) {
            if (strncmp(line, "# fastdu-cache v3", 18) == 0) { header_ok = 1; version = 3; }
            else if (strncmp(line, "# fastdu-cache v2", 18) == 0) { header_ok = 1; version = 2; }
            else if (strncmp(line, "# fastdu-cache v1", 18) == 0) { header_ok = 1; version = 1; }
            // update progress
            if (ui_enabled) {
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
                if (ms >= 30) {
                    last_draw = now;
                    int cols, rows; getmaxyx(stdscr, rows, cols);
                    int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200;
                    char bar[256]; memset(bar, '.', (size_t)barlen);
                    int filled = 0; int percent = 0;
                    if (total_bytes > 0) {
                        long cur = ftell(f);
                        if (cur < 0) cur = 0;
                        double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                        filled = (int)(frac * barlen);
                        percent = (int)(frac * 100.0 + 0.5);
                    } else {
                        spinner = (spinner + 1) % barlen; filled = spinner;
                        percent = 0;
                    }
                    if (filled < 0) filled = 0; if (filled > barlen) filled = barlen;
                    for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0';
                    char linebuf[PATH_MAX + 256];
                    snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path);
                    mvhline(rows-1, 0, ' ', cols);
                    mvaddnstr(rows-1, 0, linebuf, cols-1);
                    refresh();
                }
            }
            continue;
        }
        if (strncmp(line, "root\t", 5) == 0) {
            // informational, ignore value
            if (ui_enabled) {
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
                if (ms >= 30) {
                    last_draw = now;
                    int cols, rows; getmaxyx(stdscr, rows, cols);
                    int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200;
                    char bar[256]; memset(bar, '.', (size_t)barlen);
                    int filled = 0; int percent = 0;
                    if (total_bytes > 0) {
                        long cur = ftell(f); if (cur < 0) cur = 0;
                        double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                        filled = (int)(frac * barlen);
                        percent = (int)(frac * 100.0 + 0.5);
                    } else { spinner = (spinner + 1) % barlen; filled = spinner; }
                    if (filled < 0) filled = 0; if (filled > barlen) filled = barlen;
                    for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0';
                    char linebuf[PATH_MAX + 256];
                    snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path);
                    mvhline(rows-1, 0, ' ', cols);
                    mvaddnstr(rows-1, 0, linebuf, cols-1);
                    refresh();
                }
            }
            continue;
        }
        if (version >= 3 && strncmp(line, "totals\t", 8) == 0) {
            const char *b = line + 8;
            unsigned long long v = 0ULL; sscanf(b, "%llu", &v);
            totals_bytes = v;
            if (ui_enabled) {
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
                if (ms >= 30) {
                    last_draw = now;
                    int cols, rows; getmaxyx(stdscr, rows, cols);
                    int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200;
                    char bar[256]; memset(bar, '.', (size_t)barlen);
                    int filled = 0; int percent = 0;
                    if (total_bytes > 0) {
                        long cur = ftell(f); if (cur < 0) cur = 0;
                        double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                        filled = (int)(frac * barlen);
                        percent = (int)(frac * 100.0 + 0.5);
                    } else { spinner = (spinner + 1) % barlen; filled = spinner; }
                    if (filled < 0) filled = 0; if (filled > barlen) filled = barlen;
                    for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0';
                    char linebuf[PATH_MAX + 256];
                    snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path);
                    mvhline(rows-1, 0, ' ', cols);
                    mvaddnstr(rows-1, 0, linebuf, cols-1);
                    refresh();
                }
            }
            continue;
        }
        if (version >= 3 && strncmp(line, "totals_files\t", 14) == 0) {
            const char *b = line + 14;
            unsigned long long v = 0ULL; sscanf(b, "%llu", &v);
            totals_files = v;
            if (ui_enabled) {
                struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
                if (ms >= 30) {
                    last_draw = now;
                    int cols, rows; getmaxyx(stdscr, rows, cols);
                    int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200;
                    char bar[256]; memset(bar, '.', (size_t)barlen);
                    int filled = 0; int percent = 0;
                    if (total_bytes > 0) {
                        long cur = ftell(f); if (cur < 0) cur = 0;
                        double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                        filled = (int)(frac * barlen);
                        percent = (int)(frac * 100.0 + 0.5);
                    } else { spinner = (spinner + 1) % barlen; filled = spinner; }
                    if (filled < 0) filled = 0; if (filled > barlen) filled = barlen;
                    for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0';
                    char linebuf[PATH_MAX + 256];
                    snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path);
                    mvhline(rows-1, 0, ' ', cols);
                    mvaddnstr(rows-1, 0, linebuf, cols-1);
                    refresh();
                }
            }
            continue;
        }
        if (line[0] == 'D' && line[1] == '\t') {
            char *p = line + 2;
            char *rel = p;
            char *tab1 = strchr(p, '\t'); if (!tab1) { if (ui_enabled) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000; if (ms >= 30) { last_draw = now; int cols, rows; getmaxyx(stdscr, rows, cols); int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200; char bar[256]; memset(bar, '.', (size_t)barlen); int filled = 0; int percent = 0; if (total_bytes > 0) { long cur = ftell(f); if (cur < 0) cur = 0; double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1; filled = (int)(frac * barlen); percent = (int)(frac * 100.0 + 0.5); } else { spinner = (spinner + 1) % barlen; filled = spinner; } if (filled < 0) filled = 0; if (filled > barlen) filled = barlen; for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0'; char linebuf[PATH_MAX + 256]; snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path); mvhline(rows-1, 0, ' ', cols); mvaddnstr(rows-1, 0, linebuf, cols-1); refresh(); } } continue; } *tab1 = '\0';
            char *size_str = tab1 + 1;
            char *tab2 = strchr(size_str, '\t'); if (!tab2) { if (ui_enabled) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000; if (ms >= 30) { last_draw = now; int cols, rows; getmaxyx(stdscr, rows, cols); int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200; char bar[256]; memset(bar, '.', (size_t)barlen); int filled = 0; int percent = 0; if (total_bytes > 0) { long cur = ftell(f); if (cur < 0) cur = 0; double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1; filled = (int)(frac * barlen); percent = (int)(frac * 100.0 + 0.5); } else { spinner = (spinner + 1) % barlen; filled = spinner; } if (filled < 0) filled = 0; if (filled > barlen) filled = barlen; for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0'; char linebuf[PATH_MAX + 256]; snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path); mvhline(rows-1, 0, ' ', cols); mvaddnstr(rows-1, 0, linebuf, cols-1); refresh(); } } continue; } *tab2 = '\0';
            char *time_str = tab2 + 1;
            char *ino_str = NULL; char *mtime_str = NULL;
            if (version >= 2) {
                char *tab3 = strchr(time_str, '\t'); if (!tab3) { if (ui_enabled) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000; if (ms >= 30) { last_draw = now; int cols, rows; getmaxyx(stdscr, rows, cols); int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200; char bar[256]; memset(bar, '.', (size_t)barlen); int filled = 0; int percent = 0; if (total_bytes > 0) { long cur = ftell(f); if (cur < 0) cur = 0; double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1; filled = (int)(frac * barlen); percent = (int)(frac * 100.0 + 0.5); } else { spinner = (spinner + 1) % barlen; filled = spinner; } if (filled < 0) filled = 0; if (filled > barlen) filled = barlen; for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0'; char linebuf[PATH_MAX + 256]; snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path); mvhline(rows-1, 0, ' ', cols); mvaddnstr(rows-1, 0, linebuf, cols-1); refresh(); } } continue; } *tab3 = '\0';
                ino_str = tab3 + 1;
                char *tab4 = strchr(ino_str, '\t'); if (!tab4) { if (ui_enabled) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000; if (ms >= 30) { last_draw = now; int cols, rows; getmaxyx(stdscr, rows, cols); int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200; char bar[256]; memset(bar, '.', (size_t)barlen); int filled = 0; int percent = 0; if (total_bytes > 0) { long cur = ftell(f); if (cur < 0) cur = 0; double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1; filled = (int)(frac * barlen); percent = (int)(frac * 100.0 + 0.5); } else { spinner = (spinner + 1) % barlen; filled = spinner; } if (filled < 0) filled = 0; if (filled > barlen) filled = barlen; for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0'; char linebuf[PATH_MAX + 256]; snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path); mvhline(rows-1, 0, ' ', cols); mvaddnstr(rows-1, 0, linebuf, cols-1); refresh(); } } continue; } *tab4 = '\0';
                mtime_str = tab4 + 1;
            }
            char *rel_dec = pct_decode(rel);
            if (!rel_dec) { if (ui_enabled) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000; if (ms >= 30) { last_draw = now; int cols, rows; getmaxyx(stdscr, rows, cols); int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200; char bar[256]; memset(bar, '.', (size_t)barlen); int filled = 0; int percent = 0; if (total_bytes > 0) { long cur = ftell(f); if (cur < 0) cur = 0; double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1; filled = (int)(frac * barlen); percent = (int)(frac * 100.0 + 0.5); } else { spinner = (spinner + 1) % barlen; filled = spinner; } if (filled < 0) filled = 0; if (filled > barlen) filled = barlen; for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0'; char linebuf[PATH_MAX + 256]; snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path); mvhline(rows-1, 0, ' ', cols); mvaddnstr(rows-1, 0, linebuf, cols-1); refresh(); } } continue; }
            char *abs = abspath_from_rel(root, rel_dec);
            free(rel_dec);
            if (!abs) { if (ui_enabled) { struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000; if (ms >= 30) { last_draw = now; int cols, rows; getmaxyx(stdscr, rows, cols); int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200; char bar[256]; memset(bar, '.', (size_t)barlen); int filled = 0; int percent = 0; if (total_bytes > 0) { long cur = ftell(f); if (cur < 0) cur = 0; double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1; filled = (int)(frac * barlen); percent = (int)(frac * 100.0 + 0.5); } else { spinner = (spinner + 1) % barlen; filled = spinner; } if (filled < 0) filled = 0; if (filled > barlen) filled = barlen; for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0'; char linebuf[PATH_MAX + 256]; snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path); mvhline(rows-1, 0, ' ', cols); mvaddnstr(rows-1, 0, linebuf, cols-1); refresh(); } } continue; }
            unsigned long long size = 0ULL;
            long last_scan = 0;
            unsigned long long ino = 0ULL; long dir_mtime = 0;
            sscanf(size_str, "%llu", &size);
            sscanf(time_str, "%ld", &last_scan);
            if (version >= 2) { sscanf(ino_str, "%llu", &ino); sscanf(mtime_str, "%ld", &dir_mtime); }
            time_t t = (time_t)last_scan;
            CacheEntry *ce = cache_upsert(c, root, abs, size, t);
            if (ce) { ce->ino = ino; ce->dir_mtime = (time_t)dir_mtime; }
            free(abs);
        }
        lines_read++;
        if ((g_headless && (lines_read % 1000ULL == 0ULL)) || (debug_cache && (lines_read % 1000ULL == 0ULL)))
            fprintf(stderr, "[cache] read %llu lines\n", (unsigned long long)lines_read);
        if (ui_enabled) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
            long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
            if (ms >= 30) {
                last_draw = now;
                int cols, rows; getmaxyx(stdscr, rows, cols);
                int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200;
                char bar[256]; memset(bar, '.', (size_t)barlen);
                int filled = 0; int percent = 0;
                if (total_bytes > 0) {
                    long cur = ftell(f); if (cur < 0) cur = 0;
                    double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
                    filled = (int)(frac * barlen);
                    percent = (int)(frac * 100.0 + 0.5);
                } else { spinner = (spinner + 1) % barlen; filled = spinner; }
                if (filled < 0) filled = 0; if (filled > barlen) filled = barlen;
                for (int i = 0; i < filled; i++) bar[i] = '#'; bar[barlen] = '\0';
                char linebuf[PATH_MAX + 256];
                snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path);
                mvhline(rows-1, 0, ' ', cols);
                mvaddnstr(rows-1, 0, linebuf, cols-1);
                refresh();
            }
        }
    }
    if (g_headless || debug_cache) fprintf(stderr, "[cache] done lines=%llu\n", (unsigned long long)lines_read);
    // Clear progress line if UI is active
    if (ui_enabled) { int cols, rows; getmaxyx(stdscr, rows, cols); mvhline(rows-1, 0, ' ', cols); refresh(); }
    free(line);
    fclose(f);
    free(cache_path);
    // set runtime totals from cache if present
    if (totals_files > 0ULL) g_last_files = totals_files;
    return 1; // loaded
}

// ------------------------------
// Scanner
// ------------------------------
/*
 * Directory scanning:
 * - scan_dir_recursive_fd: walks the filesystem starting from a dirfd,
 *   avoids symlinks and sums regular file sizes. Updates the cache for
 *   every visited directory, tracking inode/mtime for invalidation.
 * - scan_dir_parallel_deep: performs a multi-threaded deep work-queue
 *   scan while keeping global stats.
 * Both variants skip the cache file at the scan root.
 */
static atomic_ullong g_progress_count = 0;
static atomic_int     g_active_workers = 0;
static atomic_ullong g_total_files = 0;
static atomic_ullong g_total_bytes = 0;
static unsigned long long g_last_files = 0ULL;
static unsigned long long g_last_bytes = 0ULL;

static int is_symlink_lstat(const char *path, struct stat *st) {
    struct stat lst;
    if (lstat(path, &lst) != 0) return 0;
    if (st) *st = lst;
    return S_ISLNK(lst.st_mode);
}

typedef struct {
    int enabled;
    unsigned long long count;   // entries visited
    unsigned long long files;   // files processed
    unsigned long long bytes;   // bytes summed
    int active;                 // active workers
    int pending;                // pending tasks
    struct timespec last_draw;
    const char *phase; // e.g. "Scansione"
} ScanUI;

// Copy progress UI
typedef struct {
    int enabled;
    unsigned long long total;
    unsigned long long done;
    struct timespec last_draw;
    const char *phase; // e.g. "Copy"
} CopyUI;

static void draw_copy_progress(CopyUI *ui, const char *current_path) {
    if (!ui || !ui->enabled) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - ui->last_draw.tv_sec) * 1000 + (now.tv_nsec - ui->last_draw.tv_nsec) / 1000000;
    if (ms < 30) return;
    ui->last_draw = now;
    int cols, rows; getmaxyx(stdscr, rows, cols);
    char bar[256];
    int barlen = cols > 40 ? (cols - 40) : 20;
    if (barlen > (int)sizeof(bar) - 1) barlen = (int)sizeof(bar) - 1;
    double frac = (ui->total > 0) ? ((double)ui->done / (double)ui->total) : 0.0;
    if (frac > 1.0) frac = 1.0;
    int filled = (int)(frac * barlen);
    if (filled < 0) filled = 0; if (filled > barlen) filled = barlen;
    for (int i=0;i<barlen;i++) bar[i] = (i < filled) ? '#' : '.';
    bar[barlen] = '\0';
    char donebuf[64], totalbuf[64];
    human_size((unsigned long long)ui->done, donebuf, sizeof(donebuf));
    human_size((unsigned long long)ui->total, totalbuf, sizeof(totalbuf));
    int percent = (int)(frac * 100.0 + 0.5);
    char line[PATH_MAX + 256];
    snprintf(line, sizeof(line), " %s: [%s] %s / %s (%d%%) - %s",
             ui->phase ? ui->phase : "Copy",
             bar, donebuf, totalbuf, percent,
             current_path ? current_path : "");
    int y = rows - 1;
    mvhline(y, 0, ' ', cols);
    mvaddnstr(y, 0, line, cols-1);
    refresh();
}

/*
 * draw_progress_ui
 * -----------------
 * Periodically redraws (throttle ~33fps) the bottom status line showing
 * a progress bar, counters and the current path. Throttling avoids
 * flickering by skipping frames when too soon since the last draw.
 */
static void draw_progress_ui(ScanUI *ui, const char *current_path) {
    if (!ui || !ui->enabled) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - ui->last_draw.tv_sec) * 1000 + (now.tv_nsec - ui->last_draw.tv_nsec) / 1000000;
    if (ms < 30) return; // throttle ~33fps
    ui->last_draw = now;
    int cols, rows; getmaxyx(stdscr, rows, cols);
    char bar[256];
    int barlen = cols > 40 ? (cols - 40) : 20;
    if (barlen > (int)sizeof(bar) - 1) barlen = (int)sizeof(bar) - 1;
    memset(bar, '.', barlen);
    int pos = (int)(ui->count % (unsigned long long)barlen);
    for (int i = 0; i <= pos && i < barlen; i++) bar[i] = '#';
    bar[barlen] = '\0';
    char sizebuf[64]; human_size((unsigned long long)ui->bytes, sizebuf, sizeof(sizebuf));
    char line[PATH_MAX + 512];
    // Example: Scansione: [#####.....] entries:12345 tasks 3/27 files:6789 size:12.3 GiB - /path
    snprintf(line, sizeof(line), " %s: [%s] entries:%llu tasks %d/%d files:%llu size:%s - %s",
             ui->phase ? ui->phase : "Scanning",
             bar,
             (unsigned long long)ui->count,
             ui->active, ui->pending,
             (unsigned long long)ui->files,
             sizebuf,
             current_path);
    int y = rows - 1;
    mvhline(y, 0, ' ', cols);
    mvaddnstr(y, 0, line, cols-1);
    refresh();
}

/*
 * scan_dir_recursive_fd
 * ---------------------
 * Parameters:
 *  - dirfd: open directory file descriptor (O_DIRECTORY)
 *  - abs_path: absolute path of the current directory
 *  - root: scan root (to skip the cache file)
 *  - cache_file_abs: absolute path to the cache file to exclude
 *  - cache: shared cache pointer (thread-safe on upsert/get)
 *  - ui: optional, to update progress
 *
 * Behavior:
 *  - Sums sizes of regular files robustly (fstatat)
 *  - Skips symlinks (AT_SYMLINK_NOFOLLOW, DT_LNK/S_ISLNK checks)
 *  - For subdirectories, recurses and updates the cache
 */
static unsigned long long scan_dir_recursive_fd(int dirfd, const char *abs_path, const char *root, const char *cache_file_abs, Cache *cache, ScanUI *ui) {
    unsigned long long total = 0ULL;
    int dupfd = dup(dirfd);
    if (dupfd < 0) return 0ULL;
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); return 0ULL; }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (strcmp(abs_path, root) == 0 && strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        unsigned char dtype = de->d_type;
        if (dtype == DT_LNK) continue;
            if (dtype == DT_UNKNOWN) {
                struct stat st0;
                if (fstatat(dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (S_ISLNK(st0.st_mode)) continue;
                if (S_ISDIR(st0.st_mode)) dtype = DT_DIR; else if (S_ISREG(st0.st_mode)) dtype = DT_REG; else dtype = DT_UNKNOWN;
                if (dtype == DT_REG) total += file_size_bytes(&st0);
            } else if (dtype == DT_REG) {
                struct stat stf;
                if (fstatat(dirfd, de->d_name, &stf, AT_SYMLINK_NOFOLLOW) == 0) total += file_size_bytes(&stf);
            } else if (dtype == DT_DIR) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd < 0) continue;
            char *child_abs = path_join(abs_path, de->d_name);
            if (!child_abs) { close(cfd); continue; }
            unsigned long long sub = scan_dir_recursive_fd(cfd, child_abs, root, cache_file_abs, cache, ui);
            struct stat stch;
            if (fstat(cfd, &stch) == 0) {
                CacheEntry *e = cache_upsert(cache, root, child_abs, sub, time(NULL));
                if (e) { e->ino = (unsigned long long)stch.st_ino; e->dir_mtime = stch.st_mtime; }
            }
            total += sub;
            free(child_abs);
            close(cfd);
        }
        if (ui && ui->enabled) { ui->count++; draw_progress_ui(ui, abs_path); }
    }
    closedir(dp);
    struct stat stc;
    if (fstat(dirfd, &stc) == 0) {
        CacheEntry *e = cache_upsert(cache, root, abs_path, total, time(NULL));
        if (e) { e->ino = (unsigned long long)stc.st_ino; e->dir_mtime = stc.st_mtime; }
    } else {
        cache_upsert(cache, root, abs_path, total, time(NULL));
    }
    return total;
}

static unsigned long long scan_dir_recursive(const char *dir, const char *root, const char *cache_file_abs, Cache *cache, ScanUI *ui) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0ULL;
    unsigned long long total = scan_dir_recursive_fd(fd, dir, root, cache_file_abs, cache, ui);
    close(fd);
    return total;
}

// ------------------------------
// Directory view / UI data
// ------------------------------
/*
 * The directory view (DirView) collects current entries: name, path,
 * type (file/dir), known size flag and mtime for files. Directory sizes
 * come from the cache; when missing, the size is "unknown" until scanned.
 *
 * Sorting is configurable (by size or name, asc/desc), and filtering can
 * restrict the list to files or directories. A case-insensitive in-memory
 * search lets you quickly jump the selection.
 */
typedef struct {
    char *name;
    char *abs_path;
    int is_dir;
    unsigned long long size;
    int size_known;
    time_t mtime;
} ViewEntry;

static int selection_changed = 0;

typedef struct {
    ViewEntry *v;
    size_t n;
    size_t cap;
    size_t selected;
    char *path; // abs
} DirView;

// Navigation stack to restore selection/top when returning to parent
typedef struct {
    char *parent_path;
    char *child_path; // path we entered from parent
    size_t selected;
    int top;
} NavState;

typedef struct {
    NavState *v;
    size_t n;
    size_t cap;
} NavStack;

static void navstack_init(NavStack *s) { s->v = NULL; s->n = s->cap = 0; }
static void navstack_free(NavStack *s) {
    for (size_t i = 0; i < s->n; i++) { free(s->v[i].parent_path); free(s->v[i].child_path); }
    free(s->v); s->v = NULL; s->n = s->cap = 0;
}
static void navstack_push(NavStack *s, const char *parent_path, const char *child_path, size_t selected, int top) {
    if (s->n == s->cap) { size_t nc = s->cap ? s->cap * 2 : 32; void *nv = realloc(s->v, nc * sizeof(NavState)); if (!nv) return; s->v = (NavState*)nv; s->cap = nc; }
    NavState *st = &s->v[s->n++]; st->parent_path = xstrdup(parent_path); st->child_path = xstrdup(child_path); st->selected = selected; st->top = top;
}
static int navstack_pop(NavStack *s, NavState *out) {
    if (s->n == 0) return 0;
    *out = s->v[--s->n];
    return 1;
}

// Mark set for selecting multiple items
typedef struct {
    char **paths;
    size_t n;
    size_t cap;
} MarkSet;

// Forward declaration needed for markset normalization helpers
static int path_is_parent_of(const char *parent, const char *child);

// Async marked-files calculator state
static atomic_int g_mf_inflight = 0;               // 1 when a background count is running
static atomic_ullong g_marked_files_value = 0ULL;  // last computed value (eventually consistent)

static void schedule_marked_files_recalc(void);

static void markset_init(MarkSet *m) { m->paths = NULL; m->n = m->cap = 0; }
static void markset_free(MarkSet *m) { for (size_t i=0;i<m->n;i++) free(m->paths[i]); free(m->paths); m->paths=NULL; m->n=m->cap=0; }
static void markset_clear(MarkSet *m) { for (size_t i=0;i<m->n;i++) free(m->paths[i]); m->n=0; schedule_marked_files_recalc(); }
static int markset_index_of(MarkSet *m, const char *p) { for (size_t i=0;i<m->n;i++) if (strcmp(m->paths[i], p)==0) return (int)i; return -1; }
static int markset_has(MarkSet *m, const char *p) { return markset_index_of(m,p) >= 0; }
static int markset_covers(MarkSet *m, const char *p) { for (size_t i=0;i<m->n;i++) { if (path_is_parent_of(m->paths[i], p)) return 1; } return 0; }
static void markset_add(MarkSet *m, const char *p) {
    // If already present, nothing to do
    if (markset_has(m, p)) return;
    // If any existing mark is a parent of p, do not add p (avoid redundant subpath)
    for (size_t i = 0; i < m->n; i++) {
        if (path_is_parent_of(m->paths[i], p)) return;
    }
    // Remove any existing marks that are subpaths of p
    size_t w = 0; size_t lp = strlen(p);
    for (size_t i = 0; i < m->n; i++) {
        if (path_is_parent_of(p, m->paths[i])) { free(m->paths[i]); continue; }
        if (w != i) m->paths[w] = m->paths[i];
        w++;
    }
    m->n = w;
    // Append p
    if (m->n == m->cap) { size_t nc = m->cap ? m->cap * 2 : 64; void *nv = realloc(m->paths, nc * sizeof(char*)); if (!nv) return; m->paths = (char**)nv; m->cap = nc; }
    m->paths[m->n++] = xstrdup(p);
    schedule_marked_files_recalc();
}
static void markset_remove(MarkSet *m, const char *p) { int i=markset_index_of(m,p); if(i<0) return; free(m->paths[i]); if((size_t)i<m->n-1) memmove(&m->paths[i], &m->paths[i+1], (m->n-i-1)*sizeof(char*)); m->n--; schedule_marked_files_recalc(); }
static void markset_remove_prefix(MarkSet *m, const char *prefix) { size_t w=0; size_t lp=strlen(prefix); for(size_t i=0;i<m->n;i++){ if (strncmp(m->paths[i], prefix, lp)==0) { free(m->paths[i]); continue; } if (w!=i) m->paths[w]=m->paths[i]; w++; } m->n=w; schedule_marked_files_recalc(); }

static void view_free(DirView *dv) {
    for (size_t i = 0; i < dv->n; i++) {
        free(dv->v[i].name);
        free(dv->v[i].abs_path);
    }
    free(dv->v);
    dv->v = NULL; dv->n = dv->cap = dv->selected = 0;
    free(dv->path); dv->path = NULL;
}

// Global sort mode
typedef enum { SORT_SIZE = 0, SORT_NAME = 1, SORT_MTIME = 2 } SortMode;
static SortMode g_sort_mode = SORT_SIZE;
static const char *sort_mode_label(void) {
    switch (g_sort_mode) {
        case SORT_NAME: return "name";
        case SORT_MTIME: return "mtime";
        default: return "size";
    }
}

// Sort order flag
static int g_sort_desc = 1; // 1=desc, 0=asc
// Return sort label with order
static void sort_label_with_order(char *out, size_t outsz) {
    const char *key = sort_mode_label();
    const char *ord = g_sort_desc ? "desc" : "asc";
    snprintf(out, outsz, "%s %s", key, ord);
}

// Filter mode
typedef enum { FILTER_ALL = 0, FILTER_DIRS = 1, FILTER_FILES = 2 } FilterMode;
static FilterMode g_filter_mode = FILTER_ALL;
static int g_filter_by_query = 0; // if 1, show only entries matching g_search_query
static const char *filter_mode_label(void) {
    switch (g_filter_mode) {
        case FILTER_DIRS: return "dirs";
        case FILTER_FILES: return "files";
        default: return "all";
    }
}

// Search state
static char g_search_query[256] = "";
static int g_regex_enabled = 0;
static regex_t g_regex;

// Info column display mode (non-space info)
// - mtime: show last modification time
// - owner+perm: show owner and octal permissions
// - hidden: hide the right column entirely
typedef enum { INFOCOL_MTIME = 0, INFOCOL_OWNER_PERM = 1, INFOCOL_HIDDEN = 2 } InfoColMode;
static InfoColMode g_info_col_mode = INFOCOL_MTIME;

// Space display mode for right column (overrides info mode when active)
// - OFF: do not show right column
// - NUM: show per-entry human size
// - PCT: show per-entry percentage of total size in current view
typedef enum { DISP_OFF = 0, DISP_NUM = 1, DISP_PCT = 2 } DisplayMode;
static DisplayMode g_display_mode = DISP_NUM;

static int cmp_entries(const void *a, const void *b) {
    const ViewEntry *ea = (const ViewEntry*)a;
    const ViewEntry *eb = (const ViewEntry*)b;
    if (g_sort_mode == SORT_NAME) {
        // pure alphabetical with asc/desc toggle
        int c = strcmp(ea->name, eb->name);
        return g_sort_desc ? -c : c;
    } else if (g_sort_mode == SORT_MTIME) {
        // sort by modification time; newer first if desc
        if (ea->mtime == eb->mtime) {
            // stable tie-breaker by name
            int c = strcmp(ea->name, eb->name);
            return g_sort_desc ? -c : c;
        }
        if (g_sort_desc) return (ea->mtime < eb->mtime) ? 1 : -1; // newer first
        else return (ea->mtime < eb->mtime) ? -1 : 1; // older first
    } else {
        // pure size with asc/desc toggle; unknown sizes last
        if (ea->size_known != eb->size_known) return eb->size_known - ea->size_known; // known first
        if (ea->size_known && eb->size_known) {
            if (ea->size == eb->size) return strcmp(ea->name, eb->name);
            if (g_sort_desc) return (ea->size < eb->size) ? 1 : -1; // desc
            else return (ea->size < eb->size) ? -1 : 1; // asc
        }
        // both unknown, fallback by name asc
        return strcmp(ea->name, eb->name);
    }
}

/*
 * build_dir_view
 * --------------
 * Builds the list of entries to display in the TUI for 'path'. Applies
 * filters, populates sizes (from cache for dirs, from filesystem for
 * files), and sorts according to current preferences.
 * Returns 0 on success, -1 if the directory cannot be opened.
 */
static int build_dir_view(const char *path, const char *root, Cache *cache, DirView *out) {
    memset(out, 0, sizeof(*out));
    out->path = xstrdup(path);
    DIR *dp = opendir(path);
    if (!dp) return -1;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (strcmp(de->d_name, CACHE_FILENAME) == 0 && strcmp(path, root) == 0) continue; // hide cache file at root
        char *abs = path_join(path, de->d_name);
        if (!abs) continue;
        unsigned char dtype = de->d_type;
        struct stat st;
        memset(&st, 0, sizeof(st));
        if (dtype == DT_UNKNOWN || dtype == DT_REG) {
            if (lstat(abs, &st) != 0) { free(abs); continue; }
            if (S_ISLNK(st.st_mode)) { free(abs); continue; }
            if (dtype == DT_UNKNOWN) dtype = S_ISDIR(st.st_mode) ? DT_DIR : (S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN);
        }
        int is_dir = (dtype == DT_DIR) ? 1 : 0;
        // ensure we have stat info for directories too to get mtime
        if (dtype == DT_DIR) {
            if (lstat(abs, &st) != 0) { free(abs); continue; }
        }
        // Filter
        int include = 1;
        if (g_filter_mode == FILTER_DIRS && !is_dir) include = 0;
        if (g_filter_mode == FILTER_FILES && is_dir) include = 0;
        if (g_filter_by_query) {
            if (g_regex_enabled) {
                if (regexec(&g_regex, de->d_name, 0, NULL, 0) != 0) include = 0;
            } else {
                if (g_search_query[0] != '\0' && !strcasestr_bool(de->d_name, g_search_query)) include = 0;
            }
        }
        if (!include) { free(abs); continue; }
        if (out->n == out->cap) {
            size_t newcap = out->cap ? out->cap * 2 : 128;
            void *nv = realloc(out->v, newcap * sizeof(ViewEntry));
            if (!nv) { free(abs); continue; }
            out->v = (ViewEntry*)nv; out->cap = newcap;
        }
        ViewEntry *ve = &out->v[out->n++];
        ve->name = xstrdup(de->d_name);
        ve->abs_path = abs;
        ve->is_dir = is_dir;
        // record mtime from stat for both files and directories
        if (dtype == DT_REG || dtype == DT_DIR) ve->mtime = st.st_mtime; else ve->mtime = time(NULL);
        if (ve->is_dir) {
            CacheEntry *ce = cache_get(cache, ve->abs_path);
            if (ce) { ve->size = ce->size; ve->size_known = 1; }
            else { ve->size = 0ULL; ve->size_known = 0; }
        } else if (dtype == DT_REG) {
            ve->size = file_size_bytes(&st);
            ve->size_known = 1;
        } else {
            ve->size = 0ULL; ve->size_known = 1;
        }
    }
    closedir(dp);
    qsort(out->v, out->n, sizeof(ViewEntry), cmp_entries);
    return 0;
}

// Utility: sum sizes for copy estimation (fd-based)
// Count files recursively (regular files only) using fd-based traversal
static unsigned long long count_dir_files_fd(int dirfd) {
    unsigned long long count = 0ULL;
    int dupfd = dup(dirfd);
    if (dupfd < 0) return 0ULL;
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); return 0ULL; }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        unsigned char dt = de->d_type;
        if (dt == DT_LNK) continue;
        if (dt == DT_REG) {
            count++;
        } else if (dt == DT_UNKNOWN) {
            struct stat st0;
            if (fstatat(dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISREG(st0.st_mode)) count++;
                else if (S_ISDIR(st0.st_mode)) {
                    int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (cfd >= 0) { count += count_dir_files_fd(cfd); close(cfd); }
                }
            }
        } else if (dt == DT_DIR) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) { count += count_dir_files_fd(cfd); close(cfd); }
        }
    }
    closedir(dp);
    return count;
}

static unsigned long long sum_dir_sizes_fd(int dirfd) {
    unsigned long long total = 0ULL;
    int dupfd = dup(dirfd);
    if (dupfd < 0) return 0ULL;
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); return 0ULL; }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        unsigned char dt = de->d_type;
        if (dt == DT_LNK) continue;
        if (dt == DT_REG) {
            struct stat st;
            if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) total += file_size_bytes(&st);
        } else if (dt == DT_UNKNOWN) {
            struct stat st0;
            if (fstatat(dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISREG(st0.st_mode)) total += file_size_bytes(&st0);
                else if (S_ISDIR(st0.st_mode)) {
                    int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (cfd >= 0) { total += sum_dir_sizes_fd(cfd); close(cfd); }
                }
            }
        } else if (dt == DT_DIR) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) { total += sum_dir_sizes_fd(cfd); close(cfd); }
        }
    }
    closedir(dp);
    return total;
}

static unsigned long long sum_path_size(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0ULL;
    if (S_ISREG(st.st_mode)) return file_size_bytes(&st);
    if (S_ISDIR(st.st_mode)) {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return 0ULL;
        unsigned long long s = sum_dir_sizes_fd(fd);
        close(fd);
        return s;
    }
    return 0ULL;
}

static unsigned long long count_files_path(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0ULL;
    if (S_ISREG(st.st_mode)) return 1ULL;
    if (S_ISDIR(st.st_mode)) {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return 0ULL;
        unsigned long long c = count_dir_files_fd(fd);
        close(fd);
        return c;
    }
    return 0ULL;
}

static int copy_file_with_progress(const char *src, const char *dst, CopyUI *ui) {
    int sfd = open(src, O_RDONLY | O_NOFOLLOW);
    if (sfd < 0) return -1;
    struct stat st; fstat(sfd, &st);
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dfd < 0) { close(sfd); return -1; }
    const size_t BUFSZ = 1<<20; // 1 MiB
    char *buf = malloc(BUFSZ);
    if (!buf) { close(sfd); close(dfd); return -1; }
    ssize_t r;
    while ((r = read(sfd, buf, BUFSZ)) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(dfd, buf + off, (size_t)(r - off));
            if (w < 0) { free(buf); close(sfd); close(dfd); return -1; }
            off += w;
            if (ui) {
                ui->done += (unsigned long long)w;
                draw_copy_progress(ui, dst);
            }
        }
    }
    free(buf);
    close(sfd);
    close(dfd);
    return (r < 0) ? -1 : 0;
}

static int copy_tree_with_progress(const char *src, const char *dst, CopyUI *ui, const char *root) {
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    if (S_ISREG(st.st_mode)) {
        // ensure parent dir exists assumed
        return copy_file_with_progress(src, dst, ui);
    } else if (S_ISDIR(st.st_mode)) {
        // create dst dir
        mkdir(dst, st.st_mode & 0777);
        int sfd = open(src, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (sfd < 0) return -1;
        DIR *dp = fdopendir(sfd);
        if (!dp) { close(sfd); return -1; }
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (is_dot_or_dotdot(de->d_name)) continue;
            if (strcmp(de->d_name, CACHE_FILENAME) == 0 && strcmp(src, root) == 0) continue;
            unsigned char dt = de->d_type;
            if (dt == DT_LNK) continue;
            char *child_src = path_join(src, de->d_name);
            char *child_dst = path_join(dst, de->d_name);
            if (!child_src || !child_dst) { free(child_src); free(child_dst); closedir(dp); return -1; }
            int rc;
            if (dt == DT_DIR || dt == DT_UNKNOWN) {
                struct stat stc;
                if (dt == DT_UNKNOWN) { if (lstat(child_src, &stc) != 0) { free(child_src); free(child_dst); continue; } }
                rc = copy_tree_with_progress(child_src, child_dst, ui, root);
            } else {
                rc = copy_file_with_progress(child_src, child_dst, ui);
            }
            free(child_src); free(child_dst);
            if (rc != 0) { /* continue best-effort */ }
        }
        closedir(dp);
        return 0;
    }
    return -1;
}

// ------------------------------
// TUI
// ------------------------------
static MarkSet g_marks; // global marks set for UI
// Forward decl for marked total
static unsigned long long compute_marked_total_bytes(Cache *cache);
static int is_subpath_of_any_marked(const char *p);

static void draw_header(const char *root, const char *cur, Cache *cache) {
    int cols; int rows; getmaxyx(stdscr, rows, cols);
    attron(COLOR_PAIR(1));
    mvhline(0, 0, ' ', cols);
    char relbuf[PATH_MAX];
    if (starts_with(cur, root)) {
        size_t lr = strlen(root);
        const char *rel = (strcmp(cur, root) == 0) ? "." : (cur[lr] == '/' ? cur + lr + 1 : cur);
        snprintf(relbuf, sizeof(relbuf), "%s", rel);
    } else {
        snprintf(relbuf, sizeof(relbuf), "%s", cur);
    }
    char sortbuf[32]; sort_label_with_order(sortbuf, sizeof(sortbuf));
    char title[PATH_MAX + 256];
    snprintf(title, sizeof(title), " fastdu - root: %s - cwd: %s - sort: %s - filter: %s  ", root, relbuf, sortbuf, filter_mode_label());
    mvaddnstr(0, 0, title, cols-1);
    attroff(COLOR_PAIR(1));

    attron(COLOR_PAIR(1));
    mvhline(rows-2, 0, ' ', cols);
    char totalbuf[64];
    human_size((unsigned long long)g_last_bytes, totalbuf, sizeof(totalbuf));
    char markedbuf[64] = "";
    unsigned long long marked_files_cached = atomic_load(&g_marked_files_value);
    if (g_marks.n > 0) {
        unsigned long long mt = compute_marked_total_bytes(cache);
        human_size(mt, markedbuf, sizeof(markedbuf));
    }
    char footer[512];
    if (g_search_query[0]) {
        char qbuf[128];
        snprintf(qbuf, sizeof(qbuf), " | query:%s", g_search_query);
        if (g_marks.n > 0)
            snprintf(footer, sizeof(footer), " h help | files:%llu | size:%s | marked:%zu (%s, %llu files)%s ", (unsigned long long)g_last_files, totalbuf, g_marks.n, markedbuf, (unsigned long long)marked_files_cached, qbuf);
        else
            snprintf(footer, sizeof(footer), " h help | files:%llu | size:%s | marked:%zu%s ", (unsigned long long)g_last_files, totalbuf, g_marks.n, qbuf);
    } else {
        if (g_marks.n > 0)
            snprintf(footer, sizeof(footer), " h help | files:%llu | size:%s | marked:%zu (%s, %llu files) ", (unsigned long long)g_last_files, totalbuf, g_marks.n, markedbuf, (unsigned long long)marked_files_cached);
        else
            snprintf(footer, sizeof(footer), " h help | files:%llu | size:%s | marked:%zu ", (unsigned long long)g_last_files, totalbuf, g_marks.n);
    }
    mvaddnstr(rows-2, 0, footer, cols-1);
    attroff(COLOR_PAIR(1));
}

static int compute_size_col_width(const DirView *dv) {
    // Display mode controls left size column width
    if (g_display_mode == DISP_OFF) return 0;
    if (g_display_mode == DISP_PCT) {
        // "100.0%" fits in 7 chars; keep a minimum width of 6
        return 7;
    }
    int w = 1;
    for (size_t i = 0; i < dv->n; i++) {
        char b[64];
        if (dv->v[i].size_known) human_size(dv->v[i].size, b, sizeof(b)); else snprintf(b, sizeof(b), "?");
        int lw = (int)strlen(b);
        if (lw > w) w = lw;
    }
    if (w < 4) w = 4;
    if (w > 12) w = 12;
    return w;
}

// Format owner and permissions (octal) for a path into out buffer.
// Example: "user 0755"; falls back to numeric uid if name not found.
static void format_owner_perm(const char *path, char *out, size_t outsz) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        snprintf(out, outsz, "-");
        return;
    }
    struct passwd *pw = getpwuid(st.st_uid);
    const char *uname = pw && pw->pw_name ? pw->pw_name : NULL;
    unsigned mode_octal = (unsigned)(st.st_mode & 07777);
    if (uname) snprintf(out, outsz, "%s %04o", uname, mode_octal);
    else snprintf(out, outsz, "%u %04o", (unsigned)st.st_uid, mode_octal);
}

/*
 * draw_list
 * ---------
 * Renders rows with columns: [mark] [size] [type] [name] [last modified].
 * - Sizes are right-aligned within a dynamic width.
 * - Last modified timestamp is right-aligned and anchored to the right edge.
 * - Uses distinct colors for directories and files.
 * - Highlights the selected row with reverse + bold.
 */
static void draw_list(const DirView *dv, int top) {
    int cols; int rows; getmaxyx(stdscr, rows, cols);
    int y = 1; // below header
    int list_rows = rows - 3; // header + footer
    int sizew = compute_size_col_width(dv);
    int mark_col = 0; // mark column at 0
    int size_col = 2; // mark + space
    int type_col = size_col + (sizew > 0 ? (sizew + 1) : 0); // if size hidden, no gap
    int name_col = type_col + 2; // type char + space
    // Right column anchored to right; width depends only on info mode
    const int info_w_default = 16; // fits date comfortably
    int info_w = (g_info_col_mode == INFOCOL_HIDDEN) ? 0 : info_w_default;
    int info_col = cols - info_w - 1; if (info_col < name_col + 4) info_col = name_col + 4;

    // Precompute total size for percentage mode (sum of known sizes in view)
    unsigned long long view_total = 0ULL;
    if (g_display_mode == DISP_PCT) {
        for (size_t i = 0; i < dv->n; i++) {
            if (dv->v[i].size_known) view_total += dv->v[i].size;
        }
        if (view_total == 0ULL) view_total = 1ULL; // avoid div-by-zero
    }

    for (int i = 0; i < list_rows; i++) {
        int idx = top + i;
        int is_sel = ((size_t)idx == dv->selected);
        if (is_sel) attron(A_REVERSE | A_BOLD);
        mvhline(y + i, 0, ' ', cols);
        if ((size_t)idx >= dv->n) {
            if (is_sel) attroff(A_REVERSE | A_BOLD);
            continue;
        }
        const ViewEntry *ve = &dv->v[idx];
        // left size/percent column
        char sizebuf[64] = "";
        if (sizew > 0) {
            if (g_display_mode == DISP_PCT) {
                if (ve->size_known) {
                    double pct = (double)ve->size * 100.0 / (double)view_total;
                    if (pct > 999.9) pct = 999.9;
                    snprintf(sizebuf, sizeof(sizebuf), "%5.1f%%", pct);
                } else {
                    snprintf(sizebuf, sizeof(sizebuf), "  --.-%%");
                }
            } else { // numeric
                if (ve->size_known) human_size(ve->size, sizebuf, sizeof(sizebuf)); else snprintf(sizebuf, sizeof(sizebuf), "?");
            }
        }
        // right-align size in its column
        int l = (int)strlen(sizebuf);
        int pad = sizew - l; if (pad < 0) pad = 0;
        // mark column: show '*' if explicitly marked, '+' if inherits from a parent
        char mchar = ' ';
        if (markset_has(&g_marks, ve->abs_path)) mchar = '*';
        else if (markset_covers(&g_marks, ve->abs_path)) mchar = '+';
        mvaddch(y + i, mark_col, mchar);
        mvaddch(y + i, mark_col + 1, ' ');
        // size column at x=size_col
        if (sizew > 0) {
            if (pad) { for (int k = 0; k < pad; k++) mvaddch(y + i, size_col + k, ' '); }
            mvaddnstr(y + i, size_col + pad, sizebuf, sizew - pad);
        }
        // type
        mvaddch(y + i, type_col, ve->is_dir ? 'D' : 'F');
        // space after type
        mvaddch(y + i, type_col + 1, ' ');
        // name uses remaining space up to right column - 1 (or full width if hidden)
        if (ve->is_dir) attron(COLOR_PAIR(3)); else attron(COLOR_PAIR(4));
        int name_max = (g_info_col_mode == INFOCOL_HIDDEN) ? (cols - name_col - 1) : (info_col - name_col - 1);
        if (name_max < 0) name_max = 0;
        mvaddnstr(y + i, name_col, ve->name, name_max);
        if (ve->is_dir) attroff(COLOR_PAIR(3)); else attroff(COLOR_PAIR(4));
        // right info column content (mtime / owner+perm)
        if (info_w > 0) {
            char ibuf[64]; ibuf[0] = '\0';
            if (g_info_col_mode == INFOCOL_MTIME) {
                struct tm lt; localtime_r(&ve->mtime, &lt);
                strftime(ibuf, sizeof(ibuf), "%Y-%m-%d %H:%M", &lt);
            } else if (g_info_col_mode == INFOCOL_OWNER_PERM) {
                format_owner_perm(ve->abs_path, ibuf, sizeof(ibuf));
            }
            if (ibuf[0] != '\0') {
                int il = (int)strlen(ibuf);
                int ipad = info_w - il; if (ipad < 0) ipad = 0;
                if (ipad) { for (int k = 0; k < ipad; k++) mvaddch(y + i, info_col + k, ' '); }
                mvaddnstr(y + i, info_col + ipad, ibuf, info_w - ipad);
            }
        }
        if (is_sel) attroff(A_REVERSE | A_BOLD);
    }
}

/* Forward declarations for cache ancestor adjustment helpers used below */
static void cache_adjust_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, long long delta);
static void cache_add_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, unsigned long long delta);

/*
 * maybe_rescan_hovered
 * --------------------
 * When the selection is on a directory, perform a debounced check
 * (>=700ms or selection changed). If the directory mtime differs from
 * what is stored in the cache, rescan just that subtree to refresh data.
 */
static void maybe_rescan_hovered(DirView *dv, const char *root, Cache *cache) {
    static char last_path[PATH_MAX] = "";
    static struct timespec last_check = {0,0};

    if (dv->n == 0) return;
    ViewEntry *ve = &dv->v[dv->selected];
    if (!ve->is_dir) return;

    // debounce: only check if selection changed or after 700ms
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
    int changed = strcmp(last_path, ve->abs_path) != 0;
    if (!changed && ms < 700) return;
    last_check = now;
    strncpy(last_path, ve->abs_path, sizeof(last_path)); last_path[sizeof(last_path)-1] = '\0';

    // If directory mtime is newer than cache entry, rescan only that subtree
    CacheEntry *ce = cache_get(cache, ve->abs_path);
    struct stat st;
    if (lstat(ve->abs_path, &st) != 0) return;
    if (!ce || st.st_mtime != ce->dir_mtime) {
        // compute old size
        unsigned long long old_sz = ce ? ce->size : 0ULL;
        char *cache_abs = path_join(root, CACHE_FILENAME);
        unsigned long long sz = scan_dir_recursive(ve->abs_path, root, cache_abs, cache, NULL);
        (void)sz;
        // compute delta and adjust ancestors + footer total
        CacheEntry *ce_new = cache_get(cache, ve->abs_path);
        unsigned long long new_sz = ce_new ? ce_new->size : 0ULL;
        long long delta = (long long)new_sz - (long long)old_sz;
        if (delta > 0) cache_add_ancestors_after_delta(cache, root, ve->abs_path, (unsigned long long)delta);
        else if (delta < 0) cache_adjust_ancestors_after_delta(cache, root, ve->abs_path, (long long)(-delta));
        if (delta >= 0) g_last_bytes += (unsigned long long)delta;
        else { unsigned long long dec = (unsigned long long)(-delta); if (g_last_bytes > dec) g_last_bytes -= dec; else g_last_bytes = 0ULL; }
        cache_save(root, cache);
        free(cache_abs);
        ce = cache_get(cache, ve->abs_path);
        if (ce) { ve->size = ce->size; ve->size_known = 1; }
    }
}

static void draw_status(const char *msg) {
    int cols; int rows; getmaxyx(stdscr, rows, cols);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, msg, cols-1);
    // Note: no pause here; message will be overwritten on next refresh
}

static int prompt_input(char *buf, size_t bufsz, const char *label) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    echo(); curs_set(1);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, label, cols-1);
    int x = (int)strlen(label);
    move(rows-1, x);
    int rc = getnstr(buf, (int)bufsz - 1);
    noecho(); curs_set(0);
    if (rc == ERR) { buf[0] = '\0'; return 0; }
    // Trim trailing spaces
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t' || buf[n-1] == '\r')) { buf[--n] = '\0'; }
    return (int)n;
}

// Background worker to compute marked files count
typedef struct MFJob {
    char **paths;
    size_t n;
} MFJob;

static void *mf_worker(void *arg) {
    MFJob *job = (MFJob*)arg;
    unsigned long long total = 0ULL;
    for (size_t i = 0; i < job->n; i++) {
        total += count_files_path(job->paths[i]);
    }
    for (size_t i = 0; i < job->n; i++) free(job->paths[i]);
    free(job->paths);
    free(job);
    atomic_store(&g_marked_files_value, total);
    atomic_store(&g_mf_inflight, 0);
    return NULL;
}

static void schedule_marked_files_recalc(void) {
    if (atomic_exchange(&g_mf_inflight, 1) == 1) return; // already running
    // snapshot current explicit marks
    if (g_marks.n == 0) { atomic_store(&g_marked_files_value, 0ULL); atomic_store(&g_mf_inflight, 0); return; }
    MFJob *job = (MFJob*)malloc(sizeof(MFJob)); if (!job) { atomic_store(&g_mf_inflight, 0); return; }
    job->n = g_marks.n;
    job->paths = (char**)malloc(job->n * sizeof(char*)); if (!job->paths) { free(job); atomic_store(&g_mf_inflight, 0); return; }
    for (size_t i = 0; i < job->n; i++) job->paths[i] = xstrdup(g_marks.paths[i]);
    pthread_t th;
    if (pthread_create(&th, NULL, mf_worker, job) == 0) {
        pthread_detach(th);
    } else {
        for (size_t i = 0; i < job->n; i++) free(job->paths[i]); free(job->paths); free(job);
        atomic_store(&g_mf_inflight, 0);
    }
}

static int path_is_parent_of(const char *parent, const char *child) {
    size_t lp = strlen(parent);
    if (lp == 0) return 0;
    if (strcmp(parent, child) == 0) return 1; // treat equal as parent for normalization
    if (strncmp(child, parent, lp) != 0) return 0;
    if (child[lp] == '\0') return 1; // exact match
    return child[lp] == '/';
}

static int is_subpath_of_any_marked(const char *p) {
    // After normalization in markset_add, this should rarely be true, but keep for safety
    for (size_t i = 0; i < g_marks.n; i++) {
        if (path_is_parent_of(g_marks.paths[i], p) && strcmp(g_marks.paths[i], p) != 0) return 1;
    }
    return 0;
}

static unsigned long long compute_marked_total_bytes(Cache *cache) {
    // Fast path: do not traverse the filesystem here; use cache only to keep UI responsive.
    // Unknown dirs (not in cache yet) are treated as 0 to avoid blocking.
    unsigned long long total = 0ULL;
    for (size_t i = 0; i < g_marks.n; i++) {
        const char *path = g_marks.paths[i];
        if (is_subpath_of_any_marked(path)) continue;
        struct stat st;
        if (lstat(path, &st) != 0) continue;
        if (S_ISREG(st.st_mode)) {
            total += (unsigned long long)st.st_size;
        } else if (S_ISDIR(st.st_mode)) {
            CacheEntry *ce = cache_get(cache, path);
            if (ce) total += ce->size;
            // else skip (0) to avoid heavy sum_path_size traversal in render loop
        }
    }
    return total;
}

static void show_help(void) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    int w = cols - 8; if (w < 48) w = cols - 4; if (w < 32) w = cols;
    int h = rows - 8; if (h < 16) h = rows - 4; if (h < 8) h = rows;
    int x = (cols - w) / 2; if (x < 0) x = 0;
    int y = (rows - h) / 2; if (y < 0) y = 0;

    // Create overlay window with border
    WINDOW *win = newwin(h, w, y, x);
    keypad(win, TRUE);

    const char *lines[] = {
        "Navigation:",
        "  Up/Down or j/k  - move selection",
        "  Enter or Right/l - open directory",
        "  Backspace or Left - go up",
        "  b - go to top, e - go to end",
        "",
        "Actions:",
        "  v - preview selected text file (scrollable layer)",
        "  r - rescan selected dir",
        "  R - rescan current dir",
        "  f - find by name (case-insensitive), n/N next/prev",
        "  F - regex search (case-insensitive), enables query filter",
        "  t - toggle type filter (all/dirs/files)",
        "  T - toggle filter by query",
        "  SPACE - mark/unmark file/dir",
"  Ctrl-A - select/deselect all in view",
        "  m - move marked to current directory",
        "  c - copy marked to current directory (with progress)",
        "  d - delete marked (if any) else delete selected",
        "  o - toggle sort key (size/name/mtime)",
        "  s - toggle sort order (asc/desc)",
"  I - size display: numeric → percent → off",
"  i - info column (mtime → owner+perm → hidden)",
        "  q - quit",
        "  h - this help",
        "",
        "Regex guide:",
        "  - Matching applies to the entry name only (not the full path)",
        "  - Use 'F' to enter a regex; 'T' toggles the filter on/off",
        "  Examples:",
        "    • TXT files:       \\\.txt$",
        "    • Only directories 'src' or 'docs':  ^(src|docs)$  then press 't' to 'dirs'",
        "    • Names containing '2024-':          2024-",
        "",
        "CLI:",
        "  fastdu [-R|--reload] [-j N|--jobs N] [path]",
        NULL
    };

    // Count lines
    int total_lines = 0; while (lines[total_lines]) total_lines++;
    int view_lines = h - 2; if (view_lines < 1) view_lines = 1;
    int off = 0;

    // Draw loop
    for (;;) {
        werase(win);
        box(win, 0, 0);
        // Title
        wattron(win, A_REVERSE | A_BOLD);
        const char *title = " Help - arrows to scroll, q to close ";
        mvwaddnstr(win, 0, 2, title, w - 4);
        wattroff(win, A_REVERSE | A_BOLD);
        // Content area
        for (int i = 0; i < view_lines; i++) {
            int li = off + i;
            if (li >= total_lines) break;
            mvwaddnstr(win, 1 + i, 2, lines[li], w - 4);
        }
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q') break;
        else if (ch == KEY_UP) { if (off > 0) off--; }
        else if (ch == KEY_DOWN) { if (off + view_lines < total_lines) off++; }
        else if (ch == KEY_PPAGE) { off -= view_lines; if (off < 0) off = 0; }
        else if (ch == KEY_NPAGE) { off += view_lines; if (off + view_lines > total_lines) off = (total_lines > view_lines) ? (total_lines - view_lines) : 0; }
        else if (ch == 'k') { if (off > 0) off--; }
        else if (ch == 'j') { if (off + view_lines < total_lines) off++; }
    }

    delwin(win);
    // Underlying screen will be redrawn by main loop on return
}

/*
 * Text file detection and preview layer
 */
static int is_textual_file(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    unsigned char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) return 1; // empty files are considered text
    // simple UTF-8 aware heuristic
    size_t i = 0; size_t bad = 0; size_t total = (size_t)n;
    while (i < (size_t)n) {
        unsigned char c = buf[i];
        if (c == '\0') { bad++; i++; continue; }
        if (c < 0x20) {
            if (c=='\n' || c=='\r' || c=='\t' || c==0x0C) { i++; continue; }
            bad++; i++; continue;
        }
        if (c < 0x80) { i++; continue; }
        // UTF-8 leading byte
        size_t need = 0;
        if ((c & 0xE0) == 0xC0) need = 1; // 2-byte
        else if ((c & 0xF0) == 0xE0) need = 2; // 3-byte
        else if ((c & 0xF8) == 0xF0) need = 3; // 4-byte
        else { bad++; i++; continue; }
        if (i + need >= (size_t)n) { // truncated sample: assume ok
            break;
        }
        int ok = 1;
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = buf[i+k];
            if ((cc & 0xC0) != 0x80) { ok = 0; break; }
        }
        if (!ok) { bad++; i++; continue; }
        i += need + 1;
    }
    double ratio = (total > 0) ? ((double)bad / (double)total) : 0.0;
    return ratio <= 0.15; // allow up to 15% control/invalid bytes
}

static void show_preview(const char *path) {
    // Try open file and read lines up to a cap
    FILE *fp = fopen(path, "r");
    if (!fp) {
        char msg[PATH_MAX + 64];
        snprintf(msg, sizeof(msg), "Cannot open '%s'", path_basename_const(path));
        draw_status(msg);
        return;
    }

    const size_t MAX_BYTES = 2 * 1024 * 1024; // 2 MiB cap for preview
    const size_t MAX_LINES = 20000;
    char **lines = NULL; size_t nlines = 0, cap = 0;
    size_t bytes = 0;
    size_t max_line_len = 0;
    char *line = NULL; size_t linecap = 0; ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        if (bytes + (size_t)linelen > MAX_BYTES) break;
        if (nlines == cap) { size_t nc = cap ? cap * 2 : 512; void *nv = realloc(lines, nc * sizeof(char*)); if (!nv) break; lines = (char**)nv; cap = nc; }
        // strip CRLF
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) linelen--;
        char *copy = (char*)malloc((size_t)linelen + 1);
        if (!copy) break;
        memcpy(copy, line, (size_t)linelen); copy[linelen] = '\0';
        lines[nlines++] = copy;
        bytes += (size_t)linelen;
        if ((size_t)linelen > max_line_len) max_line_len = (size_t)linelen;
        if (nlines >= MAX_LINES) break;
    }
    free(line);
    fclose(fp);

    int cols, rows; getmaxyx(stdscr, rows, cols);
    int w = cols - 8; if (w < 48) w = cols - 4; if (w < 32) w = cols;
    int h = rows - 8; if (h < 10) h = rows - 4; if (h < 8) h = rows;
    int x = (cols - w) / 2; if (x < 0) x = 0;
    int y = (rows - h) / 2; if (y < 0) y = 0;

    WINDOW *win = newwin(h, w, y, x);
    keypad(win, TRUE);

    int view_lines = h - 2; if (view_lines < 1) view_lines = 1;
    int off = 0;

    // Build initial wrapped lines according to current content width
    int cw = w - 4; if (cw < 1) cw = 1;
    char **wrap = NULL; size_t nwrap = 0, wrapcap = 0;

    #define APPEND_WRAP_SEG(SRC_PTR, LEN_VAL) do { \
        size_t __len = (size_t)(LEN_VAL); \
        char *__seg = (char*)malloc(__len + 1); \
        if (!__seg) break; \
        memcpy(__seg, (SRC_PTR), __len); __seg[__len] = '\0'; \
        if (nwrap == wrapcap) { \
            size_t __nc = wrapcap ? wrapcap * 2 : 1024; \
            void *__nv = realloc(wrap, __nc * sizeof(char*)); \
            if (!__nv) { free(__seg); break; } \
            wrap = (char**)__nv; wrapcap = __nc; \
        } \
        wrap[nwrap++] = __seg; \
    } while (0)

    #define FREE_WRAP() do { \
        for (size_t __i = 0; __i < nwrap; __i++) free(wrap[__i]); \
        free(wrap); wrap = NULL; nwrap = 0; wrapcap = 0; \
    } while (0)

    #define REBUILD_WRAP() do { \
        FREE_WRAP(); \
        if (cw < 1) cw = 1; \
        for (size_t __li = 0; __li < nlines; __li++) { \
            const char *__s = lines[__li]; \
            size_t __len = strlen(__s); \
            if (__len == 0) { APPEND_WRAP_SEG("", 0); continue; } \
            size_t __pos = 0; \
            while (__pos < __len) { \
                size_t __remain = __len - __pos; \
                if (__remain <= (size_t)cw) { \
                    APPEND_WRAP_SEG(__s + __pos, __remain); \
                    __pos = __len; \
                    break; \
                } \
                size_t __take = (size_t)cw; \
                size_t __k = __pos + __take; \
                int __found = 0; \
                while (__k > __pos) { \
                    unsigned char __c = (unsigned char)__s[__k - 1]; \
                    if (__c == ' ' || __c == '\t' || __c == '-' || __c == ',' || __c == ';' || __c == ':' || __c == '.') { __found = 1; break; } \
                    __k--; \
                } \
                size_t __seglen = __found ? (__k - __pos) : __take; \
                if (__seglen == 0) __seglen = __take; \
                APPEND_WRAP_SEG(__s + __pos, __seglen); \
                __pos += __seglen; \
                if (__pos < __len && __s[__pos] == ' ') __pos++; \
            } \
        } \
        if (off > (int)nwrap - 1) off = (int)((nwrap > 0) ? (nwrap - 1) : 0); \
    } while (0)

    REBUILD_WRAP();

    int wrap_on = 1; // wrapping abilitato di default
    int h_off = 0;   // offset orizzontale quando wrapping è OFF

    char title[PATH_MAX + 64];
    const char *base = path_basename_const(path);
    snprintf(title, sizeof(title), " Preview: %s - q to close ", base);

    int last_cols = cols, last_rows = rows, last_w = w, last_h = h;

    for (;;) {
        // Handle terminal resize: rebuild window and wrapping if necessario
        getmaxyx(stdscr, rows, cols);
        int nw = cols - 8; if (nw < 48) nw = cols - 4; if (nw < 32) nw = cols;
        int nh = rows - 8; if (nh < 10) nh = rows - 4; if (nh < 8) nh = rows;
        if (nw != last_w || nh != last_h) {
            if (win) delwin(win);
            w = nw; h = nh; last_w = nw; last_h = nh;
            int nx = (cols - w) / 2; if (nx < 0) nx = 0;
            int ny = (rows - h) / 2; if (ny < 0) ny = 0;
            x = nx; y = ny;
            win = newwin(h, w, y, x);
            keypad(win, TRUE);
            view_lines = h - 2; if (view_lines < 1) view_lines = 1;
            cw = w - 4; if (cw < 1) cw = 1;
            if (wrap_on) REBUILD_WRAP();
            // clamp horizontal offset when wrap OFF
            if (!wrap_on) {
                int max_h = (int)((max_line_len > (size_t)cw) ? (max_line_len - (size_t)cw) : 0);
                if (h_off > max_h) h_off = max_h;
            }
        }

        werase(win);
        box(win, 0, 0);
        // Title
        snprintf(title, sizeof(title), " Preview: %s %s - q to close ", base, wrap_on ? "[WRAP]" : "[NO WRAP]");
        wattron(win, A_REVERSE | A_BOLD);
        mvwaddnstr(win, 0, 2, title, w - 4);
        wattroff(win, A_REVERSE | A_BOLD);
        // Content
        if (wrap_on) {
            for (int i = 0; i < view_lines; i++) {
                size_t wi = (size_t)off + (size_t)i;
                if (wi >= nwrap) break;
                mvwaddnstr(win, 1 + i, 2, wrap[wi], w - 4);
            }
        } else {
            for (int i = 0; i < view_lines; i++) {
                size_t li = (size_t)off + (size_t)i;
                if (li >= nlines) break;
                const char *s = lines[li];
                size_t sl = strlen(s);
                if ((size_t)h_off >= sl) {
                    // nothing visible on this line
                    continue;
                }
                const char *sp = s + h_off;
                int avail = cw;
                int to_print = (int)((size_t)avail < (sl - (size_t)h_off) ? (size_t)avail : (sl - (size_t)h_off));
                if (to_print > 0) mvwaddnstr(win, 1 + i, 2, sp, to_print);
            }
        }
        // If truncated, show hint in last line
        if (bytes >= MAX_BYTES || nlines >= MAX_LINES) {
            const char *hint = "[truncated preview]";
            mvwaddnstr(win, h - 1, w - (int)strlen(hint) - 2, hint, (int)strlen(hint));
        }
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q' || ch == 27) break;
        else if (ch == 'w' || ch == 'W') {
            // toggle wrapping
            wrap_on = !wrap_on;
            if (wrap_on) { h_off = 0; REBUILD_WRAP(); if (off > (int)nwrap - 1) off = (int)((nwrap > 0) ? (nwrap - 1) : 0); }
            else { h_off = 0; if (off > (int)nlines - 1) off = (int)((nlines > 0) ? (nlines - 1) : 0); }
        }
        else if (ch == KEY_UP || ch == 'k') { if (off > 0) off--; }
        else if (ch == KEY_DOWN || ch == 'j') {
            if (wrap_on) { if (off + view_lines < (int)nwrap) off++; }
            else { if (off + view_lines < (int)nlines) off++; }
        }
        else if (ch == KEY_PPAGE) { off -= view_lines; if (off < 0) off = 0; }
        else if (ch == KEY_NPAGE) {
            if (wrap_on) { off += view_lines; if (off + view_lines > (int)nwrap) off = (nwrap > (size_t)view_lines) ? ((int)nwrap - view_lines) : 0; }
            else { off += view_lines; if (off + view_lines > (int)nlines) off = (nlines > (size_t)view_lines) ? ((int)nlines - view_lines) : 0; }
        }
        else if (ch == 'g') { off = 0; }
        else if (ch == 'G') { if (wrap_on) off = (nwrap > (size_t)view_lines) ? ((int)nwrap - view_lines) : 0; else off = (nlines > (size_t)view_lines) ? ((int)nlines - view_lines) : 0; }
        else if (!wrap_on && (ch == KEY_LEFT || ch == 'h')) { if (h_off > 0) h_off--; }
        else if (!wrap_on && (ch == KEY_RIGHT || ch == 'l')) {
            int max_h = (int)((max_line_len > (size_t)cw) ? (max_line_len - (size_t)cw) : 0);
            if (h_off < max_h) h_off++;
        }
    }

    delwin(win);
    FREE_WRAP();
    for (size_t i = 0; i < nlines; i++) free(lines[i]);
    free(lines);

    #undef APPEND_WRAP_SEG
    #undef FREE_WRAP
    #undef REBUILD_WRAP
}

static int confirm_delete_prompt(const char *name, int is_dir) {
    char prompt[PATH_MAX + 128];
snprintf(prompt, sizeof(prompt), "Delete %c '%s'? [y/N] ", is_dir ? 'D' : 'F', name);
    draw_status(prompt);
    refresh();
    int ch = getch();
    return (ch == 'y' || ch == 'Y');
}

/*
 * remove_tree
 * -----------
 * Recursively deletes a file/directory (no symlink following).
 * Protects the cache file (if provided) to avoid deleting it.
 * Returns 0 on success, -1 on error.
 */
static int remove_tree(const char *path, const char *cache_file_abs) {
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) {
        // do not delete cache file even if requested
        if (cache_file_abs && strcmp(path, cache_file_abs) == 0) return -1;
        return unlink(path);
    }
    if (S_ISDIR(st.st_mode)) {
        // protect against deleting the cache file by path match during walk
        DIR *dp = opendir(path);
        if (!dp) return -1;
        struct dirent *de;
        int rc = 0;
        while ((de = readdir(dp)) != NULL) {
            if (is_dot_or_dotdot(de->d_name)) continue;
            char *p = path_join(path, de->d_name);
            if (!p) { rc = -1; break; }
            if (cache_file_abs && strcmp(p, cache_file_abs) == 0) { free(p); continue; }
            if (remove_tree(p, cache_file_abs) != 0) { free(p); rc = -1; break; }
            free(p);
        }
        closedir(dp);
        if (rc != 0) return -1;
        return rmdir(path);
    }
    // other types
    return -1;
}

static void cache_remove_prefix(Cache *c, const char *prefix) {
    pthread_mutex_lock(&c->mu);
    size_t w = 0;
    for (size_t i = 0; i < c->n; i++) {
        if (starts_with(c->v[i].abs_path, prefix)) {
            free(c->v[i].abs_path);
            free(c->v[i].rel_path);
            continue; // skip
        }
        if (w != i) c->v[w] = c->v[i];
        w++;
    }
    c->n = w;
    pthread_mutex_unlock(&c->mu);
}

static void cache_adjust_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, long long delta) {
    if (delta <= 0) return;
    time_t now = time(NULL);
    char *p = xstrdup(abs_path);
    if (!p) return;
    // start from parent of abs_path
    char *cur = get_parent(p);
    free(p);
    while (cur) {
        CacheEntry *ce = cache_get(c, cur);
        if (ce) {
            if (ce->size > (unsigned long long)delta) ce->size -= (unsigned long long)delta; else ce->size = 0ULL;
            ce->last_scan = now;
            struct stat st;
            if (stat(cur, &st) == 0) ce->dir_mtime = st.st_mtime;
        }
        if (strcmp(cur, root) == 0) break;
        char *parent = get_parent(cur);
        free(cur);
        cur = parent;
    }
    if (cur) free(cur);
}

static void cache_add_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, unsigned long long delta) {
    if (delta == 0) return;
    time_t now = time(NULL);
    char *p = xstrdup(abs_path);
    if (!p) return;
    char *cur = get_parent(p);
    free(p);
    while (cur) {
        CacheEntry *ce = cache_get(c, cur);
        if (ce) {
            ce->size += delta;
            ce->last_scan = now;
            struct stat st; if (stat(cur, &st) == 0) ce->dir_mtime = st.st_mtime;
        }
        if (strcmp(cur, root) == 0) break;
        char *parent = get_parent(cur);
        free(cur);
        cur = parent;
    }
    if (cur) free(cur);
}

static void cache_move_prefix(Cache *c, const char *root, const char *old_prefix, const char *new_prefix) {
    size_t lold = strlen(old_prefix);
    pthread_mutex_lock(&c->mu);
    for (size_t i = 0; i < c->n; i++) {
        if (starts_with(c->v[i].abs_path, old_prefix)) {
            const char *suffix = c->v[i].abs_path + lold;
            char *new_abs = NULL;
            if (suffix[0] == '\0') {
                new_abs = xstrdup(new_prefix);
            } else if (suffix[0] == '/') {
                const char *rest = suffix + 1;
                new_abs = path_join(new_prefix, rest);
            } else {
                new_abs = path_join(new_prefix, suffix);
            }
            if (new_abs) {
                free(c->v[i].abs_path);
                c->v[i].abs_path = new_abs;
                free(c->v[i].rel_path);
                c->v[i].rel_path = relpath_from_abs(root, new_abs);
                struct stat st; if (stat(new_abs, &st) == 0) c->v[i].dir_mtime = st.st_mtime;
                c->v[i].last_scan = time(NULL);
            }
        }
    }
    pthread_mutex_unlock(&c->mu);
}

// ------------------------------
// Parallel scanning helpers (deep work queue)
// ------------------------------
/*
 * Task queue and worker pool:
 * - TaskQueue: bounded blocking circular queue with two condition vars
 * - WaitGroupC: primitive to wait on outstanding work
 * - DirTask: represents a directory to process; accumulates file bytes
 *   and children totals; on finalization updates the cache and propagates
 *   the total to the parent (tracking pending children to cascade finish)
 *
 * Workers run process_task, which visits entries and enqueues child dirs.
 * When a task has no more pending children, finalize_task computes the
 * total, updates cache/parent, then decrements the WaitGroup.
 */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv_nonempty;
    pthread_cond_t cv_nonfull;
    void **items;
    size_t cap, head, tail, count;
    int closed;
} TaskQueue;

static void tq_init(TaskQueue *q, size_t cap) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv_nonempty, NULL);
    pthread_cond_init(&q->cv_nonfull, NULL);
    q->items = malloc(cap * sizeof(void*));
    q->cap = cap; q->head = q->tail = q->count = 0; q->closed = 0;
}
static void tq_close(TaskQueue *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->cv_nonempty);
    pthread_cond_broadcast(&q->cv_nonfull);
    pthread_mutex_unlock(&q->mu);
}
static void tq_destroy(TaskQueue *q) {
    free(q->items);
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv_nonempty);
    pthread_cond_destroy(&q->cv_nonfull);
}

static size_t tq_count(TaskQueue *q) {
    pthread_mutex_lock(&q->mu);
    size_t c = q->count;
    pthread_mutex_unlock(&q->mu);
    return c;
}

static size_t tq_capacity(TaskQueue *q) {
    pthread_mutex_lock(&q->mu);
    size_t c = q->cap;
    pthread_mutex_unlock(&q->mu);
    return c;
}
static int tq_push(TaskQueue *q, void *item) {
    pthread_mutex_lock(&q->mu);
    while (q->count == q->cap && !q->closed) pthread_cond_wait(&q->cv_nonfull, &q->mu);
    if (q->closed) { pthread_mutex_unlock(&q->mu); return 0; }
    q->items[q->tail] = item; q->tail = (q->tail + 1) % q->cap; q->count++;
    pthread_cond_signal(&q->cv_nonempty);
    pthread_mutex_unlock(&q->mu);
    return 1;
}

// Timed push variant to avoid potential stalls under extreme load
static int tq_push_timed(TaskQueue *q, void *item, int timeout_ms) {
    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    long ns = abstime.tv_nsec + (long)timeout_ms * 1000000L;
    abstime.tv_sec += ns / 1000000000L;
    abstime.tv_nsec = ns % 1000000000L;

    pthread_mutex_lock(&q->mu);
    while (q->count == q->cap && !q->closed) {
        int rc = pthread_cond_timedwait(&q->cv_nonfull, &q->mu, &abstime);
        if (rc == ETIMEDOUT) { pthread_mutex_unlock(&q->mu); return 0; }
    }
    if (q->closed) { pthread_mutex_unlock(&q->mu); return 0; }
    q->items[q->tail] = item; q->tail = (q->tail + 1) % q->cap; q->count++;
    pthread_cond_signal(&q->cv_nonempty);
    pthread_mutex_unlock(&q->mu);
    return 1;
}
static void *tq_pop(TaskQueue *q) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) pthread_cond_wait(&q->cv_nonempty, &q->mu);
    if (q->count == 0) { pthread_mutex_unlock(&q->mu); return NULL; }
    void *item = q->items[q->head]; q->head = (q->head + 1) % q->cap; q->count--;
    pthread_cond_signal(&q->cv_nonfull);
    pthread_mutex_unlock(&q->mu);
    return item;
}

typedef struct WaitGroupC {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int value;
} WaitGroupC;
static void wg_init(WaitGroupC *wg) { pthread_mutex_init(&wg->mu, NULL); pthread_cond_init(&wg->cv, NULL); wg->value = 0; }
static void wg_add(WaitGroupC *wg, int delta) { pthread_mutex_lock(&wg->mu); wg->value += delta; if (wg->value < 0) wg->value = 0; pthread_mutex_unlock(&wg->mu); }
static void wg_done(WaitGroupC *wg) { pthread_mutex_lock(&wg->mu); if (--wg->value == 0) pthread_cond_broadcast(&wg->cv); pthread_mutex_unlock(&wg->mu); }
static int  wg_value(WaitGroupC *wg) { pthread_mutex_lock(&wg->mu); int v = wg->value; pthread_mutex_unlock(&wg->mu); return v; }
static void wg_wait(WaitGroupC *wg) { pthread_mutex_lock(&wg->mu); while (wg->value > 0) pthread_cond_wait(&wg->cv, &wg->mu); pthread_mutex_unlock(&wg->mu); }

typedef struct DirTask {
    int dirfd;
    char *abs_path;
    const char *root;
    const char *cache_abs;
    Cache *cache;
    struct DirTask *parent;
    atomic_ullong files_size;
    atomic_ullong children_size;
    atomic_int pending;
    atomic_int finalized;
    WaitGroupC *wg;
    TaskQueue *q;
    TaskQueue *finalq; // queue for finalizer thread
} DirTask;

static void finalize_task(DirTask *t);

static void enqueue_child(DirTask *parent, int cfd, const char *name) {
    DirTask *child = malloc(sizeof(DirTask));
    if (!child) { close(cfd); return; }
    child->dirfd = cfd;
    child->abs_path = path_join(parent->abs_path, name);
    if (!child->abs_path) {
        // OOM or path error: skip this child safely
        close(cfd);
        free(child);
        return;
    }
    child->root = parent->root;
    child->cache_abs = parent->cache_abs;
    child->cache = parent->cache;
    child->parent = parent;
    atomic_store(&child->files_size, 0ULL);
    atomic_store(&child->children_size, 0ULL);
    // Initialize child's pending to 1 to account for its own processing phase
    atomic_store(&child->pending, 1);
    atomic_store(&child->finalized, 0);
    child->wg = parent->wg;
    child->q = parent->q;
    child->finalq = parent->finalq;
    wg_add(parent->wg, 1);
    // Increment parent's pending for this child
    atomic_fetch_add(&parent->pending, 1);
    tq_push(parent->q, child);
}

/*
 * finalize_task
 * -------------
 * Completes a task when no more children are pending: writes the total
 * directory size into the cache, propagates the total to the parent and
 * releases resources (fd, path). Decrements the WaitGroup.
 */
static void finalize_task(DirTask *t) {
    if (atomic_load(&t->pending) != 0) return;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&t->finalized, &expected, 1)) return;
    unsigned long long total = atomic_load(&t->files_size) + atomic_load(&t->children_size);
    struct stat stc;
    if (fstat(t->dirfd, &stc) == 0) {
        cache_upsert_with_meta(t->cache, t->root, t->abs_path, total, time(NULL), (unsigned long long)stc.st_ino, stc.st_mtime);
    } else {
        cache_upsert_with_meta(t->cache, t->root, t->abs_path, total, time(NULL), 0ULL, 0);
    }
    if (t->parent) {
        atomic_fetch_add(&t->parent->children_size, total);
        if (atomic_fetch_sub(&t->parent->pending, 1) == 1) {
            finalize_task(t->parent);
        }
    }
    WaitGroupC *wgptr = t->wg;
    close(t->dirfd);
    free(t->abs_path);
    free(t);
    wg_done(wgptr);
}

/*
 * process_task
 * ------------
 * Processes a directory: accumulates regular file sizes and enqueues
 * subdirectories (openat + enqueue_child). Skips symlinks and the cache
 * file at the root. Finally calls finalize_task.
 */
static void process_task(DirTask *t) {
    int dupfd = dup(t->dirfd);
    if (dupfd < 0) {
        // Cannot process: mark processing phase done and finalize
        if (atomic_fetch_sub(&t->pending, 1) == 1) {
            if (!tq_push_timed(t->finalq, t, 2000)) finalize_task(t);
        }
        return;
    }
    DIR *dp = fdopendir(dupfd);
    if (!dp) {
        close(dupfd);
        if (atomic_fetch_sub(&t->pending, 1) == 1) {
            if (!tq_push_timed(t->finalq, t, 2000)) finalize_task(t);
        }
        return;
    }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        atomic_fetch_add(&g_progress_count, 1ULL);
        if (t->abs_path && t->root) {
            if (strcmp(t->abs_path, t->root) == 0 && strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        }
        unsigned char dt = de->d_type;
        if (dt == DT_LNK) continue;
            if (dt == DT_UNKNOWN) {
            struct stat st0;
            if (fstatat(t->dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) != 0) continue;
            if (S_ISLNK(st0.st_mode)) continue;
            if (S_ISDIR(st0.st_mode)) dt = DT_DIR; else if (S_ISREG(st0.st_mode)) dt = DT_REG;
            if (dt == DT_REG) {
                unsigned long long b = file_size_bytes(&st0);
                atomic_fetch_add(&t->files_size, b);
                atomic_fetch_add(&g_total_files, 1ULL);
                atomic_fetch_add(&g_total_bytes, b);
            }
        } else if (dt == DT_REG) {
            struct stat stf;
            if (fstatat(t->dirfd, de->d_name, &stf, AT_SYMLINK_NOFOLLOW) == 0) {
                unsigned long long b = file_size_bytes(&stf);
                atomic_fetch_add(&t->files_size, b);
                atomic_fetch_add(&g_total_files, 1ULL);
                atomic_fetch_add(&g_total_bytes, b);
            }
        } else if (dt == DT_DIR) {
            int cfd = openat(t->dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) enqueue_child(t, cfd, de->d_name);
        }
    }
    closedir(dp);
    // Mark this task's processing phase done; enqueue for finalization if pending reaches zero
    if (atomic_fetch_sub(&t->pending, 1) == 1) {
        // Try to enqueue for finalization; if queue is saturated and doesn't free within 2s, finalize inline
        if (!tq_push_timed(t->finalq, t, 2000)) {
            finalize_task(t);
        }
    }
}

typedef struct {
    TaskQueue q;
    TaskQueue finq;
    int threads;
    pthread_t *th;
    pthread_t fin_th;
    WaitGroupC wg;
} ScanPool;

static void *worker_loop(void *arg) {
    ScanPool *p = (ScanPool*)arg;
    for (;;) {
        DirTask *t = (DirTask*)tq_pop(&p->q);
        if (!t) break;
        atomic_fetch_add(&g_active_workers, 1);
        process_task(t);
        atomic_fetch_sub(&g_active_workers, 1);
    }
    return NULL;
}

static void *finalizer_loop(void *arg) {
    ScanPool *p = (ScanPool*)arg;
    for (;;) {
        DirTask *t = (DirTask*)tq_pop(&p->finq);
        if (!t) break;
        // finalize t (pending already 0)
        unsigned long long total = atomic_load(&t->files_size) + atomic_load(&t->children_size);
        struct stat stc;
        if (fstat(t->dirfd, &stc) == 0) {
            cache_upsert_with_meta(t->cache, t->root, t->abs_path, total, time(NULL), (unsigned long long)stc.st_ino, stc.st_mtime);
        } else {
            cache_upsert_with_meta(t->cache, t->root, t->abs_path, total, time(NULL), 0ULL, 0);
        }
        // propagate to parent
        if (t->parent) {
            atomic_fetch_add(&t->parent->children_size, total);
            if (atomic_fetch_sub(&t->parent->pending, 1) == 1) {
                if (!tq_push_timed(&p->finq, t->parent, 2000)) {
                    // Fallback finalize parent inline to avoid potential stall
                    finalize_task(t->parent);
                }
            }
        }
        WaitGroupC *wgptr = t->wg;
        close(t->dirfd);
        free(t->abs_path);
        free(t);
        wg_done(wgptr);
    }
    return NULL;
}

/*
 * scan_dir_parallel_deep
 * ----------------------
 * Launches a worker pool to process a deep queue of directories (BFS-like),
 * updating a shared cache guarded by a mutex. Maintains global atomic
 * counters (files/bytes) and renders a non-blocking progress bar from
 * the main thread. Returns the total size of the scan root.
 */
static unsigned long long scan_dir_parallel_deep(const char *root, const char *cache_abs, Cache *cache, int threads) {
    ScanPool p; tq_init(&p.q, 16384); tq_init(&p.finq, 16384);
    p.threads = threads; p.th = malloc((size_t)threads * sizeof(pthread_t)); wg_init(&p.wg);
    // start workers and finalizer
    for (int i = 0; i < threads; i++) pthread_create(&p.th[i], NULL, worker_loop, &p);
    pthread_create(&p.fin_th, NULL, finalizer_loop, &p);

    atomic_store(&g_progress_count, 0ULL);
    atomic_store(&g_active_workers, 0);
    atomic_store(&g_total_files, 0ULL);
    atomic_store(&g_total_bytes, 0ULL);
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { tq_close(&p.q); for (int i = 0; i < threads; i++) pthread_join(p.th[i], NULL); tq_destroy(&p.q); free(p.th); return 0ULL; }
    DirTask *rt = malloc(sizeof(DirTask));
    rt->dirfd = fd; rt->abs_path = xstrdup(root); rt->root = root; rt->cache_abs = cache_abs; rt->cache = cache; rt->parent = NULL;
    atomic_store(&rt->files_size, 0ULL); atomic_store(&rt->children_size, 0ULL);
    // Initialize pending to 1 (self processing), children will add more
    atomic_store(&rt->pending, 1);
    atomic_store(&rt->finalized, 0);
    rt->wg = &p.wg; rt->q = &p.q; rt->finalq = &p.finq;
    wg_add(&p.wg, 1);
    tq_push(&p.q, rt);

    // Progress loop with optional stall monitor (enabled if FASTDU_DEBUG_SCAN=1 or headless)
    ScanUI ui = { .enabled = (g_tui_active ? 1 : 0), .count = 0, .phase = "Scanning" };
    clock_gettime(CLOCK_MONOTONIC, &ui.last_draw);
    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50 * 1000 * 1000; // 50ms
    const char *dbg = getenv("FASTDU_DEBUG_SCAN");
    unsigned long long last_prog = 0ULL; time_t last_prog_ts = time(NULL);
    while (wg_value(&p.wg) > 0) {
        ui.count = atomic_load(&g_progress_count);
        ui.active = atomic_load(&g_active_workers);
        ui.pending = wg_value(&p.wg);
        ui.files = atomic_load(&g_total_files);
        ui.bytes = atomic_load(&g_total_bytes);
        draw_progress_ui(&ui, root);
        if (ui.count != last_prog) { last_prog = ui.count; last_prog_ts = time(NULL); }
        else {
            time_t now = time(NULL);
            if ((dbg || !ui.enabled) && now - last_prog_ts >= 20) {
                // print a heartbeat to stderr every 20s without progress
                fprintf(stderr, "[fastdu] stall? cnt=%llu active=%d wg=%d q=%zu/%zu finq=%zu/%zu files=%llu bytes=%llu path=%s\n",
                        (unsigned long long)ui.count, ui.active, ui.pending,
                        (size_t)tq_count(&p.q), (size_t)tq_capacity(&p.q),
                        (size_t)tq_count(&p.finq), (size_t)tq_capacity(&p.finq),
                        (unsigned long long)ui.files, (unsigned long long)ui.bytes,
                        root);
                last_prog_ts = now; // avoid spamming
            }
        }
        nanosleep(&ts, NULL);
    }

    // shut down queues and threads
    tq_close(&p.q);
    for (int i = 0; i < threads; i++) pthread_join(p.th[i], NULL);
    tq_close(&p.finq);
    pthread_join(p.fin_th, NULL);
    tq_destroy(&p.q); tq_destroy(&p.finq); free(p.th);

    // Clear progress line
    int cols, rows; getmaxyx(stdscr, rows, cols);
    mvhline(rows-1, 0, ' ', cols);
    refresh();

    // Snapshot totals for footer
    g_last_files = atomic_load(&g_total_files);
    g_last_bytes = atomic_load(&g_total_bytes);

    CacheEntry *ce = cache_get(cache, root);
    return ce ? ce->size : 0ULL;
}

// ------------------------------
// Signal and crash handling
// ------------------------------
/*
 * To avoid leaving the terminal in an inconsistent state when the process
 * receives fatal signals (SIGSEGV, SIGABRT, ...), we install handlers that
 * call endwin() if the TUI is active, then re-raise the signal to preserve
 * default behavior (core, exit status). We also register an atexit cleanup.
 * Note: endwin() is not async-signal-safe, but in practice this restores the
 * terminal reliably for interactive use; we also keep the handler minimal.
 */
static volatile sig_atomic_t g_tui_active = 0;

static void restore_terminal_on_exit(void) {
    if (g_tui_active) {
        endwin();
        g_tui_active = 0;
    }
}

static void crash_handler(int sig) {
    if (g_tui_active) {
        endwin();
        g_tui_active = 0;
    }
    const char msg[] = "\nfastdu: restored terminal after signal.\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg)-1);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL; sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
    raise(sig);
}

static void install_signal_handlers(void) {
    int sigs[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGTERM, SIGINT, SIGHUP };
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    for (size_t i = 0; i < sizeof(sigs)/sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

// ------------------------------
// Main
// ------------------------------
/*
 * Entry point: parse arguments (path, --reload, --jobs), TUI setup,
 * cache load/rebuild (with initial scan if needed), input loop and cleanup.
 */
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    const char *dbg_env0 = getenv("FASTDU_DEBUG");
    if (dbg_env0 && dbg_env0[0]=='1') {
        const char msg0[] = "[dbg0] main start\n";
        (void)write(STDERR_FILENO, msg0, sizeof(msg0)-1);
    }

// Arg parsing: [-R|--reload] [-j N|--jobs N] [-v|--version] [-H|--headless] [-h|--help] [path]
    int reload_flag = 0;
    int jobs_override = 0;
    int show_version = 0;
    int cli_help_flag = 0;
    const char *path_arg = NULL;
    int headless_flag = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--reload") == 0) {
            reload_flag = 1;
        } else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
            if (i + 1 < argc) { jobs_override = atoi(argv[++i]); }
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            show_version = 1;
        } else if (strcmp(argv[i], "-H") == 0 || strcmp(argv[i], "--headless") == 0) {
            headless_flag = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            cli_help_flag = 1;
        } else if (strcmp(argv[i], "-ac") == 0 || strcmp(argv[i], "--accuracy") == 0) {
            g_accuracy_mode = 1;
        } else {
            path_arg = argv[i];
        }
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("getcwd");
        return 1;
    }

    char root_in[PATH_MAX];
    if (path_arg) {
        strncpy(root_in, path_arg, sizeof(root_in));
        root_in[sizeof(root_in)-1] = '\0';
    } else {
        strncpy(root_in, cwd, sizeof(root_in));
        root_in[sizeof(root_in)-1] = '\0';
    }

    if (show_version) {
        printf("fastdu %s\n", FASTDU_VERSION);
        return 0;
    }
    if (cli_help_flag) {
        print_cli_usage();
        return 0;
    }

    int headless = !(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
    const char *env_headless = getenv("FASTDU_HEADLESS");
    if (env_headless && env_headless[0] == '1') headless = 1;
    if (headless_flag) headless = 1;
    g_headless = headless;
    int debug_all = getenv("FASTDU_DEBUG") ? 1 : 0;
    if (debug_all) fprintf(stderr, "[dbg] args parsed: headless=%d reload=%d jobs=%d path=%s\n", headless, reload_flag, jobs_override, path_arg ? path_arg : "(cwd)");

    char root[PATH_MAX];
    if (!realpath(root_in, root)) {
fprintf(stderr, "Invalid path: %s (%s)\n", root_in, strerror(errno));
        return 1;
    }

    // TTY / headless setup
    if (!headless) {
        // TUI setup early to show progress
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        curs_set(0);
        g_tui_active = 1;
        atexit(restore_terminal_on_exit);
        const char *asan_env = getenv("ASAN_OPTIONS");
        if (!asan_env || !asan_env[0]) {
            install_signal_handlers();
        }
    #ifdef NCURSES_VERSION
        set_escdelay(25);
    #endif
        if (has_colors()) {
            start_color();
    #ifdef NCURSES_VERSION
            use_default_colors();
    #endif
            init_pair(1, COLOR_BLACK, COLOR_CYAN);   // header/footer
            init_pair(3, COLOR_CYAN, COLOR_BLACK);   // dirs
            init_pair(4, COLOR_WHITE, COLOR_BLACK);  // files
        }
    }

    // Prepare cache (load or full rescan)
    Cache cache; cache_init(&cache);
    int have_cache = 0;
    int debug_cache = getenv("FASTDU_DEBUG_CACHE") ? 1 : 0;
    if (debug_cache) fprintf(stderr, "[main] cache_load root=%s\n", root);
    if (!reload_flag) have_cache = cache_load(root, &cache);
    if (debug_cache) fprintf(stderr, "[main] cache_load done have_cache=%d\n", have_cache);
    char *cache_abs = path_join(root, CACHE_FILENAME);

    if (debug_all) fprintf(stderr, "[dbg] have_cache=%d reload=%d\n", have_cache, reload_flag);
    if (g_accuracy_mode) reload_flag = 1; // accuracy implies deep rescan ignoring cache
    if (!have_cache || reload_flag) {
        if (!headless) {
            erase();
            draw_header(root, root, &cache);
            refresh();
        }
        if (g_accuracy_mode) {
            // Strict single-thread recursive scan with UI progress
            ScanUI ui = { .enabled = (g_tui_active ? 1 : 0), .count = 0, .phase = "Scanning (accurate)" };
            clock_gettime(CLOCK_MONOTONIC, &ui.last_draw);
            (void)scan_dir_recursive(root, root, cache_abs, &cache, &ui);
            // Clear progress line
            if (!headless) { int cols, rows; getmaxyx(stdscr, rows, cols); mvhline(rows-1, 0, ' ', cols); refresh(); }
            // Totali accurati
            CacheEntry *root_ce = cache_get(&cache, root);
            g_last_bytes = root_ce ? root_ce->size : 0ULL;
            g_last_files = count_files_path(root);
        } else {
            int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
            if (threads < 1) threads = 1;
            if (threads > 64) threads = 64;
            (void)scan_dir_parallel_deep(root, cache_abs, &cache, threads);
        }
        cache_save(root, &cache);
        if (!headless) {
            int cols, rows; getmaxyx(stdscr, rows, cols);
            mvhline(rows-1, 0, ' ', cols);
            refresh();
        }
    } else {
        // Cache loaded: set footer totals from cache root entry (v2/v3)
        CacheEntry *root_ce = cache_get(&cache, root);
        if (root_ce) g_last_bytes = root_ce->size; else g_last_bytes = 0ULL;
        // If the loaded cache (older versions) didn't persist totals_files, compute once and persist
        // Skip this in headless to avoid long blocking walks before summary
        if (!g_headless && g_last_files == 0ULL) {
            unsigned long long files_cnt = count_files_path(root);
            g_last_files = files_cnt;
            cache_save(root, &cache);
        }
    }

    if (headless) {
        if (debug_all) fprintf(stderr, "[dbg] entering headless summary branch\n");
        // In headless mode, if -R was requested we already rescanned; otherwise we may have loaded cache.
        // Print a short summary and exit.
        // Print on stdout
        printf("fastdu %s\n", FASTDU_VERSION);
        printf("root: %s\n", root);
        printf("files: %llu\n", (unsigned long long)g_last_files);
        printf("size: %llu bytes\n", (unsigned long long)g_last_bytes);
        fflush(stdout);
        // Also mirror summary to stderr to avoid missing output when stdout is piped
        fprintf(stderr, "[summary] fastdu %s\n", FASTDU_VERSION);
        fprintf(stderr, "[summary] root: %s\n", root);
        fprintf(stderr, "[summary] files: %llu\n", (unsigned long long)g_last_files);
        fprintf(stderr, "[summary] size: %llu bytes\n", (unsigned long long)g_last_bytes);
        // Low-level write to stderr as final assurance
        {
            char sum[512];
            int n = snprintf(sum, sizeof(sum), "[summary2] root=%s files=%llu size=%llu\n", root, (unsigned long long)g_last_files, (unsigned long long)g_last_bytes);
            if (n > 0) (void)write(STDERR_FILENO, sum, (size_t)((n < (int)sizeof(sum)) ? n : (int)sizeof(sum)));
        }
        if (debug_cache || debug_all) fprintf(stderr, "[main] headless summary printed, exiting.\n");
        // In debug, exit immediately without further cleanup to avoid any hidden blockers
        if (debug_all) { _exit(0); }
        const char *skip_free = getenv("FASTDU_SKIP_FREE_CACHE");
        if (skip_free && skip_free[0]=='1') {
            if (debug_all) fprintf(stderr, "[dbg] skipping cache_free due to FASTDU_SKIP_FREE_CACHE=1\n");
            _exit(0);
        }
        cache_free(&cache);
        if (debug_all) fprintf(stderr, "[dbg] cache_free done, exiting now\n");
        return 0;
    }

    DirView dv = (DirView){0};
    NavStack nav; navstack_init(&nav);
    markset_init(&g_marks);
    char current[PATH_MAX]; strncpy(current, root, sizeof(current)); current[sizeof(current)-1] = '\0';

    int top = 0;
    build_dir_view(current, root, &cache, &dv);

    int ch;
    while (1) {
        erase();
        draw_header(root, current, &cache);
        draw_list(&dv, top);
        refresh();

        // Controllo differenze quando il cursore è su una cartella
        maybe_rescan_hovered(&dv, root, &cache);
        // ridisegna list dopo possibile aggiornamento
        erase();
        draw_header(root, current, &cache);
        draw_list(&dv, top);
        refresh();

        ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        else if (ch == 'h' || ch == 'H') {
            show_help();
        } else if (ch == KEY_UP || ch == 'k') {
            if (dv.selected > 0) dv.selected--;
            if ((int)dv.selected < top) top = (int)dv.selected;
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (dv.selected + 1 < dv.n) dv.selected++;
            int rows, cols; getmaxyx(stdscr, rows, cols);
            int list_rows = rows - 3;
            if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
        } else if (ch == 10 || ch == KEY_RIGHT || ch == 'l') { // Enter
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (ve->is_dir) {
                    // Push parent nav state before changing directory
                    navstack_push(&nav, current, ve->abs_path, dv.selected, top);
                    strncpy(current, ve->abs_path, sizeof(current)); current[sizeof(current)-1] = '\0';
                    view_free(&dv);
                    top = 0;
                    build_dir_view(current, root, &cache, &dv);
                }
            }
        } else if (ch == 'b' || ch == 'B') {
            if (dv.n > 0) { dv.selected = 0; top = 0; }
        } else if (ch == 'e' || ch == 'E') {
            if (dv.n > 0) {
                dv.selected = dv.n - 1;
                int rows, cols; getmaxyx(stdscr, rows, cols);
                int list_rows = rows - 3;
                if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                if (top < 0) top = 0;
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8 || ch == KEY_LEFT) {
            if (strcmp(current, root) != 0) {
                char *parent = get_parent(current);
                if (parent) {
                    // Move to parent
                    strncpy(current, parent, sizeof(current)); current[sizeof(current)-1] = '\0';
                    free(parent);
                    view_free(&dv);
                    build_dir_view(current, root, &cache, &dv);
                    // Restore selection/top from nav stack
                    NavState st;
                    if (navstack_pop(&nav, &st)) {
                        // Prefer to locate the child path we came from
                        size_t idx = st.selected; int found = 0;
                        for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, st.child_path) == 0) { idx = i; found = 1; break; } }
                        dv.selected = (dv.n > 0) ? (idx < dv.n ? idx : dv.n - 1) : 0;
                        top = st.top;
                        // Ensure visibility
                        int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3;
                        if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                        if ((int)dv.selected < top) top = (int)dv.selected;
                        free(st.parent_path); free(st.child_path);
                    } else {
                        top = 0;
                    }
                }
            }
        } else if (ch == 'r') {
            if (dv.n > 0) {
                // remember current selection path
                ViewEntry *ve = &dv.v[dv.selected];
                char *sel_path = xstrdup(ve->abs_path);
                if (ve->is_dir) {
                    // compute old size and preserve current global total
                    unsigned long long old_sz = 0ULL; CacheEntry *ce_old = cache_get(&cache, ve->abs_path); if (ce_old) old_sz = ce_old->size;
                    unsigned long long prev_total = g_last_bytes;
                    // compute old files count for delta adjustment
                    unsigned long long old_files = count_files_path(ve->abs_path);
                    // duplicate scan path to avoid relying on dv memory
                    char *scan_path = xstrdup(ve->abs_path);
                    if (scan_path) {
                        int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
                        if (threads < 1) threads = 1;
                        if (threads > 64) threads = 64;
                        (void)scan_dir_parallel_deep(scan_path, cache_abs, &cache, threads);
                        free(scan_path);
                        // compute delta and adjust ancestors
                        unsigned long long new_sz = 0ULL; CacheEntry *ce_new = cache_get(&cache, ve->abs_path); if (ce_new) new_sz = ce_new->size;
                        long long delta = (long long)new_sz - (long long)old_sz;
                        if (delta > 0) cache_add_ancestors_after_delta(&cache, root, ve->abs_path, (unsigned long long)delta);
                        else if (delta < 0) cache_adjust_ancestors_after_delta(&cache, root, ve->abs_path, (long long)(-delta));
                        // restore footer total by applying delta to previous total (partial scan overwrote it)
                        if (delta >= 0) g_last_bytes = prev_total + (unsigned long long)delta;
                        else { unsigned long long dec = (unsigned long long)(-delta); g_last_bytes = (prev_total > dec) ? (prev_total - dec) : 0ULL; }
                        // adjust global files by delta between new and old subtree file counts
                        unsigned long long new_files = count_files_path(ve->abs_path);
                        if (new_files >= old_files) g_last_files += (new_files - old_files);
                        else {
                            unsigned long long decf = old_files - new_files;
                            if (g_last_files > decf) g_last_files -= decf; else g_last_files = 0ULL;
                        }
                        cache_save(root, &cache);
                    }
                }
                // rebuild and reselect same path
                size_t old_selected = dv.selected;
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                // find index of sel_path
                size_t new_idx = 0; int found = 0;
                if (sel_path) {
                    for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                }
                if (found) dv.selected = new_idx; else dv.selected = (old_selected < dv.n ? old_selected : (dv.n ? dv.n-1 : 0));
                // ensure visibility
                int rows, cols; getmaxyx(stdscr, rows, cols);
                int list_rows = rows - 3;
                if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                if ((int)dv.selected < top) top = (int)dv.selected;
                if (sel_path) free(sel_path);
            }
        } else if (ch == 'R') {
            view_free(&dv);
            build_dir_view(current, root, &cache, &dv);
        } else if (ch == ' ') {
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                // If this path is covered by a marked ancestor (and not explicitly marked), do not allow deselection
                int explicit_mark = markset_has(&g_marks, ve->abs_path);
                int covered = markset_covers(&g_marks, ve->abs_path);
                if (!explicit_mark && covered) {
                    draw_status("Item inherits mark from a parent; cannot toggle.");
                } else {
                    if (explicit_mark) markset_remove(&g_marks, ve->abs_path);
                    else markset_add(&g_marks, ve->abs_path);
                }
            }
        } else if (ch == 1) { // Ctrl-A: toggle select all in current view
            if (dv.n > 0) {
                int all_marked = 1;
                for (size_t i = 0; i < dv.n; i++) {
                    if (!markset_has(&g_marks, dv.v[i].abs_path)) { all_marked = 0; break; }
                }
                if (all_marked) {
                    for (size_t i = 0; i < dv.n; i++) markset_remove(&g_marks, dv.v[i].abs_path);
draw_status("Deselected all in view");
                } else {
                    for (size_t i = 0; i < dv.n; i++) markset_add(&g_marks, dv.v[i].abs_path);
draw_status("Selected all in view");
                }
            }
        } else if (ch == 'f') {
            char q[256];
            if (prompt_input(q, sizeof(q), "Find: ") > 0) {
                strncpy(g_search_query, q, sizeof(g_search_query)); g_search_query[sizeof(g_search_query)-1]='\0';
                if (dv.n > 0) {
                    size_t start = (dv.selected < dv.n) ? dv.selected : 0;
                    int found = -1;
                    // search forward including next, wrap around
                    for (size_t off = 1; off <= dv.n; off++) {
                        size_t idx = (start + off) % dv.n;
                        if (strcasestr_bool(dv.v[idx].name, g_search_query)) { found = (int)idx; break; }
                    }
                    if (found >= 0) {
                        dv.selected = (size_t)found;
                        int rows, cols; getmaxyx(stdscr, rows, cols);
                        int list_rows = rows - 3;
                        if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                        if ((int)dv.selected < top) top = (int)dv.selected;
                    } else {
                        draw_status("Nothing Found");
                    }
                }
            }
        } else if (ch == 'F') {
            char q[256];
            int got = prompt_input(q, sizeof(q), "Regex (case-insensitive): ");
            if (got >= 0) {
                if (q[0] == '\0') {
                    // empty input: leave regex disabled, preserve existing query filter state
                    g_regex_enabled = 0;
                } else {
                    // compile regex (case-insensitive, no submatches)
                    regex_t re;
                    int rc = regcomp(&re, q, REG_ICASE | REG_NOSUB);
                    if (rc != 0) {
                        char errbuf[256];
                        regerror(rc, &re, errbuf, sizeof(errbuf));
                        draw_status("Regex invalid");
                    } else {
                        if (g_regex_enabled) regfree(&g_regex);
                        g_regex = re;
                        g_regex_enabled = 1;
                        // enable query filter and rebuild view
                        char *sel_path = (dv.n > 0) ? xstrdup(dv.v[dv.selected].abs_path) : NULL;
                        g_filter_by_query = 1;
                        view_free(&dv);
                        build_dir_view(current, root, &cache, &dv);
                        if (dv.n == 0) {
draw_status("No elements match the regex");
                        } else if (sel_path) {
                            size_t new_idx = 0; int found = 0;
                            for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                            if (found) dv.selected = new_idx; else dv.selected = 0;
                        }
                        int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3;
                        if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                        if ((int)dv.selected < top) top = (int)dv.selected;
                        if (top < 0) top = 0;
                        if (sel_path) free(sel_path);
                    }
                }
            }
        } else if (ch == 'n') {
            if (g_search_query[0] && dv.n > 0) {
                size_t start = (dv.selected < dv.n) ? dv.selected : 0;
                int found = -1;
                for (size_t off = 1; off <= dv.n; off++) {
                    size_t idx = (start + off) % dv.n;
                    if (strcasestr_bool(dv.v[idx].name, g_search_query)) { found = (int)idx; break; }
                }
                if (found >= 0) {
                    dv.selected = (size_t)found;
                    int rows, cols; getmaxyx(stdscr, rows, cols);
                    int list_rows = rows - 3;
                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                    if ((int)dv.selected < top) top = (int)dv.selected;
                } else {
                    draw_status("Nothing Found");
                }
            }
        } else if (ch == 'N') {
            if (g_search_query[0] && dv.n > 0) {
                size_t start = (dv.selected < dv.n) ? dv.selected : 0;
                int found = -1;
                for (size_t off = 1; off <= dv.n; off++) {
                    size_t idx = (start + dv.n - off) % dv.n;
                    if (strcasestr_bool(dv.v[idx].name, g_search_query)) { found = (int)idx; break; }
                }
                if (found >= 0) {
                    dv.selected = (size_t)found;
                    int rows, cols; getmaxyx(stdscr, rows, cols);
                    int list_rows = rows - 3;
                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                    if ((int)dv.selected < top) top = (int)dv.selected;
                } else {
                    draw_status("Nothing Found");
                }
            }
        } else if (ch == 'v' || ch == 'V') {
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (ve->is_dir) {
                    draw_status("Preview available only on files");
                } else {
                    if (is_textual_file(ve->abs_path)) {
                        show_preview(ve->abs_path);
                    } else {
                        draw_status("Not a text file (binary or unsupported encoding)");
                    }
                }
            }
        } else if (ch == 'o' || ch == 'O') {
            // cycle sort key (size -> name -> mtime -> size), keep selection on same path
            SortMode next_mode = (g_sort_mode == SORT_SIZE) ? SORT_NAME : (g_sort_mode == SORT_NAME ? SORT_MTIME : SORT_SIZE);
            if (dv.n > 0) {
                char *sel_path = xstrdup(dv.v[dv.selected].abs_path);
                g_sort_mode = next_mode;
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                size_t new_idx = 0; int found = 0;
                for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                if (found) dv.selected = new_idx;
                free(sel_path);
            } else {
                g_sort_mode = next_mode;
            }
        } else if (ch == 'i') {
            // cycle info column (mtime -> owner+perm -> hidden -> mtime)
            g_info_col_mode = (g_info_col_mode == INFOCOL_MTIME) ? INFOCOL_OWNER_PERM : (g_info_col_mode == INFOCOL_OWNER_PERM ? INFOCOL_HIDDEN : INFOCOL_MTIME);
            // redraw on next loop
        } else if (ch == 'I') {
            // cycle left size column display: numeric -> percentage -> off -> numeric
            g_display_mode = (g_display_mode == DISP_NUM) ? DISP_PCT : (g_display_mode == DISP_PCT ? DISP_OFF : DISP_NUM);
        } else if (ch == 't') {
            // toggle filter: all -> dirs -> files -> all (lowercase t)
            if (dv.n > 0) {
                char *sel_path = xstrdup(dv.v[dv.selected].abs_path);
                g_filter_mode = (g_filter_mode == FILTER_ALL) ? FILTER_DIRS : (g_filter_mode == FILTER_DIRS ? FILTER_FILES : FILTER_ALL);
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                size_t new_idx = 0; int found = 0;
                for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                if (found) dv.selected = new_idx; else dv.selected = 0;
                // adjust top so selection visible
                int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3;
                if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                if ((int)dv.selected < top) top = (int)dv.selected;
                if (top < 0) top = 0;
                free(sel_path);
            } else {
                g_filter_mode = (g_filter_mode == FILTER_ALL) ? FILTER_DIRS : (g_filter_mode == FILTER_DIRS ? FILTER_FILES : FILTER_ALL);
            }
        } else if (ch == 'T') {
            // toggle filter by query
            if (!g_search_query[0]) {
draw_status("No search query stored");
            } else {
                int new_state = !g_filter_by_query;
                if (new_state) {
                    char *sel_path = (dv.n > 0) ? xstrdup(dv.v[dv.selected].abs_path) : NULL;
                    g_filter_by_query = 1;
                    view_free(&dv);
                    build_dir_view(current, root, &cache, &dv);
                    if (dv.n == 0) {
                        // nothing matches: revert
                        g_filter_by_query = 0;
                        build_dir_view(current, root, &cache, &dv);
draw_status("No elements match the query");
                    } else if (sel_path) {
                        size_t new_idx = 0; int found = 0;
                        for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                        if (found) dv.selected = new_idx; else dv.selected = 0;
                    }
                    // adjust top so selection visible
                    int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3;
                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                    if ((int)dv.selected < top) top = (int)dv.selected;
                    if (top < 0) top = 0;
                    if (sel_path) free(sel_path);
                } else {
                    g_filter_by_query = 0;
                    view_free(&dv);
                    build_dir_view(current, root, &cache, &dv);
                    // adjust top
                    int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3;
                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                    if ((int)dv.selected < top) top = (int)dv.selected;
                    if (top < 0) top = 0;
                }
            }
        } else if (ch == 's' || ch == 'S') {
            // toggle sort order (asc/desc), keep selection
            if (dv.n > 0) {
                char *sel_path = xstrdup(dv.v[dv.selected].abs_path);
                g_sort_desc = !g_sort_desc;
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                size_t new_idx = 0; int found = 0;
                for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                if (found) dv.selected = new_idx;
                free(sel_path);
            } else {
                g_sort_desc = !g_sort_desc;
            }
        } else if (ch == 'm') {
            if (g_marks.n == 0) {
draw_status("No items marked.");
            } else {
                // Move marked to current directory, if valid
                // Validate: destination must be different from each source parent, and not inside the source dir
                size_t n = g_marks.n;
                char **list = malloc(n * sizeof(char*));
                for (size_t i=0;i<n;i++) list[i] = xstrdup(g_marks.paths[i]);
                // Confirm move
                char prompt[PATH_MAX+128]; snprintf(prompt, sizeof(prompt), "Move %zu elements in '%s'? [y/N] ", n, current);
                draw_status(prompt); refresh(); int chc=getch();
                if (chc=='y'||chc=='Y'){
                    char conflict_all_m = 0; char conflict_action_all_m = 0; // 'o','r','s'
                    for (size_t i=0;i<n;i++) {
                        const char *src = list[i];
                        if (starts_with(current, src)) continue; // prevent copying dir into its subtree
                        const char *base = path_basename_const(src);
                        char *dst = path_join(current, base);
                        if (!dst) continue;
                        // compute delta
                        unsigned long long delta=0ULL; struct stat st; int is_dir=0;
                        if (lstat(src,&st)==0){ if (S_ISDIR(st.st_mode)){ is_dir=1; CacheEntry*ce=cache_get(&cache,src); if(ce) delta=ce->size; else delta=scan_dir_recursive(src, root, cache_abs, &cache, NULL);} else if (S_ISREG(st.st_mode)) delta=(unsigned long long)st.st_size; }
                        // handle conflict
                        if (path_exists(dst)) {
                            char act = conflict_all_m ? conflict_action_all_m : prompt_conflict_action(dst);
                            if (act=='O' || act=='R' || act=='S') { conflict_all_m = 1; conflict_action_all_m = (char)tolower(act); act = conflict_action_all_m; }
                            if (act=='s') { free(dst); continue; }
                            if (act=='r') { char *nd = gen_nonconflicting_path(dst); free(dst); dst = nd ? nd : NULL; if (!dst) continue; }
                            else if (act=='o') { remove_tree(dst, cache_abs); }
                        }
                        // perform rename (move)
                        if (rename(src, dst)==0){
                            if (is_dir) cache_move_prefix(&cache, root, src, dst);
                            cache_adjust_ancestors_after_delta(&cache, root, src, (long long)delta);
                            cache_add_ancestors_after_delta(&cache, root, dst, delta);
                            markset_remove_prefix(&g_marks, src);
                        } else {
                            // if rename failed, check EXDEV fallback: copy+unlink
                            if (errno == EXDEV) {
                                // copy with progress
                                CopyUI mui = { .enabled = 1, .total = delta, .done = 0, .phase = "Move (copy)" };
                                clock_gettime(CLOCK_MONOTONIC, &mui.last_draw);
                                int rc2;
                                if (is_dir) rc2 = copy_tree_with_progress(src, dst, &mui, root);
                                else rc2 = copy_file_with_progress(src, dst, &mui);
                                int cols, rows; getmaxyx(stdscr, rows, cols); mvhline(rows-1, 0, ' ', cols); refresh();
                                if (rc2 == 0) {
                                    // remove source and update cache
                                    remove_tree(src, cache_abs);
                                    if (is_dir) cache_remove_prefix(&cache, src);
                                    cache_adjust_ancestors_after_delta(&cache, root, src, (long long)delta);
                                    cache_add_ancestors_after_delta(&cache, root, dst, delta);
                                }
                            }
                        }
                        free(dst);
                    }
                    cache_save(root,&cache);
                    // clear all marks after move to avoid unintended operations
                    markset_clear(&g_marks);
                    for(size_t i=0;i<n;i++) free(list[i]); free(list);
                    // Refresh view
                    size_t old_index = dv.selected;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
                    draw_status("Move completed.");
                } else {
                    for(size_t i=0;i<n;i++) free(list[i]); free(list);
                }
            }
        } else if (ch == 'c') {
            if (g_marks.n == 0) {
draw_status("No items marked.");
            } else {
                // Copy marked to current directory with progress
                size_t n = g_marks.n;
                char **list = malloc(n * sizeof(char*));
                for (size_t i=0;i<n;i++) list[i] = xstrdup(g_marks.paths[i]);
                // Compute total bytes
                unsigned long long total = 0ULL;
                for (size_t i=0;i<n;i++) total += sum_path_size(list[i]);
                char prompt[PATH_MAX+128];
                char totbuf[64]; human_size(total, totbuf, sizeof(totbuf));
                snprintf(prompt, sizeof(prompt), "Copy %zu elements in '%s' (tot %s)? [y/N] ", n, current, totbuf);
                draw_status(prompt); refresh(); int chc=getch();
                if (chc=='y' || chc=='Y') {
                    CopyUI ui = { .enabled = 1, .total = total, .done = 0, .phase = "Copy" };
                    clock_gettime(CLOCK_MONOTONIC, &ui.last_draw);
                    char conflict_all = 0; char conflict_action_all = 0; // 'o','r','s'
                    for (size_t i=0;i<n;i++) {
                        const char *src = list[i];
                        if (starts_with(current, src)) continue; // prevent copying dir into its subtree
                        const char *base = path_basename_const(src);
                        char *dst = path_join(current, base);
                        if (!dst) continue;
                        struct stat st; if (lstat(src,&st)!=0) { free(dst); continue; }
                        // handle conflict
                        if (path_exists(dst)) {
                            char act = conflict_all ? conflict_action_all : prompt_conflict_action(dst);
                            if (act=='O' || act=='R' || act=='S') { conflict_all = 1; conflict_action_all = (char)tolower(act); act = conflict_action_all; }
                            if (act=='s') { free(dst); continue; }
                            if (act=='r') { char *nd = gen_nonconflicting_path(dst); free(dst); dst = nd ? nd : NULL; if (!dst) continue; }
                            else if (act=='o') { remove_tree(dst, cache_abs); }
                        }
                        if (S_ISDIR(st.st_mode)) {
                            (void)copy_tree_with_progress(src, dst, &ui, root);
                            // update cache for new dir
                            char *cache_abs = path_join(root, CACHE_FILENAME);
                            (void)scan_dir_recursive(dst, root, cache_abs, &cache, NULL);
                            free(cache_abs);
                            // add delta to ancestors using estimated (or recompute size via cache)
                            CacheEntry *ced = cache_get(&cache, dst);
                            unsigned long long dsz = ced ? ced->size : sum_path_size(dst);
                            cache_add_ancestors_after_delta(&cache, root, dst, dsz);
                            // increment global files by files copied under dst
                            unsigned long long finc = count_files_path(dst);
                            g_last_files += finc;
                        } else if (S_ISREG(st.st_mode)) {
                            (void)copy_file_with_progress(src, dst, &ui);
                            cache_add_ancestors_after_delta(&cache, root, dst, (unsigned long long)st.st_size);
                            // increment global files by one regular file
                            g_last_files += 1ULL;
                        }
                        free(dst);
                    }
                    // clear progress line
                    int cols, rows; getmaxyx(stdscr, rows, cols); mvhline(rows-1, 0, ' ', cols); refresh();
                    cache_save(root, &cache);
                    // clear all marks after copy to avoid unintended operations
                    markset_clear(&g_marks);
                    for (size_t i=0;i<n;i++) free(list[i]); free(list);
                    // Refresh view
                    size_t old_index = dv.selected;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
draw_status("Copy completed.");
                } else {
                    for (size_t i=0;i<n;i++) free(list[i]); free(list);
                }
            }
        } else if (ch == 'd') {
            if (g_marks.n > 0) {
                // Bulk delete marked
                // Build list and sort by path length descending to delete deep paths first
                size_t n = g_marks.n;
                // Simple selection sort by length desc without extra alloc (work on a copy of pointers)
                char **list = malloc(n * sizeof(char*));
                for (size_t i=0;i<n;i++) list[i] = xstrdup(g_marks.paths[i]);
                for (size_t i=0;i<n;i++) {
                    size_t max_i = i; size_t max_l = strlen(list[i]);
                    for (size_t j=i+1;j<n;j++){ size_t lj=strlen(list[j]); if (lj>max_l){ max_l=lj; max_i=j; }}
                    if (max_i!=i){ char*tmp=list[i]; list[i]=list[max_i]; list[max_i]=tmp; }
                }
                // Confirm
                size_t cnt_dir=0,cnt_file=0; for(size_t i=0;i<n;i++){ struct stat st; if (lstat(list[i],&st)==0){ if(S_ISDIR(st.st_mode)) cnt_dir++; else cnt_file++; }}
                char prompt[256]; snprintf(prompt,sizeof(prompt),"Delete %zu elements (%zu dir, %zu file)? [y/N] ", n, cnt_dir, cnt_file);
                draw_status(prompt); refresh(); int chc=getch();
                if (chc=='y'||chc=='Y'){
                    for (size_t i=0;i<n;i++){
                        const char *p = list[i];
                        // compute delta bytes and files
                        unsigned long long delta=0ULL; struct stat st; int is_dir=0;
                        if (lstat(p,&st)==0){ if (S_ISDIR(st.st_mode)){ is_dir=1; CacheEntry*ce=cache_get(&cache,p); if(ce) delta=ce->size; else delta=scan_dir_recursive(p, root, cache_abs, &cache, NULL);} else if (S_ISREG(st.st_mode)) delta=(unsigned long long)st.st_size; }
                        unsigned long long files_dec = count_files_path(p);
                        // delete
                        if (remove_tree(p, cache_abs)==0){ if (is_dir) cache_remove_prefix(&cache,p); cache_adjust_ancestors_after_delta(&cache, root, p, (long long)delta); if (g_last_files > files_dec) g_last_files -= files_dec; else g_last_files = 0ULL; }
                    }
                    cache_save(root,&cache);
                    // clear marks that were deleted
                    for(size_t i=0;i<n;i++){ markset_remove(&g_marks, list[i]); free(list[i]); } free(list);
                    // refresh view
                    size_t old_index = dv.selected;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
draw_status("Delete completed.");
                } else {
                    for(size_t i=0;i<n;i++) free(list[i]);
                    free(list);
                }
            } else if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (confirm_delete_prompt(ve->name, ve->is_dir)) {
                    // Calcola delta prima di rimuovere
                    struct stat st_before;
                    int exists_before = (lstat(ve->abs_path, &st_before) == 0);
                    unsigned long long delta = 0ULL;
                    int is_dir = ve->is_dir;
                    if (exists_before) {
                        if (!is_dir && S_ISREG(st_before.st_mode)) {
                            delta = (unsigned long long)st_before.st_size;
                        } else if (is_dir) {
                            CacheEntry *ce_del = cache_get(&cache, ve->abs_path);
                            if (ce_del) delta = ce_del->size;
                            else {
                                // Stima dimensione della dir da eliminare (solo il subtree target)
                                delta = scan_dir_recursive(ve->abs_path, root, cache_abs, &cache, NULL);
                            }
                        }
                    }

                    // Elimina
                    size_t old_index = dv.selected;
                    int rc = remove_tree(ve->abs_path, cache_abs);
                    if (rc == 0) {
                        if (is_dir) cache_remove_prefix(&cache, ve->abs_path);
                        cache_adjust_ancestors_after_delta(&cache, root, ve->abs_path, (long long)delta);
                        // decrement global files by files under removed path
                        unsigned long long files_dec = count_files_path(ve->abs_path);
                        if (g_last_files > files_dec) g_last_files -= files_dec; else g_last_files = 0ULL;
                        cache_save(root, &cache);
                        // unmark removed path and any subpaths
                        markset_remove_prefix(&g_marks, ve->abs_path);
                        // ricostruisci sola vista corrente (no rescan totale)
                        view_free(&dv);
                        build_dir_view(current, root, &cache, &dv);
                        if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
                        draw_status("Erase completed.");
                    } else {
                        char msg[PATH_MAX + 64];
snprintf(msg, sizeof(msg), "Error deleting '%s'", ve->name);
                        draw_status(msg);
                    }
                }
            }
        }
    }

    // Cleanup
    endwin();
    view_free(&dv);
    cache_save(root, &cache);
    cache_free(&cache);
    if (g_regex_enabled) { regfree(&g_regex); g_regex_enabled = 0; }
    free(cache_abs);
    return 0;
}
