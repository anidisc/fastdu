# fastdu

A fast terminal UI (ncurses) disk-usage explorer written in C, with parallel scanning and an on-disk cache.

Features
- Parallel scanning: deep work-queue with multiple threads (pthreads)
- Responsive TUI: sortable by size or name, incremental search, filters (all/dirs/files)
- Persistent cache: saves scan results to .fastdu_cache_v2 at the root for reuse
- Targeted refresh: automatic rescan of the hovered directory when modified (mtime)
- Operations: multi-select (mark), move, and delete with incremental cache updates

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
- -R, --reload    force cache rebuild
- -j N, --jobs N  set number of threads (default: online CPUs, max 64)

TUI keys (highlights)
- Navigation: arrows or j/k up/down, Enter/Right l to enter, Backspace/Left to go up
- View: o toggles sort key (size/name), s toggles order (asc/desc)
- Filters: t cycles all/dirs/files, T toggles filter-by-query
- Search: f prompt, n/N next/previous match
- Rescan: r rescan selected dir, R rescan current dir
- Multi-select: Space mark/unmark; m move marked into current dir
- Delete: d delete marked (if any) or the selected entry
- Help/quit: h help, q quit

Cache
- The .fastdu_cache_v2 file is stored at the scan root directory.
- Each entry includes a percent-encoded relative path, size, scan timestamp, inode and mtime.
- The cache is updated/invalidated when changes are detected (mtime differs) or after operations (move/delete).

Design notes
- FS safety: does not follow symlinks (uses O_NOFOLLOW/fstatat) and skips the cache file size in totals.
- Concurrency: mutex-protected cache + atomic counters for progress/metrics.
- UI: throttled status/progress updates to reduce flicker.

Code layout
- fastdu.c    main source (util, cache, scanner, TUI, work-queue)
- Makefile    build rules (gcc -O2 -Wall -Wextra -std=c11 -lncursesw -lpthread)

Examples
- Start on current path: ./fastdu
- Force rescan with 8 threads: ./fastdu -R -j 8 /path/to/scan

Known limitations
- Hard links to the same inode are not deduplicated across directories.
- Reported size is the sum of regular file sizes (not block disk usage).

License
- TBD (add your preferred license, e.g. MIT)
