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
#include <termios.h>
#include <sys/ioctl.h>
#include <archive.h>
#include <archive_entry.h>
#include <zstd.h>
#ifdef __linux__
#include <linux/stat.h>
#include <liburing.h>
#define HAS_IO_URING 1
#endif
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

// Forward declaration for TUI active flag used before its definition
static volatile sig_atomic_t g_tui_active;
static volatile sig_atomic_t g_interrupted = 0;

// ------------------------------
// Types and Structures (Early definitions)
// ------------------------------

typedef struct {
    char *abs_path;
    char *rel_path;
    unsigned long long size;
    time_t last_scan; // when we scanned
    unsigned long long ino; // inode of directory
    time_t dir_mtime; // mtime of directory at scan time
} CacheEntry;

#define CACHE_SHARDS 64

typedef struct {
    CacheEntry *v;
    size_t n;
    size_t cap;
    pthread_mutex_t mu;
} CacheShard;

typedef struct {
    CacheShard shards[CACHE_SHARDS];
} Cache;

typedef struct {
    char *name;
    char *abs_path;
    int is_dir;
    unsigned long long size;
    long long delta; // diff from snapshot
    int size_known;
    time_t mtime;
    int depth;    // depth in tree view
    int expanded; // if dir is expanded in tree view
} ViewEntry;

typedef struct {
    ViewEntry *v;
    size_t n;
    size_t cap;
    size_t selected;
    char *path; // abs
    int sizew;
    unsigned long long view_total;
} DirView;

static DirView g_dv_parent;
static DirView g_dv_preview;

static void draw_status(const char *msg);
static int compute_size_col_width(const DirView *dv);
static unsigned long long scan_dir_parallel_deep(const char *root, const char *cache_abs, Cache *cache, int threads);
static void cache_adjust_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, long long delta);
static void cache_add_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, unsigned long long delta);
static void cache_remove_prefix(Cache *c, const char *prefix);
static int is_textual_file(const char *path);

#define CACHE_FILENAME ".fastdu_cache_v3"
#define FASTDU_VERSION "0.70.0"

static int g_global_search_mode = 0;
typedef struct {
    char *abs_path;
    unsigned long long size;
    int is_dir;
} SearchResult;
static SearchResult *g_search_raw = NULL;
static size_t g_search_raw_count = 0;
static size_t g_search_raw_cap = 0;
static SearchResult *g_search_results = NULL;
static size_t g_search_results_count = 0;
static size_t g_search_results_cap = 0;
static int g_search_selected = 0;
static int g_search_top = 0;
static int g_search_filter = 0; // 0=all, 1=dirs, 2=files

static int g_tree_mode = 0; // Tree view mode toggle

// ------------------------------
// Remote Protocol & Server
// ------------------------------
#define R_MSG_ENTRY 0x01
#define R_MSG_DONE  0x02
#define R_MSG_ERROR 0x03

static void run_server(const char *root, Cache *cache) {
    // In server mode, we emit the entire cache to stdout
    // Protocol: [1-byte TYPE] [DATA...]
    
    for (int i = 0; i < CACHE_SHARDS; i++) {
        pthread_mutex_lock(&cache->shards[i].mu);
        for (size_t j = 0; j < cache->shards[i].n; j++) {
            CacheEntry *e = &cache->shards[i].v[j];
            
            uint8_t type = R_MSG_ENTRY;
            fwrite(&type, 1, 1, stdout);
            
            uint32_t abs_len = (uint32_t)strlen(e->abs_path);
            fwrite(&abs_len, 4, 1, stdout);
            fwrite(e->abs_path, 1, abs_len, stdout);
            
            fwrite(&e->size, 8, 1, stdout);
            fwrite(&e->ino, 8, 1, stdout);
            fwrite(&e->dir_mtime, 8, 1, stdout);
        }
        pthread_mutex_unlock(&cache->shards[i].mu);
        fflush(stdout);
    }
    
    uint8_t done = R_MSG_DONE;
    fwrite(&done, 1, 1, stdout);
    fflush(stdout);
    
    // After sending the initial cache, the server could wait for commands (DELETE, RENAME, etc.)
    // For now, we just exit after the dump to test the transport.
}

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
    printf("  -x, --one-file-system Stay on the same file system (do not cross mount points)\n");
    printf("  --server [path]      Start in server mode (SSH backend communication)\n");
    printf("  -e PAT, --exclude PAT Exclude files/dirs matching exact PAT\n");
    printf("  --diff FILE          Compare with snapshot cache FILE\n");
    printf("  --export FMT FILE    Export results to FILE in FMT (json|csv) and exit\n");
    printf("  -D, --decorative     Decorative UI (column headers, vertical separator, extra colors)\n");
    printf("  -nf, --nerd-fonts    Enable Nerd Fonts icons support (requires compatible font)\n");
    printf("  -j N, --jobs N       Number of worker threads (default: CPUs, max 64)\n\n");
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
static int g_decorative = 0;    // decorative UI: separators, header bar, extra colors
static int g_one_file_system = 0;
static int g_show_graph = 0;
static int g_use_nerd_fonts = 0; // Nerd Fonts icons support
static int g_miller_mode = 0;    // Miller columns (ranger-style) toggle
static int g_preview_focused = 0; // Focus on the preview column
static int g_preview_scroll_y = 0;
static int g_preview_scroll_x = 0;
static const char *g_bat_cmd = NULL; // Cached bat command
static dev_t g_root_dev = 0;

typedef struct {
    char ext[32];
    char cmd[128];
} ExtAssociation;

typedef struct {
    short pairs[9][2]; // [pair_id][fg, bg]
    int keys[256];     // mappatura tasti custom
    char editor[256];  // external editor command
    ExtAssociation associations[64];
    int num_associations;
} AppConfig;

static AppConfig g_config;
static const char *g_themes[] = {"dark", "dracula", "tokyonight", "light", "pastel"};
static int g_current_theme_idx = 0;

static int g_diff_mode = 0;
static Cache g_snapshot_cache;

static char *g_inside_archive_path = NULL;
static char *g_archive_subpath = NULL;

static int is_archive_file(const char *path) {
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    int r = archive_read_open_filename(a, path, 10240);
    archive_read_free(a);
    return (r == ARCHIVE_OK);
}

static short parse_color(const char *name) {
    if (strcasecmp(name, "black") == 0) return COLOR_BLACK;
    if (strcasecmp(name, "red") == 0) return COLOR_RED;
    if (strcasecmp(name, "green") == 0) return COLOR_GREEN;
    if (strcasecmp(name, "yellow") == 0) return COLOR_YELLOW;
    if (strcasecmp(name, "blue") == 0) return COLOR_BLUE;
    if (strcasecmp(name, "magenta") == 0) return COLOR_MAGENTA;
    if (strcasecmp(name, "cyan") == 0) return COLOR_CYAN;
    if (strcasecmp(name, "white") == 0) return COLOR_WHITE;
    return -1; // default/transparent
}

static void load_default_config(void) {
    // Default Color Pairs
    g_config.pairs[1][0] = COLOR_BLACK;  g_config.pairs[1][1] = COLOR_BLUE;   // header
    g_config.pairs[2][0] = COLOR_BLUE;   g_config.pairs[2][1] = -1;           // accent
    g_config.pairs[3][0] = COLOR_CYAN;   g_config.pairs[3][1] = -1;           // dirs
    g_config.pairs[4][0] = COLOR_WHITE;  g_config.pairs[4][1] = -1;           // files
    g_config.pairs[5][0] = COLOR_GREEN;  g_config.pairs[5][1] = -1;           // size small
    g_config.pairs[6][0] = COLOR_YELLOW; g_config.pairs[6][1] = -1;           // size med
    g_config.pairs[7][0] = COLOR_RED;    g_config.pairs[7][1] = -1;           // size large
    g_config.pairs[8][0] = COLOR_YELLOW; g_config.pairs[8][1] = COLOR_BLUE;   // highlight
    g_config.editor[0] = '\0'; // default empty
    g_config.num_associations = 0;
}

static void init_app_colors(void) {
    if (has_colors()) {
        for (int i = 1; i <= 8; i++) {
            init_pair((short)i, g_config.pairs[i][0], g_config.pairs[i][1]);
        }
    }
}

static void apply_theme(const char *name) {
    // Sync index if name matches a preset
    for (int i = 0; i < (int)(sizeof(g_themes)/sizeof(g_themes[0])); i++) {
        if (strcasecmp(name, g_themes[i]) == 0) {
            g_current_theme_idx = i;
            break;
        }
    }

    if (strcasecmp(name, "dracula") == 0) {
        g_config.pairs[1][0] = COLOR_MAGENTA; g_config.pairs[1][1] = COLOR_BLACK; // header
        g_config.pairs[2][0] = COLOR_CYAN;    g_config.pairs[2][1] = -1;          // accent
        g_config.pairs[3][0] = COLOR_BLUE;    g_config.pairs[3][1] = -1;          // dirs
        g_config.pairs[4][0] = COLOR_WHITE;   g_config.pairs[4][1] = -1;          // files
        g_config.pairs[5][0] = COLOR_GREEN;   g_config.pairs[5][1] = -1;          // size s
        g_config.pairs[6][0] = COLOR_YELLOW;  g_config.pairs[6][1] = -1;          // size m
        g_config.pairs[7][0] = COLOR_RED;     g_config.pairs[7][1] = -1;          // size l
        g_config.pairs[8][0] = COLOR_CYAN;    g_config.pairs[8][1] = COLOR_MAGENTA; // highlight
    } else if (strcasecmp(name, "tokyonight") == 0 || strcasecmp(name, "tokyo") == 0) {
        g_config.pairs[1][0] = COLOR_BLUE;    g_config.pairs[1][1] = COLOR_BLACK;
        g_config.pairs[2][0] = COLOR_MAGENTA; g_config.pairs[2][1] = -1;
        g_config.pairs[3][0] = COLOR_CYAN;    g_config.pairs[3][1] = -1;
        g_config.pairs[4][0] = COLOR_WHITE;   g_config.pairs[4][1] = -1;
        g_config.pairs[5][0] = COLOR_BLUE;    g_config.pairs[5][1] = -1;
        g_config.pairs[6][0] = COLOR_MAGENTA; g_config.pairs[6][1] = -1;
        g_config.pairs[7][0] = COLOR_RED;     g_config.pairs[7][1] = -1;
        g_config.pairs[8][0] = COLOR_BLACK;   g_config.pairs[8][1] = COLOR_CYAN;
    } else if (strcasecmp(name, "light") == 0) {
        g_config.pairs[1][0] = COLOR_WHITE;   g_config.pairs[1][1] = COLOR_BLUE;
        g_config.pairs[2][0] = COLOR_BLUE;    g_config.pairs[2][1] = -1;
        g_config.pairs[3][0] = COLOR_BLUE;    g_config.pairs[3][1] = -1;
        g_config.pairs[4][0] = COLOR_BLACK;   g_config.pairs[4][1] = -1;
        g_config.pairs[5][0] = COLOR_GREEN;   g_config.pairs[5][1] = -1;
        g_config.pairs[6][0] = COLOR_YELLOW;  g_config.pairs[6][1] = -1;
        g_config.pairs[7][0] = COLOR_RED;     g_config.pairs[7][1] = -1;
        g_config.pairs[8][0] = COLOR_WHITE;   g_config.pairs[8][1] = COLOR_BLUE;
    } else if (strcasecmp(name, "pastel") == 0) {
        g_config.pairs[1][0] = COLOR_CYAN;    g_config.pairs[1][1] = COLOR_BLACK;
        g_config.pairs[2][0] = COLOR_GREEN;   g_config.pairs[2][1] = -1;
        g_config.pairs[3][0] = COLOR_MAGENTA; g_config.pairs[3][1] = -1;
        g_config.pairs[4][0] = COLOR_WHITE;   g_config.pairs[4][1] = -1;
        g_config.pairs[5][0] = COLOR_CYAN;    g_config.pairs[5][1] = -1;
        g_config.pairs[6][0] = COLOR_GREEN;   g_config.pairs[6][1] = -1;
        g_config.pairs[7][0] = COLOR_YELLOW;  g_config.pairs[7][1] = -1;
        g_config.pairs[8][0] = COLOR_BLACK;   g_config.pairs[8][1] = COLOR_GREEN;
    } else { // default dark
        load_default_config();
        g_current_theme_idx = 0;
    }
    if (g_tui_active) init_app_colors();
}

static void load_config_file(void) {
    load_default_config();
    const char *home = getenv("HOME");
    if (!home) return;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/fastdu/config.toml", home);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[512];
    int section_associations = 0;
    while (fgets(line, sizeof(line), f)) {
        // Rimuovi spazi iniziali e finali
        char *p = line;
        while (isspace(*p)) p++;
        if (*p == '#' || *p == '\0') continue;

        if (*p == '[') {
            if (strstr(p, "[associations]")) section_associations = 1;
            else section_associations = 0;
            continue;
        }

        char key[128], val[128];
        if (sscanf(p, "%127[^= ] = \"%127[^\"]\"", key, val) == 2 || 
            sscanf(p, "%127[^= ] = %127s", key, val) == 2) {
            
            if (section_associations) {
                if (g_config.num_associations < 64) {
                    strncpy(g_config.associations[g_config.num_associations].ext, key, 31);
                    g_config.associations[g_config.num_associations].ext[31] = '\0';
                    strncpy(g_config.associations[g_config.num_associations].cmd, val, 127);
                    g_config.associations[g_config.num_associations].cmd[127] = '\0';
                    g_config.num_associations++;
                }
            } else {
                if (strcmp(key, "theme") == 0) apply_theme(val);
                else if (strcmp(key, "editor") == 0) {
                    strncpy(g_config.editor, val, sizeof(g_config.editor)-1);
                    g_config.editor[sizeof(g_config.editor)-1] = '\0';
                }
                else if (strcmp(key, "header_fg") == 0) g_config.pairs[1][0] = parse_color(val);
                else if (strcmp(key, "header_bg") == 0) g_config.pairs[1][1] = parse_color(val);
                else if (strcmp(key, "accent_fg") == 0) g_config.pairs[2][0] = parse_color(val);
                else if (strcmp(key, "dir_fg") == 0)    g_config.pairs[3][0] = parse_color(val);
                else if (strcmp(key, "file_fg") == 0)   g_config.pairs[4][0] = parse_color(val);
                else if (strcmp(key, "size_s_fg") == 0) g_config.pairs[5][0] = parse_color(val);
                else if (strcmp(key, "size_m_fg") == 0) g_config.pairs[6][0] = parse_color(val);
                else if (strcmp(key, "size_l_fg") == 0) g_config.pairs[7][0] = parse_color(val);
            }
        }
    }
    fclose(f);
}

static const char *get_icon(const char *name, int is_dir) {
    if (!g_use_nerd_fonts) return "";
    if (is_dir) return " "; // Folder icon
    
    const char *dot = strrchr(name, '.');
    if (!dot) return " "; // Default file icon
    
    if (strcasecmp(dot, ".c") == 0 || strcasecmp(dot, ".h") == 0) return " ";
    if (strcasecmp(dot, ".py") == 0) return " ";
    if (strcasecmp(dot, ".js") == 0 || strcasecmp(dot, ".ts") == 0) return " ";
    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".css") == 0) return " ";
    if (strcasecmp(dot, ".md") == 0) return " ";
    if (strcasecmp(dot, ".json") == 0 || strcasecmp(dot, ".yaml") == 0 || strcasecmp(dot, ".yml") == 0) return " ";
    if (strcasecmp(dot, ".pdf") == 0) return " ";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".gif") == 0) return " ";
    if (strcasecmp(dot, ".zip") == 0 || strcasecmp(dot, ".tar") == 0 || strcasecmp(dot, ".gz") == 0 || strcasecmp(dot, ".7z") == 0 || strcasecmp(dot, ".rar") == 0 || strcasecmp(dot, ".iso") == 0 || strcasecmp(dot, ".bz2") == 0 || strcasecmp(dot, ".xz") == 0) return " ";
    if (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".flac") == 0) return " ";
    if (strcasecmp(dot, ".mp4") == 0 || strcasecmp(dot, ".mkv") == 0 || strcasecmp(dot, ".avi") == 0) return " ";
    
    return " "; // Default file icon
}

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

// ------------------------------
// Duplicate Finder (Hashing & Compare)
// ------------------------------
typedef struct {
    char *path;
    unsigned long long size;
    uint64_t head_hash;
    int dupe_group_id;
} FileCandidate;

typedef struct {
    FileCandidate *v;
    size_t n;
    size_t cap;
} FileCandidateList;

static uint64_t fnv1a_hash(const void *data, size_t len) {
    uint64_t hash = 14695981039346656037ULL;
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t hash_file_head(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t r = read(fd, buf, sizeof(buf));
    close(fd);
    if (r <= 0) return 0;
    return fnv1a_hash(buf, (size_t)r);
}

static int files_are_identical(const char *p1, const char *p2) {
    int fd1 = open(p1, O_RDONLY | O_NOFOLLOW);
    if (fd1 < 0) return 0;
    int fd2 = open(p2, O_RDONLY | O_NOFOLLOW);
    if (fd2 < 0) { close(fd1); return 0; }
    
    char buf1[32768];
    char buf2[32768];
    int identical = 1;
    while (1) {
        ssize_t r1 = read(fd1, buf1, sizeof(buf1));
        ssize_t r2 = read(fd2, buf2, sizeof(buf2));
        if (r1 != r2) { identical = 0; break; }
        if (r1 <= 0) break; // EOF or error
        if (memcmp(buf1, buf2, r1) != 0) { identical = 0; break; }
    }
    close(fd1);
    close(fd2);
    return identical;
}

// ------------------------------
// InodeSet (Hard link detection)
// ------------------------------
typedef struct InodeEntry {
    dev_t dev;
    ino_t ino;
    struct InodeEntry *next;
} InodeEntry;

typedef struct {
    InodeEntry **buckets;
    size_t num_buckets;
    pthread_mutex_t mu;
} InodeSet;

static void inodeset_init(InodeSet *s, size_t buckets) {
    s->num_buckets = buckets;
    s->buckets = calloc(buckets, sizeof(InodeEntry*));
    pthread_mutex_init(&s->mu, NULL);
}

static int inodeset_check_and_add(InodeSet *s, dev_t dev, ino_t ino) {
    if (!s || !s->buckets) return 0;
    // Simple hash for dev/ino
    size_t h = (((size_t)dev) ^ ((size_t)ino)) % s->num_buckets;
    pthread_mutex_lock(&s->mu);
    InodeEntry *curr = s->buckets[h];
    while (curr) {
        if (curr->dev == dev && curr->ino == ino) {
            pthread_mutex_unlock(&s->mu);
            return 1; // Already seen
        }
        curr = curr->next;
    }
    InodeEntry *new_e = malloc(sizeof(InodeEntry));
    if (!new_e) { pthread_mutex_unlock(&s->mu); return 0; }
    new_e->dev = dev;
    new_e->ino = ino;
    new_e->next = s->buckets[h];
    s->buckets[h] = new_e;
    pthread_mutex_unlock(&s->mu);
    return 0; // New
}

static void inodeset_free(InodeSet *s) {
    if (!s || !s->buckets) return;
    for (size_t i = 0; i < s->num_buckets; i++) {
        InodeEntry *curr = s->buckets[i];
        while (curr) {
            InodeEntry *tmp = curr;
            curr = curr->next;
            free(tmp);
        }
    }
    free(s->buckets);
    pthread_mutex_destroy(&s->mu);
}

// ------------------------------
// Exclude system
// ------------------------------
typedef struct {
    char **patterns;
    size_t n;
    size_t cap;
} ExcludeList;

static ExcludeList g_excludes = {NULL, 0, 0};

static void exclude_add(const char *p) {
    if (g_excludes.n == g_excludes.cap) {
        size_t nc = g_excludes.cap ? g_excludes.cap * 2 : 8;
        char **np = realloc(g_excludes.patterns, nc * sizeof(char*));
        if (!np) return;
        g_excludes.patterns = np;
        g_excludes.cap = nc;
    }
    g_excludes.patterns[g_excludes.n++] = xstrdup(p);
}

static int is_excluded(const char *name) {
    for (size_t i = 0; i < g_excludes.n; i++) {
        // For now, simple exact match or suffix match for common cases like node_modules
        if (strcmp(name, g_excludes.patterns[i]) == 0) return 1;
    }
    return 0;
}

static void exclude_free(void) {
    for (size_t i = 0; i < g_excludes.n; i++) free(g_excludes.patterns[i]);
    free(g_excludes.patterns);
    g_excludes.patterns = NULL; g_excludes.n = g_excludes.cap = 0;
}

static void load_fastduignore(const char *root) {
    char *p = path_join(root, ".fastduignore");
    if (!p) return;
    FILE *f = fopen(p, "r");
    free(p);
    if (!f) return;
    char *line = NULL; size_t len = 0; ssize_t r;
    while ((r = getline(&line, &len, f)) != -1) {
        if (r > 0 && line[r-1] == '\n') line[--r] = '\0';
        if (r > 0 && line[r-1] == '\r') line[--r] = '\0';
        if (r == 0 || line[0] == '#') continue;
        exclude_add(line);
    }
    free(line);
    fclose(f);
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

static const char *get_file_extension(const char *path) {
    const char *base = path_basename_const(path);
    const char *dot = strrchr(base, '.');
    // If no dot, or it's the first char (hidden file with no further extension like .bashrc), return empty
    if (!dot || dot == base) return "";
    return dot;
}

// ------------------------------
// Image Metadata Parser
// ------------------------------
static int get_image_dims(const char *path, int *pw, int *ph) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char buf[32];
    if (fread(buf, 1, 32, f) < 24) { fclose(f); return 0; }

    // PNG: [8]signature, [4]length, [4]type="IHDR", [4]width, [4]height
    if (memcmp(buf, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(buf + 12, "IHDR", 4) == 0) {
        *pw = (buf[16] << 24) | (buf[17] << 16) | (buf[18] << 8) | buf[19];
        *ph = (buf[20] << 24) | (buf[21] << 16) | (buf[22] << 8) | buf[23];
        fclose(f); return 1;
    }
    // GIF: "GIF87a" or "GIF89a", [2]width LE, [2]height LE
    if (memcmp(buf, "GIF8", 4) == 0) {
        *pw = buf[6] | (buf[7] << 8);
        *ph = buf[8] | (buf[9] << 8);
        fclose(f); return 1;
    }
    // BMP: "BM", width at 18, height at 22 (int32)
    if (buf[0] == 'B' && buf[1] == 'M') {
        *pw = (int)(buf[18] | (buf[19] << 8) | (buf[20] << 16) | (buf[21] << 24));
        *ph = (int)(buf[22] | (buf[23] << 8) | (buf[24] << 16) | (buf[25] << 24));
        if (*ph < 0) *ph = -*ph;
        fclose(f); return 1;
    }
    // WebP: "RIFF" .... "WEBP"
    if (memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WEBP", 4) == 0) {
        if (memcmp(buf + 12, "VP8 ", 4) == 0) { // Lossy
            *pw = buf[26] | ((buf[27] & 0x3f) << 8);
            *ph = buf[28] | ((buf[29] & 0x3f) << 8);
            fclose(f); return 1;
        } else if (memcmp(buf + 12, "VP8L", 4) == 0) { // Lossless
            *pw = 1 + (buf[21] | ((buf[22] & 0x3f) << 8));
            *ph = 1 + (((buf[22] & 0xc0) >> 6) | (buf[23] << 2) | ((buf[24] & 0x03) << 10));
            fclose(f); return 1;
        }
    }
    // JPEG: scan for SOF markers (0xFFC0 - 0xFFC3) correctly skipping other segments
    if (buf[0] == 0xFF && buf[1] == 0xD8) {
        fseek(f, 2, SEEK_SET);
        while (1) {
            unsigned char m[2];
            if (fread(m, 1, 2, f) != 2) break;
            if (m[0] != 0xFF) break;
            if (m[1] == 0xD9 || m[1] == 0xDA) break; // End of image or Start of Scan
            
            unsigned char lbuf[2];
            if (fread(lbuf, 1, 2, f) != 2) break;
            unsigned short len = (lbuf[0] << 8) | lbuf[1];
            
            if (m[1] >= 0xC0 && m[1] <= 0xC3) { // SOF0 - SOF3 markers
                unsigned char dims[5];
                if (fread(dims, 1, 5, f) == 5) {
                    *ph = (dims[1] << 8) | dims[2];
                    *pw = (dims[3] << 8) | dims[4];
                    fclose(f); return 1;
                }
                break;
            }
            if (len < 2) break;
            fseek(f, len - 2, SEEK_CUR);
        }
    }
    fclose(f); return 0;
}

static int is_image_file(const char *path) {
    const char *base = path_basename_const(path);
    const char *ext = strrchr(base, '.');
    if (!ext) return 0;
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0 ||
        strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".gif") == 0 ||
        strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".webp") == 0 ||
        strcasecmp(ext, ".tiff") == 0 || strcasecmp(ext, ".ico") == 0) return 1;
    return 0;
}

// ------------------------------
// Base64 Encoder (for Native Graphics)
// ------------------------------
static char *base64_encode(const unsigned char *data, size_t input_length, size_t *output_length) {
    static const char encoding_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    *output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(*output_length + 1);
    if (encoded_data == NULL) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
    }

    const int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[*output_length - 1 - i] = '=';

    encoded_data[*output_length] = '\0';
    return encoded_data;
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
    snprintf(msg, sizeof(msg), "Conflict on '%s': [o]verwrite, [r]ename, [n]ew name, [s]kip, [O] overwrite all, [R] rename all, [S] skip all ", dst);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, msg, cols-1);
    refresh();
    int ch = getch();
    if (ch=='o'||ch=='O') return (ch=='O') ? 'O' : 'o';
    if (ch=='r'||ch=='R') return (ch=='R') ? 'R' : 'r';
    if (ch=='n') return 'n'; // New name option
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

static size_t cache_hash(const char *s) {
    size_t h = 5381;
    int c;
    while ((c = *s++)) h = ((h << 5) + h) + (size_t)c;
    return h % CACHE_SHARDS;
}

static void cache_init(Cache *c) {
    for (int i = 0; i < CACHE_SHARDS; i++) {
        c->shards[i].v = NULL;
        c->shards[i].n = c->shards[i].cap = 0;
        pthread_mutex_init(&c->shards[i].mu, NULL);
    }
}

static void cache_free(Cache *c) {
    for (int i = 0; i < CACHE_SHARDS; i++) {
        pthread_mutex_lock(&c->shards[i].mu);
        for (size_t j = 0; j < c->shards[i].n; j++) {
            free(c->shards[i].v[j].abs_path);
            free(c->shards[i].v[j].rel_path);
        }
        free(c->shards[i].v);
        c->shards[i].v = NULL; c->shards[i].n = c->shards[i].cap = 0;
        pthread_mutex_unlock(&c->shards[i].mu);
        pthread_mutex_destroy(&c->shards[i].mu);
    }
}

static ssize_t cache_find_index_nl(CacheShard *s, const char *abs_path) {
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->v[i].abs_path, abs_path) == 0) return (ssize_t)i;
    }
    return -1;
}

static void cache_upsert_with_meta(Cache *c, const char *root, const char *abs_path,
                                   unsigned long long size, time_t now,
                                   unsigned long long ino, time_t dir_mtime) {
    size_t h = cache_hash(abs_path);
    CacheShard *s = &c->shards[h];
    pthread_mutex_lock(&s->mu);
    ssize_t idx = cache_find_index_nl(s, abs_path);
    if (idx >= 0) {
        s->v[idx].size = size;
        s->v[idx].last_scan = now;
        s->v[idx].ino = ino;
        s->v[idx].dir_mtime = dir_mtime;
    } else {
        if (s->n == s->cap) {
            size_t newcap = s->cap ? s->cap * 2 : 16;
            void *nv = realloc(s->v, newcap * sizeof(CacheEntry));
            if (!nv) { pthread_mutex_unlock(&s->mu); return; }
            s->v = (CacheEntry*)nv; s->cap = newcap;
        }
        CacheEntry *e = &s->v[s->n++];
        e->abs_path = xstrdup(abs_path);
        e->rel_path = relpath_from_abs(root, abs_path);
        e->size = size;
        e->last_scan = now;
        e->ino = ino;
        e->dir_mtime = dir_mtime;
    }
    pthread_mutex_unlock(&s->mu);
}

// Helper for single value retrieval to avoid returning pointers to unstable memory
static int cache_get_info(Cache *c, const char *abs_path, unsigned long long *size, time_t *mtime, unsigned long long *ino) {
    size_t h = cache_hash(abs_path);
    CacheShard *s = &c->shards[h];
    int found = 0;
    pthread_mutex_lock(&s->mu);
    ssize_t idx = cache_find_index_nl(s, abs_path);
    if (idx >= 0) {
        if (size) *size = s->v[idx].size;
        if (mtime) *mtime = s->v[idx].dir_mtime;
        if (ino) *ino = s->v[idx].ino;
        found = 1;
    }
    pthread_mutex_unlock(&s->mu);
    return found;
}

static CacheEntry *cache_upsert(Cache *c, const char *root, const char *abs_path, unsigned long long size, time_t now) {
    cache_upsert_with_meta(c, root, abs_path, size, now, 0ULL, 0);
    return NULL; // Return NULL as we don't want to expose unstable pointers
}

static void cache_update_size(Cache *c, const char *abs_path, long long delta, time_t mtime) {
    size_t h = cache_hash(abs_path);
    CacheShard *s = &c->shards[h];
    pthread_mutex_lock(&s->mu);
    ssize_t idx = cache_find_index_nl(s, abs_path);
    if (idx >= 0) {
        if (delta >= 0) {
            s->v[idx].size += (unsigned long long)delta;
        } else {
            unsigned long long abs_delta = (unsigned long long)(-delta);
            if (s->v[idx].size > abs_delta) s->v[idx].size -= abs_delta;
            else s->v[idx].size = 0ULL;
        }
        s->v[idx].last_scan = time(NULL);
        if (mtime != 0) s->v[idx].dir_mtime = mtime;
    }
    pthread_mutex_unlock(&s->mu);
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
    
    size_t cap = 1024 * 1024; // 1MB initial
    char *buf = malloc(cap);
    if (!buf) { free(cache_path); return -1; }
    size_t off = 0;

    #define APPEND_BUF(...) do { \
        char _tmp[PATH_MAX + 512]; \
        int _len = snprintf(_tmp, sizeof(_tmp), __VA_ARGS__); \
        if (off + _len >= cap) { \
            cap *= 2; \
            char *_nb = realloc(buf, cap); \
            if (!_nb) { free(buf); free(cache_path); return -1; } \
            buf = _nb; \
        } \
        memcpy(buf + off, _tmp, _len); \
        off += _len; \
    } while(0)

    APPEND_BUF("# fastdu-cache v3\n");
    APPEND_BUF("root\t%s\n", root);
    
    unsigned long long total_bytes = 0ULL;
    cache_get_info((Cache*)c, root, &total_bytes, NULL, NULL);
    APPEND_BUF("totals\t%llu\n", (unsigned long long)total_bytes);
    APPEND_BUF("totals_files\t%llu\n", (unsigned long long)g_last_files);

    for (int i = 0; i < CACHE_SHARDS; i++) {
        pthread_mutex_lock((pthread_mutex_t*)&c->shards[i].mu);
        for (size_t j = 0; j < c->shards[i].n; j++) {
            CacheEntry *e = &c->shards[i].v[j];
            char *rel = relpath_from_abs(root, e->abs_path);
            char *enc = pct_encode(rel ? rel : ".");
            APPEND_BUF("D\t%s\t%llu\t%ld\t%llu\t%ld\n",
                    enc, (unsigned long long)e->size, (long)e->last_scan,
                    (unsigned long long)e->ino, (long)e->dir_mtime);
            free(enc); free(rel);
        }
        pthread_mutex_unlock((pthread_mutex_t*)&c->shards[i].mu);
    }

    // Now compress the buffer
    size_t c_cap = ZSTD_compressBound(off);
    void *c_buf = malloc(c_cap);
    if (!c_buf) { free(buf); free(cache_path); return -1; }

    size_t c_sz = ZSTD_compress(c_buf, c_cap, buf, off, 3);
    if (ZSTD_isError(c_sz)) { free(buf); free(c_buf); free(cache_path); return -1; }

    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", cache_path);
    FILE *f = fopen(temp_path, "wb");
    if (!f) { free(buf); free(c_buf); free(cache_path); return -1; }
    fwrite(c_buf, 1, c_sz, f);
    fclose(f);
    rename(temp_path, cache_path);

    free(buf);
    free(c_buf);
    free(cache_path);
    return 0;
}

static void cache_export_json(const Cache *c, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) { perror("fopen export"); return; }
    fprintf(f, "[\n");
    int first = 1;
    for (int h = 0; h < CACHE_SHARDS; h++) {
        pthread_mutex_lock((pthread_mutex_t*)&c->shards[h].mu);
        for (size_t i = 0; i < c->shards[h].n; i++) {
            if (!first) fprintf(f, ",\n");
            first = 0;
            fprintf(f, "  {\n");
            fprintf(f, "    \"path\": \"");
            for (const char *p = c->shards[h].v[i].abs_path; *p; p++) {
                if (*p == '"' || *p == '\\') fputc('\\', f);
                fputc(*p, f);
            }
            fprintf(f, "\",\n");
            fprintf(f, "    \"size\": %llu,\n", c->shards[h].v[i].size);
            fprintf(f, "    \"last_scan\": %ld,\n", (long)c->shards[h].v[i].last_scan);
            fprintf(f, "    \"ino\": %llu,\n", c->shards[h].v[i].ino);
            fprintf(f, "    \"mtime\": %ld\n", (long)c->shards[h].v[i].dir_mtime);
            fprintf(f, "  }");
        }
        pthread_mutex_unlock((pthread_mutex_t*)&c->shards[h].mu);
    }
    fprintf(f, "\n]\n");
    fclose(f);
}

static void cache_export_csv(const Cache *c, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) { perror("fopen export"); return; }
    fprintf(f, "path,size,last_scan,ino,mtime\n");
    for (int h = 0; h < CACHE_SHARDS; h++) {
        pthread_mutex_lock((pthread_mutex_t*)&c->shards[h].mu);
        for (size_t i = 0; i < c->shards[h].n; i++) {
            // Simple CSV escaping: wrap in quotes and escape quotes
            fprintf(f, "\"");
            for (const char *p = c->shards[h].v[i].abs_path; *p; p++) {
                if (*p == '"') fprintf(f, "\"\"");
                else fputc(*p, f);
            }
            fprintf(f, "\",%llu,%ld,%llu,%ld\n",
                    c->shards[h].v[i].size,
                    (long)c->shards[h].v[i].last_scan,
                    c->shards[h].v[i].ino,
                    (long)c->shards[h].v[i].dir_mtime);
        }
        pthread_mutex_unlock((pthread_mutex_t*)&c->shards[h].mu);
    }
    fclose(f);
}

static void draw_cache_progress(const char *cache_path, FILE *f, long total_bytes, int *spinner, struct timespec *last_draw) {
    if (!g_tui_active) return;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - last_draw->tv_sec) * 1000 + (now.tv_nsec - last_draw->tv_nsec) / 1000000;
    if (ms < 30) return;
    *last_draw = now;
    int cols, rows; getmaxyx(stdscr, rows, cols);
    int barlen = cols > 40 ? (cols - 40) : 20; if (barlen < 10) barlen = 10; if (barlen > 200) barlen = 200;
    char bar[256]; memset(bar, '.', (size_t)barlen);
    int filled = 0; int percent = 0;
    if (total_bytes > 0) {
        long cur = ftell(f); if (cur < 0) cur = 0;
        double frac = (double)cur / (double)total_bytes; if (frac < 0) frac = 0; if (frac > 1) frac = 1;
        filled = (int)(frac * barlen);
        percent = (int)(frac * 100.0 + 0.5);
    } else {
        *spinner = (*spinner + 1) % barlen;
        filled = *spinner;
    }
    if (filled < 0) filled = 0;
    if (filled > barlen) filled = barlen;
    for (int i = 0; i < filled; i++) bar[i] = '#';
    bar[barlen] = '\0';
    char linebuf[PATH_MAX + 256];
    snprintf(linebuf, sizeof(linebuf), " Cache: [%s] %3d%% - %s", bar, percent, cache_path);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, linebuf, cols-1);
    refresh();
}

/*
 * cache_load
 * ----------
 * Attempts to load an existing cache from disk.
 * Returns 1 if loaded, 0 if not found, -1 on error.
 */
static void cache_copy(Cache *dst, Cache *src) {
    cache_init(dst);
    for (int i = 0; i < CACHE_SHARDS; i++) {
        pthread_mutex_lock(&src->shards[i].mu);
        for (size_t j = 0; j < src->shards[i].n; j++) {
            CacheEntry *e = &src->shards[i].v[j];
            cache_upsert_with_meta(dst, e->abs_path, e->abs_path, e->size, e->last_scan, e->ino, e->dir_mtime);
        }
        pthread_mutex_unlock(&src->shards[i].mu);
    }
}

static int cache_load_file(const char *cache_path_in, const char *root, Cache *c) {
    char *cache_path = xstrdup(cache_path_in);
    if (!cache_path) return -1;
    FILE *f = fopen(cache_path, "rb");
    if (!f) { free(cache_path); return 0; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); free(cache_path); return 0; }
    
    unsigned char *fbuf = malloc(fsize);
    if (!fbuf) { fclose(f); free(cache_path); return -1; }
    if (fread(fbuf, 1, fsize, f) != (size_t)fsize) { free(fbuf); fclose(f); free(cache_path); return -1; }
    fclose(f);

    unsigned char *data = fbuf;
    size_t data_sz = fsize;
    void *decomp_buf = NULL;

    // Detect ZSTD magic: 0xFD2FB528 (little endian in file: 28 B5 2F FD)
    if (fsize >= 4 && fbuf[0] == 0x28 && fbuf[1] == 0xB5 && fbuf[2] == 0x2F && fbuf[3] == 0xFD) {
        unsigned long long const dsize = ZSTD_getFrameContentSize(fbuf, fsize);
        if (dsize != ZSTD_CONTENTSIZE_ERROR && dsize != ZSTD_CONTENTSIZE_UNKNOWN) {
            decomp_buf = malloc(dsize);
            if (decomp_buf) {
                size_t const rsize = ZSTD_decompress(decomp_buf, dsize, fbuf, fsize);
                if (!ZSTD_isError(rsize)) {
                    data = decomp_buf;
                    data_sz = rsize;
                } else {
                    free(decomp_buf); decomp_buf = NULL;
                }
            }
        }
    }

    char *p = (char *)data;
    char *end = (char *)data + data_sz;
    int header_ok = 0; int version = 1;
    unsigned long long lines_read = 0ULL;
    unsigned long long count_v1_v2 = 0ULL;

    while (p < end) {
        if (g_interrupted) break;
        char *line_start = p;
        while (p < end && *p != '\n' && *p != '\r') p++;
        size_t line_len = p - line_start;
        
        char line_buf[PATH_MAX + 1024];
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        memcpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';
        while (p < end && (*p == '\n' || *p == '\r')) p++;

        if (line_len == 0) continue;

        if (!header_ok) {
            if (strncmp(line_buf, "# fastdu-cache v3", 17) == 0) { header_ok = 1; version = 3; }
            else if (strncmp(line_buf, "# fastdu-cache v2", 17) == 0) { header_ok = 1; version = 2; }
            else if (strncmp(line_buf, "# fastdu-cache v1", 17) == 0) { header_ok = 1; version = 1; }
            continue;
        }

        if (strncmp(line_buf, "root\t", 5) == 0) continue;
        if (version >= 3 && strncmp(line_buf, "totals\t", 7) == 0) continue;
        if (version >= 3 && strncmp(line_buf, "totals_files\t", 13) == 0) {
            sscanf(line_buf + 13, "%llu", &g_last_files);
            continue;
        }

        if (line_buf[0] == 'D' && line_buf[1] == '\t') {
            count_v1_v2++;
            char *rel = line_buf + 2;
            char *tab1 = strchr(rel, '\t');
            if (!tab1) continue;
            *tab1 = '\0';
            char *size_str = tab1 + 1;
            char *tab2 = strchr(size_str, '\t');
            if (!tab2) continue;
            *tab2 = '\0';
            char *time_str = tab2 + 1;
            char *ino_str = NULL, *mtime_str = NULL;
            if (version >= 2) {
                char *tab3 = strchr(time_str, '\t');
                if (tab3) {
                    *tab3 = '\0';
                    ino_str = tab3 + 1;
                    char *tab4 = strchr(ino_str, '\t');
                    if (tab4) { *tab4 = '\0'; mtime_str = tab4 + 1; }
                }
            }
            char *rel_dec = pct_decode(rel);
            if (rel_dec) {
                char *abs = abspath_from_rel(root, rel_dec);
                if (abs) {
                    unsigned long long size = 0ULL, ino = 0ULL;
                    long last_scan = 0, dir_mtime = 0;
                    sscanf(size_str, "%llu", &size);
                    sscanf(time_str, "%ld", &last_scan);
                    if (ino_str) sscanf(ino_str, "%llu", &ino);
                    if (mtime_str) sscanf(mtime_str, "%ld", &dir_mtime);
                    cache_upsert_with_meta(c, root, abs, size, (time_t)last_scan, ino, (time_t)dir_mtime);
                    free(abs);
                }
                free(rel_dec);
            }
        }
        lines_read++;
    }

    free(fbuf);
    if (decomp_buf) free(decomp_buf);
    free(cache_path);
    return 1;
}

static int cache_load(const char *root, Cache *c) {
    char *cache_path = path_join(root, CACHE_FILENAME);
    if (!cache_path) return -1;
    int rc = cache_load_file(cache_path, root, c);
    free(cache_path);
    return rc;
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
    if (filled < 0) filled = 0;
    if (filled > barlen) filled = barlen;
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
static unsigned long long scan_dir_recursive_fd(int dirfd, const char *abs_path, const char *root, const char *cache_file_abs, Cache *cache, ScanUI *ui, InodeSet *is) {
    unsigned long long total = 0ULL;
    int dupfd = dup(dirfd);
    if (dupfd < 0) return 0ULL;
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); return 0ULL; }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (g_interrupted) break;
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (is_excluded(de->d_name)) continue;
        if (strcmp(abs_path, root) == 0 && strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        unsigned char dtype = de->d_type;
        if (dtype == DT_LNK) continue;
            if (dtype == DT_UNKNOWN) {
                struct stat st0;
                if (fstatat(dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) != 0) continue;
                if (S_ISLNK(st0.st_mode)) continue;
                if (S_ISDIR(st0.st_mode)) dtype = DT_DIR; else if (S_ISREG(st0.st_mode)) dtype = DT_REG; else dtype = DT_UNKNOWN;
                if (dtype == DT_REG) {
                    if (st0.st_nlink <= 1 || !inodeset_check_and_add(is, st0.st_dev, st0.st_ino))
                        total += file_size_bytes(&st0);
                }
            } else if (dtype == DT_REG) {
                struct stat stf;
                if (fstatat(dirfd, de->d_name, &stf, AT_SYMLINK_NOFOLLOW) == 0) {
                    if (stf.st_nlink <= 1 || !inodeset_check_and_add(is, stf.st_dev, stf.st_ino))
                        total += file_size_bytes(&stf);
                }
            } else if (dtype == DT_DIR) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd < 0) continue;
            if (g_one_file_system) {
                struct stat stch;
                if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                    close(cfd);
                    continue;
                }
            }
            char *child_abs = path_join(abs_path, de->d_name);
            if (!child_abs) { close(cfd); continue; }
            unsigned long long sub = scan_dir_recursive_fd(cfd, child_abs, root, cache_file_abs, cache, ui, is);
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
    InodeSet is;
    inodeset_init(&is, 16384);
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { inodeset_free(&is); return 0ULL; }
    unsigned long long total = scan_dir_recursive_fd(fd, dir, root, cache_file_abs, cache, ui, &is);
    close(fd);
    inodeset_free(&is);
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
static int selection_changed = 0;

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

// Breadcrumb tracking for mouse interaction
typedef struct {
    int x_start;
    int x_end;
    char *abs_path;
} BreadcrumbSegment;

#define MAX_BREADCRUMBS 64
static BreadcrumbSegment g_breadcrumbs[MAX_BREADCRUMBS];
static int g_num_breadcrumbs = 0;

static void breadcrumbs_clear(void) {
    for (int i = 0; i < g_num_breadcrumbs; i++) free(g_breadcrumbs[i].abs_path);
    g_num_breadcrumbs = 0;
}

static void breadcrumbs_add(int x_start, int x_end, const char *abs_path) {
    if (g_num_breadcrumbs < MAX_BREADCRUMBS) {
        g_breadcrumbs[g_num_breadcrumbs].x_start = x_start;
        g_breadcrumbs[g_num_breadcrumbs].x_end = x_end;
        g_breadcrumbs[g_num_breadcrumbs].abs_path = xstrdup(abs_path);
        g_num_breadcrumbs++;
    }
}

// Forward declaration needed for markset normalization helpers
static int path_is_parent_of(const char *parent, const char *child);

// Async marked-files calculator state
static atomic_int g_mf_inflight = 0;               // 1 when a background count is running
static atomic_ullong g_marked_files_value = 0ULL;  // last computed value (eventually consistent)

static void schedule_marked_files_recalc(void);

static void search_results_free(void) {
    for (size_t i = 0; i < g_search_raw_count; i++) {
        free(g_search_raw[i].abs_path);
    }
    free(g_search_raw);
    g_search_raw = NULL; g_search_raw_count = 0; g_search_raw_cap = 0;

    free(g_search_results);
    g_search_results = NULL; g_search_results_count = 0; g_search_results_cap = 0;
}

static int cmp_search_results(const void *a, const void *b) {
    const SearchResult *ra = (const SearchResult *)a;
    const SearchResult *rb = (const SearchResult *)b;
    if (ra->is_dir != rb->is_dir) return rb->is_dir - ra->is_dir; // Dirs first
    if (ra->size != rb->size) return (rb->size < ra->size) ? 1 : -1; // Largest size first
    return strcmp(ra->abs_path, rb->abs_path); // Then alpha
}

static void apply_search_filter(void) {
    g_search_results_count = 0;
    for (size_t i = 0; i < g_search_raw_count; i++) {
        int match = 0;
        if (g_search_filter == 0) match = 1;
        else if (g_search_filter == 1 && g_search_raw[i].is_dir) match = 1;
        else if (g_search_filter == 2 && !g_search_raw[i].is_dir) match = 1;
        
        if (match) {
            if (g_search_results_count == g_search_results_cap) {
                size_t nc = g_search_results_cap ? g_search_results_cap * 2 : 64;
                void *nv = realloc(g_search_results, nc * sizeof(SearchResult));
                if (!nv) break;
                g_search_results = (SearchResult*)nv;
                g_search_results_cap = nc;
            }
            g_search_results[g_search_results_count++] = g_search_raw[i];
        }
    }
    if (g_search_results_count > 0) {
        qsort(g_search_results, g_search_results_count, sizeof(SearchResult), cmp_search_results);
    }
    g_search_selected = 0;
    g_search_top = 0;
}

static void perform_global_search(Cache *cache, const char *query) {
    search_results_free();
    if (!query || query[0] == '\0') return;

    int full_path_search = (query[0] == '/');
    const char *search_term = full_path_search ? query + 1 : query;
    if (search_term[0] == '\0') return;

    for (int i = 0; i < CACHE_SHARDS; i++) {
        pthread_mutex_lock(&cache->shards[i].mu);
        for (size_t j = 0; j < cache->shards[i].n; j++) {
            CacheEntry *e = &cache->shards[i].v[j];
            int match = 0;
            if (full_path_search) {
                if (strcasestr_bool(e->abs_path, search_term)) match = 1;
            } else {
                const char *base = path_basename_const(e->abs_path);
                if (strcasestr_bool(base, search_term)) match = 1;
            }

            if (match) {
                if (g_search_raw_count >= 2000) break; // Allow more raw matches
                
                if (g_search_raw_count == g_search_raw_cap) {
                    size_t newcap = g_search_raw_cap ? g_search_raw_cap * 2 : 64;
                    void *nv = realloc(g_search_raw, newcap * sizeof(SearchResult));
                    if (!nv) break;
                    g_search_raw = (SearchResult*)nv;
                    g_search_raw_cap = newcap;
                }
                g_search_raw[g_search_raw_count].abs_path = xstrdup(e->abs_path);
                g_search_raw[g_search_raw_count].size = e->size;
                
                struct stat st;
                if (lstat(e->abs_path, &st) == 0) {
                    g_search_raw[g_search_raw_count].is_dir = S_ISDIR(st.st_mode);
                } else {
                    g_search_raw[g_search_raw_count].is_dir = (e->ino > 0 && e->size == 0);
                }
                g_search_raw_count++;
            }
        }
        pthread_mutex_unlock(&cache->shards[i].mu);
        if (g_search_raw_count >= 2000) break;
    }
    apply_search_filter();
}

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
typedef enum { SORT_SIZE = 0, SORT_NAME = 1, SORT_MTIME = 2, SORT_DELTA = 3, SORT_EXT = 4 } SortMode;
static SortMode g_sort_mode = SORT_SIZE;
static const char *sort_mode_label(void) {
    switch (g_sort_mode) {
        case SORT_NAME: return "name";
        case SORT_MTIME: return "mtime";
        case SORT_DELTA: return "delta";
        case SORT_EXT: return "extension";
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
    } else if (g_sort_mode == SORT_EXT) {
        const char *exta = get_file_extension(ea->name);
        const char *extb = get_file_extension(eb->name);
        int c = strcasecmp(exta, extb);
        if (c == 0) c = strcmp(ea->name, eb->name); // tie-break with name
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
    } else if (g_sort_mode == SORT_DELTA) {
        // sort by size delta; larger delta (more positive) first if desc
        if (ea->delta == eb->delta) {
            // stable tie-breaker by name
            int c = strcmp(ea->name, eb->name);
            return g_sort_desc ? -c : c;
        }
        if (g_sort_desc) return (ea->delta < eb->delta) ? 1 : -1;
        else return (ea->delta < eb->delta) ? -1 : 1;
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
// Tree View state
typedef struct {
    char **paths;
    size_t n;
    size_t cap;
} ExpandedSet;

static ExpandedSet g_expanded = {NULL, 0, 0};

static void expanded_add(const char *p) {
    for (size_t i = 0; i < g_expanded.n; i++) if (strcmp(g_expanded.paths[i], p) == 0) return;
    if (g_expanded.n == g_expanded.cap) {
        size_t nc = g_expanded.cap ? g_expanded.cap * 2 : 32;
        char **np = realloc(g_expanded.paths, nc * sizeof(char*));
        if (!np) return;
        g_expanded.paths = np; g_expanded.cap = nc;
    }
    g_expanded.paths[g_expanded.n++] = xstrdup(p);
}

static void expanded_remove(const char *p) {
    for (size_t i = 0; i < g_expanded.n; i++) {
        if (strcmp(g_expanded.paths[i], p) == 0) {
            free(g_expanded.paths[i]);
            if (i < g_expanded.n - 1) memmove(&g_expanded.paths[i], &g_expanded.paths[i+1], (g_expanded.n - i - 1) * sizeof(char*));
            g_expanded.n--; return;
        }
    }
}

static int expanded_has(const char *p) {
    for (size_t i = 0; i < g_expanded.n; i++) if (strcmp(g_expanded.paths[i], p) == 0) return 1;
    return 0;
}

static void build_tree_recursive(const char *path, const char *root, Cache *cache, DirView *out, int depth) {
    DIR *dp = opendir(path);
    if (!dp) return;
    
    // Temporarily collect entries to sort them
    DirView tmp = {0};
    tmp.path = xstrdup(path);
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (strcmp(de->d_name, CACHE_FILENAME) == 0 && strcmp(path, root) == 0) continue;
        char *abs = path_join(path, de->d_name);
        if (!abs) continue;
        
        struct stat st;
        if (lstat(abs, &st) != 0) { free(abs); continue; }
        if (S_ISLNK(st.st_mode)) { free(abs); continue; }
        
        int is_dir = S_ISDIR(st.st_mode);
        int include = 1;
        if (g_filter_mode == FILTER_DIRS && !is_dir) include = 0;
        if (g_filter_mode == FILTER_FILES && is_dir) include = 0;
        if (g_filter_by_query) {
            if (g_regex_enabled) { if (regexec(&g_regex, de->d_name, 0, NULL, 0) != 0) include = 0; }
            else { if (g_search_query[0] != '\0' && !strcasestr_bool(de->d_name, g_search_query)) include = 0; }
        }
        
        if (!include) { free(abs); continue; }
        
        if (tmp.n == tmp.cap) {
            size_t nc = tmp.cap ? tmp.cap * 2 : 64;
            ViewEntry *nv = realloc(tmp.v, nc * sizeof(ViewEntry));
            if (!nv) { free(abs); continue; }
            tmp.v = nv; tmp.cap = nc;
        }
        ViewEntry *ve = &tmp.v[tmp.n++];
        ve->name = xstrdup(de->d_name);
        ve->abs_path = abs;
        ve->is_dir = is_dir;
        ve->mtime = st.st_mtime;
        ve->depth = depth;
        ve->expanded = expanded_has(abs);
        if (ve->is_dir) {
            if (!cache_get_info(cache, abs, &ve->size, NULL, NULL)) { ve->size = 0; ve->size_known = 0; }
            else ve->size_known = 1;
        } else {
            ve->size = file_size_bytes(&st);
            ve->size_known = 1;
        }

        if (g_diff_mode) {
            unsigned long long snap_sz = 0;
            if (cache_get_info(&g_snapshot_cache, ve->abs_path, &snap_sz, NULL, NULL)) {
                ve->delta = (long long)ve->size - (long long)snap_sz;
            } else {
                ve->delta = (long long)ve->size;
            }
        } else {
            ve->delta = 0;
        }
    }
    closedir(dp);
    qsort(tmp.v, tmp.n, sizeof(ViewEntry), cmp_entries);
    
    // Add sorted entries to main view and recurse if expanded
    for (size_t i = 0; i < tmp.n; i++) {
        if (out->n == out->cap) {
            size_t nc = out->cap ? out->cap * 2 : 128;
            ViewEntry *nv = realloc(out->v, nc * sizeof(ViewEntry));
            if (!nv) break;
            out->v = nv; out->cap = nc;
        }
        out->v[out->n++] = tmp.v[i];
        if (tmp.v[i].is_dir && tmp.v[i].expanded) {
            build_tree_recursive(tmp.v[i].abs_path, root, cache, out, depth + 1);
        }
    }
    free(tmp.path); free(tmp.v); // don't free individual entries, they are now in 'out'
}

static int build_archive_view(DirView *out) {
    if (!g_inside_archive_path) return -1;
    memset(out, 0, sizeof(*out));
    out->path = path_join(g_inside_archive_path, g_archive_subpath ? g_archive_subpath : "");

    // Get archive file size for progress bar
    struct stat st_arc;
    long long total_compressed_bytes = 0;
    if (stat(g_inside_archive_path, &st_arc) == 0) total_compressed_bytes = st_arc.st_size;

    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, g_inside_archive_path, 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return -1;
    }

    struct archive_entry *entry;
    size_t sublen = g_archive_subpath ? strlen(g_archive_subpath) : 0;

    nodelay(stdscr, TRUE); // Non-blocking input to catch ESC
    int interrupted = 0;
    int entry_count = 0;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        entry_count++;
        // Check for ESC key (27)
        int ch = getch();
        if (ch == 27) { interrupted = 1; break; }

        // Draw progress bar every ~100 entries to avoid flickering/slowdown
        if (entry_count % 100 == 0 && total_compressed_bytes > 0) {
            long long read_bytes = archive_filter_bytes(a, -1);
            double frac = (double)read_bytes / (double)total_compressed_bytes;
            if (frac > 1.0) frac = 1.0;
            
            int cols, rows; getmaxyx(stdscr, rows, cols);
            int barlen = cols > 40 ? cols - 40 : 20;
            char bar[256];
            int filled = (int)(frac * barlen);
            for (int i=0; i<barlen; i++) bar[i] = (i < filled) ? '#' : '.';
            bar[barlen] = '\0';
            
            mvhline(rows-1, 0, ' ', cols);
            mvprintw(rows-1, 0, " Reading Archive: [%s] %3d%% - ESC to cancel", bar, (int)(frac * 100));
            refresh();
        }

        const char *path = archive_entry_pathname(entry);
        if (!path) continue;

        // Skip leading slash
        if (path[0] == '/') path++;

        // Check if entry is inside current subpath
        if (sublen > 0) {
            if (strncmp(path, g_archive_subpath, sublen) != 0) continue;
            path += sublen;
            if (path[0] == '/') path++;
            if (path[0] == '\0') continue; // it's the directory itself
        }

        // Determine current level name
        const char *slash = strchr(path, '/');
        char name[PATH_MAX];
        int is_dir = 0;
        if (slash) {
            size_t len = (size_t)(slash - path);
            if (len >= sizeof(name)) len = sizeof(name) - 1;
            memcpy(name, path, len);
            name[len] = '\0';
            is_dir = 1;
        } else {
            strncpy(name, path, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            is_dir = S_ISDIR(archive_entry_mode(entry));
        }

        // Check if already added (for simulated directories)
        int exists = 0;
        for (size_t i = 0; i < out->n; i++) {
            if (strcmp(out->v[i].name, name) == 0) {
                if (is_dir) {
                    out->v[i].size += (unsigned long long)archive_entry_size(entry);
                }
                exists = 1;
                break;
            }
        }
        if (exists) continue;

        if (out->n == out->cap) {
            size_t newcap = out->cap ? out->cap * 2 : 128;
            void *nv = realloc(out->v, newcap * sizeof(ViewEntry));
            if (!nv) break;
            out->v = (ViewEntry*)nv; out->cap = newcap;
        }
        ViewEntry *ve = &out->v[out->n++];
        memset(ve, 0, sizeof(*ve));
        ve->name = xstrdup(name);
        ve->abs_path = path_join(out->path, name);
        ve->is_dir = is_dir;
        ve->size = (unsigned long long)archive_entry_size(entry);
        ve->size_known = 1;
        ve->mtime = archive_entry_mtime(entry);
    }

    archive_read_free(a);
    nodelay(stdscr, FALSE); // Restore blocking input

    if (interrupted) {
        view_free(out);
        draw_status("Archive reading cancelled by user.");
        return -1;
    }

    qsort(out->v, out->n, sizeof(ViewEntry), cmp_entries);
    out->sizew = compute_size_col_width(out);
    out->view_total = 0;
    for (size_t i = 0; i < out->n; i++) out->view_total += out->v[i].size;
    if (out->view_total == 0) out->view_total = 1;

    return 0;
}

static int build_dir_view(const char *path, const char *root, Cache *cache, DirView *out) {
    if (g_inside_archive_path) return build_archive_view(out);
    if (g_tree_mode) {
        memset(out, 0, sizeof(*out));
        out->path = xstrdup(path);
        build_tree_recursive(path, root, cache, out, 0);
        
        out->sizew = compute_size_col_width(out);
        out->view_total = 0;
        for (size_t i = 0; i < out->n; i++) if (out->v[i].size_known && out->v[i].depth == 0) out->view_total += out->v[i].size;
        if (out->view_total == 0) out->view_total = 1;
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->path = xstrdup(path);
    DIR *dp = opendir(path);
    if (!dp) return -1;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (g_interrupted) break;
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
            if (cache_get_info(cache, ve->abs_path, &ve->size, NULL, NULL)) { ve->size_known = 1; }
            else { ve->size = 0ULL; ve->size_known = 0; }
        } else if (dtype == DT_REG) {
            ve->size = file_size_bytes(&st);
            ve->size_known = 1;
        } else {
            ve->size = 0ULL; ve->size_known = 1;
        }

        if (g_diff_mode) {
            unsigned long long snap_sz = 0;
            if (cache_get_info(&g_snapshot_cache, ve->abs_path, &snap_sz, NULL, NULL)) {
                ve->delta = (long long)ve->size - (long long)snap_sz;
            } else {
                ve->delta = (long long)ve->size;
            }
        } else {
            ve->delta = 0;
        }
    }
    closedir(dp);
    qsort(out->v, out->n, sizeof(ViewEntry), cmp_entries);

    // Cache precomputations for fast drawing
    out->sizew = compute_size_col_width(out);
    out->view_total = 0ULL;
    for (size_t i = 0; i < out->n; i++) {
        if (out->v[i].size_known) out->view_total += out->v[i].size;
    }
    if (out->view_total == 0ULL) out->view_total = 1ULL;

    return 0;
}

static void update_miller_columns(const char *current_path, const char *root, Cache *cache, DirView *dv_main) {
    if (!g_miller_mode) return;
    static char last_path[PATH_MAX] = "";

    // 1. Update Parent View (Left)
    char *parent_path = get_parent(current_path);
    if (parent_path && strcmp(current_path, root) != 0) {
        if (!g_dv_parent.path || strcmp(g_dv_parent.path, parent_path) != 0) {
            view_free(&g_dv_parent);
            build_dir_view(parent_path, root, cache, &g_dv_parent);
        }
        free(parent_path);
    } else {
        view_free(&g_dv_parent);
        if (parent_path) free(parent_path);
    }

    // 2. Update Preview View (Right)
    if (dv_main->n > 0) {
        ViewEntry *ve = &dv_main->v[dv_main->selected];
        
        // Skip if same selection as before
        if (strcmp(last_path, ve->abs_path) == 0) return;
        strncpy(last_path, ve->abs_path, sizeof(last_path)); last_path[sizeof(last_path)-1] = '\0';

        if (ve->is_dir) {
            if (!g_dv_preview.path || strcmp(g_dv_preview.path, ve->abs_path) != 0) {
                view_free(&g_dv_preview);
                build_dir_view(ve->abs_path, root, cache, &g_dv_preview);
            }
        } else {
            view_free(&g_dv_preview);
        }
    } else {
        view_free(&g_dv_preview);
        last_path[0] = '\0';
    }
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
        if (g_interrupted) break;
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (is_excluded(de->d_name)) continue;
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
                    if (cfd >= 0) {
                        if (g_one_file_system) {
                            struct stat stch;
                            if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                                close(cfd);
                                continue;
                            }
                        }
                        count += count_dir_files_fd(cfd);
                        close(cfd);
                    }
                }
            }
        } else if (dt == DT_DIR) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) {
                if (g_one_file_system) {
                    struct stat stch;
                    if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                        close(cfd);
                        continue;
                    }
                }
                count += count_dir_files_fd(cfd);
                close(cfd);
            }
        }
    }
    closedir(dp);
    return count;
}

static unsigned long long sum_dir_sizes_fd(int dirfd, InodeSet *is) {
    unsigned long long total = 0ULL;
    int dupfd = dup(dirfd);
    if (dupfd < 0) return 0ULL;
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); return 0ULL; }
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (g_interrupted) break;
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (is_excluded(de->d_name)) continue;
        if (strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        unsigned char dt = de->d_type;
        if (dt == DT_LNK) continue;
        if (dt == DT_REG) {
            struct stat st;
            if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                if (st.st_nlink <= 1 || !inodeset_check_and_add(is, st.st_dev, st.st_ino))
                    total += file_size_bytes(&st);
            }
        } else if (dt == DT_UNKNOWN) {
            struct stat st0;
            if (fstatat(dirfd, de->d_name, &st0, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISREG(st0.st_mode)) {
                    if (st0.st_nlink <= 1 || !inodeset_check_and_add(is, st0.st_dev, st0.st_ino))
                        total += file_size_bytes(&st0);
                }
                else if (S_ISDIR(st0.st_mode)) {
                    int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (cfd >= 0) {
                        if (g_one_file_system) {
                            struct stat stch;
                            if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                                close(cfd);
                                continue;
                            }
                        }
                        total += sum_dir_sizes_fd(cfd, is);
                        close(cfd);
                    }
                }
            }
        } else if (dt == DT_DIR) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) {
                if (g_one_file_system) {
                    struct stat stch;
                    if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                        close(cfd);
                        continue;
                    }
                }
                total += sum_dir_sizes_fd(cfd, is);
                close(cfd);
            }
        }
    }
    closedir(dp);
    return total;
}

static unsigned long long sum_path_size(const char *path) {
    InodeSet is;
    inodeset_init(&is, 1024);
    struct stat st;
    if (lstat(path, &st) != 0) { inodeset_free(&is); return 0ULL; }
    if (S_ISREG(st.st_mode)) {
        unsigned long long s = file_size_bytes(&st);
        inodeset_free(&is);
        return s;
    }
    if (S_ISDIR(st.st_mode)) {
        int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) { inodeset_free(&is); return 0ULL; }
        unsigned long long s = sum_dir_sizes_fd(fd, &is);
        close(fd);
        inodeset_free(&is);
        return s;
    }
    inodeset_free(&is);
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
        if (g_interrupted) break;
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

typedef struct {
    char *src;
    char *dst;
} CopyTask;

typedef struct {
    char *abs;
    char *rel;
} ZipTask;

static int archive_extract_to(const char *archive_path, const char *target_dir, const char *root, Cache *cache) {
    struct stat st_arc;
    unsigned long long total_compressed_bytes = 0;
    int arc_fd = open(archive_path, O_RDONLY);
    if (arc_fd < 0) return -1;
    if (fstat(arc_fd, &st_arc) == 0) total_compressed_bytes = (unsigned long long)st_arc.st_size;

    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    
    // Open using the file descriptor to track progress via lseek
    if (archive_read_open_fd(a, arc_fd, 10240) != ARCHIVE_OK) {
        archive_read_free(a); close(arc_fd); return -1;
    }

    struct archive_entry *entry;
    char global_action = 0;
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_FFLAGS;
    struct timespec last_draw = {0, 0};

    nodelay(stdscr, TRUE); // Catch ESC
    int final_rc = 0;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (g_interrupted || getch() == 27) { final_rc = -1; break; }
        
        const char *entry_rel = archive_entry_pathname(entry);
        char *full_dest = path_join(target_dir, entry_rel);
        if (!full_dest) continue;

        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
        if (ms >= 33) {
            last_draw = now;
            int cols, rows; getmaxyx(stdscr, rows, cols);
            // Use lseek to get real progress in the compressed file
            off_t current_pos = lseek(arc_fd, 0, SEEK_CUR);
            double frac = (total_compressed_bytes > 0) ? (double)current_pos / (double)total_compressed_bytes : 0.0;
            if (frac > 1.0) frac = 1.0;
            int barlen = cols > 40 ? cols - 40 : 20;
            char bar[256]; int filled = (int)(frac * barlen);
            for (int i=0; i<barlen; i++) bar[i] = (i < filled) ? '#' : '.';
            bar[barlen] = '\0';
            mvhline(rows-1, 0, ' ', cols);
            mvprintw(rows-1, 0, " Extracting: [%s] %3d%% - %s", bar, (int)(frac * 100), entry_rel);
            refresh();
        }

        int skip = 0;
        char *final_dest = xstrdup(full_dest);
        struct stat st;
        if (lstat(full_dest, &st) == 0) {
            nodelay(stdscr, FALSE); // Block for conflict prompt
            char action = global_action;
            if (action == 0) {
                action = prompt_conflict_action(full_dest);
                if (action == 'O' || action == 'R' || action == 'S') global_action = action;
            }
            if (action == 's' || action == 'S') skip = 1;
            else if (action == 'r' || action == 'R') {
                char *new_p = gen_nonconflicting_path(full_dest);
                free(final_dest); final_dest = new_p;
            }
            nodelay(stdscr, TRUE);
        }

        if (!skip && final_dest) {
            archive_entry_set_pathname(entry, final_dest);
            if (archive_read_extract(a, entry, flags) == ARCHIVE_OK) {
                struct stat stnew;
                if (lstat(final_dest, &stnew) == 0) {
                    if (S_ISREG(stnew.st_mode)) {
                        unsigned long long sz = file_size_bytes(&stnew);
                        cache_upsert_with_meta(cache, root, final_dest, sz, time(NULL), (unsigned long long)stnew.st_ino, stnew.st_mtime);
                        cache_add_ancestors_after_delta(cache, root, final_dest, sz);
                        g_last_bytes += sz; g_last_files += 1;
                    } else if (S_ISDIR(stnew.st_mode)) {
                        cache_upsert_with_meta(cache, root, final_dest, 0, time(NULL), (unsigned long long)stnew.st_ino, stnew.st_mtime);
                    }
                }
            }
        }
        free(full_dest); free(final_dest);
    }

    nodelay(stdscr, FALSE);
    archive_read_close(a);
    archive_read_free(a);
    close(arc_fd);
    cache_save(root, cache);
    return final_rc;
}

static int zip_compress_items(char **src_paths, size_t num_src, const char *dest_zip, const char *root) {
    unsigned long long total_uncompressed_bytes = 0ULL;
    for (size_t i = 0; i < num_src; i++) total_uncompressed_bytes += sum_path_size(src_paths[i]);

    struct archive *a = archive_write_new();
    archive_write_set_format_zip(a);
    if (archive_write_open_filename(a, dest_zip) != ARCHIVE_OK) {
        archive_write_free(a);
        return -1;
    }

    size_t cap = 128, n = 0;
    ZipTask *list = malloc(cap * sizeof(ZipTask));
    if (!list) { archive_write_free(a); return -1; }

    // Initial items
    for (size_t i = 0; i < num_src; i++) {
        const char *base = path_basename_const(src_paths[i]);
        list[n++] = (ZipTask){xstrdup(src_paths[i]), xstrdup(base)};
    }

    unsigned long long processed_bytes = 0ULL;
    struct timespec last_draw = {0, 0};

    nodelay(stdscr, TRUE); // Catch ESC
    int final_rc = 0;
    for (size_t i = 0; i < n; i++) {
        if (g_interrupted || getch() == 27) { final_rc = -1; break; }
        
        struct stat st;
        if (lstat(list[i].abs, &st) != 0) continue;

        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, list[i].rel);
        archive_entry_copy_stat(entry, &st);
        
        if (S_ISDIR(st.st_mode)) {
            archive_entry_set_size(entry, 0);
            if (archive_write_header(a, entry) != ARCHIVE_OK) {
                archive_entry_free(entry); continue;
            }
            archive_entry_free(entry);

            // Traverse directory
            int sfd = open(list[i].abs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sfd >= 0) {
                DIR *dp = fdopendir(sfd);
                if (dp) {
                    struct dirent *de;
                    while ((de = readdir(dp)) != NULL) {
                        if (is_dot_or_dotdot(de->d_name)) continue;
                        if (strcmp(de->d_name, CACHE_FILENAME) == 0 && strcmp(list[i].abs, root) == 0) continue;
                        char *abs = path_join(list[i].abs, de->d_name);
                        char *rel = path_join(list[i].rel, de->d_name);
                        if (abs && rel) {
                            if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof(ZipTask)); }
                            list[n++] = (ZipTask){abs, rel};
                        } else { free(abs); free(rel); }
                    }
                    closedir(dp);
                } else close(sfd);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (archive_write_header(a, entry) != ARCHIVE_OK) {
                archive_entry_free(entry); continue;
            }
            if (st.st_size > 0) {
                int fd = open(list[i].abs, O_RDONLY);
                if (fd >= 0) {
                    char buf[16384];
                    ssize_t r;
                    while ((r = read(fd, buf, sizeof(buf))) > 0) {
                        if (g_interrupted || getch() == 27) { final_rc = -1; break; }
                        archive_write_data(a, buf, (size_t)r);
                        processed_bytes += (unsigned long long)r;
                        
                        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
                        long ms = (now.tv_sec - last_draw.tv_sec) * 1000 + (now.tv_nsec - last_draw.tv_nsec) / 1000000;
                        if (ms >= 33) {
                            last_draw = now;
                            int cols, rows; getmaxyx(stdscr, rows, cols);
                            double frac = (total_uncompressed_bytes > 0) ? (double)processed_bytes / (double)total_uncompressed_bytes : 0.0;
                            if (frac > 1.0) frac = 1.0;
                            int barlen = cols > 40 ? cols - 40 : 20;
                            char bar[256]; int filled = (int)(frac * barlen);
                            for (int b=0; b<barlen; b++) bar[b] = (b < filled) ? '#' : '.';
                            bar[barlen] = '\0';
                            mvhline(rows-1, 0, ' ', cols);
                            mvprintw(rows-1, 0, " Compressing: [%s] %3d%% - %s", bar, (int)(frac * 100), list[i].rel);
                            refresh();
                        }
                    }
                    close(fd);
                }
            }
            archive_entry_free(entry);
        } else {
            archive_entry_free(entry);
        }
        if (final_rc != 0) break;
    }
    nodelay(stdscr, FALSE);

    for (size_t i = 0; i < n; i++) {
        free(list[i].abs);
        free(list[i].rel);
    }
    free(list);
    archive_write_close(a);
    archive_write_free(a);
    return final_rc;
}

static int copy_tree_with_progress(const char *src, const char *dst, CopyUI *ui, const char *root) {
    size_t cap = 128, n = 0;
    CopyTask *list = malloc(cap * sizeof(CopyTask));
    if (!list) return -1;
    list[n++] = (CopyTask){xstrdup(src), xstrdup(dst)};

    int final_rc = 0;
    // Walk and collect subtasks iteratively
    for (size_t i = 0; i < n; i++) {
        if (g_interrupted) { final_rc = -1; break; }
        struct stat st;
        if (lstat(list[i].src, &st) != 0) { final_rc = -1; continue; }
        if (S_ISDIR(st.st_mode)) {
            mkdir(list[i].dst, st.st_mode & 0777);
            int sfd = open(list[i].src, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (sfd >= 0) {
                DIR *dp = fdopendir(sfd);
                if (dp) {
                    struct dirent *de;
                    while ((de = readdir(dp)) != NULL) {
                        if (is_dot_or_dotdot(de->d_name)) continue;
                        if (strcmp(de->d_name, CACHE_FILENAME) == 0 && strcmp(list[i].src, root) == 0) continue;
                        if (de->d_type == DT_LNK) continue;
                        char *cs = path_join(list[i].src, de->d_name);
                        char *cd = path_join(list[i].dst, de->d_name);
                        if (cs && cd) {
                            if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof(CopyTask)); }
                            list[n++] = (CopyTask){cs, cd};
                        } else { free(cs); free(cd); }
                    }
                    closedir(dp);
                } else { close(sfd); }
            }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file_with_progress(list[i].src, list[i].dst, ui) != 0) final_rc = -1;
        }
    }

    for (size_t i = 0; i < n; i++) {
        free(list[i].src);
        free(list[i].dst);
    }
    free(list);
    return final_rc;
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
    char filterbuf[64];
    if (g_filter_by_query) {
        snprintf(filterbuf, sizeof(filterbuf), "%s+%s", filter_mode_label(), g_regex_enabled ? "regex" : "query");
    } else {
        snprintf(filterbuf, sizeof(filterbuf), "%s", filter_mode_label());
    }

    // --- Line 0: Main Header ---
    move(0, 0);
    attron(COLOR_PAIR(1)); addstr(" fastdu ");
    attron(A_BOLD); addstr(FASTDU_VERSION);
    attron(A_NORMAL); addstr(" - root: ");
    attron(COLOR_PAIR(8) | A_BOLD); addstr(root);
    attron(COLOR_PAIR(1)); addstr(" - cwd: ");
    
    int cx, cy; getyx(stdscr, cy, cx); (void)cy;
    breadcrumbs_clear();
    char temp[PATH_MAX]; strncpy(temp, relbuf, sizeof(temp)); temp[sizeof(temp)-1] = '\0';
    if (strcmp(temp, ".") == 0) {
        attron(COLOR_PAIR(8) | A_BOLD); addstr(".");
        int ex; getyx(stdscr, cy, ex);
        breadcrumbs_add(cx, ex, root);
        cx = ex;
    } else {
        char *tok = strtok(temp, "/");
        char current_abs[PATH_MAX]; strncpy(current_abs, root, sizeof(current_abs));
        int first = 1;
        while (tok) {
            if (!first) { attron(COLOR_PAIR(1)); addstr("/"); cx++; }
            char *next_abs = path_join(current_abs, tok);
            if (!next_abs) break;
            strncpy(current_abs, next_abs, sizeof(current_abs));
            free(next_abs);
            attron(COLOR_PAIR(8) | A_BOLD); addstr(tok);
            int ex; getyx(stdscr, cy, ex);
            breadcrumbs_add(cx, ex, current_abs);
            cx = ex;
            tok = strtok(NULL, "/");
            first = 0;
        }
    }

    // Add sort and filter info on Line 0
    attron(COLOR_PAIR(1)); addstr(" - sort: ");
    attron(COLOR_PAIR(8) | A_BOLD); addstr(sortbuf);
    attron(COLOR_PAIR(1)); addstr(" - filter: ");
    attron(COLOR_PAIR(8) | A_BOLD); addstr(filterbuf);
    attroff(COLOR_PAIR(8) | A_BOLD);

    // --- Line 1: Archive context (if active) ---
    if (g_inside_archive_path) {
        attron(COLOR_PAIR(2));
        mvhline(1, 0, ' ', cols);
        mvaddstr(1, 1, "VIRTUAL FS: ");
        attroff(COLOR_PAIR(2));
        attron(COLOR_PAIR(8) | A_BOLD);
        addstr(path_basename_const(g_inside_archive_path));
        attron(A_NORMAL); addstr(" // ");
        attron(A_BOLD); addstr(g_archive_subpath ? g_archive_subpath : ".");
        attroff(A_BOLD);
    }

    // --- Bottom Bar (Footer) ---
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

    move(rows-2, 0);
    attron(COLOR_PAIR(1)); addstr(" h help | ");
    if (g_diff_mode) {
        attron(COLOR_PAIR(8) | A_BOLD); addstr("DIFF");
        attron(COLOR_PAIR(1)); addstr(" | ");
    }
    addstr("files: ");
    attron(COLOR_PAIR(8) | A_BOLD); printw("%llu", (unsigned long long)g_last_files);
    attron(COLOR_PAIR(1)); addstr(" | size: ");
    attron(COLOR_PAIR(8) | A_BOLD); addstr(totalbuf);
    attron(COLOR_PAIR(1)); addstr(" | marked: ");
    attron(COLOR_PAIR(8) | A_BOLD); printw("%zu", g_marks.n);
    if (g_marks.n > 0) {
        attron(COLOR_PAIR(1)); addstr(" (");
        attron(COLOR_PAIR(8) | A_BOLD); addstr(markedbuf);
        attron(COLOR_PAIR(1)); addstr(", ");
        attron(COLOR_PAIR(8) | A_BOLD); printw("%llu", (unsigned long long)marked_files_cached);
        attron(COLOR_PAIR(1)); addstr(" files)");
    }
    if (g_search_query[0]) {
        attron(COLOR_PAIR(1)); addstr(" | query: ");
        attron(COLOR_PAIR(8) | A_BOLD); addstr(g_search_query);
    }

    // --- Theme info at the far right of the Footer ---
    char themebuf[32];
    snprintf(themebuf, sizeof(themebuf), " T:[%s] ", g_themes[g_current_theme_idx]);
    int tw = (int)strlen(themebuf);
    if (cols > tw + cx + 20) {
        attron(COLOR_PAIR(1));
        mvaddnstr(rows-2, cols - tw, themebuf, tw);
    }

    attron(COLOR_PAIR(1)); // Final reset
    attroff(COLOR_PAIR(8) | A_BOLD);
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
        if (g_diff_mode) {
            unsigned long long abs_delta = (dv->v[i].delta < 0) ? (unsigned long long)(-dv->v[i].delta) : (unsigned long long)dv->v[i].delta;
            human_size(abs_delta, b, sizeof(b));
            int lw = (int)strlen(b) + 2; // +1 space +1 sign
            if (lw > w) w = lw;
        } else {
            if (dv->v[i].size_known) human_size(dv->v[i].size, b, sizeof(b)); else snprintf(b, sizeof(b), "?");
            int lw = (int)strlen(b);
            if (lw > w) w = lw;
        }
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

static void draw_truncated_name(int y, int x, const char *name, int max_w) {
    if (max_w <= 0) return;
    int len = (int)strlen(name);
    if (len <= max_w) {
        mvaddnstr(y, x, name, max_w);
    } else {
        if (max_w <= 3) {
            for (int i = 0; i < max_w; i++) mvaddch(y, x + i, '.');
        } else {
            // End truncation: "verylongna..."
            char buf[512]; // reasonable stack buffer
            int take = (max_w - 3 < (int)sizeof(buf) - 4) ? (max_w - 3) : ((int)sizeof(buf) - 4);
            memcpy(buf, name, (size_t)take);
            strcpy(buf + take, "...");
            mvaddnstr(y, x, buf, max_w);
        }
    }
}

typedef struct {
    char *ext;
    unsigned long long size;
    unsigned long long count;
} ExtEntry;

static int cmp_ext_entries(const void *a, const void *b) {
    const ExtEntry *ea = (const ExtEntry*)a;
    const ExtEntry *eb = (const ExtEntry*)b;
    if (ea->size == eb->size) return 0;
    return (ea->size < eb->size) ? 1 : -1;
}

static int collect_files_recursive_fd(int dirfd, const char *abs_path, FileCandidateList *list, int *interrupted_out) {
    int dupfd = dup(dirfd);
    if (dupfd < 0) return 0;
    DIR *dp = fdopendir(dupfd);
    if (!dp) { close(dupfd); return 0; }
    
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (g_interrupted) break;
        
        // Check for ESC periodically
        if (list->n % 256 == 0) {
            if (getch() == 27) { *interrupted_out = 1; break; }
            int cols, rows; getmaxyx(stdscr, rows, cols);
            mvhline(rows-1, 0, ' ', cols);
            mvprintw(rows-1, 0, " Collecting files: %zu found... - ESC to cancel", list->n);
            refresh();
        }

        if (is_dot_or_dotdot(de->d_name)) continue;
        if (is_excluded(de->d_name)) continue;
        if (strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        
        unsigned char dtype = de->d_type;
        if (dtype == DT_LNK) continue;
        
        struct stat st;
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        
        if (S_ISDIR(st.st_mode)) {
            int cfd = openat(dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) {
                if (g_one_file_system) {
                    struct stat stch;
                    if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                        close(cfd);
                        continue;
                    }
                }
                char *child_abs = path_join(abs_path, de->d_name);
                if (child_abs) {
                    collect_files_recursive_fd(cfd, child_abs, list, interrupted_out);
                    free(child_abs);
                }
                close(cfd);
                if (*interrupted_out) break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_size > 0) {
                if (list->n == list->cap) {
                    size_t newcap = list->cap ? list->cap * 2 : 1024;
                    void *nv = realloc(list->v, newcap * sizeof(FileCandidate));
                    if (nv) { list->v = nv; list->cap = newcap; }
                    else break;
                }
                char *child_abs = path_join(abs_path, de->d_name);
                if (child_abs) {
                    FileCandidate *fc = &list->v[list->n++];
                    fc->path = child_abs;
                    fc->size = (unsigned long long)st.st_size;
                    fc->head_hash = 0;
                    fc->dupe_group_id = 0;
                }
            }
        }
    }
    closedir(dp);
    return *interrupted_out;
}

static int cmp_file_candidates_size(const void *a, const void *b) {
    const FileCandidate *ea = (const FileCandidate *)a;
    const FileCandidate *eb = (const FileCandidate *)b;
    if (ea->size != eb->size) return (ea->size < eb->size) ? 1 : -1; // Descending size
    return strcmp(ea->path, eb->path);
}

static int cmp_file_candidates_hash(const void *a, const void *b) {
    const FileCandidate *ea = (const FileCandidate *)a;
    const FileCandidate *eb = (const FileCandidate *)b;
    if (ea->head_hash != eb->head_hash) return (ea->head_hash < eb->head_hash) ? -1 : 1;
    return strcmp(ea->path, eb->path);
}

static void show_extension_view(const DirView *dv) {
    if (dv->n == 0) return;
    size_t cap = 32, n = 0;
    ExtEntry *entries = malloc(cap * sizeof(ExtEntry));
    if (!entries) return;

    for (size_t i = 0; i < dv->n; i++) {
        if (dv->v[i].is_dir) continue;
        const char *name = dv->v[i].name;
        const char *dot = strrchr(name, '.');
        char *ext = NULL;
        if (dot && dot != name) ext = xstrdup(dot);
        else ext = xstrdup("(no ext)");

        int found = 0;
        for (size_t j = 0; j < n; j++) {
            if (strcmp(entries[j].ext, ext) == 0) {
                entries[j].size += dv->v[i].size;
                entries[j].count++;
                found = 1;
                break;
            }
        }
        if (!found) {
            if (n == cap) {
                cap *= 2;
                ExtEntry *ne = realloc(entries, cap * sizeof(ExtEntry));
                if (ne) entries = ne;
            }
            entries[n].ext = ext;
            entries[n].size = dv->v[i].size;
            entries[n].count = 1;
            n++;
        } else {
            free(ext);
        }
    }

    if (n == 0) { free(entries); draw_status("No files in current view."); return; }
    qsort(entries, n, sizeof(ExtEntry), cmp_ext_entries);

    int cols, rows; getmaxyx(stdscr, rows, cols);
    int w = cols - 12; if (w < 40) w = cols - 4;
    int h = rows - 10; if (h < 10) h = rows - 4;
    int x = (cols - w) / 2; int y = (rows - h) / 2;
    WINDOW *win = newwin(h, w, y, x);
    keypad(win, TRUE);
    unsigned long long tot_size = 0; for (size_t i = 0; i < n; i++) tot_size += entries[i].size;

    int off = 0;
    for (;;) {
        werase(win); box(win, 0, 0);
        wattron(win, A_REVERSE | A_BOLD);
        mvwaddnstr(win, 0, 2, " Extension Distribution - q to close ", w - 4);
        wattroff(win, A_REVERSE | A_BOLD);
        for (int i = 0; i < h - 2; i++) {
            size_t idx = (size_t)off + (size_t)i;
            if (idx >= n) break;
            char sz[32], pc[32];
            human_size(entries[idx].size, sz, sizeof(sz));
            double p = (tot_size > 0) ? (double)entries[idx].size * 100.0 / (double)tot_size : 0.0;
            snprintf(pc, sizeof(pc), "%5.1f%%", p);
            mvwprintw(win, 1 + i, 2, "%-12s %10s %5s (%llu files)", entries[idx].ext, sz, pc, entries[idx].count);
        }
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 'q' || ch == 'Q' || ch == 27) break;
        if (ch == KEY_UP && off > 0) off--;
        if (ch == KEY_DOWN && (size_t)off + (size_t)(h-2) < n) off++;
    }
    delwin(win);
    for (size_t i = 0; i < n; i++) free(entries[i].ext);
    free(entries);
}

typedef struct {
    int is_header;
    int group_id;
    unsigned long long size;
    char *path;
    int marked;
} DupeViewItem;

static void show_duplicates_view(const char *scan_root, Cache *cache, char *cache_abs) {
    (void)cache_abs;
    int cols, rows; getmaxyx(stdscr, rows, cols);
    int interrupted = 0;
    
    nodelay(stdscr, TRUE);
    FileCandidateList list = {0};
    int fd = open(scan_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        collect_files_recursive_fd(fd, scan_root, &list, &interrupted);
        close(fd);
    }

    if (interrupted) {
        for (size_t i = 0; i < list.n; i++) free(list.v[i].path);
        free(list.v);
        nodelay(stdscr, FALSE);
        draw_status("Duplicate finding cancelled by user.");
        return;
    }

    if (list.n < 2) {
        for (size_t i = 0; i < list.n; i++) free(list.v[i].path);
        free(list.v);
        nodelay(stdscr, FALSE);
        draw_status("Not enough files to find duplicates.");
        return;
    }

    qsort(list.v, list.n, sizeof(FileCandidate), cmp_file_candidates_size);

    int current_group_id = 1;
    size_t i = 0;
    while (i < list.n) {
        size_t j = i + 1;
        while (j < list.n && list.v[j].size == list.v[i].size) j++;
        
        size_t group_size = j - i;
        if (group_size > 1) {
            // Hashing Phase
            for (size_t k = i; k < j; k++) {
                if (getch() == 27) { interrupted = 1; break; }
                list.v[k].head_hash = hash_file_head(list.v[k].path);
                
                // Progress Bar for Hashing
                if (k % 10 == 0) {
                    double frac = (double)k / (double)list.n;
                    int barlen = cols > 40 ? cols - 40 : 20;
                    char bar[256]; int filled = (int)(frac * barlen);
                    for (int b=0; b<barlen; b++) bar[b] = (b < filled) ? '#' : '.';
                    bar[barlen] = '\0';
                    mvhline(rows-1, 0, ' ', cols);
                    mvprintw(rows-1, 0, " Hashing candidates: [%s] %d%% - ESC to cancel", bar, (int)(frac*100));
                    refresh();
                }
            }
            if (interrupted) break;

            qsort(&list.v[i], group_size, sizeof(FileCandidate), cmp_file_candidates_hash);
            
            size_t sub_i = i;
            while (sub_i < j) {
                size_t sub_j = sub_i + 1;
                while (sub_j < j && list.v[sub_j].head_hash == list.v[sub_i].head_hash) sub_j++;
                
                size_t sub_size = sub_j - sub_i;
                if (sub_size > 1 && list.v[sub_i].head_hash != 0) {
                    // Exact byte-by-byte compare Phase
                    for (size_t x = sub_i; x < sub_j; x++) {
                        if (list.v[x].dupe_group_id != 0) continue;
                        int new_group = 0;
                        for (size_t y = x + 1; y < sub_j; y++) {
                            if (getch() == 27) { interrupted = 1; break; }
                            if (list.v[y].dupe_group_id == 0 && files_are_identical(list.v[x].path, list.v[y].path)) {
                                if (!new_group) { new_group = 1; list.v[x].dupe_group_id = current_group_id; }
                                list.v[y].dupe_group_id = current_group_id;
                            }
                        }
                        if (interrupted) break;
                        if (new_group) current_group_id++;
                    }
                }
                if (interrupted) break;
                sub_i = sub_j;
            }
        }
        if (interrupted) break;
        i = j;
    }

    nodelay(stdscr, FALSE);

    if (interrupted) {
        for (size_t k = 0; k < list.n; k++) free(list.v[k].path);
        free(list.v);
        draw_status("Duplicate finding cancelled by user.");
        return;
    }

    // Build View UI Array
    size_t dupes_count = 0;
    for (size_t k = 0; k < list.n; k++) if (list.v[k].dupe_group_id > 0) dupes_count++;
    
    if (dupes_count == 0) {
        for (size_t k = 0; k < list.n; k++) free(list.v[k].path);
        free(list.v);
        draw_status("No duplicates found.");
        return;
    }

    size_t vi_cap = dupes_count + current_group_id;
    DupeViewItem *vi = calloc(vi_cap, sizeof(DupeViewItem));
    size_t vi_n = 0;
    unsigned long long total_wasted = 0;

    for (int g = 1; g < current_group_id; g++) {
        unsigned long long g_size = 0;
        int count_in_group = 0;
        for (size_t k = 0; k < list.n; k++) {
            if (list.v[k].dupe_group_id == g) {
                g_size = list.v[k].size;
                count_in_group++;
            }
        }
        if (count_in_group > 1) {
            total_wasted += g_size * (count_in_group - 1);
            vi[vi_n++] = (DupeViewItem){.is_header=1, .group_id=g, .size=g_size, .path=NULL, .marked=0};
            for (size_t k = 0; k < list.n; k++) {
                if (list.v[k].dupe_group_id == g) {
                    vi[vi_n++] = (DupeViewItem){.is_header=0, .group_id=g, .size=g_size, .path=xstrdup(list.v[k].path), .marked=0};
                }
            }
        }
    }

    for (size_t k = 0; k < list.n; k++) free(list.v[k].path);
    free(list.v);

    getmaxyx(stdscr, rows, cols);
    int top = 0; 
    size_t sel = 0;
    // Start on the first actual file item
    while (sel < vi_n && vi[sel].is_header) sel++;
    if (sel >= vi_n) sel = 0; // fallback
    
    for (;;) {
        erase();
        attron(COLOR_PAIR(1)); mvhline(0, 0, ' ', cols);
        char wst[64]; human_size(total_wasted, wst, sizeof(wst));
        mvprintw(0, 0, " Duplicate Finder - %llu duplicates, %s total waste - [SPACE] mark, [d] delete, [q] close", (unsigned long long)dupes_count, wst);
        attroff(COLOR_PAIR(1));
        
        int list_rows = rows - 1;
        for (int r = 0; r < list_rows; r++) {
            int idx = top + r;
            if (idx >= (int)vi_n) break;
            int y = r + 1;
            int is_sel = (idx == (int)sel);
            
            if (vi[idx].is_header) {
                attron(COLOR_PAIR(3) | A_BOLD);
                char sz[32]; human_size(vi[idx].size, sz, sizeof(sz));
                mvprintw(y, 0, "--- [Group %d] Size: %s ---", vi[idx].group_id, sz);
                attroff(COLOR_PAIR(3) | A_BOLD);
            } else {
                if (is_sel) attron(A_REVERSE | A_BOLD);
                mvprintw(y, 2, "[%c] %s", vi[idx].marked ? '*' : ' ', vi[idx].path);
                if (is_sel) attroff(A_REVERSE | A_BOLD);
            }
        }
        refresh();
        
        int ch = getch();
        if (ch == 'q' || ch == 'Q' || ch == 27) break;
        else if (ch == KEY_UP || ch == 'k') {
            size_t next_sel = sel;
            while (next_sel > 0) {
                next_sel--;
                if (!vi[next_sel].is_header) { sel = next_sel; break; }
            }
            if ((int)sel < top) top = (int)sel;
        } else if (ch == KEY_DOWN || ch == 'j') {
            size_t next_sel = sel;
            while (next_sel + 1 < vi_n) {
                next_sel++;
                if (!vi[next_sel].is_header) { sel = next_sel; break; }
            }
            if ((int)sel >= top + list_rows) top = (int)sel - list_rows + 1;
        } else if (ch == ' ' && !vi[sel].is_header) {
            vi[sel].marked = !vi[sel].marked;
        } else if (ch == 'd' || ch == 'D' || ch == KEY_DC) {
            // Delete marked
            int count_deleted = 0;
            unsigned long long freed = 0;
            for (size_t k = 0; k < vi_n; k++) {
                if (!vi[k].is_header && vi[k].marked) {
                    // Protect against deleting all files in a group
                    int group = vi[k].group_id;
                    int total_in_group = 0; int marked_in_group = 0;
                    for (size_t m = 0; m < vi_n; m++) {
                        if (!vi[m].is_header && vi[m].group_id == group) {
                            total_in_group++;
                            if (vi[m].marked) marked_in_group++;
                        }
                    }
                    if (marked_in_group == total_in_group) {
                        // Unmark one randomly (the first one) to protect data
                        for (size_t m = 0; m < vi_n; m++) {
                            if (!vi[m].is_header && vi[m].group_id == group && vi[m].marked) {
                                vi[m].marked = 0;
                                break;
                            }
                        }
                    }
                    if (vi[k].marked) {
                        if (unlink(vi[k].path) == 0) {
                            count_deleted++;
                            freed += vi[k].size;
                            // Update cache
                            cache_adjust_ancestors_after_delta(cache, scan_root, vi[k].path, (long long)vi[k].size);
                            vi[k].path[0] = '\0'; // Mark as deleted internally
                            vi[k].marked = 0;
                        }
                    }
                }
            }
            if (count_deleted > 0) {
                if (g_last_bytes >= freed) g_last_bytes -= freed;
                if (g_last_files >= (unsigned long long)count_deleted) g_last_files -= count_deleted;
                cache_save(scan_root, cache);
                char msg[64];
                snprintf(msg, sizeof(msg), "Deleted %d duplicates.", count_deleted);
                draw_status(msg);
                napms(1000); // let user see it
                break; // exit view to let normal ui refresh, could also rebuild in place but breaking is safer
            } else {
                draw_status("No items marked for deletion.");
                napms(500);
            }
        }
    }
    
    for (size_t k = 0; k < vi_n; k++) free(vi[k].path);
    free(vi);
}

static void draw_list_item(const ViewEntry *ve, int y, int x, int width, int is_sel, unsigned long long view_total, int sizew) {
    int size_col = x + 2;
    int type_col = size_col + (sizew > 0 ? (sizew + 1) : 0);
    int name_col = type_col + 2;
    
    int base_pair = ve->is_dir ? 3 : 4;
    int highlight_pair = 8; // Highlight pair from config

    // Setup row base attributes
    if (is_sel) attron(COLOR_PAIR(highlight_pair) | A_BOLD);
    else attron(COLOR_PAIR(base_pair));

    mvhline(y, x, ' ', width);

    // 1. Mark
    if (markset_has(&g_marks, ve->abs_path)) mvaddch(y, x, '*');
    else if (markset_covers(&g_marks, ve->abs_path)) mvaddch(y, x, '+');

    // 2. Size
    if (sizew > 0) {
        char sizebuf[64] = "";
        if (g_diff_mode) {
            unsigned long long abs_delta = (ve->delta < 0) ? (unsigned long long)(-ve->delta) : (unsigned long long)ve->delta;
            char hbuf[32]; human_size(abs_delta, hbuf, sizeof(hbuf));
            snprintf(sizebuf, sizeof(sizebuf), "%c%s", (ve->delta >= 0) ? '+' : '-', hbuf);
        } else if (g_display_mode == DISP_PCT) {
            if (ve->size_known) {
                double pct = (double)ve->size * 100.0 / (double)view_total;
                if (pct > 999.9) pct = 999.9;
                snprintf(sizebuf, sizeof(sizebuf), "%5.1f%%", pct);
            } else snprintf(sizebuf, sizeof(sizebuf), "  --.-%%");
        } else {
            if (ve->size_known) human_size(ve->size, sizebuf, sizeof(sizebuf)); else snprintf(sizebuf, sizeof(sizebuf), "?");
        }
        int pad = sizew - (int)strlen(sizebuf); if (pad < 0) pad = 0;

        if (!is_sel) {
            int sc = 5;
            if (g_diff_mode) { if (ve->delta > 0) sc = 7; else if (ve->delta < 0) sc = 5; else sc = base_pair; }
            else if (ve->size_known) { if (ve->size >= (1ULL<<30)) sc = 7; else if (ve->size >= (10ULL<<20)) sc = 6; else sc = 5; }
            attron(COLOR_PAIR(sc));
            mvaddnstr(y, size_col + pad, sizebuf, sizew - pad);
            attroff(COLOR_PAIR(sc));
            attron(COLOR_PAIR(base_pair));
        } else {
            mvaddnstr(y, size_col + pad, sizebuf, sizew - pad);
        }
    }

    // 3. Type
    mvaddch(y, type_col, ve->is_dir ? 'D' : 'F');

    // 4. Graph bar (only in normal mode, if enabled)
    int bar_w = (g_show_graph && !g_miller_mode) ? 12 : 0;
    if (bar_w > 0) {
        char barbuf[16]; barbuf[0] = '['; int filled = 0;
        if (ve->size_known && view_total > 0) {
            double frac = (double)ve->size / (double)view_total;
            filled = (int)(frac * 10.0 + 0.5); if (filled > 10) filled = 10;
        }
        for (int k = 0; k < 10; k++) barbuf[k+1] = (k < filled) ? '#' : ' ';
        barbuf[11] = ']'; barbuf[12] = '\0';
        int bcol = type_col + 2;
        if (!is_sel) attron(COLOR_PAIR(2));
        mvaddnstr(y, bcol, barbuf, bar_w);
        if (!is_sel) { attroff(COLOR_PAIR(2)); attron(COLOR_PAIR(base_pair)); }
        name_col += bar_w + 1;
    }

    // 5. Name and Tree markers
    int icon_w = g_use_nerd_fonts ? 3 : 0;
    int indent = g_tree_mode ? (ve->depth * 2) : 0;
    if (g_tree_mode && indent > 0) {
        for (int k = 0; k < indent - 2; k++) mvaddch(y, name_col + k, ' ');
        mvaddstr(y, name_col + indent - 2, "└ ");
    }
    if (g_use_nerd_fonts) mvaddstr(y, name_col + indent, get_icon(ve->name, ve->is_dir));
    if (ve->is_dir) attron(A_BOLD);
    if (g_tree_mode && ve->is_dir) {
        mvaddch(y, name_col + indent + icon_w, ve->expanded ? '-' : '+');
        mvaddch(y, name_col + indent + icon_w + 1, ' ');
        indent += 2;
    }
    
    int info_w_default = 16;
    int info_w = (g_info_col_mode == INFOCOL_HIDDEN || width < 60) ? 0 : info_w_default;
    int info_col = x + width - info_w - 1;
    int name_max = (info_w > 0) ? (info_col - (name_col + indent + icon_w) - 1) : (x + width - (name_col + indent + icon_w) - 1);

    if (name_max > 0) {
        if (g_inside_archive_path && !is_sel) attron(COLOR_PAIR(2));
        draw_truncated_name(y, name_col + indent + icon_w, ve->name, name_max);
        if (g_inside_archive_path && !is_sel) { attroff(COLOR_PAIR(2)); attron(COLOR_PAIR(base_pair)); }
    }

    // 6. Info Column
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
            if (ipad) { for (int k = 0; k < ipad; k++) mvaddch(y, info_col + k, ' '); }
            mvaddnstr(y, info_col + ipad, ibuf, info_w - ipad);
        }
    }

    if (is_sel) attroff(COLOR_PAIR(highlight_pair) | A_BOLD);
    else attroff(COLOR_PAIR(base_pair));
    if (ve->is_dir) attroff(A_BOLD);
}

static void draw_text_preview_column(const char *path, int y, int x, int width, int height) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        attron(COLOR_PAIR(7));
        mvaddstr(y + height/2, x + (width-12)/2, "Access Denied");
        attroff(COLOR_PAIR(7));
        return;
    }

    char line[1024];
    int current_line = 0;
    int drawn_lines = 0;
    attron(COLOR_PAIR(4));
    
    if (g_preview_focused) {
        attron(A_BOLD);
        mvaddstr(y - 1, x, " [ PREVIEW FOCUS - ESC to exit ] ");
        attroff(A_BOLD);
    }

    while (fgets(line, sizeof(line), fp) && drawn_lines < height) {
        if (current_line >= g_preview_scroll_y) {
            size_t l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
            
            if ((int)l > g_preview_scroll_x) {
                mvaddnstr(y + drawn_lines, x, line + g_preview_scroll_x, width - 1);
            }
            drawn_lines++;
        }
        current_line++;
    }
    attroff(COLOR_PAIR(4));
    fclose(fp);
}

static void draw_image_preview_column(const char *path, int y, int x, int width, int height) {
    // Note: This only works effectively with Kitty Graphics Protocol (Ghostty/Kitty)
    // and doesn't use Chafa fallback here to avoid terminal corruption during list drawing.
    if (!(getenv("KITTY_WINDOW_ID") || getenv("GHOSTTY_BIN_DIR") || (getenv("TERM") && strstr(getenv("TERM"), "kitty")))) {
        mvaddstr(y + height/2, x + (width-20)/2, "No Native Graphics");
        return;
    }

    FILE *fimg = fopen(path, "rb");
    if (!fimg) return;
    fseek(fimg, 0, SEEK_END);
    long fsize = ftell(fimg);
    fseek(fimg, 0, SEEK_SET);
    unsigned char *buf = malloc(fsize);
    if (buf && fread(buf, 1, fsize, fimg) == (size_t)fsize) {
        size_t b64_len;
        char *b64 = base64_encode(buf, fsize, &b64_len);
        if (b64) {
            int img_pw = 0, img_ph = 0;
            int final_c = width - 2, final_r = height - 2;
            if (get_image_dims(path, &img_pw, &img_ph)) {
                struct winsize ws;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0) {
                    double cell_w = (double)ws.ws_xpixel / (double)ws.ws_col;
                    double cell_h = (double)ws.ws_ypixel / (double)ws.ws_row;
                    double img_ratio = (double)img_pw / (double)img_ph;
                    double target_ratio = ((double)final_c * cell_w) / ((double)final_r * cell_h);
                    if (img_ratio > target_ratio) final_r = (int)(((double)final_c * cell_w) / (img_ratio * cell_h));
                    else final_c = (int)(((double)final_r * cell_h * img_ratio) / cell_w);
                }
            }
            
            int start_x = x + (width - final_c) / 2;
            int start_y = y + (height - final_r) / 2;
            
            // Delete previous image in this area (simplified for now)
            printf("\033_Ga=d,d=a\033\\");
            
            const size_t chunk_size = 4096;
            size_t sent = 0;
            printf("\033[%d;%dH", start_y, start_x);
            while (sent < b64_len) {
                size_t to_send = b64_len - sent;
                if (to_send > chunk_size) to_send = chunk_size;
                int is_last = (sent + to_send >= b64_len);
                if (sent == 0) printf("\033_Gq=1,a=T,t=d,f=100,c=%d,r=%d,m=%d;", final_c, final_r, is_last ? 0 : 1);
                else printf("\033_Gm=%d;", is_last ? 0 : 1);
                fwrite(b64 + sent, 1, to_send, stdout);
                printf("\033\\");
                sent += to_send;
            }
            fflush(stdout);
            free(b64);
        }
        free(buf);
    }
    fclose(fimg);
}

static void draw_column(const DirView *dv, int top, int x, int width, int is_active) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    int archive_offset = g_inside_archive_path ? 1 : 0;
    int y_start = (g_decorative ? 3 : 1) + archive_offset;
    int list_rows = rows - 3 - (g_decorative ? 2 : 0) - archive_offset;

    for (int i = 0; i < list_rows; i++) {
        size_t idx = (size_t)top + (size_t)i;
        if (idx >= dv->n) break;
        draw_list_item(&dv->v[idx], y_start + i, x, width, is_active && (idx == dv->selected), dv->view_total, dv->sizew);
    }
    // Draw vertical separator
    attron(COLOR_PAIR(2));
    for (int i = 0; i < list_rows + (g_decorative?2:0); i++) {
        int vy = (g_inside_archive_path ? 2 : 1) + i;
        if (vy < rows - 2) mvaddch(vy, x + width - 1, ACS_VLINE);
    }
    attroff(COLOR_PAIR(2));
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
static void draw_search_results(int rows, int cols) {
    attron(COLOR_PAIR(2) | A_BOLD);
    mvhline(1, 0, ' ', cols);
    const char *fstr = (g_search_filter == 1) ? "DIRS" : (g_search_filter == 2 ? "FILES" : "ALL");
    mvprintw(1, 0, " Global Search - %zu matches (Filter: %s) ", g_search_results_count, fstr);
    attroff(COLOR_PAIR(2) | A_BOLD);

    int list_rows = rows - 3 - (g_decorative ? 2 : 0);
    for (int i = 0; i < list_rows; i++) {
        int idx = g_search_top + i;
        if (idx >= (int)g_search_results_count) break;

        int y = (g_decorative ? 3 : 2) + i;
        int is_sel = (idx == g_search_selected);
        SearchResult *res = &g_search_results[idx];

        if (is_sel) attron(COLOR_PAIR(8) | A_BOLD);
        else if (res->is_dir) attron(COLOR_PAIR(3));
        else attron(COLOR_PAIR(4));

        mvhline(y, 0, ' ', cols);
        char szbuf[32]; human_size(res->size, szbuf, sizeof(szbuf));
        mvprintw(y, 0, "%-10s ", szbuf);
        
        const char *type_label = res->is_dir ? "[D] " : "[F] ";
        mvaddstr(y, 11, type_label);
        
        const char *base = path_basename_const(res->abs_path);
        mvaddstr(y, 15, base);
        
        // Draw directory path in gray/faded color if space permits
        int base_len = (int)strlen(base);
        if (cols > 15 + base_len + 5) {
            attron(A_DIM);
            mvprintw(y, 15 + base_len + 2, "(%s)", res->abs_path);
            attroff(A_DIM);
        }

        if (is_sel) attroff(COLOR_PAIR(8) | A_BOLD);
        else if (res->is_dir) attroff(COLOR_PAIR(3));
        else attroff(COLOR_PAIR(4));
    }
    
    mvhline(rows-1, 0, ' ', cols);
    attron(COLOR_PAIR(2));
    mvprintw(rows-1, 0, " [a]All [d]Dirs [f]Files | [Enter] Jump  [ESC/q] Exit");
    attroff(COLOR_PAIR(2));
}

static void draw_list(const DirView *dv, int top) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    if (!g_miller_mode || cols < 60) {
        // Standard full-width view
        int archive_offset = g_inside_archive_path ? 1 : 0;
        int y = (g_decorative ? 3 : 1) + archive_offset;
        int list_rows = rows - 3 - (g_decorative ? 2 : 0) - archive_offset;
        int sizew = dv->sizew;
        
        // Draw column headers if decorative
        if (g_decorative) {
            int h_y = 1 + archive_offset;
            attron(COLOR_PAIR(1)); mvhline(h_y, 0, ' ', cols);
            mvaddstr(h_y, 2, "Size"); mvaddstr(h_y, sizew + 3, "T"); mvaddstr(h_y, sizew + 6, "Name");
            
            int info_w = (g_info_col_mode == INFOCOL_HIDDEN) ? 0 : 16;
            if (info_w > 0 && cols > 60) {
                char info_title[24];
                if (g_info_col_mode == INFOCOL_MTIME) snprintf(info_title, sizeof(info_title), "Info(mtime)");
                else if (g_info_col_mode == INFOCOL_OWNER_PERM) snprintf(info_title, sizeof(info_title), "Info(perm)");
                else snprintf(info_title, sizeof(info_title), "Info(hidden)");
                mvaddnstr(h_y, cols - info_w, info_title, info_w);
            }
            
            attroff(COLOR_PAIR(1));
            attron(COLOR_PAIR(2)); mvhline(h_y + 1, 0, ACS_HLINE, cols); attroff(COLOR_PAIR(2));
        }

        for (int i = 0; i < list_rows; i++) {
            size_t idx = (size_t)top + (size_t)i;
            if (idx >= dv->n) break;
            draw_list_item(&dv->v[idx], y + i, 0, cols, (idx == dv->selected), dv->view_total, sizew);
        }
    } else {
        // Miller Columns (15% / 40% / 45%)
        int w_parent = cols * 15 / 100;
        int w_main = cols * 40 / 100;
        int w_preview = cols - w_parent - w_main;

        if (g_dv_parent.n > 0) {
            draw_column(&g_dv_parent, 0, 0, w_parent, 0);
        } else {
            // Draw centered ROOT label if we are at scan root
            int archive_offset = g_inside_archive_path ? 1 : 0;
            int y_mid = (rows - 3) / 2 + archive_offset;
            const char *label = "/ROOT";
            int lx = (w_parent - (int)strlen(label)) / 2;
            if (lx < 0) lx = 0;
            attron(COLOR_PAIR(2) | A_BOLD);
            mvaddstr(y_mid, lx, label);
            attroff(COLOR_PAIR(2) | A_BOLD);
            // Draw vertical separator manually
            attron(COLOR_PAIR(2));
            int list_rows = rows - 3 - (g_decorative ? 2 : 0) - archive_offset;
            for (int i = 0; i < list_rows + (g_decorative?2:0); i++) {
                int vy = (g_inside_archive_path ? 2 : 1) + i;
                if (vy < rows - 2) mvaddch(vy, w_parent - 1, ACS_VLINE);
            }
            attroff(COLOR_PAIR(2));
        }
        draw_column(dv, top, w_parent, w_main, 1);
        
        // Draw preview
        if (dv->n > 0) {
            ViewEntry *ve = &dv->v[dv->selected];
            int archive_offset = g_inside_archive_path ? 1 : 0;
            int y_start = (g_decorative ? 3 : 1) + archive_offset;
            int list_rows = rows - 3 - (g_decorative ? 2 : 0) - archive_offset;

            if (ve->is_dir) {
                // Clear any leftover native images before drawing directory list
                printf("\033_Ga=d,d=a\033\\"); fflush(stdout);
                draw_column(&g_dv_preview, 0, w_parent + w_main, w_preview, 0);
            } else {
                if (is_image_file(ve->abs_path)) {
                    draw_image_preview_column(ve->abs_path, y_start, w_parent + w_main + 1, w_preview - 1, list_rows);
                } else {
                    // Always clear native images before drawing text or error messages
                    printf("\033_Ga=d,d=a\033\\"); fflush(stdout);
                    if (is_textual_file(ve->abs_path)) {
                        draw_text_preview_column(ve->abs_path, y_start, w_parent + w_main + 1, w_preview - 1, list_rows);
                    } else {
                        attron(COLOR_PAIR(6));
                        int msg_x = w_parent + w_main + (w_preview - 18) / 2;
                        if (msg_x < w_parent + w_main + 1) msg_x = w_parent + w_main + 1;
                        mvaddstr(y_start + list_rows/2, msg_x, "Unsupported format");
                        attroff(COLOR_PAIR(6));
                    }
                }
            }
        }
    }
}

static void launch_subshell(const char *path) {
    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";

    char old_cwd[PATH_MAX];
    if (!getcwd(old_cwd, sizeof(old_cwd))) old_cwd[0] = '\0';

    def_prog_mode();
    endwin();
    
    printf("\n[fastdu] Dropping to subshell in: %s\n", path);
    printf("[fastdu] Type 'exit' or press Ctrl-D to return to fastdu.\n\n");
    fflush(stdout);

    if (chdir(path) == 0) {
        int rc = system(shell);
        (void)rc;
        if (old_cwd[0] != '\0') {
            int r = chdir(old_cwd);
            (void)r;
        }
    } else {
        perror("chdir");
        printf("\nPress Enter to continue...");
        getchar();
    }

    printf("\033[2J\033[H"); fflush(stdout); // Clear screen
    reset_prog_mode();
    refresh();
}

static void open_with_default(const char *path) {
    const char *ext = get_file_extension(path);
    const char *cmd_to_use = "xdg-open"; // Default fallback
    char msg[256];

    // Se l'estensione ha un punto (es. ".pdf"), cerchiamo sia con che senza
    const char *ext_no_dot = (ext[0] == '.') ? ext + 1 : ext;

    for (int i = 0; i < g_config.num_associations; i++) {
        const char *assoc_ext = g_config.associations[i].ext;
        const char *assoc_ext_no_dot = (assoc_ext[0] == '.') ? assoc_ext + 1 : assoc_ext;
        
        if (strcasecmp(ext_no_dot, assoc_ext_no_dot) == 0) {
            cmd_to_use = g_config.associations[i].cmd;
            break;
        }
    }

    char shell_cmd[PATH_MAX + 256];
    snprintf(shell_cmd, sizeof(shell_cmd), "%s \"%s\" >/dev/null 2>&1 &", cmd_to_use, path);
    
    if (system(shell_cmd) == -1) {
        snprintf(msg, sizeof(msg), "Failed to launch: %s", cmd_to_use);
        draw_status(msg);
    } else {
        snprintf(msg, sizeof(msg), "Opening with: %s", cmd_to_use);
        draw_status(msg);
    }
}

static void launch_external_editor(const char *path) {
    char cmd[PATH_MAX + 256];
    const char *editor = g_config.editor;
    
    if (editor[0] == '\0') {
        editor = getenv("EDITOR");
    }
    if (!editor || editor[0] == '\0') {
        editor = "vim"; // Last resort fallback
    }

    snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, path);

    def_prog_mode();
    endwin();
    
    int rc = system(cmd);
    if (rc == -1) {
        // system failed to run
    }

    reset_prog_mode();
    refresh();
}

/*
 * maybe_rescan_hovered
 * --------------------
 * When the selection is on a directory, perform a debounced check
 * (>=700ms or selection changed). If the directory mtime differs from
 * what is stored in the cache, rescan just that subtree to refresh data.
 */
static int maybe_rescan_hovered(DirView *dv, const char *root, Cache *cache) {
    static char last_path[PATH_MAX] = "";
    static struct timespec last_check = {0,0};

    if (dv->n == 0) return 0;
    ViewEntry *ve = &dv->v[dv->selected];
    if (!ve->is_dir) return 0;

    // debounce: only check if selection changed or after 700ms
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
    int changed = strcmp(last_path, ve->abs_path) != 0;
    if (!changed && ms < 700) return 0;
    last_check = now;
    strncpy(last_path, ve->abs_path, sizeof(last_path)); last_path[sizeof(last_path)-1] = '\0';

    // Update entry size from cache if it was unknown, but NEVER trigger a new disk scan automatically.
    // Automatic disk scans during scrolling cause severe lag on large trees.
    if (!ve->size_known) {
        if (cache_get_info(cache, ve->abs_path, &ve->size, NULL, NULL)) {
            ve->size_known = 1;
            return 1; // Need redraw
        }
    }
    return 0;
}

static void draw_status(const char *msg) {
    int cols; int rows; getmaxyx(stdscr, rows, cols);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, msg, cols-1);
    // Note: no pause here; message will be overwritten on next refresh
}

static int prompt_input(char *buf, size_t bufsz, const char *label) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    curs_set(1);
    mvhline(rows-1, 0, ' ', cols);
    mvaddnstr(rows-1, 0, label, cols-1);
    int x_start = (int)strlen(label);
    
    size_t len = strlen(buf);
    size_t cursor_pos = len; // Initial cursor at the end
    
    while (1) {
        // Redraw line
        mvhline(rows-1, x_start, ' ', cols - x_start);
        mvaddstr(rows-1, x_start, buf);
        move(rows-1, x_start + (int)cursor_pos);
        refresh();
        
        int ch = getch();
        if (ch == 27) { // ESC
            buf[0] = '\0';
            curs_set(0);
            return -1; 
        } else if (ch == 10 || ch == 13 || ch == KEY_ENTER) { // ENTER
            break;
        } else if (ch == KEY_LEFT) {
            if (cursor_pos > 0) cursor_pos--;
        } else if (ch == KEY_RIGHT) {
            if (cursor_pos < len) cursor_pos++;
        } else if (ch == KEY_HOME || ch == 1) { // Home or Ctrl-A
            cursor_pos = 0;
        } else if (ch == KEY_END || ch == 5) { // End or Ctrl-E
            cursor_pos = len;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { // BACKSPACE
            if (cursor_pos > 0) {
                memmove(buf + cursor_pos - 1, buf + cursor_pos, len - cursor_pos + 1);
                len--;
                cursor_pos--;
            }
        } else if (ch == KEY_DC) { // DELETE key
            if (cursor_pos < len) {
                memmove(buf + cursor_pos, buf + cursor_pos + 1, len - cursor_pos);
                len--;
            }
        } else if (isprint(ch)) {
            if (len < bufsz - 1) {
                memmove(buf + cursor_pos + 1, buf + cursor_pos, len - cursor_pos + 1);
                buf[cursor_pos] = (char)ch;
                len++;
                cursor_pos++;
            }
        }
    }
    buf[len] = '\0';
    curs_set(0);
    
    // Trim trailing spaces
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\t' || buf[len-1] == '\r')) { buf[--len] = '\0'; }
    return (int)len;
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
        for (size_t i = 0; i < job->n; i++)
            free(job->paths[i]);
        free(job->paths);
        free(job);
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
            unsigned long long sz = 0ULL;
            if (cache_get_info(cache, path, &sz, NULL, NULL)) total += sz;
            // else skip (0) to avoid heavy sum_path_size traversal in render loop
        }
    }
    return total;
}

static void cycle_theme(void) {
    g_current_theme_idx = (g_current_theme_idx + 1) % (sizeof(g_themes) / sizeof(g_themes[0]));
    apply_theme(g_themes[g_current_theme_idx]);
    char msg[64];
    snprintf(msg, sizeof(msg), "Theme switched to: %s", g_themes[g_current_theme_idx]);
    draw_status(msg);
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
        "  Enter or Right/l - open/expand directory",
        "  Backspace or Left - go up",
        "  b - go to top, e - go to end",
        "",
        "Actions:",
        "  v - preview selected text file (scrollable layer)",
        "  a - toggle tree view mode",
        "  M - toggle Miller Columns mode (ranger-style)",
        "  E - extension distribution view (current directory)",
        "  U - duplicate finder (waste space analyzer)",
        "  O - open selected item with system default (xdg-open)",
        "  Ctrl-E - edit selected file with external editor",
        "  Ctrl-S - drop to subshell in current directory",
        "  Ctrl-n - create new folder",
        "  ALT-n - create new empty file",
        "  z - compress marked/selected items to .zip",
        "  x - extract selected archive",
        "  r - rescan selected dir",
        "  R - rescan current dir (parallel)",
        "  / - global search (entire cache)",
        "  f - find by name (case-insensitive), n/N next/prev",
        "  F - regex search (case-insensitive), enables query filter",
        "  t - toggle type filter (all/dirs/files)",
        "  T - toggle filter by query",
        "  Ctrl-T - reset all filters and search queries",
        "  SPACE - mark/unmark file/dir",
        "  Ctrl-A - select/deselect all in view",
        "  m - move marked to current directory",
        "  c - copy marked to current directory (with progress)",
        "  d - delete marked (if any) else delete selected",
        "  o - toggle sort key (size/name/mtime/delta)",
        "  s - toggle sort order (asc/desc)",
        "  K - cycle theme presets (Dracula, TokyoNight, etc.)",
        "  Y - take baseline snapshot & toggle DIFF mode",
        "  I - size display: numeric → percent → off",
        "  i - info column (mtime → owner+perm → hidden)",
        "  TAB / Ctrl-i - toggle graph bar",
        "  ALT-r - rename selected item",
        "  q - quit",
        "  h - this help",
        "",
        "CLI:",
        "  fastdu [-R] [-x] [-e PAT] [-j N] [--export FMT FILE] [path]",
        "",
        "Version:",
        "  fastdu v" FASTDU_VERSION,
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
        char title[128];
        snprintf(title, sizeof(title), " fastdu v%s - Help ", FASTDU_VERSION);
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

static void show_image_preview(const char *path) {
    int cols, rows; getmaxyx(stdscr, rows, cols);
    int w = cols - 4; if (w < 20) w = cols;
    int h = rows - 4; if (h < 10) h = rows;
    int x = (cols - w) / 2;
    int y = (rows - h) / 2;

    WINDOW *win = newwin(h, w, y, x);
    keypad(win, TRUE);
    box(win, 0, 0);
    wattron(win, A_REVERSE | A_BOLD);
    mvwprintw(win, 0, 2, " Image Preview: %s - [q] to close ", path_basename_const(path));
    wattroff(win, A_REVERSE | A_BOLD);
    wrefresh(win);

    // Try Native Kitty Graphics Protocol (Ghostty, Kitty, WezTerm)
    int kitty_supported = (getenv("KITTY_WINDOW_ID") != NULL || getenv("GHOSTTY_BIN_DIR") != NULL || (getenv("TERM") != NULL && strstr(getenv("TERM"), "kitty") != NULL));
    
    int shown_natively = 0;
    if (kitty_supported) {
        // Use t=f (path transfer) for maximum format support and performance
        // Path must be Base64 encoded. We use the absolute path.
        size_t b64_len;
        char *b64_path = base64_encode((const unsigned char *)path, strlen(path), &b64_len);
        
        if (b64_path) {
            // Calculate optimal cells to preserve aspect ratio
            int img_pw = 0, img_ph = 0;
            int final_c = w - 2, final_r = h - 2;
            if (get_image_dims(path, &img_pw, &img_ph)) {
                struct winsize ws;
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_xpixel > 0) {
                    double cell_w = (double)ws.ws_xpixel / (double)ws.ws_col;
                    double cell_h = (double)ws.ws_ypixel / (double)ws.ws_row;
                    double img_ratio = (double)img_pw / (double)img_ph;
                    double target_ratio = ((double)(w - 2) * cell_w) / ((double)(h - 2) * cell_h);
                    if (img_ratio > target_ratio) {
                        final_c = w - 2;
                        final_r = (int)(((double)final_c * cell_w) / (img_ratio * cell_h));
                    } else {
                        final_r = h - 2;
                        final_c = (int)(((double)final_r * cell_h * img_ratio) / cell_w);
                    }
                }
            }

            def_prog_mode();
            endwin();
            printf("\033[2J\033[H"); fflush(stdout); // Clear screen

            // Position cursor (centered in the window area)
            int start_x = x + 2 + (w - 2 - final_c) / 2;
            int start_y = y + 2 + (h - 2 - final_r) / 2;
            printf("\033[%d;%dH", start_y, start_x);
            
            // a=T (transfer/display), t=f (file path), f=100 (auto), c,r (target cells)
            // No chunking needed for path transfer
            printf("\033_Gq=1,a=T,t=f,f=100,c=%d,r=%d;%s\033\\", final_c, final_r, b64_path);
            
            printf("\n\033[7m Native Preview (Kitty Protocol) - Press any key to return... \033[0m");
            fflush(stdout);
            
            struct termios oldt, newt;
            tcgetattr(STDIN_FILENO, &oldt);
            newt = oldt;
            newt.c_lflag &= ~(ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &newt);
            (void)getchar();
            tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            
            printf("\033_Ga=d,d=a\033\\"); 
            printf("\033[2J\033[H"); fflush(stdout);
            reset_prog_mode();
            refresh();
            shown_natively = 1;
            free(b64_path);
        }
    }

    if (!shown_natively) {
        int has_chafa = (system("command -v chafa >/dev/null 2>&1") == 0);
        if (!has_chafa) {
            const char *msg1 = "Error: Terminal graphics not detected.";
            const char *msg2 = "Install 'chafa' for universal image support.";
            mvwprintw(win, h/2, (w-(int)strlen(msg1))/2, "%s", msg1);
            mvwprintw(win, h/2 + 1, (w-(int)strlen(msg2))/2, "%s", msg2);
            wrefresh(win);
            while (1) { int ch = wgetch(win); if (ch == 'q' || ch == 'Q' || ch == 27) break; }
        } else {
            def_prog_mode(); endwin();
            printf("\033[2J\033[H"); fflush(stdout); // Clear
            printf("\033[%d;%dH", y + 2, x + 2);
            char direct_cmd[PATH_MAX + 256];
            snprintf(direct_cmd, sizeof(direct_cmd), "chafa --size=%dx%d \"%s\"", w - 2, h - 2, path);
            system(direct_cmd);
            printf("\n\033[7m Chafa Preview - Press any key to return... \033[0m");
            fflush(stdout);
            struct termios oldt, newt;
            tcgetattr(STDIN_FILENO, &oldt); newt = oldt; newt.c_lflag &= ~(ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &newt); (void)getchar(); tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            printf("\033[2J\033[H"); fflush(stdout); // Clear
            reset_prog_mode(); refresh();
        }
    }
    delwin(win);
}

static void show_preview(const char *path) {
    if (is_image_file(path)) {
        show_image_preview(path);
        return;
    }

    if (g_bat_cmd) {
        char cmd[PATH_MAX + 128];
        snprintf(cmd, sizeof(cmd), "%s --paging=always \"%s\"", g_bat_cmd, path);
        
        def_prog_mode();
        endwin();
        printf("\033[2J\033[H"); fflush(stdout); // Clear screen
        
        int rc = system(cmd);
        (void)rc;
        
        printf("\033[2J\033[H"); fflush(stdout); // Clear again
        reset_prog_mode();
        refresh();
        return;
    }

    // Fallback to internal viewer if 'bat' is not found
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
    // Iterative removal using a depth-sorted list of paths
    size_t cap = 128, n = 0;
    char **list = malloc(cap * sizeof(char*));
    if (!list) return -1;
    list[n++] = xstrdup(path);

    // 1. Collect all paths (BFS-like walk)
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        if (lstat(list[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            DIR *dp = opendir(list[i]);
            if (dp) {
                struct dirent *de;
                while ((de = readdir(dp)) != NULL) {
                    if (is_dot_or_dotdot(de->d_name)) continue;
                    char *child = path_join(list[i], de->d_name);
                    if (!child) continue;
                    if (cache_file_abs && strcmp(child, cache_file_abs) == 0) { free(child); continue; }
                    if (n == cap) { cap *= 2; list = realloc(list, cap * sizeof(char*)); }
                    list[n++] = child;
                }
                closedir(dp);
            }
        }
    }

    // 2. Remove in reverse order (children first)
    int final_rc = 0;
    for (ssize_t i = (ssize_t)n - 1; i >= 0; i--) {
        struct stat st;
        int rc = 0;
        if (lstat(list[i], &st) == 0) {
            if (S_ISDIR(st.st_mode)) rc = rmdir(list[i]);
            else rc = unlink(list[i]);
        }
        if (rc != 0) final_rc = -1;
        free(list[i]);
    }
    free(list);
    return final_rc;
}

static void cache_remove_prefix(Cache *c, const char *prefix) {
    for (int h = 0; h < CACHE_SHARDS; h++) {
        pthread_mutex_lock(&c->shards[h].mu);
        size_t w = 0;
        for (size_t i = 0; i < c->shards[h].n; i++) {
            if (starts_with(c->shards[h].v[i].abs_path, prefix)) {
                free(c->shards[h].v[i].abs_path);
                free(c->shards[h].v[i].rel_path);
                continue; // skip
            }
            if (w != i) c->shards[h].v[w] = c->shards[h].v[i];
            w++;
        }
        c->shards[h].n = w;
        pthread_mutex_unlock(&c->shards[h].mu);
    }
}

static void cache_adjust_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, long long delta) {
    if (delta <= 0) return;
    char *p = xstrdup(abs_path);
    if (!p) return;
    // start from parent of abs_path
    char *cur = get_parent(p);
    free(p);
    while (cur) {
        struct stat st;
        time_t m = (stat(cur, &st) == 0) ? st.st_mtime : 0;
        cache_update_size(c, cur, -delta, m);
        if (strcmp(cur, root) == 0) break;
        char *parent = get_parent(cur);
        free(cur);
        cur = parent;
    }
    if (cur) free(cur);
}

static void cache_add_ancestors_after_delta(Cache *c, const char *root, const char *abs_path, unsigned long long delta) {
    if (delta == 0) return;
    char *p = xstrdup(abs_path);
    if (!p) return;
    char *cur = get_parent(p);
    free(p);
    while (cur) {
        struct stat st;
        time_t m = (stat(cur, &st) == 0) ? st.st_mtime : 0;
        cache_update_size(c, cur, (long long)delta, m);
        if (strcmp(cur, root) == 0) break;
        char *parent = get_parent(cur);
        free(cur);
        cur = parent;
    }
    if (cur) free(cur);
}

static void cache_move_prefix(Cache *c, const char *root, const char *old_prefix, const char *new_prefix) {
    size_t lold = strlen(old_prefix);
    // Temporary storage for entries being moved
    size_t cap = 128, n = 0;
    CacheEntry *temp = malloc(cap * sizeof(CacheEntry));
    if (!temp) return;

    for (int h = 0; h < CACHE_SHARDS; h++) {
        pthread_mutex_lock(&c->shards[h].mu);
        size_t w = 0;
        for (size_t i = 0; i < c->shards[h].n; i++) {
            if (starts_with(c->shards[h].v[i].abs_path, old_prefix)) {
                if (n == cap) {
                    cap *= 2;
                    temp = realloc(temp, cap * sizeof(CacheEntry));
                }
                temp[n++] = c->shards[h].v[i];
                continue;
            }
            if (w != i) c->shards[h].v[w] = c->shards[h].v[i];
            w++;
        }
        c->shards[h].n = w;
        pthread_mutex_unlock(&c->shards[h].mu);
    }

    // Update and re-insert
    for (size_t i = 0; i < n; i++) {
        const char *suffix = temp[i].abs_path + lold;
        char *new_abs = NULL;
        if (suffix[0] == '\0') new_abs = xstrdup(new_prefix);
        else if (suffix[0] == '/') new_abs = path_join(new_prefix, suffix + 1);
        else new_abs = path_join(new_prefix, suffix);

        if (new_abs) {
            free(temp[i].abs_path);
            temp[i].abs_path = new_abs;
            free(temp[i].rel_path);
            temp[i].rel_path = relpath_from_abs(root, new_abs);
            struct stat st; if (stat(new_abs, &st) == 0) temp[i].dir_mtime = st.st_mtime;
            temp[i].last_scan = time(NULL);
            
            // Re-insert into correct shard
            cache_upsert_with_meta(c, root, temp[i].abs_path, temp[i].size, temp[i].last_scan, temp[i].ino, temp[i].dir_mtime);
            // Free the memory we just moved (upsert copied it)
            free(temp[i].abs_path);
            free(temp[i].rel_path);
        }
    }
    free(temp);
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
    InodeSet *is;
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
    child->is = parent->is;
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
#ifdef HAS_IO_URING
struct io_uring_context {
    struct io_uring ring;
    int active;
};
static _Thread_local struct io_uring_context g_uring = { .active = 0 };

struct async_stat_batch {
    char name[256];
    struct statx stx;
    int active;
};
#endif

static void process_task(DirTask *t) {
    int dupfd = dup(t->dirfd);
    if (dupfd < 0) {
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

#ifdef HAS_IO_URING
    #define URING_BATCH 64
    struct async_stat_batch batch[URING_BATCH];
    int batch_count = 0;
#endif

    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        if (g_interrupted) break;
        if (is_dot_or_dotdot(de->d_name)) continue;
        if (is_excluded(de->d_name)) continue;
        atomic_fetch_add(&g_progress_count, 1ULL);
        if (t->abs_path && t->root) {
            if (strcmp(t->abs_path, t->root) == 0 && strcmp(de->d_name, CACHE_FILENAME) == 0) continue;
        }
        unsigned char dt = de->d_type;
        if (dt == DT_LNK) continue;

#ifdef HAS_IO_URING
        if (g_uring.active && (dt == DT_REG || dt == DT_UNKNOWN)) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&g_uring.ring);
            if (sqe) {
                batch[batch_count].active = 1;
                strncpy(batch[batch_count].name, de->d_name, 255);
                batch[batch_count].name[255] = '\0';
                io_uring_prep_statx(sqe, t->dirfd, batch[batch_count].name, AT_SYMLINK_NOFOLLOW, STATX_SIZE | STATX_TYPE | STATX_INO, &batch[batch_count].stx);
                io_uring_sqe_set_data(sqe, &batch[batch_count]);
                batch_count++;
                if (batch_count == URING_BATCH) {
                    io_uring_submit_and_wait(&g_uring.ring, batch_count);
                    struct io_uring_cqe *cqe;
                    unsigned head;
                    int count = 0;
                    io_uring_for_each_cqe(&g_uring.ring, head, cqe) {
                        count++;
                        if (cqe->res == 0) {
                            struct async_stat_batch *data = (struct async_stat_batch *)io_uring_cqe_get_data(cqe);
                            if (S_ISREG(data->stx.stx_mode)) {
                                if (data->stx.stx_nlink <= 1 || !inodeset_check_and_add(t->is, (dev_t)((unsigned long)data->stx.stx_dev_major << 32 | data->stx.stx_dev_minor), data->stx.stx_ino)) {
                                    unsigned long long b = data->stx.stx_size;
                                    if (g_accuracy_mode) b = data->stx.stx_blocks * 512ULL;
                                    atomic_fetch_add(&t->files_size, b);
                                    atomic_fetch_add(&g_total_files, 1ULL);
                                    atomic_fetch_add(&g_total_bytes, b);
                                }
                            } else if (S_ISDIR(data->stx.stx_mode)) {
                                int cfd = openat(t->dirfd, data->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                                if (cfd >= 0) {
                                    if (g_one_file_system) {
                                        struct stat stch;
                                        if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) { close(cfd); }
                                        else { enqueue_child(t, cfd, data->name); }
                                    } else { enqueue_child(t, cfd, data->name); }
                                }
                            }
                        }
                    }
                    io_uring_cq_advance(&g_uring.ring, count);
                    batch_count = 0;
                }
                continue;
            }
        }
#endif

        if (dt == DT_REG || dt == DT_UNKNOWN) {
            struct stat stf;
            if (fstatat(t->dirfd, de->d_name, &stf, AT_SYMLINK_NOFOLLOW) == 0) {
                if (S_ISLNK(stf.st_mode)) continue;
                if (S_ISREG(stf.st_mode)) {
                    if (stf.st_nlink <= 1 || !inodeset_check_and_add(t->is, stf.st_dev, stf.st_ino)) {
                        unsigned long long b = file_size_bytes(&stf);
                        atomic_fetch_add(&t->files_size, b);
                        atomic_fetch_add(&g_total_files, 1ULL);
                        atomic_fetch_add(&g_total_bytes, b);
                    }
                } else if (S_ISDIR(stf.st_mode)) {
                    dt = DT_DIR;
                }
            }
        }
        
        if (dt == DT_DIR) {
            int cfd = openat(t->dirfd, de->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (cfd >= 0) {
                if (g_one_file_system) {
                    struct stat stch;
                    if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) {
                        close(cfd);
                        continue;
                    }
                }
                enqueue_child(t, cfd, de->d_name);
            }
        }
    }

#ifdef HAS_IO_URING
    if (batch_count > 0) {
        io_uring_submit_and_wait(&g_uring.ring, batch_count);
        struct io_uring_cqe *cqe;
        unsigned head;
        int count = 0;
        io_uring_for_each_cqe(&g_uring.ring, head, cqe) {
            count++;
            if (cqe->res == 0) {
                struct async_stat_batch *data = (struct async_stat_batch *)io_uring_cqe_get_data(cqe);
                if (S_ISREG(data->stx.stx_mode)) {
                    if (data->stx.stx_nlink <= 1 || !inodeset_check_and_add(t->is, (dev_t)((unsigned long)data->stx.stx_dev_major << 32 | data->stx.stx_dev_minor), data->stx.stx_ino)) {
                        unsigned long long b = data->stx.stx_size;
                        if (g_accuracy_mode) b = data->stx.stx_blocks * 512ULL;
                        atomic_fetch_add(&t->files_size, b);
                        atomic_fetch_add(&g_total_files, 1ULL);
                        atomic_fetch_add(&g_total_bytes, b);
                    }
                } else if (S_ISDIR(data->stx.stx_mode)) {
                    int cfd = openat(t->dirfd, data->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                    if (cfd >= 0) {
                        if (g_one_file_system) {
                            struct stat stch;
                            if (fstat(cfd, &stch) == 0 && stch.st_dev != g_root_dev) { close(cfd); }
                            else { enqueue_child(t, cfd, data->name); }
                        } else { enqueue_child(t, cfd, data->name); }
                    }
                }
            }
        }
        io_uring_cq_advance(&g_uring.ring, count);
    }
#endif

    closedir(dp);
    if (atomic_fetch_sub(&t->pending, 1) == 1) {
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
    InodeSet is;
} ScanPool;

static void *worker_loop(void *arg) {
    ScanPool *p = (ScanPool*)arg;
#ifdef HAS_IO_URING
    if (io_uring_queue_init(256, &g_uring.ring, 0) == 0) {
        g_uring.active = 1;
    }
#endif
    for (;;) {
        DirTask *t = (DirTask*)tq_pop(&p->q);
        if (!t) break;
        atomic_fetch_add(&g_active_workers, 1);
        process_task(t);
        atomic_fetch_sub(&g_active_workers, 1);
    }
#ifdef HAS_IO_URING
    if (g_uring.active) {
        io_uring_queue_exit(&g_uring.ring);
        g_uring.active = 0;
    }
#endif
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
    inodeset_init(&p.is, 65536);
    p.threads = threads; p.th = malloc((size_t)threads * sizeof(pthread_t)); wg_init(&p.wg);
    // start workers and finalizer
    for (int i = 0; i < threads; i++) pthread_create(&p.th[i], NULL, worker_loop, &p);
    pthread_create(&p.fin_th, NULL, finalizer_loop, &p);

    atomic_store(&g_progress_count, 0ULL);
    atomic_store(&g_active_workers, 0);
    atomic_store(&g_total_files, 0ULL);
    atomic_store(&g_total_bytes, 0ULL);
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) { tq_close(&p.q); for (int i = 0; i < threads; i++) pthread_join(p.th[i], NULL); tq_destroy(&p.q); free(p.th); inodeset_free(&p.is); return 0ULL; }
    DirTask *rt = malloc(sizeof(DirTask));
    rt->dirfd = fd; rt->abs_path = xstrdup(root); rt->root = root; rt->cache_abs = cache_abs; rt->cache = cache; rt->parent = NULL;
    rt->is = &p.is;
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
    inodeset_free(&p.is);

    // Clear progress line
    int cols, rows; getmaxyx(stdscr, rows, cols);
    mvhline(rows-1, 0, ' ', cols);
    refresh();

    unsigned long long rs = 0ULL;
    cache_get_info(cache, root, &rs, NULL, NULL);
    return rs;
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

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static void install_signal_handlers(void) {
    int sigs[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGTERM, SIGHUP };
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER;
    for (size_t i = 0; i < sizeof(sigs)/sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
    // Specific handler for SIGINT to allow clean interruption
    struct sigaction sa_int; memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_handler = sigint_handler;
    sigemptyset(&sa_int.sa_mask);
    sigaction(SIGINT, &sa_int, NULL);
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
    
    // Detect bat/batcat once
    if (system("command -v bat >/dev/null 2>&1") == 0) g_bat_cmd = "bat";
    else if (system("command -v batcat >/dev/null 2>&1") == 0) g_bat_cmd = "batcat";

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
    int server_mode = 0;
    const char *path_arg = NULL;
    int headless_flag = 0;
    const char *export_format = NULL;
    const char *export_file = NULL;
    const char *snapshot_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-R") == 0 || strcmp(argv[i], "--reload") == 0) {
            reload_flag = 1;
        } else if (strcmp(argv[i], "--server") == 0) {
            server_mode = 1;
        } else if (strcmp(argv[i], "--diff") == 0) {
            if (i + 1 < argc) snapshot_file = argv[++i];
        } else if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--exclude") == 0) {
            if (i + 1 < argc) { exclude_add(argv[++i]); }
        } else if (strcmp(argv[i], "--export") == 0) {
            if (i + 2 < argc) {
                export_format = argv[++i];
                export_file = argv[++i];
                headless_flag = 1; // Export implies headless
            }
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
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--one-file-system") == 0) {
            g_one_file_system = 1;
        } else if (strcmp(argv[i], "-D") == 0 || strcmp(argv[i], "--decorative") == 0) {
            g_decorative = 1;
        } else if (strcmp(argv[i], "-nf") == 0 || strcmp(argv[i], "--nerd-fonts") == 0) {
            g_use_nerd_fonts = 1;
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

    struct stat st_root;
    if (stat(root, &st_root) == 0) {
        g_root_dev = st_root.st_dev;
    }

    load_fastduignore(root);

    // TTY / headless setup
    if (!headless) {
        // TUI setup early to show progress
        load_config_file();
        initscr();
        cbreak(); // Standard input mode
        noecho();
        
        // Disable flow control so Ctrl-S can be used as a shortcut
        struct termios t;
        if (tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_iflag &= ~(IXON | IXOFF);
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }

        keypad(stdscr, TRUE);
        mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
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
            for (int i = 1; i <= 8; i++) {
                init_pair((short)i, g_config.pairs[i][0], g_config.pairs[i][1]);
            }
        }
    }

    // Prepare cache (load or full rescan)
    Cache cache; cache_init(&cache);
    cache_init(&g_snapshot_cache);

    if (snapshot_file) {
        if (cache_load_file(snapshot_file, root, &g_snapshot_cache) > 0) {
            g_diff_mode = 1;
            if (debug_all) fprintf(stderr, "[main] snapshot loaded from %s\n", snapshot_file);
        } else {
            fprintf(stderr, "Warning: could not load snapshot from %s\n", snapshot_file);
        }
    }

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
            unsigned long long rs = 0ULL;
            cache_get_info(&cache, root, &rs, NULL, NULL);
            g_last_bytes = rs;
            g_last_files = count_files_path(root);
        } else {
            int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
            if (threads < 1) threads = 1;
            if (threads > 64) threads = 64;
            (void)scan_dir_parallel_deep(root, cache_abs, &cache, threads);
            
            // Sync footer totals from fresh scan
            unsigned long long rs = 0ULL;
            if (cache_get_info(&cache, root, &rs, NULL, NULL)) {
                g_last_bytes = rs;
            }
            g_last_files = count_files_path(root);
        }
        cache_save(root, &cache);
        if (!headless) {
            int cols, rows; getmaxyx(stdscr, rows, cols);
            mvhline(rows-1, 0, ' ', cols);
            refresh();
        }
    } else {
        // Cache loaded: set footer totals from cache root entry (v2/v3)
        unsigned long long rs = 0ULL;
        cache_get_info(&cache, root, &rs, NULL, NULL);
        g_last_bytes = rs;
        // totals_files is already set by cache_load parsing (version 3 or row counting)
    }

    if (server_mode) {
        run_server(root, &cache);
        cache_free(&cache);
        cache_free(&g_snapshot_cache);
        free(cache_abs);
        return 0;
    }

    if (headless) {
        if (debug_all) fprintf(stderr, "[dbg] entering headless summary branch\n");

        if (export_format && export_file) {
            if (strcmp(export_format, "json") == 0) {
                cache_export_json(&cache, export_file);
                fprintf(stderr, "[export] JSON written to %s\n", export_file);
            } else if (strcmp(export_format, "csv") == 0) {
                cache_export_csv(&cache, export_file);
                fprintf(stderr, "[export] CSV written to %s\n", export_file);
            } else {
                fprintf(stderr, "Unsupported export format: %s\n", export_format);
            }
        }

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
        if (debug_all) { free(cache_abs); _exit(0); }
        const char *skip_free = getenv("FASTDU_SKIP_FREE_CACHE");
        if (skip_free && skip_free[0]=='1') {
            if (debug_all) fprintf(stderr, "[dbg] skipping cache_free due to FASTDU_SKIP_FREE_CACHE=1\n");
            free(cache_abs);
            _exit(0);
        }
        cache_free(&cache);
        free(cache_abs);
        exclude_free();
        if (debug_all) fprintf(stderr, "[dbg] cache_free done, exiting now\n");
        return 0;
    }

    DirView dv = (DirView){0};
    NavStack nav; navstack_init(&nav);
    markset_init(&g_marks);
    char current[PATH_MAX]; strncpy(current, root, sizeof(current)); current[sizeof(current)-1] = '\0';

    int top = 0;
    build_dir_view(current, root, &cache, &dv);
    update_miller_columns(current, root, &cache, &dv);

    int ch;
    while (1) {
        if (g_global_search_mode) {
            int rows, cols; getmaxyx(stdscr, rows, cols);
            erase();
            draw_header(root, current, &cache);
            draw_search_results(rows, cols);
            refresh();

            ch = getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) {
                g_global_search_mode = 0;
                search_results_free();
                continue;
            }
            if (ch == 'a') { g_search_filter = 0; apply_search_filter(); continue; }
            if (ch == 'd') { g_search_filter = 1; apply_search_filter(); continue; }
            if (ch == 'f') { g_search_filter = 2; apply_search_filter(); continue; }

            if (ch == KEY_UP || ch == 'k') {
                if (g_search_selected > 0) {
                    g_search_selected--;
                    if (g_search_selected < g_search_top) g_search_top = g_search_selected;
                }
            } else if (ch == KEY_DOWN || ch == 'j') {
                if (g_search_selected + 1 < (int)g_search_results_count) {
                    g_search_selected++;
                    int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                    if (g_search_selected >= g_search_top + list_rows) g_search_top = g_search_selected - list_rows + 1;
                }
            } else if (ch == 10 || ch == 13 || ch == KEY_ENTER) {
                if (g_search_results_count > 0) {
                    char *target = xstrdup(g_search_results[g_search_selected].abs_path);
                    g_global_search_mode = 0;
                    search_results_free();

                    // Jump to target logic
                    struct stat st;
                    if (lstat(target, &st) == 0) {
                        char *parent = get_parent(target);
                        if (parent) {
                            // If it's a directory, we can jump into it or its parent.
                            // Better jump to parent and select the item.
                            strncpy(current, parent, sizeof(current)); current[sizeof(current)-1] = '\0';
                            view_free(&dv); top = 0;
                            build_dir_view(current, root, &cache, &dv);
                            // Select the item
                            for (size_t i = 0; i < dv.n; i++) {
                                if (strcmp(dv.v[i].abs_path, target) == 0) {
                                    dv.selected = i; 
                                    int r, c; getmaxyx(stdscr, r, c);
                                    int lr = r - 3 - (g_decorative ? 2 : 0);
                                    if ((int)dv.selected >= top + lr) top = (int)dv.selected - lr + 1;
                                    break;
                                }
                            }
                            if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                            free(parent);
                        }
                    }
                    free(target);
                }
                continue;
            }
            continue;
        }

        // Handle automatic updates (if any) and trigger redraw if needed
        (void)maybe_rescan_hovered(&dv, root, &cache);

        erase();
        draw_header(root, current, &cache);
        draw_list(&dv, top);
        refresh();

        ch = getch();
        if (ch == 'q' || ch == 'Q') break;
        else if (ch == 27) { // ESC or ALT sequence
            nodelay(stdscr, TRUE);
            int next_ch = getch();
            nodelay(stdscr, FALSE);
            if (next_ch == 'r') { // ALT+r: Rename selected
                if (dv.n > 0) {
                    ViewEntry *ve = &dv.v[dv.selected];
                    if (g_inside_archive_path) {
                        draw_status("Renaming inside archives not supported");
                    } else {
                        char name_buf[256];
                        const char *old_base = path_basename_const(ve->abs_path);
                        strncpy(name_buf, old_base, sizeof(name_buf)-1);
                        name_buf[sizeof(name_buf)-1] = '\0';
                        
                        int retry = 1;
                        while (retry) {
                            retry = 0;
                            int got = prompt_input(name_buf, sizeof(name_buf), "Rename to: ");
                            if (got > 0) {
                                char *new_abs = path_join(current, name_buf);
                                if (new_abs) {
                                    if (strcmp(new_abs, ve->abs_path) == 0) {
                                        free(new_abs); break;
                                    }
                                    int skip = 0;
                                    if (path_exists(new_abs)) {
                                        char action = prompt_conflict_action(new_abs);
                                        if (action == 's' || action == 'S') skip = 1;
                                        else if (action == 'r' || action == 'R') {
                                            char *auto_new = gen_nonconflicting_path(new_abs);
                                            free(new_abs); new_abs = auto_new;
                                        } else if (action == 'n') {
                                            retry = 1; free(new_abs); continue;
                                        }
                                        if (action == 'o' || action == 'O') unlink(new_abs);
                                    }
                                    
                                    if (!skip && new_abs) {
                                        unsigned long long old_sz = ve->size;
                                        int old_known = ve->size_known;
                                        if (rename(ve->abs_path, new_abs) == 0) {
                                            cache_remove_prefix(&cache, ve->abs_path);
                                            if (old_known) {
                                                struct stat stn;
                                                if (lstat(new_abs, &stn) == 0) {
                                                    cache_upsert_with_meta(&cache, root, new_abs, old_sz, time(NULL), (unsigned long long)stn.st_ino, stn.st_mtime);
                                                }
                                            }
                                            cache_save(root, &cache);
                                            view_free(&dv);
                                            build_dir_view(current, root, &cache, &dv);
                                            // Find new index
                                            for(size_t i=0; i<dv.n; i++) {
                                                if (strcmp(dv.v[i].abs_path, new_abs) == 0) {
                                                    dv.selected = i; break;
                                                }
                                            }
                                            if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                                            draw_status("Renamed successfully.");
                                        } else {
                                            draw_status("Rename failed.");
                                        }
                                    }
                                    free(new_abs);
                                }
                            }
                        }
                    }
                }
            } else if (next_ch == 'n') { // ALT+n: New File
                if (g_inside_archive_path) {
                    draw_status("Cannot create files inside archives");
                } else {
                    char name_buf[256] = "";
                    int got = prompt_input(name_buf, sizeof(name_buf), "New file name: ");
                    if (got > 0) {
                        char *abs = path_join(current, name_buf);
                        if (abs) {
                            if (path_exists(abs)) {
                                draw_status("File already exists!");
                            } else {
                                int fd = creat(abs, 0644);
                                if (fd >= 0) {
                                    close(fd);
                                    struct stat st;
                                    if (lstat(abs, &st) == 0) {
                                        cache_upsert_with_meta(&cache, root, abs, 0, time(NULL), (unsigned long long)st.st_ino, st.st_mtime);
                                        cache_add_ancestors_after_delta(&cache, root, abs, 0);
                                        g_last_files++;
                                    }
                                    view_free(&dv);
                                    build_dir_view(current, root, &cache, &dv);
                                    for(size_t i=0; i<dv.n; i++) { if (strcmp(dv.v[i].abs_path, abs) == 0) { dv.selected = i; break; } }
                                    if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                                    draw_status("File created.");
                                } else {
                                    draw_status("Failed to create file.");
                                }
                            }
                            free(abs);
                        }
                    }
                }
            } else {
                if (g_preview_focused) {
                    g_preview_focused = 0;
                    draw_status("Preview focus DISABLED");
                }
            }
        } else if (ch == 14) { // Ctrl-n: New Folder
            if (g_inside_archive_path) {
                draw_status("Cannot create folders inside archives");
            } else {
                char name_buf[256] = "";
                int got = prompt_input(name_buf, sizeof(name_buf), "New folder name: ");
                if (got > 0) {
                    char *abs = path_join(current, name_buf);
                    if (abs) {
                        if (path_exists(abs)) {
                            draw_status("Folder already exists!");
                        } else {
                            if (mkdir(abs, 0755) == 0) {
                                struct stat st;
                                if (lstat(abs, &st) == 0) {
                                    cache_upsert_with_meta(&cache, root, abs, 0, time(NULL), (unsigned long long)st.st_ino, st.st_mtime);
                                }
                                view_free(&dv);
                                build_dir_view(current, root, &cache, &dv);
                                for(size_t i=0; i<dv.n; i++) { if (strcmp(dv.v[i].abs_path, abs) == 0) { dv.selected = i; break; } }
                                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                                draw_status("Folder created.");
                            } else {
                                draw_status("Failed to create folder.");
                            }
                        }
                        free(abs);
                    }
                }
            }
        } else if (g_preview_focused) {
            // Internal preview navigation (scrolling)
            if (ch == KEY_UP || ch == 'k') { if (g_preview_scroll_y > 0) g_preview_scroll_y--; }
            else if (ch == KEY_DOWN || ch == 'j') { g_preview_scroll_y++; }
            else if (ch == KEY_LEFT || ch == 'h') { if (g_preview_scroll_x > 0) g_preview_scroll_x--; }
            else if (ch == KEY_RIGHT || ch == 'l') { g_preview_scroll_x++; }
            continue; // Intercept all other keys while focused
        } else if (ch == 5) { // Ctrl-E: edit file
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (ve->is_dir) {
                    draw_status("Cannot edit a directory");
                } else if (g_inside_archive_path) {
                    draw_status("Editing files inside archives not supported");
                } else if (!is_textual_file(ve->abs_path)) {
                    draw_status("Cannot edit binary or non-textual files");
                } else {
                    unsigned long long old_sz = ve->size;
                    launch_external_editor(ve->abs_path);
                    // refresh size after edit
                    struct stat st;
                    if (lstat(ve->abs_path, &st) == 0 && S_ISREG(st.st_mode)) {
                        unsigned long long new_sz = file_size_bytes(&st);
                        if (new_sz != old_sz) {
                            long long delta = (long long)new_sz - (long long)old_sz;
                            cache_upsert_with_meta(&cache, root, ve->abs_path, new_sz, time(NULL), (unsigned long long)st.st_ino, st.st_mtime);
                            if (delta > 0) cache_add_ancestors_after_delta(&cache, root, ve->abs_path, (unsigned long long)delta);
                            else cache_adjust_ancestors_after_delta(&cache, root, ve->abs_path, (long long)(-delta));
                            if (delta >= 0) g_last_bytes += (unsigned long long)delta;
                            else {
                                unsigned long long dec = (unsigned long long)(-delta);
                                if (g_last_bytes > dec) g_last_bytes -= dec; else g_last_bytes = 0ULL;
                            }
                            cache_save(root, &cache);
                            view_free(&dv);
                            build_dir_view(current, root, &cache, &dv);
                            if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                            draw_status("File updated.");
                        }
                    }
                }
            }
        } else if (ch == 'x' || ch == 'X') { // Extract archive
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (!ve->is_dir && is_archive_file(ve->abs_path)) {
                    // Suggest a directory name (strip extension)
                    char name_buf[256];
                    const char *base = path_basename_const(ve->abs_path);
                    strncpy(name_buf, base, sizeof(name_buf) - 1);
                    name_buf[sizeof(name_buf)-1] = '\0';
                    char *dot = strrchr(name_buf, '.');
                    if (dot && dot != name_buf) *dot = '\0';
                    // Special case for .tar.gz etc
                    dot = strrchr(name_buf, '.');
                    if (dot && (strcasecmp(dot, ".tar") == 0)) *dot = '\0';

                    char prompt[128];
                    snprintf(prompt, sizeof(prompt), "Extract to (blank for cwd): ");
                    int got = prompt_input(name_buf, sizeof(name_buf), prompt);
                    if (got >= 0) {
                        char *target_dir = NULL;
                        if (got == 0) target_dir = xstrdup(current);
                        else target_dir = path_join(current, name_buf);

                        if (target_dir) {
                            // Ensure target exists
                            if (got > 0 && !path_exists(target_dir)) {
                                mkdir(target_dir, 0777);
                            }
                            
                            draw_status("Extracting archive..."); refresh();
                            if (archive_extract_to(ve->abs_path, target_dir, root, &cache) == 0) {
                                draw_status("Extraction complete.");
                            } else {
                                draw_status("Extraction failed.");
                            }
                            
                            // Rebuild view and find the new item index
                            view_free(&dv);
                            build_dir_view(current, root, &cache, &dv);
                            size_t new_idx = 0; int found = 0;
                            for (size_t i = 0; i < dv.n; i++) {
                                if (strcmp(dv.v[i].abs_path, target_dir) == 0) {
                                    new_idx = i; found = 1; break;
                                }
                            }
                            if (found) {
                                dv.selected = new_idx;
                                int rows, cols; getmaxyx(stdscr, rows, cols);
                                int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                                if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                                if ((int)dv.selected < top) top = (int)dv.selected;
                            }
                            if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                            free(target_dir);
                        }
                    }
                } else {
                    draw_status("Selected item is not a supported archive.");
                }
            }
        } else if (ch == 'z' || ch == 'Z') { // Zip compression
            if (dv.n > 0) {
                // Determine source items
                char **src_paths = NULL;
                size_t num_src = 0;
                if (g_marks.n > 0) {
                    num_src = g_marks.n;
                    src_paths = malloc(num_src * sizeof(char*));
                    for (size_t i = 0; i < num_src; i++) src_paths[i] = xstrdup(g_marks.paths[i]);
                } else {
                    num_src = 1;
                    src_paths = malloc(num_src * sizeof(char*));
                    src_paths[0] = xstrdup(dv.v[dv.selected].abs_path);
                }

                // Suggest default name
                char name_buf[256];
                if (num_src == 1) {
                    const char *base = path_basename_const(src_paths[0]);
                    strncpy(name_buf, base, sizeof(name_buf) - 5);
                    name_buf[sizeof(name_buf) - 5] = '\0';
                } else {
                    strncpy(name_buf, "archive", sizeof(name_buf) - 5);
                }
                strcat(name_buf, ".zip"); // Add extension by default

                char prompt[128];
                snprintf(prompt, sizeof(prompt), "Zip name: ");
                
                // Now prompt_input will show name_buf as default
                int got = prompt_input(name_buf, sizeof(name_buf), prompt);
                if (got > 0) {
                    // Force .zip extension if user removed it
                    size_t nl = strlen(name_buf);
                    if (nl < 4 || strcasecmp(name_buf + nl - 4, ".zip") != 0) {
                        strncat(name_buf, ".zip", sizeof(name_buf) - nl - 1);
                    }
                    
                    char *dest_path = path_join(current, name_buf);
                    if (dest_path) {
                        int skip = 0;
                        if (path_exists(dest_path)) {
                            char action = prompt_conflict_action(dest_path);
                            if (action == 's' || action == 'S') skip = 1;
                            else if (action == 'r' || action == 'R') {
                                char *new_dest = gen_nonconflicting_path(dest_path);
                                free(dest_path); dest_path = new_dest;
                            }
                        }
                        
                        if (!skip && dest_path) {
                            draw_status("Compressing to ZIP..."); refresh();
                            if (zip_compress_items(src_paths, num_src, dest_path, root) == 0) {
                                draw_status("Compression complete.");
                                if (g_marks.n > 0) markset_clear(&g_marks);
                                
                                // Rebuild view and find the new zip file index
                                view_free(&dv);
                                build_dir_view(current, root, &cache, &dv);
                                size_t new_idx = 0;
                                int found = 0;
                                for (size_t i = 0; i < dv.n; i++) {
                                    if (strcmp(dv.v[i].abs_path, dest_path) == 0) {
                                        new_idx = i; found = 1; break;
                                    }
                                }
                                if (found) {
                                    dv.selected = new_idx;
                                    int rows, cols; getmaxyx(stdscr, rows, cols);
                                    int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                                    if ((int)dv.selected < top) top = (int)dv.selected;
                                }
                                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                            } else {
                                draw_status("Compression cancelled or failed.");
                                view_free(&dv);
                                build_dir_view(current, root, &cache, &dv);
                                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                            }
                        }
                        free(dest_path);
                    }
                }
                for (size_t i = 0; i < num_src; i++) free(src_paths[i]);
                free(src_paths);
            }
        } else if (ch == 19) { // Ctrl-S: Subshell
            if (g_inside_archive_path) {
                draw_status("Subshell not available inside archives");
            } else {
                launch_subshell(current);
                // Rescan current dir in case user modified anything
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
            }
        } else if (ch == 20) { // Ctrl-T: reset all filters
            g_filter_mode = FILTER_ALL;
            g_filter_by_query = 0;
            g_search_query[0] = '\0';
            if (g_regex_enabled) { regfree(&g_regex); g_regex_enabled = 0; }
            view_free(&dv);
            top = 0;
            build_dir_view(current, root, &cache, &dv);
            if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
            draw_status("Filters reset to ALL");
        } else if (ch == 'a' || ch == 'A') {
            if (g_miller_mode) {
                draw_status("Tree View is not compatible with Miller Columns.");
            } else {
                g_tree_mode = !g_tree_mode;
                view_free(&dv);
                top = 0;
                build_dir_view(current, root, &cache, &dv);
            }
        } else if (ch == 'M') {
            if (g_tree_mode) {
                draw_status("Miller Columns are not compatible with Tree View.");
            } else {
                g_miller_mode = !g_miller_mode;
                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
            }
        } else if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                int y_start = g_decorative ? 3 : 1;
                int rows, cols; getmaxyx(stdscr, rows, cols);
                int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                if (event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED)) {
                    if (event.y == 0) {
                        // Check breadcrumbs
                        for (int i = 0; i < g_num_breadcrumbs; i++) {
                            if (event.x >= g_breadcrumbs[i].x_start && event.x < g_breadcrumbs[i].x_end) {
                                if (strcmp(g_breadcrumbs[i].abs_path, current) != 0) {
                                    strncpy(current, g_breadcrumbs[i].abs_path, sizeof(current));
                                    current[sizeof(current)-1] = '\0';
                                    view_free(&dv);
                                    top = 0;
                                    build_dir_view(current, root, &cache, &dv);
                                }
                                break;
                            }
                        }
                    } else if (event.y >= y_start && event.y < y_start + list_rows) {
                        size_t clicked_idx = (size_t)(top + (event.y - y_start));
                        if (clicked_idx < dv.n) {
                            if (clicked_idx == dv.selected || (event.bstate & BUTTON1_DOUBLE_CLICKED)) {
                                ViewEntry *ve = &dv.v[clicked_idx];
                                if (ve->is_dir) {
                                    navstack_push(&nav, current, ve->abs_path, dv.selected, top);
                                    strncpy(current, ve->abs_path, sizeof(current)); current[sizeof(current)-1] = '\0';
                                    view_free(&dv);
                                    top = 0;
                                    build_dir_view(current, root, &cache, &dv);
                                }
                            } else {
                                dv.selected = clicked_idx;
                            }
                        }
                    }
                } else if (event.bstate & (BUTTON3_CLICKED | BUTTON3_PRESSED)) {
                    // Right click: go back
                    ungetch(KEY_LEFT);
                } else if (event.bstate & 0x10000) { // Scroll Up (BUTTON4)
                    if (top > 0) {
                        top--;
                        if ((int)dv.selected >= top + list_rows) dv.selected = (size_t)(top + list_rows - 1);
                    }
                } else if (event.bstate & 0x200000) { // Scroll Down (BUTTON5)
                    if (top + list_rows < (int)dv.n) {
                        top++;
                        if ((int)dv.selected < top) dv.selected = (size_t)top;
                    }
                }
            }
        } else if (ch == 'h' || ch == 'H') {
            show_help();
        } else if (ch == 'M') {
            g_miller_mode = !g_miller_mode;
            if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
        } else if (ch == KEY_UP || ch == 'k') {
            if (dv.selected > 0) {
                dv.selected--;
                g_preview_scroll_y = 0; g_preview_scroll_x = 0;
                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
            }
            if ((int)dv.selected < top) top = (int)dv.selected;
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (dv.selected + 1 < dv.n) {
                dv.selected++;
                g_preview_scroll_y = 0; g_preview_scroll_x = 0;
                if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
            }
            int rows, cols; getmaxyx(stdscr, rows, cols);

int list_rows = rows - 3 - (g_decorative ? 2 : 0);
            if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
        } else if (ch == 10 || ch == KEY_RIGHT || ch == 'l') { // Enter
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (g_inside_archive_path) {
                    if (ve->is_dir) {
                        navstack_push(&nav, g_archive_subpath ? g_archive_subpath : "", ve->abs_path, dv.selected, top);
                        if (g_archive_subpath) {
                            char *next = path_join(g_archive_subpath, ve->name);
                            free(g_archive_subpath); g_archive_subpath = next;
                        } else {
                            g_archive_subpath = xstrdup(ve->name);
                        }
                        view_free(&dv); top = 0;
                        build_dir_view(NULL, root, &cache, &dv);
                    }
                } else if (ve->is_dir) {
                    if (g_tree_mode) {
                        if (ve->expanded) expanded_remove(ve->abs_path);
                        else expanded_add(ve->abs_path);
                        size_t sel = dv.selected; int old_top = top;
                        view_free(&dv);
                        build_dir_view(current, root, &cache, &dv);
                        dv.selected = sel; top = old_top;
                    } else {
                        // Push parent nav state before changing directory
                        navstack_push(&nav, current, ve->abs_path, dv.selected, top);
                        strncpy(current, ve->abs_path, sizeof(current)); current[sizeof(current)-1] = '\0';
                        view_free(&dv);
                        top = 0;
                        build_dir_view(current, root, &cache, &dv);
                        if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                    }
                } else if (g_miller_mode && !ve->is_dir && (ch == KEY_RIGHT || ch == 'l')) {
                    if (is_textual_file(ve->abs_path)) {
                        // Focus preview on file
                        g_preview_focused = 1;
                        g_preview_scroll_y = 0; g_preview_scroll_x = 0;
                        draw_status("Preview focus ENABLED - Use Arrows/hjkl to scroll, ESC to exit");
                    } else if (is_image_file(ve->abs_path)) {
                        draw_status("Image scrolling not supported - Use 'v' for full preview");
                    } else {
                        draw_status("Preview focus not available for this file type");
                    }
                } else if (is_archive_file(ve->abs_path)) {
                    // Enter archive
                    navstack_push(&nav, current, ve->abs_path, dv.selected, top);
                    g_inside_archive_path = xstrdup(ve->abs_path);
                    g_archive_subpath = NULL;
                    view_free(&dv); top = 0;
                    build_dir_view(NULL, root, &cache, &dv);
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
            if (g_inside_archive_path) {
                if (g_archive_subpath == NULL) {
                    // Exit archive entirely
                    free(g_inside_archive_path); g_inside_archive_path = NULL;
                } else {
                    // Go up one level in archive
                    char *p = get_parent(g_archive_subpath);
                    free(g_archive_subpath);
                    if (strcmp(p, "/") == 0) { free(p); g_archive_subpath = NULL; }
                    else g_archive_subpath = p;
                }
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
                // Restore selection/top from nav stack
                NavState st;
                if (navstack_pop(&nav, &st)) {
                    size_t idx = st.selected;
                    for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, st.child_path) == 0) { idx = i; break; } }
                    dv.selected = (dv.n > 0) ? (idx < dv.n ? idx : dv.n - 1) : 0;
                    top = st.top;
                    int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                    if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                    if ((int)dv.selected < top) top = (int)dv.selected;
                    free(st.parent_path); free(st.child_path);
                } else {
                    top = 0;
                }
            } else if (strcmp(current, root) != 0) {
                char *parent = get_parent(current);
                if (parent) {
                    // Move to parent
                    strncpy(current, parent, sizeof(current)); current[sizeof(current)-1] = '\0';
                    free(parent);
                    view_free(&dv);
                    build_dir_view(current, root, &cache, &dv);
                    if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
                    // Restore selection/top from nav stack
                    NavState st;
                    if (navstack_pop(&nav, &st)) {
                        // Prefer to locate the child path we came from
                        size_t idx = st.selected; int found = 0;
                        for (size_t i = 0; i < dv.n; i++) { if (strcmp(dv.v[i].abs_path, st.child_path) == 0) { idx = i; found = 1; break; } }
                        dv.selected = (dv.n > 0) ? (idx < dv.n ? idx : dv.n - 1) : 0;
                        top = st.top;
                        // Ensure visibility
int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3 - (g_decorative ? 2 : 0);
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
                    unsigned long long old_sz = 0ULL; cache_get_info(&cache, ve->abs_path, &old_sz, NULL, NULL);
                    unsigned long long old_files = count_files_path(ve->abs_path);
                    unsigned long long prev_global_bytes = g_last_bytes;
                    unsigned long long prev_global_files = g_last_files;

                    // duplicate scan path to avoid relying on dv memory
                    char *scan_path = xstrdup(ve->abs_path);
                    if (scan_path) {
                        int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
                        if (threads < 1) threads = 1;
                        if (threads > 64) threads = 64;
                        // Execute scan (updates subdirectories in cache)
                        unsigned long long new_sz = scan_dir_parallel_deep(scan_path, cache_abs, &cache, threads);
                        
                        // Explicitly update the scanned directory metadata in cache
                        struct stat st;
                        if (lstat(scan_path, &st) == 0) {
                            cache_upsert_with_meta(&cache, root, scan_path, new_sz, time(NULL), (unsigned long long)st.st_ino, st.st_mtime);
                        }

                        // Compute deltas
                        long long delta_sz = (long long)new_sz - (long long)old_sz;
                        unsigned long long new_files = count_files_path(scan_path);
                        long long delta_fl = (long long)new_files - (long long)old_files;

                        // Restore and adjust global totals
                        g_last_bytes = (unsigned long long)((long long)prev_global_bytes + delta_sz);
                        g_last_files = (unsigned long long)((long long)prev_global_files + delta_fl);

                        // Propagate delta to ancestors in cache
                        if (delta_sz > 0) cache_add_ancestors_after_delta(&cache, root, scan_path, (unsigned long long)delta_sz);
                        else if (delta_sz < 0) cache_adjust_ancestors_after_delta(&cache, root, scan_path, (long long)(-delta_sz));

                        // Persist to disk immediately
                        cache_save(root, &cache);
                        free(scan_path);
                    }
                } else {
                    // Single file refresh logic
                    struct stat st;
                    if (lstat(ve->abs_path, &st) == 0 && S_ISREG(st.st_mode)) {
                        unsigned long long old_sz = ve->size;
                        unsigned long long new_sz = file_size_bytes(&st);
                        long long delta = (long long)new_sz - (long long)old_sz;
                        if (delta != 0) {
                            cache_upsert_with_meta(&cache, root, ve->abs_path, new_sz, time(NULL), (unsigned long long)st.st_ino, st.st_mtime);
                            if (delta > 0) cache_add_ancestors_after_delta(&cache, root, ve->abs_path, (unsigned long long)delta);
                            else cache_adjust_ancestors_after_delta(&cache, root, ve->abs_path, (long long)(-delta));
                            g_last_bytes = (unsigned long long)((long long)g_last_bytes + delta);
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
                int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                if ((int)dv.selected < top) top = (int)dv.selected;
                if (sel_path) free(sel_path);
            }
        } else if (ch == 'R') {
            // Full rescan of current directory (parallel)
            char *scan_path = xstrdup(current);
            if (scan_path) {
                int threads = jobs_override > 0 ? jobs_override : (int)sysconf(_SC_NPROCESSORS_ONLN);
                if (threads < 1) threads = 1;
                if (threads > 64) threads = 64;
                (void)scan_dir_parallel_deep(scan_path, cache_abs, &cache, threads);
                free(scan_path);
                
                // Update global totals if we scanned from root or adjust current state
                unsigned long long new_root_sz = 0ULL;
                if (cache_get_info(&cache, root, &new_root_sz, NULL, NULL)) {
                    g_last_bytes = new_root_sz;
                }
                cache_save(root, &cache);
            }
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
        } else if (ch == '/') { // Global Search
            char q[256] = "";
            int got = prompt_input(q, sizeof(q), "Global Search (entire cache): ");
            if (got > 0) {
                perform_global_search(&cache, q);
                if (g_search_results_count > 0) {
                    g_global_search_mode = 1;
                } else {
                    draw_status("No matches found in cache.");
                }
            }
        } else if (ch == 'f') {
            char q[256];
            int got = prompt_input(q, sizeof(q), "Find: ");
            if (got > 0) {
                strncpy(g_search_query, q, sizeof(g_search_query)); g_search_query[sizeof(g_search_query)-1]='\0';
                if (dv.n > 0) {
                    size_t start = (dv.selected < dv.n) ? dv.selected : 0;
                    int found = -1;
                    for (size_t off = 1; off <= dv.n; off++) {
                        size_t idx = (start + off) % dv.n;
                        if (strcasestr_bool(dv.v[idx].name, g_search_query)) { found = (int)idx; break; }
                    }
                    if (found >= 0) {
                        dv.selected = (size_t)found;
                        int rows, cols; getmaxyx(stdscr, rows, cols);
                        int list_rows = rows - 3 - (g_decorative ? 2 : 0);
                        if ((int)dv.selected >= top + list_rows) top = (int)dv.selected - list_rows + 1;
                        if ((int)dv.selected < top) top = (int)dv.selected;
                    } else draw_status("Nothing Found");
                }
            } else if (got == 0) {
                g_search_query[0] = '\0';
                draw_status("Search query cleared");
            }
        } else if (ch == 'F') {
            char q[256];
            int got = prompt_input(q, sizeof(q), "Regex (case-insensitive): ");
            if (got == 0) {
                g_regex_enabled = 0;
                g_filter_by_query = 0;
                view_free(&dv); build_dir_view(current, root, &cache, &dv);
                draw_status("Regex filter disabled");
            } else if (got > 0) {
                regex_t re;
                int rc = regcomp(&re, q, REG_ICASE | REG_NOSUB);
                if (rc != 0) draw_status("Regex invalid");
                else {
                    if (g_regex_enabled) regfree(&g_regex);
                    g_regex = re; g_regex_enabled = 1;
                    g_filter_by_query = 1;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (g_miller_mode) update_miller_columns(current, root, &cache, &dv);
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
int list_rows = rows - 3 - (g_decorative ? 2 : 0);
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
                    if (is_textual_file(ve->abs_path) || is_image_file(ve->abs_path)) {
                        show_preview(ve->abs_path);
                    } else {
                        draw_status("Not a supported preview format (text/image)");
                    }
                }
            }
        } else if (ch == 'E') {
            show_extension_view(&dv);
        } else if (ch == 'U') {
            if (g_inside_archive_path) {
                draw_status("Duplicate finder is not supported inside archives.");
            } else {
                show_duplicates_view(current, &cache, cache_abs);
                // rebuild view in case duplicates were deleted
                view_free(&dv);
                build_dir_view(current, root, &cache, &dv);
            }
        } else if (ch == 'O' || ch == 15) { // Shift+O or Ctrl+O
            if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                open_with_default(ve->abs_path);
            }
        } else if (ch == 'o') {
            // cycle sort key (size -> name -> mtime -> extension [-> delta] -> size)
            SortMode next_mode;
            if (g_sort_mode == SORT_SIZE) next_mode = SORT_NAME;
            else if (g_sort_mode == SORT_NAME) next_mode = SORT_MTIME;
            else if (g_sort_mode == SORT_MTIME) next_mode = SORT_EXT;
            else if (g_sort_mode == SORT_EXT) next_mode = g_diff_mode ? SORT_DELTA : SORT_SIZE;
            else next_mode = SORT_SIZE; // back from delta

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
        } else if (ch == '\t' || ch == 9) {
            if (g_miller_mode) {
                draw_status("Graph Bar is not available in Miller Columns mode.");
            } else {
                g_show_graph = !g_show_graph;
            }
        } else if (ch == 'I') {
            // cycle left size column display: numeric -> percentage -> off -> numeric
            g_display_mode = (g_display_mode == DISP_NUM) ? DISP_PCT : (g_display_mode == DISP_PCT ? DISP_OFF : DISP_NUM);
        } else if (ch == 'Y') {
            if (g_diff_mode) {
                g_diff_mode = 0;
                cache_free(&g_snapshot_cache);
                draw_status("Diff mode DISABLED");
            } else {
                cache_copy(&g_snapshot_cache, &cache);
                g_diff_mode = 1;
                draw_status("Snapshot taken! Diff mode ENABLED relative to this moment.");
            }
            // rebuild view to update deltas
            view_free(&dv);
            build_dir_view(current, root, &cache, &dv);
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
int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3 - (g_decorative ? 2 : 0);
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
int rows, cols; getmaxyx(stdscr, rows, cols); int list_rows = rows - 3 - (g_decorative ? 2 : 0);
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
        } else if (ch == 'K') {
            cycle_theme();
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
                        if (lstat(src,&st)==0){ if (S_ISDIR(st.st_mode)){ is_dir=1; if(!cache_get_info(&cache,src,&delta,NULL,NULL)) delta=scan_dir_recursive(src, root, cache_abs, &cache, NULL);} else if (S_ISREG(st.st_mode)) delta=(unsigned long long)st.st_size; }
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
                    if (list) {
                        for(size_t i=0;i<n;i++) {
                            free(list[i]);
                        }
                        free(list);
                    }
                    // Refresh view and restore selection
                    size_t old_index = dv.selected;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
                    draw_status("Move completed.");
                } else {
                    if (list) {
                        for(size_t i=0;i<n;i++) {
                            free(list[i]);
                        }
                        free(list);
                    }
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
                            unsigned long long dsz = 0ULL;
                            if (!cache_get_info(&cache, dst, &dsz, NULL, NULL)) dsz = sum_path_size(dst);
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
                    if (list) {
                        for (size_t i = 0; i < n; i++) {
                            free(list[i]);
                        }
                        free(list);
                    }
                    // Refresh view and restore selection
                    size_t old_index = dv.selected;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
                    draw_status("Copy completed.");
                } else {
                    if (list) {
                        for (size_t i = 0; i < n; i++) {
                            free(list[i]);
                        }
                        free(list);
                    }
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
                        if (lstat(p,&st)==0){ if (S_ISDIR(st.st_mode)){ is_dir=1; if(!cache_get_info(&cache,p,&delta,NULL,NULL)) delta=scan_dir_recursive(p, root, cache_abs, &cache, NULL);} else if (S_ISREG(st.st_mode)) delta=(unsigned long long)st.st_size; }
                        unsigned long long files_dec = count_files_path(p);
                        // delete
                        if (remove_tree(p, cache_abs)==0){ if (is_dir) cache_remove_prefix(&cache,p); cache_adjust_ancestors_after_delta(&cache, root, p, (long long)delta); if (g_last_files > files_dec) g_last_files -= files_dec; else g_last_files = 0ULL; }
                    }
                    cache_save(root,&cache);
                    // clear marks that were deleted
                    if (list) {
                        for(size_t i=0;i<n;i++) {
                            markset_remove(&g_marks, list[i]);
                            free(list[i]);
                        }
                        free(list);
                    }
                    // refresh view and restore selection
                    size_t old_index = dv.selected;
                    view_free(&dv); build_dir_view(current, root, &cache, &dv);
                    if (dv.n > 0) dv.selected = (old_index < dv.n ? old_index : dv.n - 1);
                    draw_status("Delete completed.");
                } else {
                    if (list) {
                        for(size_t i=0;i<n;i++) free(list[i]);
                        free(list);
                    }
                }
            } else if (dv.n > 0) {
                ViewEntry *ve = &dv.v[dv.selected];
                if (confirm_delete_prompt(ve->name, ve->is_dir)) {
                    // Calcola delta prima di rimuovere
                    struct stat st_before;
                    int exists_before = (lstat(ve->abs_path, &st_before) == 0);
                    unsigned long long delta = 0ULL;
                    unsigned long long files_dec = 0ULL;
                    int is_dir = ve->is_dir;
                    if (exists_before) {
                        files_dec = count_files_path(ve->abs_path);
                        if (!is_dir && S_ISREG(st_before.st_mode)) {
                            delta = (unsigned long long)st_before.st_size;
                        } else if (is_dir) {
                            if (!cache_get_info(&cache, ve->abs_path, &delta, NULL, NULL)) {
                                // Stima dimensione della dir da eliminare (solo il subtree target)
                                delta = scan_dir_recursive(ve->abs_path, root, cache_abs, &cache, NULL);
                            }
                        }
                    } else {
                        draw_status("Error: File or directory no longer exists.");
                        refresh();
                        napms(1000);
                    }

                    // Elimina
                    size_t old_index = dv.selected;
                    int rc = remove_tree(ve->abs_path, cache_abs);
                    if (rc == 0) {
                        if (is_dir) cache_remove_prefix(&cache, ve->abs_path);
                        cache_adjust_ancestors_after_delta(&cache, root, ve->abs_path, (long long)delta);
                        if (g_last_bytes > delta) g_last_bytes -= delta; else g_last_bytes = 0ULL;
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
    
    // FINAL DUMP: Overwrite cache file with current memory state
    if (cache_save(root, &cache) == 0) {
        if (debug_all) fprintf(stderr, "[cleanup] Cache saved successfully to %s\n", root);
    } else {
        fprintf(stderr, "[cleanup] Error saving cache to %s\n", root);
    }

    cache_free(&cache);
    if (g_diff_mode) cache_free(&g_snapshot_cache);
    if (g_regex_enabled) { regfree(&g_regex); g_regex_enabled = 0; }
    free(cache_abs);
    exclude_free();
    return 0;
}
