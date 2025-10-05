# fastdu

A fast terminal UI (ncurses) disk-usage explorer written in C, with parallel scanning and an on-disk cache.

- Current version: 0.30
- License: MIT (suggested; adjust if different)

---

## Highlights

- Parallel scanning (deep work-queue, pthreads)
- On-disk cache per root (.fastdu_cache_v2)
- Responsive TUI: sort by size/name/mtime; live search; type filters; regex filter
- Incremental updates on move/copy/delete (delta propagation in cache and UI)
- Robust progress bars (scan/copy), stall resilience on large trees
- Headless mode for scripts and CI (new)
- Safer marks semantics and clearer UI (new)

---

## What’s new in 0.30

- Accurate global totals across selective rescans (delta-based updates of bytes and files)
- Global files count persisted in cache and used for footer/summary
- Headless mode: `--headless` flag and `FASTDU_HEADLESS=1` env override
- Non-blocking “marked files” in footer via background counter; no traversal in render loop
- Mark semantics:
  - Marking a directory implicitly marks all its contents (displayed as `+`)
  - Only explicit marks show `*`
  - Items covered by a parent mark cannot be unmarked individually (prevents orphaned operations)
- Stall robustness in deep scans:
  - Fixed a bug where a task could be left pending in early error paths
  - Finalizer queue uses timed push with inline finalize fallback to avoid deadlocks
- Debuggability:
  - Cache-load progress logging (`FASTDU_DEBUG_CACHE=1`)
  - Scan stall heartbeat (`FASTDU_DEBUG_SCAN=1`)
  - Headless summary mirrored to stderr when debugging

---

## Requirements

- gcc (or another C11-compatible compiler)
- ncurses with wide-char support (`-lncursesw`)
- pthreads

On Fedora/RHEL:

```bash
sudo dnf install -y gcc make ncurses-devel
```

---

## Build

```bash
make
```

Produces the `./fastdu` binary.

---

## Usage

```bash
./fastdu [options] [path]
```

### Common examples

```bash
# Open TUI on current directory
./fastdu

# Full rescan with 8 workers
./fastdu -R -j 8 /data

# Print version and exit
./fastdu -v

# Headless summary (no TUI; good for scripts)
./fastdu --headless /mnt/volume
```

### Options

- `-R, --reload`  Force cache rebuild (ignore existing cache)
- `-H, --headless` Force headless (non-TUI) mode
- `-j N, --jobs N` Number of worker threads (default: online CPUs, max 64)
- `-v, --version`  Show version and exit
- `-h, --help`     Show help and exit

### Environment variables (advanced)

- `FASTDU_HEADLESS=1`  Force headless mode
- `FASTDU_DEBUG_SCAN=1`  Print a heartbeat when scanning makes no progress (debug)
- `FASTDU_DEBUG_CACHE=1` Print cache-load progress (debug)
- `FASTDU_DEBUG=1`       Extra milestones in headless path (debug)

> Headless mode prints a concise summary (root, files, size). When a cache exists, totals come from the cache and no heavy traversal is performed.

---

## TUI cheat sheet

- Navigation: Up/Down or j/k; Enter/Right (l) to enter; Backspace/Left to go up; `b`/`e` to jump to first/last
- Sorting: `o` toggles sort key (size → name → mtime), `s` toggles order (asc/desc)
- Size column: `I` cycles numeric → percent → hidden
- Info column: `i` cycles mtime → owner+perm → hidden
- Filters: `t` cycles all/dirs/files; `T` toggles the query filter
- Search: `f` substring (case-insensitive); `F` regex; `n`/`N` next/prev match
- Rescan: `r` rescan selected dir, `R` rescan current dir (parallel)
- Marks & operations:
  - Space: toggle explicit mark on item (directories marked explicitly show `*`)
  - Inherited marks: items under a marked directory show `+` and cannot be individually unmarked
  - `Ctrl-A`: select/deselect all in view
  - `m` move marked; `c` copy marked; `d` delete marked (if any) otherwise delete selected
  - Conflicts during copy/move: `o` overwrite, `r` rename, `s` skip; `O`/`R`/`S` applies to all
- Help/quit: `h` help; `q` quit

---

## Cache format

- Stored at the scan root as `.fastdu_cache_v2`
- Header + totals (v3):
  - `# fastdu-cache v3`
  - `root\t<abs_root>`
  - `totals\t<bytes>`
  - `totals_files\t<count>`
- Entries (one per directory):
  - `D\t<rel_path_pct>\t<size>\t<last_scan_epoch>\t<ino>\t<dir_mtime>`
- Paths are percent-encoded to keep TSVs safe

The program uses cache totals when available; selective rescans adjust ancestor sizes and global totals by delta.

---

## Troubleshooting

- “Seems stuck after cache read” in headless:
  - Ensure you’re using `--headless` (or set `FASTDU_HEADLESS=1`) and a recent build (0.30 or later). In headless, no full traversal is performed to compute file counts.
  - For diagnostics: set `FASTDU_DEBUG_CACHE=1` (to see cache read progress) and `FASTDU_DEBUG=1` (to see milestones); output is mirrored on stderr.
- Long scans on huge trees:
  - Use fewer threads on very constrained filesystems if I/O saturates
  - On remote mounts, prefer headless+cache reuse
  - If progress appears stalled, set `FASTDU_DEBUG_SCAN=1` to print a heartbeat (active workers, waitgroup, queue sizes)

---

## Performance notes

- The renderer avoids filesystem traversal in the render loop; marked totals rely on the cache when possible, and background workers compute “marked files” asynchronously.
- The work-queue has bounded capacity with timed push and inline finalize fallback to remain responsive under extreme fan-out.

---

## Code layout

```
fastdu.c    # main source (util, cache, scanner, TUI, work-queue, operations)
Makefile    # build rules (gcc -O2 -Wall -Wextra -std=c11 -lncursesw -lpthread)
README.md   # this file
index.html  # optional landing page (static)
```

---

## Contributing

Issues, suggestions and PRs are welcome. For bug reports on scanning, please include:

- Command used (include flags)
- Whether a cache existed; if yes, attach the first 50 and last 50 lines of the cache file (redact paths if needed)
- Output with `FASTDU_DEBUG_SCAN=1` and/or `FASTDU_DEBUG_CACHE=1`

---

## License

MIT (or your preferred license)
