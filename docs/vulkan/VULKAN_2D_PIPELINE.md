# 2D Rendering Pipeline Architecture

This document provides comprehensive technical documentation of the FreeSpace 2 Open 2D rendering pipeline, with specific focus on the Vulkan backend implementation. The 2D pipeline handles all user interface elements including the HUD, menus, text, bitmaps, and vector graphics.

## Document Information

| Property | Value |
|----------|-------|
| Related Docs | [VULKAN_ARCHITECTURE.md](VULKAN_ARCHITECTURE.md), [VULKAN_HUD_RENDERING.md](VULKAN_HUD_RENDERING.md), [VULKAN_RENDER_PASS_STRUCTURE.md](VULKAN_RENDER_PASS_STRUCTURE.md) |
| Key Files | `code/graphics/render.cpp`, `code/graphics/matrix.cpp`, `code/graphics/vulkan/VulkanClip.h` |

## Prerequisites

Before reading this document, you should be familiar with:

- Basic Vulkan concepts (viewports, scissors, command buffers)
- The FSO graphics abstraction layer (`code/graphics/2d.h`)
- Matrix transformations (orthographic projection, model-view)
- The concept of NDC (Normalized Device Coordinates)

For Vulkan-specific context, see [VULKAN_ARCHITECTURE.md](VULKAN_ARCHITECTURE.md) first.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Screen-Space to NDC Coordinate Conversion](#2-screen-space-to-ndc-coordinate-conversion)
3. [Viewport and Scissor Management](#3-viewport-and-scissor-management)
4. [Orthographic Projection Setup](#4-orthographic-projection-setup)
5. [2D Primitive Rendering](#5-2d-primitive-rendering)
6. [Shader System for 2D](#6-shader-system-for-2d)
7. [Integration with 3D Rendering](#7-integration-with-3d-rendering)
8. [Performance Considerations and Optimization](#8-performance-considerations-and-optimization)
9. [Thread Safety and Synchronization](#9-thread-safety-and-synchronization)
10. [Debugging and Troubleshooting](#10-debugging-and-troubleshooting)
11. [File Reference](#11-file-reference)

---

## 1. Overview

The 2D rendering pipeline provides screen-space rendering for interface elements. It operates with an orthographic projection where pixel coordinates map directly to screen positions.

### Architecture Layers

```
+---------------------------+
|    High-Level 2D API      |  gr_bitmap(), gr_string(), gr_line(), gr_rect()
+---------------------------+
|    Material System        |  material, interface_material, batched_bitmap_material
+---------------------------+
|    Render Functions       |  gr_render_primitives(), gr_render_primitives_2d_immediate()
+---------------------------+
|    Backend (Vulkan)       |  VulkanGraphics.cpp render functions
+---------------------------+
|    GPU (Shaders)          |  interface.vert/frag, default-material.vert/frag
+---------------------------+
```

### Key Components

| Component | File | Purpose |
|-----------|------|---------|
| 2D API | `code/graphics/render.h`, `code/graphics/render.cpp` | Public drawing functions |
| Screen State | `code/graphics/2d.h` (`struct screen`) | Viewport, clip regions, current state |
| Matrix System | `code/graphics/matrix.h`, `code/graphics/matrix.cpp` | Projection and view matrix management |
| Vulkan Backend | `code/graphics/vulkan/VulkanGraphics.cpp` | GPU command recording |
| Clip Management | `code/graphics/vulkan/VulkanClip.h` | Scissor rectangle computation |

### Data Flow Overview

```
User Code                   Graphics API                Vulkan Backend
---------                   ------------                --------------
gr_bitmap(x,y)  ------>  bitmap_ex_internal()  ------>  cmd.draw()
     |                         |                            |
     v                         v                            v
  Position             Calculate UVs,              Bind pipeline,
  + resize mode        apply resize,              set uniforms,
                       create material            record draw call
```

---

## 2. Screen-Space to NDC Coordinate Conversion

### Coordinate System

FreeSpace 2 uses a screen-space coordinate system where:

- **Origin (0, 0)** is at the top-left corner of the screen
- **X axis** increases rightward (0 to `max_w`)
- **Y axis** increases downward (0 to `max_h`)
- **Coordinates** are specified in pixels

This matches typical 2D graphics conventions but differs from OpenGL/Vulkan NDC where Y increases upward. The Vulkan backend handles this discrepancy via a Y-flipped viewport (see Section 3).

### The gr_screen Structure

The global `gr_screen` variable (defined in `code/graphics/2d.h`) maintains all screen state:

```cpp
typedef struct screen {
    // Physical screen dimensions (actual pixels)
    int max_w, max_h;

    // Logical (unscaled) dimensions for resolution-independent UI
    int max_w_unscaled, max_h_unscaled;

    // Current clip region (offset from origin + dimensions)
    int offset_x, offset_y;
    int clip_width, clip_height;

    // Clip boundaries (relative to offset, typically left=top=0)
    int clip_left, clip_right, clip_top, clip_bottom;

    // Unscaled variants for resolution-independent calculations
    int offset_x_unscaled, offset_y_unscaled;
    int clip_width_unscaled, clip_height_unscaled;

    // Computed values
    float clip_aspect;          // clip_width / clip_height
    float clip_center_x, clip_center_y;  // Center of clip region

    // Render target: -1 for backbuffer, otherwise texture handle
    int rendering_to_texture;

    // ... additional fields omitted for brevity
} screen;
```

**Key Invariant**: `clip_left` and `clip_top` are always 0 in the current implementation. The clip region is defined by `offset_x/y` (the origin) plus `clip_width/height` (the extent).

### Resize Modes

The engine supports multiple resize modes for scaling UI elements (defined in `code/graphics/2d.h`):

| Mode | Value | Behavior |
|------|-------|----------|
| `GR_RESIZE_NONE` | 0 | No scaling; use raw pixel coordinates |
| `GR_RESIZE_FULL` | 1 | Scale to fill screen maintaining aspect ratio |
| `GR_RESIZE_FULL_CENTER` | 2 | Scale and center on screen |
| `GR_RESIZE_MENU` | 3 | Menu-specific scaling with letterboxing |
| `GR_RESIZE_MENU_ZOOMED` | 4 | Menu scaling with zoom factor |
| `GR_RESIZE_MENU_NO_OFFSET` | 5 | Menu scaling without offset adjustment |
| `GR_RESIZE_REPLACE` | 6 | Direct replacement; bypass all scaling and clamping |

### Coordinate Transformation Functions

**From logical to physical coordinates** (`code/graphics/2d.h`):

```cpp
// Scale logical (unscaled) coordinates to physical screen coordinates
bool gr_resize_screen_pos(int *x, int *y, int *w = NULL, int *h = NULL,
                          int resize_mode = GR_RESIZE_FULL);
bool gr_resize_screen_posf(float *x, float *y, float *w = NULL, float *h = NULL,
                           int resize_mode = GR_RESIZE_FULL);

// Reverse: physical to logical (unscaled) coordinates
bool gr_unsize_screen_pos(int *x, int *y, int *w = NULL, int *h = NULL,
                          int resize_mode = GR_RESIZE_FULL);
bool gr_unsize_screen_posf(float *x, float *y, float *w = NULL, float *h = NULL,
                           int resize_mode = GR_RESIZE_FULL);
```

**When scaling is applied**:

```cpp
// From code/graphics/render.cpp - gr_aabitmap()
if (resize_mode != GR_RESIZE_NONE &&
    (gr_screen.custom_size || gr_screen.rendering_to_texture != -1)) {
    do_resize = 1;
} else {
    do_resize = 0;
}
```

Scaling occurs only when:
1. `resize_mode` is not `GR_RESIZE_NONE`, AND
2. Either `custom_size` is true (non-native resolution) OR rendering to texture

---

## 3. Viewport and Scissor Management

### Vulkan Viewport Configuration

The Vulkan backend uses a **Y-flip viewport** to match the OpenGL/screen-space coordinate convention (from `code/graphics/vulkan/VulkanGraphics.cpp`):

```cpp
vk::Viewport createFullScreenViewport()
{
    vk::Viewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(gr_screen.max_h);  // Start at bottom edge
    viewport.width = static_cast<float>(gr_screen.max_w);
    viewport.height = -static_cast<float>(gr_screen.max_h);  // Negative = flip Y
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    return viewport;
}
```

**Why this works**:
| Property | Standard Vulkan | With Y-Flip |
|----------|-----------------|-------------|
| Viewport origin | Top-left | Bottom-left |
| Y direction | Downward (+Y) | Upward (+Y after flip) |
| Screen coord Y=0 | Top | Top (matches FSO expectation) |

The negative height effectively inverts the Y axis, allowing existing screen-space code (Y=0 at top) to work unchanged with Vulkan's coordinate system.

### Scissor Rectangle Computation

The scissor rectangle clips rendering to the current clip region. The Vulkan backend derives it from `gr_screen` (from `code/graphics/vulkan/VulkanClip.h`):

```cpp
struct ClipScissorRect {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

inline ClipScissorRect getClipScissorFromScreen(const ::screen& screen)
{
    ClipScissorRect rect{};
    rect.x = static_cast<int32_t>(screen.offset_x);
    rect.y = static_cast<int32_t>(screen.offset_y);
    rect.width = static_cast<uint32_t>(screen.clip_width);
    rect.height = static_cast<uint32_t>(screen.clip_height);
    return rect;
}
```

### Scissor Clamping

**Critical constraint**: Vulkan requires scissor offsets to be non-negative and the scissor extent to not exceed the framebuffer. Some engine paths (e.g., HUD jitter effects) can temporarily produce negative or out-of-bounds clip origins.

The clamping function (from `code/graphics/vulkan/VulkanClip.h`) handles this:

```cpp
inline ClipScissorRect clampClipScissorToFramebuffer(
    const ClipScissorRect& in,
    int32_t fbWidth,
    int32_t fbHeight)
{
    ClipScissorRect out{};

    const int64_t fbW = std::max<int64_t>(0, fbWidth);
    const int64_t fbH = std::max<int64_t>(0, fbHeight);

    // Treat the input as a half-open box [x0,x1) x [y0,y1)
    // and intersect with [0,fbW) x [0,fbH)
    int64_t x0 = in.x;
    int64_t y0 = in.y;
    int64_t x1 = x0 + static_cast<int64_t>(in.width);
    int64_t y1 = y0 + static_cast<int64_t>(in.height);

    auto clampI64 = [](int64_t v, int64_t lo, int64_t hi) {
        return std::max(lo, std::min(v, hi));
    };

    x0 = clampI64(x0, 0, fbW);
    y0 = clampI64(y0, 0, fbH);
    x1 = clampI64(x1, 0, fbW);
    y1 = clampI64(y1, 0, fbH);

    out.x = static_cast<int32_t>(x0);
    out.y = static_cast<int32_t>(y0);
    out.width = static_cast<uint32_t>(std::max<int64_t>(0, x1 - x0));
    out.height = static_cast<uint32_t>(std::max<int64_t>(0, y1 - y0));
    return out;
}
```

**Behavior**: If the clip region is entirely outside the framebuffer, the result is a zero-extent scissor (width=0 or height=0), which causes all fragments to be discarded.

### Clip State Application

The `applyClipToScreen()` function (in `code/graphics/vulkan/VulkanClip.h`) updates `gr_screen` clip state following the engine's clip semantics:

```cpp
inline void applyClipToScreen(int x, int y, int w, int h, int resize_mode)
{
    // Sanity clamp inputs to avoid negative values
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    // Determine if coordinate scaling is needed
    const bool to_resize = (resize_mode != GR_RESIZE_NONE &&
                           resize_mode != GR_RESIZE_REPLACE &&
                           (gr_screen.custom_size || gr_screen.rendering_to_texture != -1));

    // Determine max bounds based on scaling mode
    int max_w = to_resize ? gr_screen.max_w_unscaled : gr_screen.max_w;
    int max_h = to_resize ? gr_screen.max_h_unscaled : gr_screen.max_h;

    // Clamp to screen bounds (unless GR_RESIZE_REPLACE)
    if (resize_mode != GR_RESIZE_REPLACE) {
        if (x >= max_w) x = max_w - 1;
        if (y >= max_h) y = max_h - 1;
        if (x + w > max_w) w = max_w - x;
        if (y + h > max_h) h = max_h - y;
    }

    // Store unscaled values first
    gr_screen.offset_x_unscaled = x;
    gr_screen.offset_y_unscaled = y;
    gr_screen.clip_width_unscaled = w;
    gr_screen.clip_height_unscaled = h;
    // ... (clip_left/right/top/bottom_unscaled)

    // Apply resize transformation if needed
    if (to_resize) {
        gr_resize_screen_pos(&x, &y, &w, &h, resize_mode);
    }

    // Update scaled values
    gr_screen.offset_x = x;
    gr_screen.offset_y = y;
    gr_screen.clip_left = 0;
    gr_screen.clip_right = w - 1;
    gr_screen.clip_top = 0;
    gr_screen.clip_bottom = h - 1;
    gr_screen.clip_width = w;
    gr_screen.clip_height = h;

    gr_screen.clip_aspect = i2fl(w) / i2fl(h);
    gr_screen.clip_center_x = (gr_screen.clip_left + gr_screen.clip_right) * 0.5f;
    gr_screen.clip_center_y = (gr_screen.clip_top + gr_screen.clip_bottom) * 0.5f;
}
```

### Dynamic Scissor Updates

When the clip region changes during rendering, the Vulkan backend immediately updates the dynamic scissor state via `vkCmdSetScissor`. This is done automatically when `gr_set_clip()` is called while a recording session is active.

---

## 4. Orthographic Projection Setup

### Projection Matrix Creation

The orthographic projection matrix maps screen coordinates directly to NDC (from `code/graphics/matrix.cpp`):

```cpp
static void create_orthographic_projection_matrix(
    matrix4* out,
    float left, float right,
    float bottom, float top,
    float near_dist, float far_dist)
{
    memset(out, 0, sizeof(matrix4));

    // X and Y scale factors (2D mapping)
    out->a1d[0] = 2.0f / (right - left);           // X scale
    out->a1d[5] = 2.0f / (top - bottom);           // Y scale

    // X and Y translation (centering)
    out->a1d[12] = -(right + left) / (right - left);
    out->a1d[13] = -(top + bottom) / (top - bottom);

    out->a1d[15] = 1.0f;  // Homogeneous coordinate

    // Depth mapping differs between Vulkan and OpenGL
    if (gr_screen.mode == GR_VULKAN) {
        // Vulkan: maps Z from [near, far] to [0, 1]
        out->a1d[10] = -1.0f / (far_dist - near_dist);
        out->a1d[14] = -near_dist / (far_dist - near_dist);
    } else {
        // OpenGL: maps Z from [near, far] to [-1, 1]
        out->a1d[10] = -2.0f / (far_dist - near_dist);
        out->a1d[14] = -(far_dist + near_dist) / (far_dist - near_dist);
    }
}
```

**Matrix layout** (column-major, OpenGL-style):
```
| a1d[0]  a1d[4]  a1d[8]   a1d[12] |   | 2/(r-l)    0         0      -(r+l)/(r-l) |
| a1d[1]  a1d[5]  a1d[9]   a1d[13] | = |    0    2/(t-b)      0      -(t+b)/(t-b) |
| a1d[2]  a1d[6]  a1d[10]  a1d[14] |   |    0       0      -1/(f-n)  -n/(f-n)     |
| a1d[3]  a1d[7]  a1d[11]  a1d[15] |   |    0       0         0           1       |
```

### 2D Matrix Setup

The `gr_set_2d_matrix()` function (in `code/graphics/matrix.cpp`) configures rendering for 2D mode:

```cpp
void gr_set_2d_matrix()
{
    // Only valid if 3D projection was previously set
    if (!gr_htl_projection_matrix_set) {
        return;
    }

    Assert(htl_2d_matrix_set == 0);
    Assert(htl_2d_matrix_depth == 0);

    // Set viewport to full screen (orthographic needs full screen for proper mapping)
    gr_set_viewport(0, 0, gr_screen.max_w, gr_screen.max_h);

    // Save current projection for later restoration
    gr_last_projection_matrix = gr_projection_matrix;

    // Create orthographic projection
    // Note: top/bottom are swapped depending on render target
    if (gr_screen.rendering_to_texture != -1) {
        // Render-to-texture: Y increases upward (standard GL/Vulkan)
        create_orthographic_projection_matrix(&gr_projection_matrix,
            0, i2fl(gr_screen.max_w),
            0, i2fl(gr_screen.max_h),
            -1, 1);
    } else {
        // Backbuffer: Y increases downward (screen-space convention)
        create_orthographic_projection_matrix(&gr_projection_matrix,
            0, i2fl(gr_screen.max_w),
            i2fl(gr_screen.max_h), 0,  // Swapped: bottom=max_h, top=0
            -1, 1);
    }

    // Set identity model-view matrix
    matrix4 identity_mat;
    vm_matrix4_set_identity(&identity_mat);
    gr_model_matrix_stack.push_and_replace(identity_mat);

    gr_last_view_matrix = gr_view_matrix;
    gr_view_matrix = identity_mat;
    vm_matrix4_x_matrix4(&gr_model_view_matrix, &gr_view_matrix, &identity_mat);

    htl_2d_matrix_set = true;
    htl_2d_matrix_depth++;

    matrix_uniform_up_to_date = false;
}
```

**Why swap top/bottom for backbuffer?** Combined with the Y-flipped viewport, this ensures that screen coordinate (0,0) maps to the top-left of the display, matching user expectations for 2D UI rendering.

### End 2D Matrix

The `gr_end_2d_matrix()` function (in `code/graphics/matrix.cpp`) restores the previous 3D projection state:

```cpp
void gr_end_2d_matrix()
{
    if (!htl_2d_matrix_set)
        return;

    Assert(htl_2d_matrix_depth == 1);

    // Restore viewport to the 3D clip region
    // Note: Y is inverted for backbuffer rendering
    gr_set_viewport(gr_screen.offset_x,
                   (gr_screen.max_h - gr_screen.offset_y - gr_screen.clip_height),
                   gr_screen.clip_width,
                   gr_screen.clip_height);

    // Restore saved matrices
    gr_projection_matrix = gr_last_projection_matrix;
    gr_model_matrix_stack.pop();
    gr_view_matrix = gr_last_view_matrix;

    const auto& model_matrix = gr_model_matrix_stack.get_transform();
    vm_matrix4_x_matrix4(&gr_model_view_matrix, &gr_view_matrix, &model_matrix);

    htl_2d_matrix_set = false;
    htl_2d_matrix_depth = 0;

    matrix_uniform_up_to_date = false;
}
```

---

## 5. 2D Primitive Rendering

### Immediate Buffer System

The immediate buffer provides dynamic vertex storage for 2D primitives (from `code/graphics/render.cpp`):

```cpp
gr_buffer_handle gr_immediate_buffer_handle;
static size_t immediate_buffer_offset = 0;
static size_t immediate_buffer_size = 0;
static const size_t IMMEDIATE_BUFFER_RESIZE_BLOCK_SIZE = 2048;

size_t gr_add_to_immediate_buffer(size_t size, void* data) {
    if (!gr_immediate_buffer_handle.isValid()) {
        gr_immediate_buffer_handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Dynamic);
    }

    Assert(size > 0 && data != NULL);

    // Check if resize is needed (buffer orphaning pattern)
    if (immediate_buffer_offset + size > immediate_buffer_size) {
        immediate_buffer_offset = 0;
        immediate_buffer_size += MAX(IMMEDIATE_BUFFER_RESIZE_BLOCK_SIZE, size);
        gr_resize_buffer(gr_immediate_buffer_handle, immediate_buffer_size);
    }

    // Sub-update at current offset
    gr_update_buffer_data_offset(gr_immediate_buffer_handle,
                                  immediate_buffer_offset, size, data);

    auto old_offset = immediate_buffer_offset;
    immediate_buffer_offset += size;
    return old_offset;
}
```

**Buffer orphaning**: When the buffer is full, `gr_resize_buffer()` creates new backing storage. The old buffer data remains valid for any in-flight GPU commands, avoiding synchronization stalls.

### 2D Immediate Rendering

The `gr_render_primitives_2d_immediate()` function (in `code/graphics/render.cpp`) wraps 2D matrix setup:

```cpp
void gr_render_primitives_2d_immediate(
    material* material_info,
    primitive_type prim_type,
    vertex_layout* layout,
    int n_verts,
    void* data,
    size_t size)
{
    if (gr_screen.mode == GR_STUB) {
        return;
    }

    gr_set_2d_matrix();  // Enable orthographic projection

    gr_render_primitives_immediate(material_info, prim_type, layout, n_verts, data, size);

    gr_end_2d_matrix();  // Restore previous projection
}
```

### Bitmap Rendering

#### gr_aabitmap (Anti-Aliased Bitmap)

Renders grayscale bitmaps (fonts, icons) multiplied with the current color (from `code/graphics/render.cpp`):

```cpp
void gr_aabitmap(int x, int y, int resize_mode, bool mirror, float scale_factor) {
    int w, h;
    bm_get_info(gr_screen.current_bitmap, &w, &h);

    // Apply scale factor if specified
    if (scale_factor != 1.0f) {
        w = static_cast<int>(w * scale_factor);
        h = static_cast<int>(h * scale_factor);
    }

    // Determine if scaling is needed
    int do_resize = (resize_mode != GR_RESIZE_NONE &&
                    (gr_screen.custom_size || gr_screen.rendering_to_texture != -1)) ? 1 : 0;

    // Calculate destination rectangle
    int dx1 = x, dx2 = x + w - 1;
    int dy1 = y, dy2 = y + h - 1;
    int sx = 0, sy = 0;

    // Get appropriate clip bounds
    int clip_left = do_resize ? gr_screen.clip_left_unscaled : gr_screen.clip_left;
    int clip_right = do_resize ? gr_screen.clip_right_unscaled : gr_screen.clip_right;
    int clip_top = do_resize ? gr_screen.clip_top_unscaled : gr_screen.clip_top;
    int clip_bottom = do_resize ? gr_screen.clip_bottom_unscaled : gr_screen.clip_bottom;

    // Early-out if completely outside clip region
    if ((dx1 > clip_right) || (dx2 < clip_left) ||
        (dy1 > clip_bottom) || (dy2 < clip_top)) {
        return;
    }

    // Clip to visible region, adjusting source offset
    if (dx1 < clip_left) { sx = clip_left - dx1; dx1 = clip_left; }
    if (dy1 < clip_top)  { sy = clip_top - dy1;  dy1 = clip_top;  }
    if (dx2 > clip_right)  dx2 = clip_right;
    if (dy2 > clip_bottom) dy2 = clip_bottom;

    // Render the clipped portion
    bitmap_ex_internal(dx1, dy1, (dx2 - dx1 + 1), (dy2 - dy1 + 1),
                       sx, sy, resize_mode, true, mirror,
                       &GR_CURRENT_COLOR, scale_factor);
}
```

#### Internal Bitmap Rendering

The `bitmap_ex_internal()` function creates the textured quad (from `code/graphics/render.cpp`):

```cpp
static void bitmap_ex_internal(int x, int y, int w, int h, int sx, int sy,
                               int resize_mode, bool aabitmap, bool mirror,
                               color* clr, float scale_factor) {
    int bw, bh;
    bm_get_info(gr_screen.current_bitmap, &bw, &bh, nullptr, nullptr, nullptr);

    // Adjust for scale factor
    if (scale_factor != 1.0f) {
        bw = static_cast<int>(bw * scale_factor);
        bh = static_cast<int>(bh * scale_factor);
    }

    // Calculate UV coordinates (normalized to texture dimensions)
    float u0 = i2fl(sx) / i2fl(bw);
    float v0 = i2fl(sy) / i2fl(bh);
    float u1 = i2fl(sx + w) / i2fl(bw);
    float v1 = i2fl(sy + h) / i2fl(bh);

    // Determine if scaling is needed
    bool do_resize = (resize_mode != GR_RESIZE_NONE &&
                     (gr_screen.custom_size || gr_screen.rendering_to_texture != -1));

    // Calculate screen positions with clip offset
    float x1 = i2fl(x + (do_resize ? gr_screen.offset_x_unscaled : gr_screen.offset_x));
    float y1 = i2fl(y + (do_resize ? gr_screen.offset_y_unscaled : gr_screen.offset_y));
    float x2 = x1 + i2fl(w);
    float y2 = y1 + i2fl(h);

    // Apply scaling if needed
    if (do_resize) {
        gr_resize_screen_posf(&x1, &y1, NULL, NULL, resize_mode);
        gr_resize_screen_posf(&x2, &y2, NULL, NULL, resize_mode);
    }

    // Configure material
    material render_mat;
    render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
    render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);
    render_mat.set_texture_map(TM_BASE_TYPE, gr_screen.current_bitmap);
    render_mat.set_color(clr->red, clr->green, clr->blue, clr->alpha);
    render_mat.set_cull_mode(false);

    if (aabitmap) {
        render_mat.set_texture_type(material::TEX_TYPE_AABITMAP);
    }

    draw_textured_quad(&render_mat, x1, y1, u0, v0, x2, y2, u1, v1);
}
```

### Vector Graphics Rendering (NanoVG)

Lines, rectangles, circles, and arcs are rendered via NanoVG path rendering for high-quality anti-aliased output.

#### Line Rendering

```cpp
static void gr_line(float x1, float y1, float x2, float y2, int resize_mode) {
    auto path = beginDrawing(resize_mode);

    if ((x1 == x2) && (y1 == y2)) {
        // Degenerate case: single point rendered as small circle
        path->circle(x1, y1, 1.5);
        path->setFillColor(&GR_CURRENT_COLOR);
        path->fill();
    } else {
        // Line segment
        path->moveTo(x1, y1);
        path->lineTo(x2, y2);
        path->setStrokeColor(&GR_CURRENT_COLOR);
        path->stroke();
    }

    endDrawing(path);
}
```

#### Rectangle Rendering

```cpp
void gr_rect(int x, int y, int w, int h, int resize_mode, float angle) {
    if (gr_screen.mode == GR_STUB) {
        return;
    }

    auto path = beginDrawing(resize_mode);

    if (angle != 0) {
        // Rotate around rectangle center
        float offsetX = x + w / 2.0f;
        float offsetY = y + h / 2.0f;
        path->translate(offsetX, offsetY);
        path->rotate(angle);
        path->translate(-offsetX, -offsetY);
    }

    path->rectangle(i2fl(x), i2fl(y), i2fl(w), i2fl(h));
    path->setFillColor(&GR_CURRENT_COLOR);
    path->fill();

    endDrawing(path);
}
```

#### Circle and Arc Rendering

```cpp
void gr_circle(int xc, int yc, int d, int resize_mode) {
    auto path = beginDrawing(resize_mode);
    path->circle(i2fl(xc), i2fl(yc), d / 2.0f);
    path->setFillColor(&GR_CURRENT_COLOR);
    path->fill();
    endDrawing(path);
}

void gr_unfilled_circle(int xc, int yc, int d, int resize_mode) {
    auto path = beginDrawing(resize_mode);
    path->circle(i2fl(xc), i2fl(yc), d / 2.0f);
    path->setStrokeColor(&GR_CURRENT_COLOR);
    path->stroke();
    endDrawing(path);
}

void gr_arc(int xc, int yc, float r, float angle_start, float angle_end,
            bool fill, int resize_mode) {
    // Ensure angle_start < angle_end
    if (angle_end < angle_start) {
        std::swap(angle_start, angle_end);
    }

    auto path = beginDrawing(resize_mode);

    if (fill) {
        path->arc(i2fl(xc), i2fl(yc), r,
                  fl_radians(angle_start), fl_radians(angle_end), DIR_CW);
        path->lineTo(i2fl(xc), i2fl(yc));  // Close to center for pie shape
        path->setFillColor(&GR_CURRENT_COLOR);
        path->fill();
    } else {
        path->arc(i2fl(xc), i2fl(yc), r,
                  fl_radians(angle_start), fl_radians(angle_end), DIR_CW);
        path->setStrokeColor(&GR_CURRENT_COLOR);
        path->stroke();
    }

    endDrawing(path);
}
```

---

## 6. Shader System for 2D

### Interface Shader

The interface shader (in `code/graphics/shaders/`) handles UI rendering with support for textured and untextured primitives.

**Vertex Shader** (`interface.vert`):

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Vertex attributes - position and texture coordinates only
layout (location = 0) in vec4 vertPosition;
layout (location = 2) in vec4 vertTexCoord;

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;

// Uniforms - same binding layout as default-material for compatibility
layout (binding = 0, std140) uniform matrixData {
    mat4 modelViewMatrix;
    mat4 projMatrix;
};

void main()
{
    fragTexCoord = vertTexCoord.xy;
    gl_Position = projMatrix * modelViewMatrix * vertPosition;
}
```

**Fragment Shader** (`interface.frag`):

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout (location = 0) in vec2 fragTexCoord;
layout (location = 0) out vec4 fragOut0;

layout (binding = 1, std140) uniform genericData {
    vec4 color;          // Uniform color multiplier
    int baseMapIndex;    // Texture array layer index
    int alphaTexture;    // 1 = AA bitmap (R8 mask), 0 = RGBA texture
    int noTexturing;     // 1 = solid color only, 0 = use texture
    int srgb;            // 1 = apply sRGB-to-linear conversion
    float intensity;     // Overall intensity multiplier
    float alphaThreshold; // Alpha test threshold (discard if below)
};

layout(binding = 2) uniform sampler2DArray baseMap;

void main()
{
    vec4 baseColor = texture(baseMap, vec3(fragTexCoord, float(baseMapIndex)));

    // AA bitmaps (fonts, etc.) are R8 textures - use .r for coverage
    float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
    if (alphaThreshold > coverage) discard;

    // sRGB conversion for color accuracy (skip for alpha textures)
    baseColor.rgb = (srgb == 1 && alphaTexture == 0)
        ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;

    vec4 blendColor = (srgb == 1)
        ? vec4(srgb_to_linear(color.rgb), color.a) : color;

    // Final output:
    // - If noTexturing: use blendColor directly
    // - If alphaTexture: use blendColor.rgb with baseColor.r as alpha
    // - Otherwise: multiply baseColor by blendColor
    fragOut0 = mix(
        mix(baseColor * blendColor,
            vec4(blendColor.rgb, baseColor.r * blendColor.a),
            float(alphaTexture)),
        blendColor,
        float(noTexturing)
    ) * intensity;
}
```

### Shader Type Enumeration

Relevant 2D shader types (from `code/graphics/2d.h`):

| Shader Type | Enum Value | Purpose |
|-------------|------------|---------|
| `SDR_TYPE_INTERFACE` | 201 | UI elements, bitmaps, HUD icons |
| `SDR_TYPE_BATCHED_BITMAP` | 199 | Batched particle/sprite rendering |
| `SDR_TYPE_DEFAULT_MATERIAL` | 200 | General-purpose with vertex colors |
| `SDR_TYPE_NANOVG` | 202 | NanoVG vector graphics rendering |
| `SDR_TYPE_ROCKET_UI` | 206 | RocketUI (libRocket) integration |
| `SDR_TYPE_FLAT_COLOR` | 219 | Position-only with uniform color output |

### Uniform Data Binding

The Vulkan backend prepares uniform data based on shader type. For `SDR_TYPE_INTERFACE`:

```cpp
// From code/graphics/vulkan/VulkanGraphics.cpp
genericData_interface_frag interfaceData{};
interfaceData.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};
interfaceData.baseMapIndex = baseMapIndex;
interfaceData.alphaTexture = (material_info->get_texture_type() == TCACHE_TYPE_AABITMAP) ? 1 : 0;
interfaceData.noTexturing = material_info->is_textured() ? 0 : 1;
interfaceData.srgb = 1;  // Always use sRGB for UI
interfaceData.intensity = intensity;
interfaceData.alphaThreshold = 0.f;
```

---

## 7. Integration with 3D Rendering

### Rendering Order

The typical frame rendering order in FSO:

1. **3D Scene Rendering**
   - Set perspective projection via `gr_set_proj_matrix()`
   - Set camera view via `gr_set_view_matrix()`
   - Render 3D geometry (models, effects, environment)

2. **Batched Effects**
   - Particle systems, laser effects via `batching_render_all()`
   - Uses batched bitmap material for efficiency

3. **2D Overlay (HUD)**
   - Individual draw calls invoke `gr_set_2d_matrix()` internally
   - Orthographic projection overlays on the 3D scene
   - Z-buffer is typically disabled for 2D

4. **Buffered NanoVG**
   - `gr_2d_start_buffer()` / `gr_2d_stop_buffer()` batches vector ops
   - Reduces draw call overhead for complex 2D scenes

### Matrix State Tracking

The matrix system tracks current mode via static flags (in `code/graphics/matrix.cpp`):

```cpp
static int modelview_matrix_depth = 1;      // Nested model matrix count
static bool htl_view_matrix_set = false;    // Is 3D view matrix active?
static int htl_2d_matrix_depth = 0;         // Nested 2D matrix count
static bool htl_2d_matrix_set = false;      // Is 2D mode active?
```

**Invariant**: At most one 2D matrix level should be active at a time (`htl_2d_matrix_depth` is 0 or 1).

### Transition Between 2D and 3D

When rendering 2D content during a 3D scene:

```cpp
void gr_render_primitives_2d_immediate(...) {
    gr_set_2d_matrix();      // Save 3D matrices, set orthographic

    // ... render 2D content ...

    gr_end_2d_matrix();      // Restore 3D matrices
}
```

This pattern is used internally by most 2D draw functions, so callers typically do not need to manage 2D/3D transitions manually.

### Render Target Considerations

When rendering to a texture (RTT) vs. the backbuffer:

| Aspect | Backbuffer | Render-to-Texture |
|--------|------------|-------------------|
| Y direction | Swapped (screen-space) | Standard (Y up) |
| Viewport Y offset | Inverted calculation | Direct calculation |
| Projection bottom/top | Swapped | Standard |

The engine handles these differences automatically in `gr_set_2d_matrix()` and `gr_set_viewport()`.

---

## 8. Performance Considerations and Optimization

### 2D Buffering System

The NanoVG buffering system reduces draw calls by batching multiple path operations:

```cpp
void gr_2d_start_buffer() {
    Assertion(!buffering_nanovg, "Tried to enable 2D buffering but it was already enabled!");
    buffering_nanovg = true;
    auto path = graphics::paths::PathRenderer::instance();
    path->beginFrame();  // Start batching all path operations
}

void gr_2d_stop_buffer() {
    Assertion(buffering_nanovg, "Tried to stop 2D buffering but it was not enabled!");
    buffering_nanovg = false;
    auto path = graphics::paths::PathRenderer::instance();
    path->endFrame();  // Flush all batched operations in a single draw
}
```

**Benefits**:
- Multiple lines, circles, arcs batched into single NanoVG frame
- Reduces CPU overhead from state changes
- Minimizes GPU draw call overhead
- Especially beneficial for complex HUD elements

**Usage pattern**:
```cpp
gr_2d_start_buffer();
// Render many 2D primitives...
gr_line(...);
gr_circle(...);
gr_rect(...);
gr_2d_stop_buffer();  // Single GPU submission
```

### Immediate Buffer Management

The immediate buffer uses a **buffer orphaning pattern** to avoid GPU synchronization stalls:

```cpp
if (immediate_buffer_offset + size > immediate_buffer_size) {
    immediate_buffer_offset = 0;  // Reset offset to beginning
    immediate_buffer_size += MAX(IMMEDIATE_BUFFER_RESIZE_BLOCK_SIZE, size);
    gr_resize_buffer(gr_immediate_buffer_handle, immediate_buffer_size);  // Orphan old buffer
}
```

**Why this works**:
- `gr_resize_buffer()` allocates new backing storage
- Old buffer data remains valid until GPU finishes reading it
- No CPU/GPU synchronization required
- Trade-off: slightly higher memory usage

### Frame Reset

Each frame resets the immediate buffer offset:

```cpp
void gr_reset_immediate_buffer() {
    if (!gr_immediate_buffer_handle.isValid()) {
        return;  // Never used this frame
    }

    // Orphan buffer for new frame
    gr_resize_buffer(gr_immediate_buffer_handle, immediate_buffer_size);
    immediate_buffer_offset = 0;
}
```

### Batched Bitmap Rendering

For particle effects and sprites, the batching system accumulates geometry and issues a single draw call per texture/material combination:

**Optimization strategies**:
- Group draw calls by texture to minimize descriptor updates
- Use texture arrays for varied sprites (single bind, multiple layers)
- Pre-sort by blend mode to reduce pipeline switches
- Coalesce adjacent primitives into larger vertex buffers

### Push Descriptors

The Vulkan backend uses push descriptors for per-draw data, avoiding descriptor set allocation overhead:

```cpp
std::array<vk::WriteDescriptorSet, 3> writes{};

writes[0].dstBinding = 0;  // Matrix UBO
writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
writes[0].pBufferInfo = &matrixInfo;

writes[1].dstBinding = 1;  // Generic data UBO
writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
writes[1].pBufferInfo = &genericInfo;

writes[2].dstBinding = 2;  // Texture sampler
writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
writes[2].pImageInfo = &baseMapInfo;

cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,
    pipelineLayout, 0, writes.size(), writes.data());
```

**Benefits of push descriptors**:
- No descriptor pool exhaustion concerns
- Lower overhead than descriptor set allocation
- Ideal for frequently changing per-draw data
- No need to track descriptor set lifetimes

### Dynamic State Usage

The Vulkan backend uses extensive dynamic state to minimize pipeline object count:

```cpp
// Dynamic state set per-draw
cmd.setPrimitiveTopology(topology);
cmd.setCullMode(cullMode);
cmd.setFrontFace(vk::FrontFace::eClockwise);
cmd.setDepthTestEnable(depthTest);
cmd.setDepthWriteEnable(depthWrite);
cmd.setDepthCompareOp(compareOp);

// Extended dynamic state 3 (if available)
if (supportsExtendedDynamicState3()) {
    cmd.setColorBlendEnableEXT(0, &blendEnable);
    cmd.setColorBlendEquationEXT(0, &blendEquation);
}

// Always dynamic in Vulkan 1.3
cmd.setViewport(0, 1, &viewport);
cmd.setScissor(0, 1, &scissor);
```

**Benefits**:
- Fewer pipeline objects needed (share pipelines across blend modes)
- Reduced pipeline creation overhead at startup
- More flexible state changes without pipeline switches

---

## 9. Thread Safety and Synchronization

### Single-Threaded Rendering

The FSO rendering system is **single-threaded**. All graphics calls must occur on the main thread. The following constraints apply:

- `gr_screen` is a global variable accessed without synchronization
- Command recording occurs sequentially on the main thread
- No concurrent access to graphics state is permitted

### Frames-In-Flight

The Vulkan backend uses multiple frames-in-flight (typically 2) to overlap CPU and GPU work:

```
Frame N:   [Record]-------------------[Submit]------>
Frame N+1:            [Record]-------------------[Submit]------>
GPU:       -------[Execute N]-------[Execute N+1]------>
```

**Per-frame resources**:
- Command buffers
- Uniform buffers (via ring buffer or per-frame allocation)
- Immediate vertex buffers (orphaned each frame)

**Synchronization**:
- Fence per frame-in-flight to wait before reusing resources
- Semaphores for acquire/present synchronization
- See [VULKAN_SYNCHRONIZATION.md](VULKAN_SYNCHRONIZATION.md) for details

### Buffer Upload Synchronization

Dynamic buffer updates (immediate buffer, uniform buffers) are designed to avoid synchronization:

- **Buffer orphaning**: Create new backing storage instead of modifying in-use data
- **Ring buffers**: Rotate through per-frame slices
- **Write-no-overwrite**: Only append to buffers, never modify previous data in same frame

---

## 10. Debugging and Troubleshooting

### Common Issues

#### Scissor Validation Errors

**Symptom**: Vulkan validation layer reports "scissor extent exceeds framebuffer" or "negative scissor offset".

**Cause**: Some engine paths (HUD jitter, window resize) can produce out-of-bounds clip regions.

**Solution**: The `clampClipScissorToFramebuffer()` function handles this. If errors persist, verify:
- `gr_screen.max_w` and `gr_screen.max_h` are positive
- Clip region is being clamped before `vkCmdSetScissor`

#### Missing 2D Content

**Symptom**: 2D elements (HUD, menus) don't appear.

**Possible causes**:
1. Z-buffer not disabled - 2D should use `ZBUFFER_TYPE_NONE`
2. Blend mode incorrect - verify `ALPHA_BLEND_ALPHA_BLEND_ALPHA`
3. Scissor too restrictive - check clip region
4. Wrong render target - ensure rendering to swapchain for UI

**Debug steps**:
```cpp
// Add debug logging in render path
mprintf(("2D render: clip=(%d,%d,%d,%d) bitmap=%d\n",
    gr_screen.offset_x, gr_screen.offset_y,
    gr_screen.clip_width, gr_screen.clip_height,
    gr_screen.current_bitmap));
```

#### Coordinate Mismatch

**Symptom**: 2D elements appear in wrong position or are scaled incorrectly.

**Check**:
- `resize_mode` parameter matches expected behavior
- `gr_screen.custom_size` reflects actual resolution state
- RTT vs. backbuffer Y-flip is handled correctly

#### Font Rendering Issues

**Symptom**: Text appears garbled or with wrong colors.

**Check**:
- `alphaTexture` flag is set correctly for font bitmaps (should be 1)
- Font textures are R8 format (not RGBA)
- sRGB conversion is enabled (`srgb = 1`)

### Debug Scope Macros

The engine uses debug scope macros for GPU profiling and debugging:

```cpp
GR_DEBUG_SCOPE("Draw AA-bitmap");
// ... rendering code ...
```

These integrate with RenderDoc and validation layers to provide labeled regions in captures.

### Validation Layer Integration

Enable Vulkan validation layers for debugging:

1. Set environment variable: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
2. Or enable via FSO command line (if supported)

Common validation messages and their meanings:
- "Scissor count must match viewport count" - dynamic state mismatch
- "Image layout transition invalid" - missing barrier
- "Descriptor binding not updated" - push descriptor issue

---

## 11. File Reference

### Core 2D Rendering Files

| File | Purpose |
|------|---------|
| `code/graphics/render.h` | Public 2D API declarations |
| `code/graphics/render.cpp` | 2D rendering implementation (primitives, bitmaps, text) |
| `code/graphics/2d.h` | Screen state struct, shader types, resize mode defines |
| `code/graphics/matrix.h` | Matrix function declarations |
| `code/graphics/matrix.cpp` | Projection/view matrix implementation |

### Vulkan Backend Files

| File | Purpose |
|------|---------|
| `code/graphics/vulkan/VulkanGraphics.cpp` | Vulkan rendering commands and state management |
| `code/graphics/vulkan/VulkanClip.h` | Scissor/clip computation and clamping |
| `code/graphics/vulkan/VulkanRenderer.h` | Renderer class declaration |
| `code/graphics/vulkan/VulkanRenderer.cpp` | Renderer implementation |

### Shader Files

| File | Purpose |
|------|---------|
| `code/graphics/shaders/interface.vert` | Interface vertex shader (2D UI) |
| `code/graphics/shaders/interface.frag` | Interface fragment shader (texture/color blending) |
| `code/graphics/shaders/default-material.vert` | Default material vertex shader |
| `code/graphics/shaders/default-material.frag` | Default material fragment shader |
| `code/graphics/shaders/nanovg.vert` | NanoVG vector graphics vertex shader |
| `code/graphics/shaders/nanovg.frag` | NanoVG vector graphics fragment shader |
| `code/graphics/shaders/gamma.sdr` | sRGB conversion functions (included by others) |

### Key Functions Quick Reference

| Function | File | Purpose |
|----------|------|---------|
| `gr_bitmap()` | `2d.cpp` | Draw textured rectangle at position |
| `gr_aabitmap()` | `render.cpp` | Draw anti-aliased (font) bitmap |
| `gr_string()` | `render.cpp` | Render text string |
| `gr_line()` | `render.cpp` | Draw line segment |
| `gr_rect()` | `render.cpp` | Draw filled rectangle |
| `gr_circle()` | `render.cpp` | Draw filled circle |
| `gr_arc()` | `render.cpp` | Draw arc (filled or outline) |
| `gr_set_2d_matrix()` | `matrix.cpp` | Enable 2D orthographic projection |
| `gr_end_2d_matrix()` | `matrix.cpp` | Restore previous 3D projection |
| `gr_set_clip()` | `2d.h` | Set clipping region |
| `gr_reset_clip()` | `2d.h` | Reset clip to full screen |
| `gr_render_primitives_2d_immediate()` | `render.cpp` | Immediate 2D primitive rendering |
| `gr_2d_start_buffer()` | `render.cpp` | Begin NanoVG batching |
| `gr_2d_stop_buffer()` | `render.cpp` | End NanoVG batching and flush |
| `createFullScreenViewport()` | `VulkanGraphics.cpp` | Create Y-flipped viewport |
| `createClipScissor()` | `VulkanGraphics.cpp` | Create scissor from clip state |
| `applyClipToScreen()` | `VulkanClip.h` | Update gr_screen clip state |
| `clampClipScissorToFramebuffer()` | `VulkanClip.h` | Clamp scissor to valid bounds |

---

## Appendix A: Vulkan vs OpenGL Differences

### Coordinate System Comparison

| Aspect | OpenGL | Vulkan |
|--------|--------|--------|
| NDC Y direction | -1 (bottom) to +1 (top) | -1 (top) to +1 (bottom) |
| Depth range | [-1, 1] | [0, 1] |
| Framebuffer origin | Bottom-left | Top-left |
| Texture origin | Bottom-left | Top-left |

### Viewport Y-Flip Solution

The Vulkan backend uses negative viewport height to match OpenGL conventions:

```cpp
viewport.y = gr_screen.max_h;      // Start at bottom edge
viewport.height = -gr_screen.max_h; // Negative height flips Y
```

This allows existing screen-space code (Y=0 at top) to work unchanged with Vulkan.

### Scissor Handling

| Aspect | OpenGL | Vulkan |
|--------|--------|--------|
| Scissor enable | Can be disabled (`glDisable(GL_SCISSOR_TEST)`) | Always active |
| Default scissor | Full framebuffer when disabled | Must be explicitly set |
| Negative offsets | Clamped automatically | Validation error |

**FSO solution**: Set scissor to full screen at frame start; update dynamically on each clip change; clamp to valid bounds.

### Projection Matrix Depth Mapping

```cpp
if (gr_screen.mode == GR_VULKAN) {
    // Vulkan [0, 1] depth: maps near to 0, far to 1
    out->a1d[10] = -1.0f / (far_dist - near_dist);
    out->a1d[14] = -near_dist / (far_dist - near_dist);
} else {
    // OpenGL [-1, 1] depth: maps near to -1, far to 1
    out->a1d[10] = -2.0f / (far_dist - near_dist);
    out->a1d[14] = -(far_dist + near_dist) / (far_dist - near_dist);
}
```

---

## Appendix B: Resize Mode Behavior Details

| Mode | Scaling | Offset | Clamping | Use Case |
|------|---------|--------|----------|----------|
| `GR_RESIZE_NONE` | None | None | To screen bounds | Raw pixel rendering |
| `GR_RESIZE_FULL` | To screen aspect | Centered | Yes | General UI elements |
| `GR_RESIZE_FULL_CENTER` | To screen aspect | Always centered | Yes | Centered dialogs |
| `GR_RESIZE_MENU` | To menu aspect | Letterboxed | Yes | Main menu backgrounds |
| `GR_RESIZE_MENU_ZOOMED` | With zoom factor | Letterboxed + zoom | Yes | Zoomed menu elements |
| `GR_RESIZE_MENU_NO_OFFSET` | To menu aspect | No offset | Yes | Full-bleed menu elements |
| `GR_RESIZE_REPLACE` | None | None | **No** | Direct pixel access (e.g., debug) |

**Note**: `GR_RESIZE_REPLACE` bypasses all bounds checking and is intended for special cases where exact pixel control is required.
