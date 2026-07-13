# Complete Guide and Analysis of qdux

`qdux` is an extremely fast terminal disk-usage analyzer written in C11 and based on **ncurses** for the TUI (Terminal User Interface) and **pthreads** for parallel scanning.

This document provides a detailed analysis of the internal mechanics of the source code ([qdux.c](file:///home/anidisc/git-source/qdux/qdux.c)) and a complete user guide for all the application features.

---

## 1. Functional Analysis of the Code (`qdux.c`)

The program architecture is divided into the following main functional modules:

### 1.1 Sharded Cache System
To speed up subsequent scans, `qdux` saves the scan state in a file named `.qdux_cache_v3` in the main folder.
* **Sharding for Concurrency:** To reduce lock contention among worker threads during parallel scans, the in-memory cache is split into **64 shards** (`CACHE_SHARDS`). Each shard is managed independently and protected by a dedicated mutex (`pthread_mutex_t mu`).
* **Modification Detection:** Each record stores the inode (`ino`) and modification time (`mtime`). If the directory modification time on disk does not match the cached value, the corresponding subtree is rescanned, while unchanged portions are reused instantly.

### 1.2 Parallel Scan Engine (BFS)
Parallel scanning is implemented in the `scan_dir_parallel_deep` function:
* **Task Queue:** It uses a task queue at the directory level (`TaskQueue`), where every directory to be scanned is queued as a single task (`DirTask`).
* **Worker & Finalizer Loop:** A worker thread pool (`worker_loop`) pops directories from the queue, collects file sizes, and queues any subdirectories found. Once all descendants of a directory are scanned, the finalizer thread (`finalizer_loop`) computes the overall total size and updates the cache.
* **Asynchronous I/O with `io_uring`:** On compatible Linux systems, `qdux` can leverage `io_uring` to perform asynchronous stat calls (`statx`), maximizing I/O performance on fast SSDs.

### 1.3 User Interface (Ncurses TUI)
The terminal interface offers several display modes:
* **Tree View:** Expands and collapses nodes while keeping the indentation hierarchy.
* **Miller Columns (Ranger-style):** Classic side-by-side view (Parent Directory $\rightarrow$ Current Selection $\rightarrow$ Content Preview).
* **Theme & Color Management:** Flexible customization of colors and preconfigured themes (e.g. Dracula, TokyoNight, Light, Pastel).
* **Multiline Footer & Adaptive Layout:** Long status messages and input prompts automatically wrap to multiple lines. The TUI dynamically shifts the footer bar up and shrinks the list view height to adapt, returning to the default single-line state once dismissed.

### 1.4 Archive Management
Integrated with `libarchive` to compress selected files/folders into `.zip` format (`zip_compress_items`) and extract archives in-place (`archive_extract_to`). During extraction or compression, the status bar shows real-time progress by reading the compressed file descriptor via `lseek`.

### 1.5 Remote Client-Server Protocol
One of the advanced features of `qdux` is the ability to explore remote filesystems over SSH:
* **Server Mode (`--server`):** Runs `qdux` on the remote machine, sending the filesystem state over `stdout` in a TSV text format, and executing remote commands.
* **Client Mode (`--connect URI`):** Connects the local TUI to a server via a persistent SSH session configured with connection multiplexing to minimize latency.

---

## 2. Usage and CLI Options

### 2.1 Command Line Syntax
```bash
qdux [options] [path]
```

#### Main Options:
* `-h, --help`: Show help and exit.
* `-v, --version`: Show current version.
* `-R, --reload`: Ignore cache and perform a full parallel scan from scratch.
* `-j N, --jobs N`: Set the number of worker threads (Default: CPU count, max 64).
* `-x, --one-file-system`: Stay on the same file system (do not cross mount points).
* `-e PAT, --exclude PAT`: Exclude files/folders matching the exact pattern `PAT`.
* `--diff FILE`: Compare current directory with a snapshot cache FILE.
* `--export FMT FILE`: Export results in `json` or `csv` format to the specified path.
* `-D, --decorative`: Enable decorative UI (borders, column headers, extra separators).
* `-nf, --nerd-fonts`: Enable Nerd Fonts icons rendering (requires a compatible font in your terminal).
* `--connect URI`: Connect to a remote server via the specified URI (format `user@host:/path`).

---

## 3. TUI Keyboard Shortcuts

The interactive interface supports the following keyboard commands:

### Navigation
| Key | Action |
|---|---|
| `Arrow Up` / `Arrow Down` or `j` / `k` | Move selection. |
| `Enter` / `Arrow Right` or `l` | Open / expand the selected folder. |
| `Backspace` / `Arrow Left` or `h` | Go up to parent folder. |
| `b` / `e` | Go to the top or bottom of the current list. |

### Special Views & Search
| Key | Action |
|---|---|
| `v` | Preview selected text file (scrollable overlay). |
| `a` | Toggle Tree View mode. |
| `M` | Toggle Miller Columns (Ranger-style side-by-side view). |
| `E` | Show space distribution by file extension. |
| `U` | Launch **Duplicate Finder** (scans for duplicate files to find wasted space). |
| `/` | Global search across the entire cache (both files and directories). |
| `f` | Find by name in the current folder (press `n` / `N` for next / previous match). |
| `F` | Search using regular expressions (Regex). |
| `t` | Filter displayed items (All / Directories Only / Files Only). |
| `T` | Toggle text filtering based on the search query. |
| `Ctrl + T` | Instantly reset all search filters. |

### File Operations
| Key | Action |
|---|---|
| `SPACE` | Mark / unmark the current item for batch operations. |
| `Ctrl + A` | Select / deselect all items in the current view. |
| `L` | View interactive overlay of marked items (allows selective or global deselection). |
| `m` | Move all marked items to the current directory. |
| `c` | Copy all marked items to the current directory (shows progress percentage). |
| `d` | Delete marked items (if any) or the currently selected item. |
| `ALT + r` | Rename the selected item. |
| `Ctrl + n` | Create a new directory in the current path. |
| `ALT + n` | Create a new empty file. |
| `z` | Compress selected items into a `.zip` archive. |
| `x` | Extract the selected archive. |

### General Utilities
| Key | Action |
|---|---|
| `r` | Rescan the selected directory. |
| `R` | Perform a full parallel rescan of the current directory. |
| `O` | Open file or directory with system default application (`xdg-open`). |
| `Ctrl + E` | Open selected file with the external editor configured in `$EDITOR` or `vim`. |
| `Ctrl + S` | Temporarily suspend `qdux` and spawn a subshell in the current folder (type `exit` to return). |
| `o` | Toggle sort key (Size $\rightarrow$ Name $\rightarrow$ Modification Date $\rightarrow$ Delta). |
| `s` | Reverse sort order (Ascending $\leftrightarrow$ Descending). |
| `K` | Cycle through available color themes. |
| `Y` | Capture a baseline snapshot and toggle **DIFF** mode (highlights space changes). |
| `I` | Toggle size display formatting (Numeric $\rightarrow$ Percentage of parent $\rightarrow$ Hidden). |
| `TAB` / `Ctrl + i` | Show or hide the graph bar column on the left. |
| `h` | Show built-in help screen. |
| `q` | Exit `qdux`. |

---

## 4. Configuration (`config.toml`)

The program searches for its configuration file in `~/.config/qdux/config.toml`. A valid configuration file example:

```toml
# Default theme selection
theme = "dracula" # Options: dark, dracula, tokyonight, light, pastel

# Default text editor (overrides $EDITOR)
editor = "nano"

# Custom colors definition
dir_fg = "blue"
file_fg = "white"
size_s_fg = "green"   # Small files (< 1 MB)
size_m_fg = "yellow"  # Medium files (1 MB - 1 GB)
size_l_fg = "red"     # Large files (> 1 GB)

# Associate extensions with custom external applications
[associations]
pdf = "zathura"
png = "feh"
jpg = "feh"
mp4 = "mpv"
zip = "file-roller"
```
