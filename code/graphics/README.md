# Graphics Abstraction Layer — `code/graphics`

This directory defines the primary graphics API (`gr_*`) used by the rest of the engine and provides common rendering logic shared across backends.

## Architecture

The graphics subsystem follows a **Backend Pattern**. The engine interacts with a generic interface, and a specific backend (Vulkan or OpenGL) provides the implementation at runtime.

### 1. The `gr_screen` Interface (`2d.h`)
The core of the abstraction is the `screen` struct, globally accessible as `gr_screen`. It acts as a manual VTable, containing function pointers for all graphics operations:
*   **Lifecycle:** `gf_flip`, `gf_setup_frame`.
*   **State:** `gf_set_color`, `gf_set_clip`, `gf_set_cull`, `gf_zbuffer_set`.
*   **Drawing:** `gf_render_model`, `gf_render_primitives`, `gf_bitmap`.
*   **Resource Management:** `gf_create_buffer`, `gf_preload` (textures).

### 2. Implementation Subdirectories
*   **`vulkan/`**: The modern, high-performance Vulkan backend.
*   **`opengl/`**: The legacy hardware renderer backend.
*   **`software/`**: Legacy software-based font management and utilities.
*   **`paths/`**: Vector graphics and path rendering (e.g., NanoVG for UI).
*   **`shaders/`**: Common shader definitions and GLSL/SPIR-V management.
*   **`util/`**: Cross-backend utilities, including GPU memory heaps and uniform buffer management.

## Key Components

### 2D Drawing (`2d.cpp` / `render.h`)
Provides high-level helpers for UI and HUD rendering:
*   `gr_string()`: Font rendering.
*   `gr_line()`, `gr_rect()`, `gr_circle()`: Primitive shapes.
*   `gr_bitmap()`: Standard texture blitting.

### Material System (`material.cpp`)
Encapsulates rendering state for a draw call:
*   **`material`**: Base class for texture bindings and blend modes.
*   **`model_material`**: Specialized for 3D ship models (diffuse, normal, specular maps).

### Memory & Buffers
*   **`gr_buffer_handle`**: A type-safe ID used to reference GPU buffers (vertex, index, uniform).
*   **`gr_render_primitives_immediate`**: A bridge function that allows legacy immediate-mode code to pass raw vertex pointers, which the backend then uploads to transient GPU memory.

### Post-Processing (`post_processing.cpp`)
Manages screen-space effects like Bloom, FXAA/SMAA, and Tonemapping.

## Design Patterns

### Immediate-to-Buffered Bridge
Because the engine logic was originally written for immediate-mode APIs (DirectX 5/6), the abstraction layer often emulates this. Backends use internal **Ring Buffers** (`VulkanRingBuffer`) to provide high-speed, per-frame transient storage for these legacy calls.

### Coordinate Scaling
The engine supports multiple resolutions and aspect ratios. The `resize_mode` parameters (e.g., `GR_RESIZE_FULL`) control how 2D coordinates are translated from the "virtual" 640x480 or 1024x768 space to the actual screen resolution.

## Common Pitfalls
*   **Global State:** Many `gr_*` calls modify global state in `gr_screen` (current color, current font). Always verify the state before drawing.
*   **Y-Axis Convention:** FSO historically uses top-left (0,0). Backends (especially Vulkan) must handle the Y-flip internally.
*   **Bitmaps vs. Textures:** In this subsystem, "bitmap" usually refers to an engine-side handle from `bmpman`, while "texture" refers to the GPU-resident resource.
