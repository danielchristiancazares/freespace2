# 2D Rendering Pipeline Architecture

This document provides comprehensive technical documentation of the FreeSpace 2 Open 2D rendering pipeline, with specific focus on the Vulkan backend implementation. The 2D pipeline handles all user interface elements including the HUD, menus, text, bitmaps, and vector graphics.

## Table of Contents

1. [Overview](#1-overview)
2. [Screen-Space to NDC Coordinate Conversion](#2-screen-space-to-ndc-coordinate-conversion)
3. [Viewport and Scissor Management](#3-viewport-and-scissor-management)
4. [Orthographic Projection Setup](#4-orthographic-projection-setup)
5. [2D Primitive Rendering](#5-2d-primitive-rendering)
6. [Shader System for 2D](#6-shader-system-for-2d)
7. [Integration with 3D Rendering](#7-integration-with-3d-rendering)
8. [Performance Considerations and Optimization](#8-performance-considerations-and-optimization)
9. [File Reference](#9-file-reference)

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
| 2D API | `render.h`, `render.cpp` | Public drawing functions |
| Screen State | `2d.h` (struct screen) | Viewport, clip regions, current state |
| Matrix System | `matrix.h`, `matrix.cpp` | Projection and view matrix management |
| Vulkan Backend | `VulkanGraphics.cpp` | GPU command recording |
| Clip Management | `VulkanClip.h` | Scissor rectangle computation |

---

## 2. Screen-Space to NDC Coordinate Conversion

### Coordinate System

FreeSpace 2 uses a screen-space coordinate system where:
- Origin (0, 0) is at the top-left corner
- X increases rightward
- Y increases downward
- Coordinates are in pixels

This matches typical 2D graphics conventions but differs from OpenGL/Vulkan NDC (Normalized Device Coordinates) where Y increases upward.

### The gr_screen Structure

The global `gr_screen` variable (`code/graphics/2d.h:667-717`) maintains all screen state:

```cpp
typedef struct screen {
    int max_w, max_h;           // Physical screen dimensions
    int max_w_unscaled, max_h_unscaled;  // Logical (unscaled) dimensions

    int offset_x, offset_y;     // Current clip region offset
    int clip_width, clip_height; // Current clip region size

    int clip_left, clip_right, clip_top, clip_bottom;  // Clip boundaries

    // Unscaled variants for resolution-independent UI
    int offset_x_unscaled, offset_y_unscaled;
    int clip_width_unscaled, clip_height_unscaled;

    float clip_aspect;          // clip_width / clip_height
    float clip_center_x, clip_center_y;  // Center of clip region

    int rendering_to_texture;   // -1 for backbuffer, else RTT handle
    // ...
} screen;
```

### Resize Modes

The engine supports multiple resize modes for scaling UI elements (`code/graphics/2d.h:1049-1055`):

| Mode | Value | Behavior |
|------|-------|----------|
| `GR_RESIZE_NONE` | 0 | No scaling, use raw pixel coordinates |
| `GR_RESIZE_FULL` | 1 | Scale to fill screen maintaining aspect ratio |
| `GR_RESIZE_FULL_CENTER` | 2 | Scale and center on screen |
| `GR_RESIZE_MENU` | 3 | Menu-specific scaling with letterboxing |
| `GR_RESIZE_MENU_ZOOMED` | 4 | Menu scaling with zoom factor |
| `GR_RESIZE_MENU_NO_OFFSET` | 5 | Menu scaling without offset adjustment |
| `GR_RESIZE_REPLACE` | 6 | Direct replacement, bypass all scaling |

### Coordinate Transformation Functions

**Screen Position Scaling** (`code/graphics/2d.h:1059-1062`):

```cpp
// Scale logical coordinates to physical screen coordinates
bool gr_resize_screen_pos(int *x, int *y, int *w, int *h, int resize_mode);
bool gr_resize_screen_posf(float *x, float *y, float *w, float *h, int resize_mode);

// Reverse: physical to logical coordinates
bool gr_unsize_screen_pos(int *x, int *y, int *w, int *h, int resize_mode);
bool gr_unsize_screen_posf(float *x, float *y, float *w, float *h, int resize_mode);
```

These functions handle the translation between:
- **Unscaled coordinates**: Resolution-independent logical coordinates (e.g., 1024x768 base)
- **Scaled coordinates**: Actual pixel positions on the physical display

---

## 3. Viewport and Scissor Management

### Vulkan Viewport Configuration

The Vulkan backend uses a Y-flip viewport to match the OpenGL/screen-space coordinate convention (`code/graphics/vulkan/VulkanGraphics.cpp:138-148`):

```cpp
vk::Viewport createFullScreenViewport()
{
    vk::Viewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(gr_screen.max_h);  // Y at bottom
    viewport.width = static_cast<float>(gr_screen.max_w);
    viewport.height = -static_cast<float>(gr_screen.max_h);  // Negative height for Y-flip
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    return viewport;
}
```

This viewport configuration:
- Sets viewport origin at bottom-left (y = max_h)
- Uses negative height to flip Y axis
- Allows screen-space coordinates (0,0 = top-left) to work correctly

### Scissor Rectangle Computation

The scissor rectangle is derived from the current clip region (`code/graphics/vulkan/VulkanClip.h:10-58`):

```cpp
struct ClipScissorRect {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Extract clip from gr_screen
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

Vulkan requires non-negative scissor offsets. The engine clamps scissors to the framebuffer bounds (`code/graphics/vulkan/VulkanClip.h:29-58`):

```cpp
inline ClipScissorRect clampClipScissorToFramebuffer(
    const ClipScissorRect& in,
    int32_t fbWidth,
    int32_t fbHeight)
{
    // Intersect scissor with [0, fbW) x [0, fbH)
    int64_t x0 = clamp(in.x, 0, fbW);
    int64_t y0 = clamp(in.y, 0, fbH);
    int64_t x1 = clamp(in.x + in.width, 0, fbW);
    int64_t y1 = clamp(in.y + in.height, 0, fbH);

    out.x = x0;
    out.y = y0;
    out.width = max(0, x1 - x0);
    out.height = max(0, y1 - y0);
    return out;
}
```

### Clip State Application

The `applyClipToScreen()` function (`code/graphics/vulkan/VulkanClip.h:65-143`) updates `gr_screen` clip state:

```cpp
inline void applyClipToScreen(int x, int y, int w, int h, int resize_mode)
{
    // Sanity clamp inputs
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    // Determine if scaling is needed
    const bool to_resize = (resize_mode != GR_RESIZE_NONE &&
                           resize_mode != GR_RESIZE_REPLACE &&
                           (gr_screen.custom_size || gr_screen.rendering_to_texture != -1));

    // Clamp to screen bounds
    int max_w = to_resize ? gr_screen.max_w_unscaled : gr_screen.max_w;
    int max_h = to_resize ? gr_screen.max_h_unscaled : gr_screen.max_h;

    // Apply resize transformation if needed
    if (to_resize) {
        gr_resize_screen_pos(&x, &y, &w, &h, resize_mode);
    }

    // Update both scaled and unscaled clip state
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

When the clip region changes, the Vulkan backend immediately updates the dynamic scissor state (`code/graphics/vulkan/VulkanGraphics.cpp:371-377`):

```cpp
if (g_backend && g_backend->recording.has_value()) {
    auto ctxBase = currentFrameCtx();
    vk::Rect2D scissor = createClipScissor();
    ctxBase.renderer.setScissor(ctxBase, scissor);
}
```

---

## 4. Orthographic Projection Setup

### Projection Matrix Creation

The orthographic projection matrix maps screen coordinates directly to NDC (`code/graphics/matrix.cpp:70-90`):

```cpp
static void create_orthographic_projection_matrix(
    matrix4* out,
    float left, float right,
    float bottom, float top,
    float near_dist, float far_dist)
{
    memset(out, 0, sizeof(matrix4));

    out->a1d[0] = 2.0f / (right - left);           // X scale
    out->a1d[5] = 2.0f / (top - bottom);           // Y scale
    out->a1d[12] = -(right + left) / (right - left);  // X translation
    out->a1d[13] = -(top + bottom) / (top - bottom);  // Y translation
    out->a1d[15] = 1.0f;

    // Vulkan uses [0, 1] depth range (OpenGL uses [-1, 1])
    if (gr_screen.mode == GR_VULKAN) {
        out->a1d[10] = -1.0f / (far_dist - near_dist);
        out->a1d[14] = -near_dist / (far_dist - near_dist);
    } else {
        out->a1d[10] = -2.0f / (far_dist - near_dist);
        out->a1d[14] = -(far_dist + near_dist) / (far_dist - near_dist);
    }
}
```

### 2D Matrix Setup

The `gr_set_2d_matrix()` function (`code/graphics/matrix.cpp:246-282`) configures rendering for 2D mode:

```cpp
void gr_set_2d_matrix()
{
    // Skip if no 3D projection is active
    if (!gr_htl_projection_matrix_set) {
        return;
    }

    // Set viewport to full screen
    gr_set_viewport(0, 0, gr_screen.max_w, gr_screen.max_h);

    // Save current projection for restoration
    gr_last_projection_matrix = gr_projection_matrix;

    // Create orthographic projection
    // Note: top/bottom are flipped for screen-space coordinates
    if (gr_screen.rendering_to_texture != -1) {
        // RTT: Y increases upward
        create_orthographic_projection_matrix(&gr_projection_matrix,
            0, i2fl(gr_screen.max_w),
            0, i2fl(gr_screen.max_h),
            -1, 1);
    } else {
        // Backbuffer: Y increases downward (screen-space)
        create_orthographic_projection_matrix(&gr_projection_matrix,
            0, i2fl(gr_screen.max_w),
            i2fl(gr_screen.max_h), 0,  // Note: swapped top/bottom
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
}
```

### End 2D Matrix

The `gr_end_2d_matrix()` function (`code/graphics/matrix.cpp:284-308`) restores 3D projection state:

```cpp
void gr_end_2d_matrix()
{
    if (!htl_2d_matrix_set)
        return;

    // Restore viewport to 3D clip region
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
}
```

---

## 5. 2D Primitive Rendering

### Core Rendering Functions

#### Immediate Buffer System

The immediate buffer provides dynamic vertex storage for 2D primitives (`code/graphics/render.cpp:1227-1277`):

```cpp
gr_buffer_handle gr_immediate_buffer_handle;
static size_t immediate_buffer_offset = 0;
static size_t immediate_buffer_size = 0;
static const size_t IMMEDIATE_BUFFER_RESIZE_BLOCK_SIZE = 2048;

size_t gr_add_to_immediate_buffer(size_t size, void* data) {
    if (!gr_immediate_buffer_handle.isValid()) {
        gr_immediate_buffer_handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Dynamic);
    }

    // Resize if needed (orphan buffer pattern)
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

#### 2D Immediate Rendering

The `gr_render_primitives_2d_immediate()` function (`code/graphics/render.cpp:1294-1310`) wraps 2D matrix setup:

```cpp
void gr_render_primitives_2d_immediate(
    material* material_info,
    primitive_type prim_type,
    vertex_layout* layout,
    int n_verts,
    void* data,
    size_t size)
{
    gr_set_2d_matrix();  // Enable orthographic projection

    gr_render_primitives_immediate(material_info, prim_type, layout, n_verts, data, size);

    gr_end_2d_matrix();  // Restore previous projection
}
```

### Bitmap Rendering

#### gr_aabitmap (Anti-Aliased Bitmap)

Renders grayscale bitmaps multiplied with current color (`code/graphics/render.cpp:199-278`):

```cpp
void gr_aabitmap(int x, int y, int resize_mode, bool mirror, float scale_factor) {
    int w, h;
    bm_get_info(gr_screen.current_bitmap, &w, &h);

    if (scale_factor != 1.0f) {
        w = static_cast<int>(w * scale_factor);
        h = static_cast<int>(h * scale_factor);
    }

    // Clip to visible region
    int clip_left = do_resize ? gr_screen.clip_left_unscaled : gr_screen.clip_left;
    // ... clipping logic ...

    // Render with interface material
    bitmap_ex_internal(dx1, dy1, (dx2 - dx1 + 1), (dy2 - dy1 + 1),
                       sx, sy, resize_mode, true, mirror, &GR_CURRENT_COLOR, scale_factor);
}
```

#### Internal Bitmap Rendering

The `bitmap_ex_internal()` function (`code/graphics/render.cpp:120-197`) creates the textured quad:

```cpp
static void bitmap_ex_internal(int x, int y, int w, int h, int sx, int sy,
                               int resize_mode, bool aabitmap, bool mirror,
                               color* clr, float scale_factor) {
    // Calculate UV coordinates
    u0 = i2fl(sx) / i2fl(bw);
    v0 = i2fl(sy) / i2fl(bh);
    u1 = i2fl(sx + w) / i2fl(bw);
    v1 = i2fl(sy + h) / i2fl(bh);

    // Calculate screen positions with offset
    x1 = i2fl(x + (do_resize ? gr_screen.offset_x_unscaled : gr_screen.offset_x));
    y1 = i2fl(y + (do_resize ? gr_screen.offset_y_unscaled : gr_screen.offset_y));
    x2 = x1 + i2fl(w);
    y2 = y1 + i2fl(h);

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

### Line Rendering

Lines are rendered via NanoVG path rendering (`code/graphics/render.cpp:976-1001`):

```cpp
static void gr_line(float x1, float y1, float x2, float y2, int resize_mode) {
    auto path = beginDrawing(resize_mode);

    if ((x1 == x2) && (y1 == y2)) {
        // Point: draw as small circle
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

### Rectangle Rendering

Filled rectangles use path rendering (`code/graphics/render.cpp:1158-1177`):

```cpp
void gr_rect(int x, int y, int w, int h, int resize_mode, float angle) {
    auto path = beginDrawing(resize_mode);

    if (angle != 0) {
        // Rotate around center
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

### Circle and Arc Rendering

```cpp
void gr_circle(int xc, int yc, int d, int resize_mode) {
    auto path = beginDrawing(resize_mode);
    path->circle(i2fl(xc), i2fl(yc), d / 2.0f);
    path->setFillColor(&GR_CURRENT_COLOR);
    path->fill();
    endDrawing(path);
}

void gr_arc(int xc, int yc, float r, float angle_start, float angle_end,
            bool fill, int resize_mode) {
    auto path = beginDrawing(resize_mode);
    path->arc(i2fl(xc), i2fl(yc), r,
              fl_radians(angle_start), fl_radians(angle_end), DIR_CW);

    if (fill) {
        path->lineTo(i2fl(xc), i2fl(yc));
        path->setFillColor(&GR_CURRENT_COLOR);
        path->fill();
    } else {
        path->setStrokeColor(&GR_CURRENT_COLOR);
        path->stroke();
    }
    endDrawing(path);
}
```

---

## 6. Shader System for 2D

### Interface Shader

The interface shader (`code/graphics/shaders/interface.vert`, `interface.frag`) handles UI rendering:

**Vertex Shader** (`interface.vert:1-21`):
```glsl
#version 450

layout (location = 0) in vec4 vertPosition;
layout (location = 2) in vec4 vertTexCoord;

layout (location = 0) out vec2 fragTexCoord;

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

**Fragment Shader** (`interface.frag:1-46`):
```glsl
#version 450

layout (location = 0) in vec2 fragTexCoord;
layout (location = 0) out vec4 fragOut0;

layout (binding = 1, std140) uniform genericData {
    vec4 color;
    int baseMapIndex;
    int alphaTexture;  // 1 = AA bitmap (R8 mask)
    int noTexturing;   // 1 = solid color only
    int srgb;          // 1 = apply sRGB conversion
    float intensity;
    float alphaThreshold;
};

layout(binding = 2) uniform sampler2DArray baseMap;

void main()
{
    vec4 baseColor = texture(baseMap, vec3(fragTexCoord, float(baseMapIndex)));

    // AA bitmaps use R channel as alpha mask
    float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
    if (alphaThreshold > coverage) discard;

    // sRGB conversion for color accuracy
    baseColor.rgb = (srgb == 1 && alphaTexture == 0)
        ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;
    vec4 blendColor = (srgb == 1)
        ? vec4(srgb_to_linear(color.rgb), color.a) : color;

    // Final output with texture/color mixing
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

Relevant 2D shader types (`code/graphics/2d.h:180-205`):

| Shader Type | Purpose |
|-------------|---------|
| `SDR_TYPE_INTERFACE` | UI elements, bitmaps, HUD |
| `SDR_TYPE_BATCHED_BITMAP` | Batched particle/sprite rendering |
| `SDR_TYPE_DEFAULT_MATERIAL` | General-purpose with vertex colors |
| `SDR_TYPE_NANOVG` | NanoVG vector graphics rendering |
| `SDR_TYPE_ROCKET_UI` | RocketUI integration |
| `SDR_TYPE_FLAT_COLOR` | Solid color primitives |

### Uniform Data Structures

The Vulkan backend prepares uniform data based on shader type (`code/graphics/vulkan/VulkanGraphics.cpp:1163-1241`):

```cpp
// For SDR_TYPE_INTERFACE
genericData_interface_frag interfaceData{};
interfaceData.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};
interfaceData.baseMapIndex = baseMapIndex;
interfaceData.alphaTexture = (material_info->get_texture_type() == TCACHE_TYPE_AABITMAP) ? 1 : 0;
interfaceData.noTexturing = material_info->is_textured() ? 0 : 1;
interfaceData.srgb = 1;
interfaceData.intensity = intensity;
interfaceData.alphaThreshold = 0.f;
```

---

## 7. Integration with 3D Rendering

### Rendering Order

The typical frame rendering order:

1. **3D Scene Rendering**
   - Set perspective projection via `gr_set_proj_matrix()`
   - Set camera view via `gr_set_view_matrix()`
   - Render 3D geometry (models, effects)

2. **Batched Effects**
   - Particle systems, laser effects via `batching_render_all()`
   - Uses batched bitmap material for efficiency

3. **2D Overlay**
   - HUD elements call `gr_set_2d_matrix()` internally
   - Orthographic projection overlays on 3D scene

4. **Buffered NanoVG**
   - `gr_2d_start_buffer()` / `gr_2d_stop_buffer()` batches vector ops

### Matrix State Machine

The matrix system tracks current mode (`code/graphics/matrix.cpp:21-28`):

```cpp
static int modelview_matrix_depth = 1;
static bool htl_view_matrix_set = false;
static int htl_2d_matrix_depth = 0;
static bool htl_2d_matrix_set = false;
```

### Transition Between 2D and 3D

When rendering 2D during 3D scene (`gr_render_primitives_2d_immediate`):

```cpp
void gr_render_primitives_2d_immediate(...) {
    gr_set_2d_matrix();      // Save 3D matrices, set orthographic

    // ... render 2D content ...

    gr_end_2d_matrix();      // Restore 3D matrices
}
```

### Vulkan Render Target Management

The Vulkan backend manages render target transitions (`code/graphics/vulkan/VulkanGraphics.cpp:1718-1751`):

```cpp
// For RocketUI and other UI rendering
void gr_vulkan_render_rocket_primitives(...) {
    // RocketUI expects 2D projection
    gr_set_2d_matrix();

    // Ensure rendering to swapchain for UI
    auto renderCtx = ctxBase.renderer.ensureRenderingStarted(ctxBase);
    auto rt = renderCtx.targetInfo;

    if (rt.colorAttachmentCount != 1 || rt.colorFormat != swapchainFormat) {
        ctxBase.renderer.setPendingRenderTargetSwapchain();
        renderCtx = ctxBase.renderer.ensureRenderingStarted(ctxBase);
    }

    // ... render UI ...
}
```

---

## 8. Performance Considerations and Optimization

### 2D Buffering System

The NanoVG buffering system reduces draw calls (`code/graphics/render.cpp:1201-1225`):

```cpp
void gr_2d_start_buffer() {
    Assertion(!buffering_nanovg, "2D buffering already enabled!");
    buffering_nanovg = true;

    auto path = graphics::paths::PathRenderer::instance();
    path->beginFrame();  // Start batching all path operations
}

void gr_2d_stop_buffer() {
    Assertion(buffering_nanovg, "2D buffering not enabled!");
    buffering_nanovg = false;

    auto path = graphics::paths::PathRenderer::instance();
    path->endFrame();  // Flush all batched operations
}
```

**Benefits**:
- Multiple lines, circles, arcs batched into single NanoVG frame
- Reduces CPU overhead from state changes
- Minimizes GPU draw call overhead

### Immediate Buffer Management

The immediate buffer uses buffer orphaning pattern (`code/graphics/render.cpp:1245-1260`):

```cpp
if (immediate_buffer_offset + size > immediate_buffer_size) {
    immediate_buffer_offset = 0;  // Reset to start
    immediate_buffer_size += MAX(IMMEDIATE_BUFFER_RESIZE_BLOCK_SIZE, size);
    gr_resize_buffer(gr_immediate_buffer_handle, immediate_buffer_size);  // Orphan
}
```

**Why This Works**:
- Avoids GPU stalls from modifying in-use buffer
- `gr_resize_buffer` creates new backing storage
- Previous frame's data remains valid until GPU finishes

### Frame Reset

Each frame resets the immediate buffer (`code/graphics/render.cpp:1262-1277`):

```cpp
void gr_reset_immediate_buffer() {
    if (!gr_immediate_buffer_handle.isValid()) {
        return;  // Never used
    }

    // Orphan buffer for new frame
    gr_resize_buffer(gr_immediate_buffer_handle, immediate_buffer_size);
    immediate_buffer_offset = 0;
}
```

### Batched Bitmap Rendering

For particle effects and sprites, the batching system accumulates geometry:

```cpp
// Batched rendering uses single draw call per texture
void gr_vulkan_render_primitives_batched(
    batched_bitmap_material* material_info,
    primitive_type prim_type,
    vertex_layout* layout,
    int offset,
    int n_verts,
    gr_buffer_handle buffer_handle)
```

**Optimization Strategies**:
- Group by texture to minimize descriptor updates
- Use texture arrays for varied sprites
- Pre-sort by blend mode to reduce pipeline switches

### Push Descriptors

The Vulkan backend uses push descriptors for per-draw data (`code/graphics/vulkan/VulkanGraphics.cpp:1274-1301`):

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
    ctxBase.renderer.getPipelineLayout(), 0, 3, writes.data());
```

**Benefits of Push Descriptors**:
- No descriptor pool exhaustion
- Lower overhead than descriptor set allocation
- Ideal for frequently changing per-draw data

### Dynamic State Usage

The Vulkan backend uses extensive dynamic state (`code/graphics/vulkan/VulkanGraphics.cpp:1308-1354`):

```cpp
// Primitive topology
cmd.setPrimitiveTopology(convertPrimitiveType(prim_type));

// Cull mode
cmd.setCullMode(material_info->get_cull_mode() ?
    vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone);
cmd.setFrontFace(vk::FrontFace::eClockwise);

// Depth state
cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);

// Extended dynamic state 3 (blend, color write mask)
if (ctxBase.renderer.supportsExtendedDynamicState3()) {
    if (caps.colorBlendEnable) {
        vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE);
        cmd.setColorBlendEnableEXT(0, &blendEnable);
    }
}

// Viewport and scissor (always dynamic in Vulkan 1.3)
cmd.setViewport(0, 1, &viewport);
cmd.setScissor(0, 1, &scissor);
```

**Benefits**:
- Fewer pipeline objects needed
- Reduced pipeline creation overhead
- More flexible state changes without pipeline switches

---

## 9. File Reference

### Core 2D Rendering Files

| File | Lines | Purpose |
|------|-------|---------|
| `code/graphics/render.h` | 1-240 | Public 2D API declarations |
| `code/graphics/render.cpp` | 1-1488 | 2D rendering implementation |
| `code/graphics/2d.h` | 1-1600+ | Screen state, shader types, API |
| `code/graphics/matrix.h` | 1-47 | Matrix function declarations |
| `code/graphics/matrix.cpp` | 1-414 | Projection/view matrix implementation |

### Vulkan Backend Files

| File | Lines | Purpose |
|------|-------|---------|
| `code/graphics/vulkan/VulkanGraphics.cpp` | 1-2000+ | Vulkan rendering commands |
| `code/graphics/vulkan/VulkanClip.h` | 1-146 | Scissor/clip management |
| `code/graphics/vulkan/VulkanRenderer.h` | 1-320 | Renderer class declaration |
| `code/graphics/vulkan/VulkanRenderer.cpp` | 1-1000+ | Renderer implementation |

### Shader Files

| File | Purpose |
|------|---------|
| `code/graphics/shaders/interface.vert` | Interface vertex shader |
| `code/graphics/shaders/interface.frag` | Interface fragment shader |
| `code/graphics/shaders/default-material.vert` | Default material vertex shader |
| `code/graphics/shaders/default-material.frag` | Default material fragment shader |
| `code/graphics/shaders/nanovg.vert` | NanoVG vertex shader |
| `code/graphics/shaders/nanovg.frag` | NanoVG fragment shader |

### Key Functions Quick Reference

| Function | File:Line | Purpose |
|----------|-----------|---------|
| `gr_bitmap()` | `2d.cpp:~2181` | Draw textured rectangle |
| `gr_aabitmap()` | `render.cpp:199` | Draw anti-aliased bitmap |
| `gr_string()` | `render.cpp:805` | Render text string |
| `gr_line()` | `render.cpp:995` | Draw line segment |
| `gr_rect()` | `render.cpp:1158` | Draw filled rectangle |
| `gr_circle()` | `render.cpp:1051` | Draw filled circle |
| `gr_set_2d_matrix()` | `matrix.cpp:246` | Enable 2D projection |
| `gr_end_2d_matrix()` | `matrix.cpp:285` | Restore 3D projection |
| `gr_set_clip()` | `2d.h:1106` | Set clipping region |
| `gr_render_primitives_2d_immediate()` | `render.cpp:1294` | Immediate 2D rendering |
| `createFullScreenViewport()` | `VulkanGraphics.cpp:138` | Create Y-flipped viewport |
| `createClipScissor()` | `VulkanGraphics.cpp:150` | Create scissor from clip |
| `applyClipToScreen()` | `VulkanClip.h:65` | Update gr_screen clip state |

---

## Appendix: Vulkan vs OpenGL Differences

### Coordinate System

| Aspect | OpenGL | Vulkan |
|--------|--------|--------|
| NDC Y direction | -1 (bottom) to +1 (top) | -1 (top) to +1 (bottom) |
| Depth range | [-1, 1] | [0, 1] |
| Framebuffer origin | Bottom-left | Top-left |

### Viewport Y-Flip Solution

The Vulkan backend uses negative viewport height:
```cpp
viewport.y = gr_screen.max_h;      // Start at bottom
viewport.height = -gr_screen.max_h; // Negative = flip Y
```

This allows existing screen-space code (Y=0 at top) to work unchanged.

### Scissor Handling

OpenGL scissor can be disabled; Vulkan scissor is always active. The solution:
- Frame start: set scissor to full screen
- Each clip change: update scissor immediately
- Ensure scissor bounds are non-negative (clamp to framebuffer)

### Projection Matrix Differences

The orthographic projection accounts for depth range:
```cpp
if (gr_screen.mode == GR_VULKAN) {
    // Vulkan [0, 1] depth
    out->a1d[10] = -1.0f / (far_dist - near_dist);
    out->a1d[14] = -near_dist / (far_dist - near_dist);
} else {
    // OpenGL [-1, 1] depth
    out->a1d[10] = -2.0f / (far_dist - near_dist);
    out->a1d[14] = -(far_dist + near_dist) / (far_dist - near_dist);
}
```
