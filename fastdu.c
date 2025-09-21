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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define CACHE_FILENAME ".fastdu_cache_v2"

// ------------------------------
// Util
// ------------------------------
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

static void human_size(unsigned long long v, char *buf, size_t bufsz) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
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

static int cache_save(const char *root, const Cache *c) {
    char *cache_path = path_join(root, CACHE_FILENAME);
    if (!cache_path) return -1;
    FILE *f = fopen(cache_path, "w");
    if (!f) { free(cache_path); return -1; }
    fprintf(f, "# fastdu-cache v2\n");
    fprintf(f, "root\t%s\n", root);
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

static int cache_load(const char *root, Cache *c) {
    char *cache_path = path_join(root, CACHE_FILENAME);
    if (!cache_path) return -1;
    FILE *f = fopen(cache_path, "r");
    free(cache_path);
    if (!f) return 0; // no cache file, not an error

    char *line = NULL; size_t len = 0; ssize_t r;
    int header_ok = 0; int version = 1;
    while ((r = getline(&line, &len, f)) != -1) {
        if (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) line[--r] = '\0';
        if (!header_ok) {
            if (strncmp(line, "# fastdu-cache v2", 18) == 0) { header_ok = 1; version = 2; }
            else if (strncmp(line, "# fastdu-cache v1", 18) == 0) { header_ok = 1; version = 1; }
            continue;
        }
        if (strncmp(line, "root\t", 5) == 0) {
            // informational, ignore value
            continue;
        }
        if (line[0] == 'D' && line[1] == '\t') {
            char *p = line + 2;
            char *rel = p;
            char *tab1 = strchr(p, '\t'); if (!tab1) continue; *tab1 = '\0';
            char *size_str = tab1 + 1;
            char *tab2 = strchr(size_str, '\t'); if (!tab2) continue; *tab2 = '\0';
            char *time_str = tab2 + 1;
            char *ino_str = NULL; char *mtime_str = NULL;
            if (version >= 2) {
                char *tab3 = strchr(time_str, '\t'); if (!tab3) continue; *tab3 = '\0';
                ino_str = tab3 + 1;
                char *tab4 = strchr(ino_str, '\t'); if (!tab4) continue; *tab4 = '\0';
                mtime_str = tab4 + 1;
            }
            char *rel_dec = pct_decode(rel);
            if (!rel_dec) continue;
            char *abs = abspath_from_rel(root, rel_dec);
            free(rel_dec);
            if (!abs) continue;
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
    }
    free(line);
    fclose(f);
    return 1; // loaded
}

// ------------------------------
// Scanner
// ------------------------------
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
            if (dtype == DT_REG) total += (unsigned long long)st0.st_size;
        } else if (dtype == DT_REG) {
            struct stat stf;
            if (fstatat(dirfd, de->d_name, &stf, AT_SYMLINK_NOFOLLOW) == 0) total += (unsigned long long)stf.st_size;
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
typedef enum { SORT_SIZE = 0, SORT_NAME = 1 } SortMode;
static SortMode g_sort_mode = SORT_SIZE;
static int g_sort_desc = 1; // 1=desc, 0=asc
static const char *sort_mode_label(void) {
    static char buf[32];
    const char *key = (g_sort_mode == SORT_NAME) ? "name" : "size";
    const char *ord = g_sort_desc ? "desc" : "asc";
    snprintf(buf, sizeof(buf), "%s %s", key, ord);
    return buf;
}

static int cmp_entries(const void *a, const void *b) {
    const ViewEntry *ea = (const ViewEntry*)a;
    const ViewEntry *eb = (const ViewEntry*)b;
    if (g_sort_mode == SORT_NAME) {
        // pure alphabetical with asc/desc toggle
        int c = strcmp(ea->name, eb->name);
        return g_sort_desc ? -c : c;
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
        if (out->n == out->cap) {
            size_t newcap = out->cap ? out->cap * 2 : 128;
            void *nv = realloc(out->v, newcap * sizeof(ViewEntry));
            if (!nv) { free(abs); continue; }
            out->v = (ViewEntry*)nv; out->cap = newcap;
        }
        ViewEntry *ve = &out->v[out->n++];
        ve->name = xstrdup(de->d_name);
        ve->abs_path = abs;
        ve->is_dir = (dtype == DT_DIR) ? 1 : 0;
        if (dtype == DT_REG) ve->mtime = st.st_mtime; else ve->mtime = time(NULL);
        if (ve->is_dir) {
            CacheEntry *ce = cache_get(cache, ve->abs_path);
            if (ce) { ve->size = ce->size; ve->size_known = 1; }
            else { ve->size = 0ULL; ve->size_known = 0; }
        } else if (dtype == DT_REG) {
            ve->size = (unsigned long long)st.st_size;
            ve->size_known = 1;
        } else {
            ve->size = 0ULL; ve->size_known = 1;
        }
    }
    closedir(dp);
    qsort(out->v, out->n, sizeof(ViewEntry), cmp_entries);
    return 0;
}

// ------------------------------
// TUI
// ------------------------------
static void draw_header(const char *root, const char *cur) {
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
    char title[PATH_MAX + 128];
    snprintf(title, sizeof(title), " fastdu - root: %s - cwd: %s - sort: %s  ", root, relbuf, sort_mode_label());
    mvaddnstr(0, 0, title, cols-1);
    attroff(COLOR_PAIR(1));

    attron(COLOR_PAIR(1));
    mvhline(rows-2, 0, ' ', cols);
    char totalbuf[64];
    human_size((unsigned long long)g_last_bytes, totalbuf, sizeof(totalbuf));
    char footer[256];
    snprintf(footer, sizeof(footer), " h help | files:%llu | size:%s ", (unsigned long long)g_last_files, totalbuf);
    mvaddnstr(rows-2, 0, footer, cols-1);
    attroff(COLOR_PAIR(1));
}

static int compute_size_col_width(const DirView *dv) {
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

static void draw_list(const DirView *dv, int top) {
    int cols; int rows; getmaxyx(stdscr, rows, cols);
    int y = 1; // below header
    int list_rows = rows - 3; // header + footer
    int sizew = compute_size_col_width(dv);
    int type_col = 1 + sizew + 1; // leading space + size + space
    int name_col = type_col + 2; // type char + space

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
        char sizebuf[64];
        if (ve->size_known) human_size(ve->size, sizebuf, sizeof(sizebuf));
        else snprintf(sizebuf, sizeof(sizebuf), "?");
        // draw size right-aligned in its column
        char sizefmt[32]; snprintf(sizefmt, sizeof(sizefmt), " %%-%ds", sizew); // left pad by printing into fixed area then align by mvaddnstr with right shift
        // To ensure right alignment, compute padding
        int l = (int)strlen(sizebuf);
        int pad = sizew - l; if (pad < 0) pad = 0;
        // size column at x=1
        if (pad) { for (int k = 0; k < pad; k++) mvaddch(y + i, 1 + k, ' '); }
        mvaddnstr(y + i, 1 + pad, sizebuf, sizew - pad);
        // type
        mvaddch(y + i, type_col, ve->is_dir ? 'D' : 'F');
        // space after type
        mvaddch(y + i, type_col + 1, ' ');
        // name uses remaining space
        if (ve->is_dir) attron(COLOR_PAIR(3)); else attron(COLOR_PAIR(4));
        mvaddnstr(y + i, name_col, ve->name, cols - name_col - 1);
        if (ve->is_dir) attroff(COLOR_PAIR(3)); else attroff(COLOR_PAIR(4));
        if (is_sel) attroff(A_REVERSE | A_BOLD);
    }
}

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
        char *cache_abs = path_join(root, CACHE_FILENAME);
        unsigned long long sz = scan_dir_recursive(ve->abs_path, root, cache_abs, cache, NULL);
        (void)sz;
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

static void show_help(void) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    int w = cols - 4; if (w < 40) w = cols - 2; if (w < 20) w = cols;
    int h = rows - 6; if (h < 10) h = rows - 2; if (h < 5) h = rows;
    int x = (cols - w) / 2; if (x < 0) x = 0;
    int y = (rows - h) / 2; if (y < 0) y = 0;
    // draw a simple box area
    for (int i = 0; i < h; i++) {
        mvhline(y + i, x, ' ', w);
    }
    attron(A_REVERSE);
    mvaddnstr(y, x, " Help - press any key to close ", w);
    attroff(A_REVERSE);

    int line = y + 2;
    const char *lines[] = {
        "Navigation:",
        "  Up/Down or j/k  - move selection",
        "  Enter or Right/l - open directory",
        "  Backspace or Left - go up",
        "  b - go to top, e - go to end",
        "",
        "Actions:",
        "  r - rescan selected dir",
        "  R - rescan current dir",
        "  d - delete file/dir (confirm)",
        "  o - toggle sort key (size/name)",
        "  s - toggle sort order (asc/desc)",
        "  q - quit",
        "  h - this help",
        "",
        "CLI:",
        "  fastdu [-R|--reload] [-j N|--jobs N] [path]",
        NULL
    };
    for (int i = 0; lines[i]; i++) {
        mvaddnstr(line++, x + 1, lines[i], w - 2);
        if (line >= y + h - 1) break;
    }
    refresh();
    getch();
}

static int confirm_delete_prompt(const char *name, int is_dir) {
    char prompt[PATH_MAX + 128];
    snprintf(prompt, sizeof(prompt), "Eliminare %c '%s'? [y/N] ", is_dir ? 'D' : 'F', name);
    draw_status(prompt);
    refresh();
    int ch = getch();
    return (ch == 'y' || ch == 'Y');
}

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

// ------------------------------
// Parallel scanning helpers (deep work queue)
// ------------------------------

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
static int tq_push(TaskQueue *q, void *item) {
    pthread_mutex_lock(&q->mu);
    while (q->count == q->cap && !q->closed) pthread_cond_wait(&q->cv_nonfull, &q->mu);
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
    WaitGroupC *wg;
    TaskQueue *q;
} DirTask;

static void finalize_task(DirTask *t);

static void enqueue_child(DirTask *parent, int cfd, const char *name) {
    DirTask *child = malloc(sizeof(DirTask));
    child->dirfd = cfd;
    child->abs_path = path_join(parent->abs_path, name);
    child->root = parent->root;
    child->cache_abs = parent->cache_abs;
    child->cache = parent->cache;
    child->parent = parent;
    atomic_store(&child->files_size, 0ULL);
    atomic_store(&child->children_size, 0ULL);
    atomic_store(&child->pending, 0);
    child->wg = parent->wg;
    child->q = parent->q;
    wg_add(parent->wg, 1);
    atomic_fetch_add(&parent->pending, 1);
    tq_push(parent->q, child);
}

static void finalize_task(DirTask *t) {
    if (atomic_load(&t->pending) != 0) return;
    unsigned long long total = atomic_load(&t->files_size) + atomic_load(&t->children_size);
    struct stat stc;
    if (fstat(t->dirfd, &stc) == 0) {
        CacheEntry *e = cache_upsert(t->cache, t->root, t->abs_path, total, time(NULL));
        if (e) { e->ino = (unsigned long long)stc.st_ino; e->dir_mtime = stc.st_mtime; }
    } else {
        cache_upsert(t->cache, t->root, t->abs_path, total, time(NULL));
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

static void process_task(DirTask *t) {
    int dupfd = dup(t->dirfd);
    if (dupfd < 0) { finalize_task(t); return; }
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); finalize_task(t); return; }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        atomic_fetch_add(&g_progress_count, 1ULL);
        if (strcmp(t->abs_path, t->root) == 0 && strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        unsigned char dt = de->d_type;
        if (dt == DT_LNK) continue;
        if (dt == DT_UNKNOWN) {
            struct stat st0;
            if (fstatat(t->dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) != 0) continue;
            if (S_ISLNK(st0.st_mode)) continue;
            if (S_ISDIR(st0.st_mode)) dt = DT_DIR; else if (S_ISREG(st0.st_mode)) dt = DT_REG;
            if (dt == DT_REG) {
                atomic_fetch_add(&t->files_size, (unsigned long long)st0.st_size);
                atomic_fetch_add(&g_total_files, 1ULL);
                atomic_fetch_add(&g_total_bytes, (unsigned long long)st0.st_size);
            }
        } else if (dt == DT_REG) {
            struct stat stf;
            if (fstatat(t->dirfd, de->d_name, &stf, AT_SYMLINK_NOFOLLOW) == 0) {
                atomic_fetch_add(&t->files_size, (unsigned long long)stf.st_size);
                atomic_fetch_add(&g_total_files, 1ULL);
                atomic_fetch_add(&g_total_bytes, (unsigned long long)stf.st_size);
            }
        } else if (dt == DT_DIR) {
            int cfd = openat(t->dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) enqueue_child(t, cfd, de->d_name);
        }
    }
    closedir(dp);
    finalize_task(t);
}

typedef struct {
    TaskQueue q;
    int threads;
    pthread_t *th;
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

static unsigned long long scan_dir_parallel_deep(const char *root, const char *cache_abs, Cache *cache, int threads) {
    ScanPool p; tq_init(&p.q, 4096); p.threads = threads; p.th = malloc((size_t)threads * sizeof(pthread_t)); wg_init(&p.wg);
    for (int i = 0; i < threads; i++) pthread_create(&p.th[i], NULL, worker_loop, &p);

    atomic_store(&g_progress_count, 0ULL);
    atomic_store(&g_active_workers, 0);
    atomic_store(&g_total_files, 0ULL);
    atomic_store(&g_total_bytes, 0ULL);
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { tq_close(&p.q); for (int i = 0; i < threads; i++) pthread_join(p.th[i], NULL); tq_destroy(&p.q); free(p.th); return 0ULL; }
    DirTask *rt = malloc(sizeof(DirTask));
    rt->dirfd = fd; rt->abs_path = xstrdup(root); rt->root = root; rt->cache_abs = cache_abs; rt->cache = cache; rt->parent = NULL;
    atomic_store(&rt->files_size, 0ULL); atomic_store(&rt->children_size, 0ULL); atomic_store(&rt->pending, 0);
    rt->wg = &p.wg; rt->q = &p.q;
    wg_add(&p.wg, 1);
    tq_push(&p.q, rt);

    // Progress loop (UI updated from main thread)
    ScanUI ui = { .enabled = 1, .count = 0, .phase = "Scansione" };
    clock_gettime(CLOCK_MONOTONIC, &ui.last_draw);
    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50 * 1000 * 1000; // 50ms
    while (wg_value(&p.wg) > 0) {
        ui.count = atomic_load(&g_progress_count);
        ui.active = atomic_load(&g_active_workers);
        ui.pending = wg_value(&p.wg);
        ui.files = atomic_load(&g_total_files);
        ui.bytes = atomic_load(&g_total_bytes);
        draw_progress_ui(&ui, root);
        nanosleep(&ts, NULL);
    }

    tq_close(&p.q);
    for (int i = 0; i < threads; i++) pthread_join(p.th[i], NULL);
    tq_destroy(&p.q); free(p.th);

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
// Main
// ------------------------------
int main(int argc, char **argv) {
    setlocale(LC_ALL, "");

// Arg parsing: [-R|--reload] [-j N|--jobs N] [path]
    int reload_flag = 0;
    int jobs_override = 0;
    const char *path_arg = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--reload") == 0) {
            reload_flag = 1;
        } else if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
            if (i + 1 < argc) { jobs_override = atoi(argv[++i]); }
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

    char root[PATH_MAX];
    if (!realpath(root_in, root)) {
        fprintf(stderr, "Percorso non valido: %s (%s)\n", root_in, strerror(errno));
        return 1;
    }

    // TUI setup early to show progress
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
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

    // Prepare cache (load or full rescan)
    Cache cache; cache_init(&cache);
    int have_cache = 0;
    if (!reload_flag) have_cache = cache_load(root, &cache);
    char *cache_abs = path_join(root, CACHE_FILENAME);

    if (!have_cache || reload_flag) {
        erase();
        draw_header(root, root);
        refresh();
        int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (threads < 1) threads = 1;
        if (threads > 64) threads = 64;
        (void)scan_dir_parallel_deep(root, cache_abs, &cache, threads);
        cache_save(root, &cache);
        int cols, rows; getmaxyx(stdscr, rows, cols);
        mvhline(rows-1, 0, ' ', cols);
        refresh();
    }

    DirView dv = {0};
    char current[PATH_MAX]; strncpy(current, root, sizeof(current)); current[sizeof(current)-1] = '\0';

    int top = 0;
    build_dir_view(current, root, &cache, &dv);

    int ch;
    while (1) {
        erase();
        draw_header(root, current);
        draw_list(&dv, top);
        refresh();

        // Controllo differenze quando il cursore è su una cartella
        maybe_rescan_hovered(&dv, root, &cache);
        // ridisegna list dopo possibile aggiornamento
        erase();
        draw_header(root, current);
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
                    strncpy(current, parent, sizeof(current)); current[sizeof(current)-1] = '\0';
                    free(parent);
                    view_free(&dv);
                    top = 0;
                    build_dir_view(current, root, &cache, &dv);
                }
            }
        } else if (ch == 'r') {
            if (dv.n > 0) {
                // remember current selection path
                ViewEntry *ve = &dv.v[dv.selected];
                char *sel_path = xstrdup(ve->abs_path);
                if (ve->is_dir) {
                    int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
                    if (threads < 1) threads = 1;
                    if (threads > 64) threads = 64;
                    (void)scan_dir_parallel_deep(ve->abs_path, cache_abs, &cache, threads);
                    cache_save(root, &cache);
                    // rebuild and reselect same path
                    size_t old_selected = dv.selected;
                    view_free(&dv);
                    build_dir_view(current, root, &cache, &dv);
                    // find index of sel_path
                    size_t new_idx = 0; int found = 0;
                    for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                    if (found) dv.selected = new_idx; else dv.selected = (old_selected < dv.n ? old_selected : (dv.n ? dv.n-1 : 0));
                    // ensure visibility
                    int rows, cols; getmaxyx(stdscr, rows, cols);
                    int list_rows = rows - 3;
                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                    if ((int)dv.selected < top) top = (int)dv.selected;
                }
                free(sel_path);
            }
        } else if (ch == 'R') {
            int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
            if (threads < 1) threads = 1;
            if (threads > 64) threads = 64;
            (void)scan_dir_parallel_deep(current, cache_abs, &cache, threads);
            cache_save(root, &cache);
            view_free(&dv);
            build_dir_view(current, root, &cache, &dv);
        } else if (ch == 'o' || ch == 'O') {
            // toggle sort key (size/name), keep selection on same path
            if (dv.n > 0) {
                char *sel_path = xstrdup(dv.v[dv.selected].abs_path);
                g_sort_mode = (g_sort_mode == SORT_SIZE) ? SORT_NAME : SORT_SIZE;
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                size_t new_idx = 0; int found = 0;
                for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, sel_path) == 0) { new_idx = i; found = 1; break; } }
                if (found) dv.selected = new_idx;
                free(sel_path);
            } else {
                g_sort_mode = (g_sort_mode == SORT_SIZE) ? SORT_NAME : SORT_SIZE;
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
        } else if (ch == 'd') {
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (confirm_delete_prompt(ve->name, ve->is_dir)) {
                    // remember current index to keep position after deletion
                    size_t old_index = dv.selected;
                    int rc = remove_tree(ve->abs_path, cache_abs);
                    if (rc == 0) {
                        cache_remove_prefix(&cache, ve->abs_path);
                        // rescan current directory to update sizes (parallel with progress)
                        int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
                        if (threads < 1) threads = 1; if (threads > 64) threads = 64;
                        unsigned long long sz = scan_dir_parallel_deep(current, cache_abs, &cache, threads);
                        (void)sz;
                        cache_save(root, &cache);
                        view_free(&dv);
                        build_dir_view(current, root, &cache, &dv);
                        if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
                        draw_status("Eliminazione completata.");
                    } else {
                        char msg[PATH_MAX + 64];
                        snprintf(msg, sizeof(msg), "Errore eliminazione di '%s'", ve->name);
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
    free(cache_abs);
    return 0;
}
