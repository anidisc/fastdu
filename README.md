# fastdu

A fast terminal UI (ncurses) disk-usage explorer written in C, with parallel scanning and an on-disk cache.

- Current version: 0.41.0
- License: MIT (suggested; adjust if different)

---

## Highlights

- **Ultra-Fluid Scrolling**: Optimized TUI rendering engine with cached metrics, providing instant response even in directories with 100,000+ files.
- **Sharded Cache**: Performance-optimized cache with 64 independent locks to minimize thread contention.
- **Hard Link Detection**: Accurate disk usage calculation by counting unique (dev, ino) pairs only once.
- **Iterative Operations**: Tree removal and copying are now iterative, supporting extremely deep directory structures without stack overflow.
- **Advanced Exclusions**: Support for `.fastduignore` files and `--exclude` CLI flag.
- **Extension Analysis**: Dedicated view to analyze space distribution by file extension (Press `E`).
- **Data Export**: Export scan results to JSON or CSV formats for external analysis.
- **System Integration**: Open files/folders directly with the system default application (Press `O`).
- **Mount-point Awareness**: Option to stay on a single filesystem (`-x` / `--one-file-system`).
- **Robust TUI**: Improved visibility with occupation bars, smart name truncation, and dynamic resize support.

---

## What’s new in 0.41.0

- **Rendering Optimization**: Pre-calculated column widths and view totals to eliminate lag during scrolling.
- **Improved Hover Checks**: `maybe_rescan_hovered` now uses parallel scanning and only triggers a redraw if changes are detected.
- **TUI Controls**:
  - `TAB / Ctrl-i`: Toggle the graphical occupation bar.
- **Bugfixes**: Removed redundant draw calls in the main loop to save CPU cycles.

## What’s new in 0.40.0

- **Hard Link Tracking**: Implemented a global `InodeSet` to prevent double-counting of hard-linked files.
- **Sharded Cache Architecture**: Refactored the internal cache to use 64 shards, significantly improving scan speed on high-core systems.
- **Stability & Depth**: Replaced recursive tree traversal in `delete` and `copy` operations with iterative logic using dynamic task lists.
- **Rich TUI Features**:
  - **Occupation Bars**: Visual `[####------]` bars showing relative size.
  - **Smart Truncation**: Names are elegantly truncated with `...` on small terminals.
  - **Extension View**: Press `E` to see which file types consume the most space.
  - **External Open**: Press `O` to launch the selected item with `xdg-open`.
- **Exclusion System**: Full support for `.fastduignore` (root-based) and `-e` / `--exclude` CLI patterns.
- **CLI Power-ups**:
  - `--export json|csv <file>`: Dump the entire cache to structured data.
  - `-x, --one-file-system`: Prevent crossing filesystem boundaries.
- **Reliability**:
  - Graceful `SIGINT` (Ctrl+C) handling to stop operations without data loss.
  - Fixed multiple memory leaks in headless and error paths.
  - Refactored `cache_load` for better readability and progress feedback.

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

# Scan with exclusions and stay on one filesystem
./fastdu -x -e node_modules -e .git /home/user

# Export scan results to JSON
./fastdu --export json report.json /data

# Full rescan with 8 workers
./fastdu -R -j 8 /data
```

### Options

- `-R, --reload`         Force cache rebuild (ignore existing cache)
- `-H, --headless`       Force headless (non-TUI) mode
- `-ac, --accuracy`      Accurate disk usage: force deep rescan and use allocated blocks
- `-x, --one-file-system` Stay on the same file system (do not cross mount points)
- `-e PAT, --exclude PAT` Exclude files/dirs matching exact PAT
- `--export FMT FILE`    Export results to FILE in FMT (json|csv) and exit
- `-D, --decorative`     Decorative UI (column headers, vertical separator, extra colors)
- `-j N, --jobs N`       Number of worker threads (default: online CPUs, max 64)
- `-v, --version`        Show version and exit
- `-h, --help`           Show help and exit

---

## TUI cheat sheet

- **Navigation**: Up/Down or j/k; Enter/Right (l) to enter; Backspace/Left to go up; `b`/`e` to jump to first/last
- **Views**:
  - `E`: Open Extension Distribution view
  - `v`: Preview selected text file
  - `O`: Open selected item with system default (`xdg-open`)
- **Sorting**: `o` toggles sort key (size → name → mtime), `s` toggles order (asc/desc)
- **Display**:
  - `I`: Cycles size column (numeric → percent → hidden)
  - `i`: Cycles info column (mtime → owner+perm → hidden)
- **Filters**: `t` cycles type filter (all/dirs/files); `T` toggles the query filter
- **Search**: `f` substring (case-insensitive); `F` regex; `n`/`N` next/prev match
- **Rescan**: `r` rescan selected dir, `R` rescan current dir (parallel)
- **Marks & Operations**:
  - `Space`: Toggle explicit mark on item
  - `Ctrl-A`: Select/deselect all in view
  - `m` move marked; `c` copy marked; `d` delete marked (if any) otherwise delete selected
- **Help/Quit**: `h` help; `q` quit

---

## Cache format

- Stored at the scan root as `.fastdu_cache_v2`
- Paths are percent-encoded to keep TSVs safe.
- Version 3 supports global totals and inode tracking for fast invalidation.

---

## License

MIT
