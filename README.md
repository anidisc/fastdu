# fastdu

A fast terminal UI (ncurses) disk-usage explorer written in C, with parallel scanning and an on-disk cache.

- **Current version: 0.45.0**
- **License: MIT**

---

## Highlights

- **Full Mouse Support**: Selection, double-click to enter, right-click to go back, and scroll wheel support.
- **Navigable Breadcrumbs**: Clickable path in the header for instant navigation to parent directories.
- **Nerd Fonts Integration**: Visual icons for folders and files (requires a Nerd Font and `-nf` flag).
- **Customizable TUI**: Colors and themes via `~/.config/fastdu/config.toml`.
- **Instant Cache Loading**: Eliminated redundant disk walks during cache loading. Even massive cache files are now parsed instantly.
- **Cache Progress Bar**: Visual feedback during the cache loading phase, showing real-time progress for large datasets.
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

## What’s new in 0.45.0 (UX & Customization)

- **Interactive UI**: Added full mouse support (scroll, selection, navigation).
- **Navigation Breadcrumbs**: The current path in the header is now decomposed into clickable segments.
- **Nerd Fonts Support**: Added gliphs/icons for directories and file types (CLI: `-nf`).
- **TOML Configuration**: Support for a local configuration file to customize UI colors.
- **Version Bump**: Major UX improvements consolidated into this release.

## What’s new in 0.44.0

- **UI Refinement**: Removed redundant redraw calls in the main loop for smoother interaction.
- **Improved Deletion UX**: After deleting an item, the selection logic is smarter (points to the next item or the new last item), keeping the navigation flow uninterrupted.
- **Memory Safety**: Added explicit validation before freeing memory in bulk move/copy/delete operations.
- **Robust Error Handling**: Added proper checks for `lstat` failures during deletion with user feedback.

---

## Configuration

You can customize **fastdu** colors by creating a file at `~/.config/fastdu/config.toml`.

```toml
# Supported colors: black, red, green, yellow, blue, magenta, cyan, white

# Header/Footer bar
header_fg = "black"
header_bg = "blue"

# Separators and icons
accent_fg = "blue"

# Entry names
dir_fg = "cyan"
file_fg = "white"

# Size thresholds
size_s_fg = "green"   # Small files
size_m_fg = "yellow"  # Medium files
size_l_fg = "red"     # Large files
```

---

## Requirements

- gcc (or another C11-compatible compiler)
- ncurses with wide-char support (`-lncursesw`)
- pthreads

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

### Options

- `-nf, --nerd-fonts`    Enable Nerd Fonts icons support
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

- **Mouse**: Scroll wheel; Left-click (select/open); Right-click (back); Click header path to jump.
- **Navigation**: Up/Down or j/k; Enter/Right (l) to enter; Backspace/Left to go up; `b`/`e` to jump to first/last
- **Views**:
  - `E`: Open Extension Distribution view
  - `v`: Preview selected text file (Toggle wrap: `w`)
  - `O`: Open selected item with system default (`xdg-open`)
- **Sorting**: `o` toggles sort key (size → name → mtime), `s` toggles order (asc/desc)
- **Display**:
  - `I`: Cycles size column (numeric → percent → hidden)
  - `i`: Cycles info column (mtime → owner+perm → hidden)
- **Filters**: `t` cycles type filter (all/dirs/files); `T` toggles the query filter
- **Search**: `f` substring (case-insensitive); `F` regex; `n`/`N` next/prev match
- **Help/Quit**: `h` help; `q` quit

---

## License

MIT
