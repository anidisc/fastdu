# fastdu

A fast terminal UI (ncurses) disk-usage explorer written in C, with parallel scanning and an on-disk cache.

Status: 0.25.0

Features
- Parallel scanning: deep work-queue with multiple threads (pthreads)
- Responsive TUI: sortable by size, name or last modified (mtime) with asc/desc; incremental search (f, n/N); filters (all/dirs/files and by query); preserved selection on back
- Right-aligned Last Modified column: shows YYYY-MM-DD HH:MM anchored to the right edge
- Regex search: press F to enter a case-insensitive regex; use T to toggle the filter on/off (matching applies to entry names)
- Persistent cache: saves scan results to .fastdu_cache_v2 at the root for reuse (stores path, size, last_scan, inode, dir_mtime)
- Targeted refresh: automatic rescan of the hovered directory when modified (mtime)
- Operations with marks: Space to mark/unmark; move (m), copy (c), delete (d) with incremental cache updates
- Marks auto-clear after copy/move to avoid repeated or accidental operations
- Copy/Move enhancements: conflict handling (overwrite/rename/skip) with apply-to-all (O/R/S), EXDEV fallback for move (copy+unlink) with progress bar
- Progress bars: scanning (entries/tasks) and copy/move bytes progress
- Footer info: totals (files/size), marked count, and last search query

Requirements
- gcc (or a C11-compatible compiler)
- ncurses with wide-char support (linked as -lncursesw)
- pthreads

Install (Fedora/RHEL)
- sudo dnf install gcc make ncurses-devel

Build
- make

Run
- ./fastdu [options] [path]

Options
- -R, --reload        force cache rebuild
- -j N, --jobs N      set number of threads (default: online CPUs, max 64)
- -v, --version       print version and exit
- -h, --help          print CLI usage and exit

TUI keys (highlights)
- Navigation: Up/Down or j/k; Enter/Right(l) to enter; Backspace/Left to go up; b/e to jump to top/end
- View: o toggles sort key (size/name/mtime); s toggles order (asc/desc)
- Filters: t cycles all/dirs/files; T toggles filter-by-query (uses last f/F query)
- Search: f substring prompt (case-insensitive), F regex prompt (case-insensitive), n/N next/previous match (wraps around)
- Rescan: r rescan selected dir, R rescan current dir (parallel)
- Marks/ops: Space mark/unmark; m move marked; c copy marked; d delete marked (if any) else selected
  - After successful copy/move, marked items are cleared automatically
  - During copy/move conflicts: o overwrite, r rename (with suffix), s skip; or O/R/S to apply to all
- Help/quit: h help, q quit

Cache
- The .fastdu_cache_v2 file is stored at the scan root directory.
- Each entry includes a percent-encoded relative path, size, scan timestamp, inode and mtime.
- The cache is updated/invalidated when changes are detected (mtime differs) or after operations (move/copy/delete).

Progress
- Scan: shows entries visited, active/pending tasks, files counted and bytes summed
- Copy/Move (fallback copy): shows bytes copied over total with percentage

Design notes
- FS safety: does not follow symlinks (uses O_NOFOLLOW/fstatat) and skips the cache file size in totals.
- Concurrency: mutex-protected cache + atomic counters for progress/metrics.
- UI: throttled status/progress updates to reduce flicker; selection/top restored when returning from subdirs.

Code layout
- fastdu.c    main source (util, cache, scanner, TUI, work-queue, ops)
- Makefile    build rules (gcc -O2 -Wall -Wextra -std=c11 -lncursesw -lpthread)

Examples
- Start on current path: ./fastdu
- Full reload with 8 workers: ./fastdu -R -j 8 /path/to/scan
- Print version: ./fastdu -v
- CLI help: ./fastdu -h

Regex usage in TUI
- Press F to enter a regex (case-insensitive) and enable the query filter. Press T to toggle it on/off.
- Matching applies to entry names only (not the full path).
- Examples:
  - TXT files: \.txt$
  - Only directories named src or docs: ^(src|docs)$ (then press t to switch to dirs)
  - Names containing 2024-: 2024-

Known limitations
- Hard links to the same inode are not deduplicated across directories.
- Reported size is the sum of regular file sizes (not block disk usage).

License
- TBD (add your preferred license, e.g. MIT)
