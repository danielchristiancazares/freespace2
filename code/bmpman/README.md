# Bitmap Manager — `code/bmpman`

The Bitmap Manager (`bmpman`) is the central subsystem responsible for loading, caching, and managing all image assets (bitmaps and animations) used by the engine.

## Architecture

`bmpman` acts as a resource provider, decoupling the high-level game logic from raw file formats and low-level GPU texture management.

### 1. Handle-Based System
Assets are never referenced by raw pointers in engine logic. Instead, `bm_load()` returns an `int` handle.
*   **Safety:** Handles prevent use-after-free errors when assets are paged out or reloaded.
*   **Stability:** Handles are stable across frames even if the underlying memory location of the bitmap changes.

### 2. Supported Formats
*   **Still Images:** PCX, TGA, DDS, PNG, JPG.
*   **Animations:** ANI (proprietary), EFF (text-based sequence), APNG.
*   **Internal Types:** User-created bitmaps (`BM_TYPE_USER`) and 3D textures.

### 3. Rendering Targets (RTT)
`bmpman` manages bitmaps that serve as render destinations (`BM_TYPE_RENDER_TARGET_STATIC` and `BM_TYPE_RENDER_TARGET_DYNAMIC`). 
*   These are used for features like the targeting monitor, cockpit displays, and environment maps.
*   The Vulkan backend implements this via `VulkanTextureManager::createRenderTarget`.

## Core API (`bmpman.h`)

*   **Loading:** `bm_load()`, `bm_load_animation()`.
*   **Lifecycle:** `bm_lock()` (pages data into memory), `bm_unlock()`, `bm_unload()`, `bm_release()`.
*   **Metadata:** `bm_get_info()`, `bm_get_type()`, `bm_has_alpha_channel()`.
*   **Paging:** `bm_page_in_texture()` allows the engine to pre-load assets required for a specific mission or sequence.

## Internal Storage (`bm_internal.h`)

*   **`bitmap_entry`**: Contains the metadata for a single bitmap (dimensions, flags, ref-count, filename).
*   **`bitmap_slot`**: Pairs a `bitmap_entry` with a backend-specific `gr_bitmap_info` pointer.
*   **`bm_blocks`**: A dynamic array of `bitmap_slot` blocks, allowing for thousands of loaded assets.

## Interaction with Graphics Backends

`bmpman` is the primary source of data for the `gr_*` texture API.
1.  Engine calls `bm_lock()`.
2.  `bmpman` reads the file and converts it to a standard RGBA buffer if necessary.
3.  Engine passes the handle to `gr_set_bitmap()`.
4.  The backend (e.g., Vulkan) uses `bm_get_gr_info()` to retrieve its internal texture object or schedules an upload if the handle hasn't been seen by the GPU yet.

## Common Pitfalls

*   **Locking Overhead:** Never call `bm_lock()` every frame if you can avoid it. Use it only when the asset is first needed or when its format must change.
*   **Handle Validity:** Always check `bm_is_valid()` before using a handle that might have been released.
*   **Texture Memory:** Monitor `bm_texture_ram` to ensure assets aren't exceeding the physical capabilities of the user's hardware.
*   **Handle Reuse:** In Vulkan, when a bitmap handle is released and immediately reused by `bmpman`, the backend must ensure the old GPU mapping is retired before the new one is created.
