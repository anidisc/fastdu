# qdux

A fast terminal UI (ncurses) disk-usage explorer written in C, with parallel scanning and an on-disk cache.

- **Current version: 0.77.0**
- **License: MIT**

---

## Highlights

- **Instant Global Search**: Search for any file or directory in the entire scanned cache instantly (Press `/`).
- **Quick Creation**: Create new folders (`Ctrl+n`) and empty files (`ALT+n`) directly from the TUI.
- **Subshell Access**: Drop into a shell at your current browsing directory directly from the TUI (Press `Ctrl+S`).
- **Rename Items**: Quickly rename any file or directory directly from the TUI (Press `ALT+r`). Includes conflict resolution.
- **Enhanced Text Previews**: Integrated support for **bat** (or `batcat`) to provide syntax highlighting and advanced paging (Press `v`).
- **Archive Extraction**: Unpack `.zip`, `.tar`, `.7z`, and more directly from the TUI (Press `x`).
- **Zip Compression**: Compress files and folders into a `.zip` archive directly from the TUI (Press `z`).
- **External Editor Integration**: Open and edit any text file directly from the TUI (Press `Ctrl+E`).
- **Ranger-style Navigation**: Multi-column Miller Columns with automatic live previews for text and images (Press `M` to toggle).
- **Native Image Previews**: High-performance image viewing using the **Kitty Graphics Protocol** (Ghostty, Kitty, WezTerm) or **Chafa** as a fallback. Aspect ratio is always preserved.
- **Duplicate Finder**: Identify and remove identical files safely. Now with progress bars and ESC-to-cancel support (Press `U`).
- **Archive Exploration**: Browse inside `.zip`, `.tar`, `.7z`, `.iso` and more as if they were directories (Press `Enter` to enter, `Backspace` to exit, `ESC` to cancel).
- **Snapshot Comparison (Diff Mode)**: Compare your current disk usage against a reference baseline (Press `Y`).
- **Theme Presets**: Switch between Dracula, Tokyo Night, Pastel, Light, and Dark themes (Press `K`).
- **Tree View Mode**: Toggle a hierarchical view of your directories (Press `A`).
- **Full Mouse Support**: Selection, double-click to enter, right-click to go back, and scroll wheel support.
- **Navigable Breadcrumbs**: Clickable path in the header for instant navigation to parent directories.
- **Nerd Fonts Integration**: Visual icons for folders and files (requires a Nerd Font and `-nf` flag).
- **Customizable TUI**: Colors and themes via `~/.config/qdux/config.toml`.
- **Instant Cache Loading**: Eliminated redundant disk walks during cache loading. Even massive cache files are now parsed instantly.
- **Cache Progress Bar**: Visual feedback during the cache loading phase, showing real-time progress for large datasets.
- **Ultra-Fluid Scrolling**: Optimized TUI rendering engine with cached metrics, providing instant response even in directories with 100,000+ files.
- **Sharded Cache**: Performance-optimized cache with 64 independent locks to minimize thread contention.
- **Hard Link Detection**: Accurate disk usage calculation by counting unique (dev, ino) pairs only once.
- **Iterative Operations**: Tree removal and copying are now iterative, supporting extremely deep directory structures without stack overflow.
- **Advanced Exclusions**: Support for `.qduxignore` files and `--exclude` CLI flag.
- **Extension Analysis**: Dedicated view to analyze space distribution by file extension (Press `E`).
- **Data Export**: Export scan results to JSON or CSV formats for external analysis.
- **System Integration**: Open files/folders directly with the system default application (Press `O`).
- **Mount-point Awareness**: Option to stay on a single filesystem (`-x` / `--one-file-system`).
- **Robust TUI**: Improved visibility with occupation bars, smart name truncation, and dynamic resize support.

---

## What’s new in 0.77.0 (Interactive Marked Files List View)

### 📌 Interactive Marked Files List
A new interactive overlay view allows you to see and manage all marked files instantly by pressing `L`.
- **Interactive List**: Scroll through all marked items and selectively deselect (unmark) files using `Space`, `d`, or `u`.
- **Clear All**: Deselect all marked items instantly with `c` or `C`.
- **Path Truncation**: Smart front-truncation ensures long absolute file paths remain readable.

---

## What’s new in 0.76.0 (Dynamic Multiline Footer Messages & Adaptive TUI)

### 💬 Dynamic Multiline Footer & Adaptive TUI
Long status messages and prompts in the footer now dynamically wrap to multiple lines when the terminal window is narrow, automatically shifting the footer bar up and shrinking the list view height to fit.
- **Adaptive Shifting**: The TUI dynamically adjusts directory lists, separators, and column views when multiline text is active, returning to its default layout once dismissed.
- **Improved Prompts**: Conflict dialogs, delete confirmations, and name inputs all support dynamic text wrapping and precise 2D cursor alignment.

---

## What’s new in 0.75.0 (Remote Client/Server Support & Core Reliability)

### 🌐 Full SSH Remote Client/Server Protocol
Scan, browse, and manage remote file systems via SSH directly from your local terminal.
- **Zero-Latency Multiplexing**: Automatically sets up SSH Multiplexing (`ControlMaster`) to avoid password prompting and speed up file access and metadata syncing.
- **Remote Operations**: Support for creating directories (`Ctrl+n`), files (`ALT+n`), renaming (`ALT+r`), and deleting (`d`) directly on remote servers.
- **On-Demand Remote Previews**: Preview remote files (`v`) or open them using local default apps (`O`) through automatic temporary staging.
- **Synchronized Caching**: Both local client cache and remote server cache are kept in sync automatically without full rescans.

### 🛡️ Core Reliability & Safety Enhancements
The codebase has been refactored to check memory allocations and prevent instability.
- **Safe Reallocations**: Added checks to prevent null dereferences during task queue and list resizing (such as archive compression and directory copies).
- **Graceful Thread Pool Teardown**: Ensures all background scanner and finalizer threads are correctly joined and cleaned up in case of folder I/O errors or early cancellation.

---

## What’s new in 0.65.0 (Instant Global Search)

### 🔍 Search Everywhere
Leverage the power of the in-memory cache to find anything, anywhere.
- **Lightning Fast**: Scans millions of entries in milliseconds.
- **Context Jumper**: Select a result and press `Enter` to jump directly to its parent directory with the item pre-selected.
- **Deep Discovery**: Finds files even in deeply nested subdirectories that haven't been visited in the current session.

---

## What’s new in 0.64.0 (Quick Creation)

### 📁 Create Files & Folders
You can now create new items without leaving **qdux**.
- **Folders**: Press `Ctrl+n` to create a new directory.
- **Files**: Press `ALT+n` to create a new empty file.
- **Interactive**: Integrated prompts with full line editing support.
- **Auto-Sync**: New items appear instantly and are added to the cache.

---

## What’s new in 0.63.0 (Subshell Access)

### 🐚 Instant Shell Access
You can now jump to your terminal's shell without losing your place in **qdux**.
- **Context Aware**: The shell opens exactly in the directory you are currently browsing.
- **Seamless Flow**: Press `Ctrl+S` to drop to shell, type `exit` or `Ctrl+D` to return to exactly where you were.
- **Auto-Sync**: When you return, **qdux** automatically refreshes the current view to reflect any changes made in the shell.

---

## What’s new in 0.61.1 (Renaming Support)

### ✏️ Item Renaming
You can now rename files and directories directly within the TUI.
- **Quick Shortcut**: Press `ALT+r` on any item.
- **Pre-filled Prompt**: The current name is pre-loaded for easy editing.
- **Robust Conflicts**: Integrated conflict management (Overwrite, Auto-rename, Manual Retry, Cancel).
- **Cache Sync**: Automatically updates the internal cache to reflect the name change without full rescans.

---

## What’s new in 0.61.0 (Syntax Highlighting)

### 📄 Advanced Text Previews
File inspection is now much better with **bat** integration.
- **Syntax Highlighting**: Automatically detects file type and applies colors (C, Python, Markdown, etc.).
- **Smart Paging**: Seamlessly scroll, search, and navigate through large text files.
- **Auto-fallback**: Uses the built-in internal viewer if `bat` is not installed.

---

## What’s new in 0.60.0 (Archive Extraction)

### 🔓 Integrated Unpacking
Extract compressed files with full control.
- **Versatile**: Supports all formats provided by `libarchive` (zip, tar, 7z, iso, etc.).
- **Flexible Destination**: Extract into a new folder or direttamente into the current directory.
- **Granular Conflicts**: If files already exist, choose to overwrite, rename, or skip for every single item.
- **Auto-Sync**: The new contents are instantly scanned and added to the cache.

---

## What’s new in 0.59.0 (Zip Compression)

### 🤐 Built-in Compression
You can now create `.zip` archives directly within **qdux**.
- **Context Aware**: Compresses marked items if any, otherwise the selected item (Press `z`).
- **Interactive**: Prompt for archive name before starting.
- **Conflict Management**: Detects if the target zip already exists and asks for overwrite or rename.
- **Recursive**: Full support for directory tree compression.

---

## What’s new in 0.58.0 (Editor Integration)

### 📝 External Editor support
You can now edit files without leaving **qdux**.
- **Quick Edit**: Press `Ctrl+E` on any file to open it in your preferred editor.
- **Customizable**: Set your editor in `config.toml` (e.g., `editor = "nvim"`) or rely on the `$EDITOR` environment variable.
- **Seamless Transition**: The TUI suspends while you edit and resumes exactly where you left off.

---

## What’s new in 0.57.0 (Ranger Mode & UI Refinements)

### 📂 Integrated Miller Columns
A new way to navigate your filesystem inspired by **ranger**.
- **Automatic Previews**: When a file is selected in Miller mode (Press `M`), its content (text/markdown) or image is automatically displayed in the third column.
- **Smart Image Handling**: Graphics are automatically cleared when switching between files to avoid overlapping.
- **Improved Context**: Added a `/ROOT` label in the parent column when at the scan root for better spatial orientation.
- **Filter Reset**: Quickly return to the default view by pressing `Ctrl+T` to reset all active filters and search queries.

---

## What’s new in 0.55.0 (Native Graphics)

### 🖼 Enhanced Image Previews
The image preview system is now more powerful and efficient.
- **Native Support**: Direct rendering on compatible terminal emulators (Ghostty, Kitty, etc.) using the Kitty Graphics Protocol. No external tools needed!
- **Aspect Ratio Preservation**: Images are automatically scaled to fit the preview window while maintaining their original proportions (no more stretching!).
- **Universal Fallback**: Continues to support **Chafa** for older or less capable terminals.

---

## What’s new in 0.54.0 (Image Previews)

### 🖼 Terminal Image Viewing
Now you can preview images without leaving the TUI.
- **Instant Preview**: Press `v` on common image formats (jpg, png, webp, gif, etc.).
- **Automatic Rendering**: Powered by **Chafa**, it automatically adapts to your terminal's capabilities (Sixel, Kitty, or Unicode blocks).
- **Seamless Flow**: Simply press any key to exit the preview and return to browsing.

---

## What’s new in 0.53.0 (Robust Waste Analysis)

### 🧹 Reactive Duplicate Finder
The Waste Space Analyzer is now fully interruptible and provides visual feedback during all stages.
- **Three-Stage Progress**: Separate progress bars for file collection, hashing, and deep comparison.
- **User Control**: Press `ESC` at any stage to safely cancel the operation and return to the main view.
- **Fluid Navigation**: Group headers are automatically skipped for a better UX.

---

## What’s new in 0.52.0 (UX Refinements)

### 📦 Robust Archive Browsing
Improved the experience when opening very large compressed files.
- **Progress Tracking**: A new progress bar shows the reading status of the archive based on processed bytes.
- **Interruptible Reading**: You can now press `ESC` at any time to stop reading a large archive and return to the normal view without blocking the program.

---

## What’s new in 0.51.0 (Duplicate Finder)

### 🧹 Waste Space Analyzer
A powerful tool to find identical files across your directories.
- **Smart Detection**: Uses a multi-stage approach (Size matching -> Header hashing -> Byte-by-byte comparison) to ensure 100% accuracy with maximum speed.
- **Interactive UI**: Grouped view of duplicates showing total wasted space (Press `U`).
- **Fluid Navigation**: Easily navigate through duplicated files while group headers are automatically skipped for a better UX.
- **Safety First**: Automatically prevents you from accidentally deleting all copies of a file within a group.
- **Instant Updates**: Deleting duplicates instantly updates your directory totals and the on-disk cache.

---

## What’s new in 0.49.0 (Archive Exploration)

### 📦 In-place Archive Browsing
You can now look inside compressed files without extracting them.
- **Seamless Navigation**: Press `Enter` on any supported archive (zip, tar, 7z, iso, etc.) to explore its contents.
- **Virtual FS**: A dedicated secondary header bar shows your current path inside the archive.
- **Accurate Sizes**: View the uncompressed size of archived files to understand their real impact.
- **Powered by libarchive**: Wide format support and high performance.

## What’s new in 0.48.0 (Snapshot Comparison & Themes)

### 📊 Snapshot Comparison (Diff Mode)
Analyze how your disk usage has changed over time.
- **Online Baseline**: Press `Y` to take an instant snapshot of the current state. From that point on, all deletions, moves, or rescans will show the **relative delta** (e.g., `+1.2 GiB` or `-500 MiB`).
- **Offline Comparison**: Compare with a previous cache file using the `--diff <file>` flag.
- **Visual Feedback**: Increases are shown in **Red**, decreases in **Green**, and unchanged items in default colors. An indicator `DIFF` appears in the footer.

### 🎨 Theme Presets
Instantly change the look of the TUI.
- **Cycle Themes**: Press `K` to cycle through built-in presets: *Dark*, *Dracula*, *Tokyo Night*, *Light*, and *Pastel*.
- **Persistence**: Set your favorite theme in the config file (`theme = "tokyonight"`).

## What’s new in 0.46.0 (Hierarchical Exploration)

- **Tree View**: Added a new mode to explore the filesystem hierarchy in-place. Press `A` to toggle.
- **Expand/Collapse**: In tree mode, press `Enter` or click to expand/collapse directories.
- **Visual Indentation**: Clear tree lines and +/- indicators for expanded nodes.

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

You can customize **qdux** colors by creating a file at `~/.config/qdux/config.toml`.

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
- **libarchive** (for archive exploration)
- **chafa** (optional, for image previews)

---

## Build

```bash
# On Debian/Ubuntu: sudo apt install libarchive-dev chafa
make
```

Produces the `./qdux` binary.

---

## Usage

```bash
./qdux [options] [path]
```

### Options

- `-nf, --nerd-fonts`    Enable Nerd Fonts icons support
- `-R, --reload`         Force cache rebuild (ignore existing cache)
- `-H, --headless`       Force headless (non-TUI) mode
- `-ac, --accuracy`      Accurate disk usage: force deep rescan and use allocated blocks
- `-x, --one-file-system` Stay on the same file system (do not cross mount points)
- `-e PAT, --exclude PAT` Exclude files/dirs matching exact PAT
- `--diff FILE`          Compare current sizes with snapshot FILE
- `--export FMT FILE`    Export results to FILE in FMT (json|csv) and exit
- `-D, --decorative`     Decorative UI (column headers, vertical separator, extra colors)
- `-j N, --jobs N`       Number of worker threads (default: online CPUs, max 64)
- `-v, --version`        Show version and exit
- `-h, --help`           Show help and exit

---

## TUI cheat sheet

- **Mouse**: Scroll wheel; Left-click (select/open/expand); Right-click (back); Click header path to jump.
- **Navigation**: Up/Down or j/k; Enter/Right (l) to enter/expand; Backspace/Left to go up; `b`/`e` to jump to first/last
- **Views**:
  - `A`: Toggle Tree View mode
  - `M`: Toggle Miller Columns mode (Ranger-style)
  - `E`: Open Extension Distribution view
  - `K`: Cycle built-in theme presets
  - `Y`: Toggle Baseline Snapshot (Diff Mode)
  - `v`: Preview selected text file (Toggle wrap: `w`)
  - `O`: Open selected item with system default (`xdg-open`)
  - `ALT+r`: Rename selected item
  - `Ctrl+n`: Create new folder
  - `ALT+n`: Create new empty file
  - `Ctrl+S`: Drop to subshell in current directory
  - `Ctrl+E`: Edit selected file with external editor
  - `z`: Compress marked/selected items to .zip archive
  - `x`: Extract selected archive
- **Sorting**: `o` toggles sort key (size → name → mtime → delta), `s` toggles order (asc/desc)
- **Display**:
  - `I`: Cycles size column (numeric → percent → hidden)
  - `i`: Cycles info column (mtime → owner+perm → hidden)
- **Filters**: `t` cycles type filter (all/dirs/files); `T` toggles the query filter; `Ctrl+T` resets all filters.
- **Search**: `/` global cache search; `f` substring (case-insensitive); `F` regex; `n`/`N` next/prev match
- **Help/Quit**: `h` help; `q` quit

---

## License

MIT
