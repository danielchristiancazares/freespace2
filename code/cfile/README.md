# Filesystem Abstraction — `code/cfile`

The Filesystem subsystem (`cfile`) provides a Virtual Filesystem (VFS) layer that abstracts physical disk access, archive files (VP/VPP), and mod-specific data paths.

## Architecture

`cfile` ensures that the rest of the engine does not need to know whether a file is a standalone file on disk or a member of a compressed archive.

### 1. Path Types (`CF_TYPE_*`)
Files are categorized into logical "Path Types" defined in `cfile.h`.
*   **`CF_TYPE_DATA`**: General game data.
*   **`CF_TYPE_TABLES`**: Engine configuration tables (.tbl).
*   **`CF_TYPE_MODELS`**: 3D ship models (.pof).
*   **`CF_TYPE_MISSIONS`**: Mission files (.fs2).
*   Each type maps to a specific relative directory (e.g., `data/`, `data/tables/`) and can have associated default extensions.

### 2. Location Precedence
When searching for a file, `cfile` follows a strict precedence order to support modding:
1.  **Primary Mod**: The `-mod` directory specified on the command line.
2.  **Secondary Mods**: Additional mod directories.
3.  **Game Root**: The base FS2 directory (or retail data).
4.  **Archive Files (VP/VPP)**: Searched within each of the above locations.

### 3. Archive System (`cfilearchive.h`)
The engine supports the **Volition Pack (VP/VPP)** format.
*   **Search Logic:** When `cfile_init()` is called, it builds a master index of all files contained within all VP archives in the search path.
*   **Shadowing:** A standalone file on disk will always "shadow" (override) a file with the same name inside a VP archive.

## Core API (`cfile.h`)

*   **Handle:** `CFILE*` is an opaque pointer used for all file operations.
*   **Opening:** `cfopen()` is the primary entry point. It accepts a filename and a path type.
*   **Reading:** `cfread()`, `cfgetc()`, `cfgets()`, and high-level helpers like `cfread_int()` and `cfread_float()`.
*   **Navigation:** `cfseek()`, `cftell()`, `cfeof()`.
*   **Lifecycle:** `cfclose()` (can be used with `std::unique_ptr` via a custom deleter).

## Internal Components

*   **`cfilesystem.cpp`**: Manages the low-level mapping of path types to physical directories and handles the mod search order.
*   **`cfilearchive.cpp`**: Implementation of the VP archive parser and indexer.
*   **`cfilecompression.cpp`**: Transparently handles decompressing files on-the-fly if they are stored compressed within an archive.

## Interaction with Vulkan

The Vulkan backend uses `cfile` for:
*   **Shader Loading:** Reading SPIR-V binaries from the `data/shaders/compiled/` path.
*   **Texture Loading:** Passing filenames to `bmpman`, which then uses `cfile` to read the raw image bytes.
*   **Pipeline Cache:** Storing and retrieving the `vulkan_pipeline.cache` file in the user's config directory (`CF_TYPE_CONFIG`).

## Common Pitfalls

*   **Filename Length:** Legacy constraints limit filenames to 31 characters (`CF_MAX_FILENAME_LENGTH`).
*   **Directory Separators:** Always use the `DIR_SEPARATOR_CHAR` macro or forward slashes; `cfile` handles platform-specific translation.
*   **Write Access:** Most `cfile` operations are read-only. Writing is typically restricted to "User" paths like `CF_TYPE_PLAYERS` or `CF_TYPE_CONFIG`.
*   **Case Sensitivity:** While Windows is case-insensitive, `cfile` aims for cross-platform compatibility. It is best practice to treat all filenames as case-sensitive.
