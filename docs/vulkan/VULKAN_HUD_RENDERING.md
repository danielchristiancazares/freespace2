# HUD Rendering Pipeline - Comprehensive Technical Documentation

This document provides a complete technical reference for the HUD (Heads-Up Display) rendering system in FreeSpace 2's Vulkan graphics backend. It covers the rendering architecture, shader pipeline, alpha blending, coordinate systems, and font/glyph management.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Frame Rendering Flow](#2-frame-rendering-flow)
3. [HUD Gauge System](#3-hud-gauge-system)
4. [Vulkan 2D/Interface Rendering](#4-vulkan-2dinterface-rendering)
5. [Interface Shader Pipeline](#5-interface-shader-pipeline)
6. [Alpha Blending and Blend Modes](#6-alpha-blending-and-blend-modes)
7. [AA Bitmap Handling and Masking](#7-aa-bitmap-handling-and-masking)
8. [Screen-Space Coordinate Systems](#8-screen-space-coordinate-systems)
9. [Font and Glyph Management](#9-font-and-glyph-management)
10. [Render-to-Texture and Cockpit Displays](#10-render-to-texture-and-cockpit-displays)
11. [Dynamic State and Pipeline Configuration](#11-dynamic-state-and-pipeline-configuration)
12. [Known Issues and Edge Cases](#12-known-issues-and-edge-cases)

---

## 1. Architecture Overview

The HUD rendering system bridges the legacy immediate-mode graphics API (`gr_*` functions) with the modern Vulkan backend. The architecture follows a material-driven pipeline where all rendering state (blending, depth testing, texturing) is encapsulated in `material` objects.

### Key Components

| Component | File | Purpose |
|-----------|------|---------|
| `HudGauge` | `code/hud/hud.h:214-385` | Base class for all HUD gauges with render methods |
| `material` | `code/graphics/material.h:32-165` | Encapsulates all pipeline state for a draw call |
| `interface_material` | `code/graphics/material.h:342-355` | Specialized material for HUD/UI rendering |
| `gr_vulkan_render_primitives` | `code/graphics/vulkan/VulkanGraphics.cpp:1065-1358` | Core Vulkan primitive rendering function |
| `interface.frag` | `code/graphics/shaders/interface.frag` | Fragment shader for HUD rendering |
| `interface.vert` | `code/graphics/shaders/interface.vert` | Vertex shader for HUD rendering |

### Shader Type Hierarchy

```
SDR_TYPE_INTERFACE (2d.h:201)
    |-- Used by: HUD bitmaps, UI elements, gr_aabitmap()
    |-- Shader files: interface.vert, interface.frag
    |-- No vertex color input (color from uniform)

SDR_TYPE_DEFAULT_MATERIAL (2d.h:200)
    |-- Used by: Legacy text rendering (VFNT fonts)
    |-- Shader files: default-material.vert, default-material.frag
    |-- Requires vertex color attribute

SDR_TYPE_NANOVG (2d.h:202)
    |-- Used by: NanoVG text rendering (TrueType fonts)
    |-- Requires stencil buffer for path rendering

SDR_TYPE_BATCHED_BITMAP (2d.h:199)
    |-- Used by: Particle systems, batched effects
    |-- Optimized for many similar draw calls
```

---

## 2. Frame Rendering Flow

HUD rendering occurs after all 3D scene rendering is complete but before the frame is presented. This ensures HUD elements overlay the scene correctly.

### Rendering Sequence

**File:** `freespace2/freespace.cpp`

```
game_render_frame() (line 3436)
|
+-- g3_start_frame() (line 3441)
|       Set up 3D projection matrices
|
+-- game_render_frame_setup() (line 3140)
|       Configure camera, view matrices
|
+-- [3D Scene Rendering]
|   +-- obj_render_queue_all()
|   +-- gr_deferred_lighting_begin/end()
|   +-- scene.render_all()
|
+-- g3_end_frame() (line 3898)
|       End 3D rendering context
|
+-- hud_render_all(frametime) (line 3902)   <-- HUD RENDERING ENTRY POINT
|   +-- hud_render_gauges(-1, frametime)    Default HUD gauges
|   +-- hud_render_gauges(i, frametime)     Cockpit display gauges (loop)
|   +-- hud_clear_msg_buffer()
|   +-- font::set_font(FONT1)               Restore default font
|
+-- game_flash_diminish(frametime) (line 3905)
```

### hud_render_all() Implementation

**File:** `code/hud/hud.cpp:2064-2079`

```cpp
void hud_render_all(float frametime)
{
    int i;

    // Render default HUD gauges (not associated with cockpit displays)
    hud_render_gauges(-1, frametime);

    // Render gauges for each cockpit display (render-to-texture)
    for ( i = 0; i < (int)Player_displays.size(); ++i ) {
        hud_render_gauges(i, frametime);
    }

    hud_clear_msg_buffer();
    font::set_font(font::FONT1);
}
```

### hud_render_gauges() Implementation

**File:** `code/hud/hud.cpp:2081-2165`

```cpp
void hud_render_gauges(int cockpit_display_num, float frametime)
{
    ship_info* sip = &Ship_info[Player_ship->ship_info_index];
    int render_target = -1;

    // For cockpit displays, set up render-to-texture
    if ( cockpit_display_num >= 0 ) {
        if ( sip->cockpit_model_num < 0 ) return;
        if ( !sip->hud_enabled ) return;

        render_target = ship_start_render_cockpit_display(cockpit_display_num);
        if ( render_target < 0 ) return;
    } else {
        if( supernova_stage() >= SUPERNOVA_STAGE::HIT) return;
    }

    // Render each gauge
    size_t num_gauges = sip->hud_enabled ? sip->hud_gauges.size()
                                         : default_hud_gauges.size();

    for(size_t j = 0; j < num_gauges; j++) {
        GR_DEBUG_SCOPE("Render HUD gauge");

        HudGauge* gauge = sip->hud_enabled ? sip->hud_gauges[j].get()
                                           : default_hud_gauges[j].get();

        // Preprocess (3D-dependent calculations)
        if ( cockpit_display_num < 0 ) {
            gauge->preprocess();
        }

        gauge->onFrame(frametime);

        // Setup render canvas (handles RTT switching)
        if ( !gauge->setupRenderCanvas(render_target) ) continue;

        // Check if gauge can render
        if ( !gauge->canRender() ) continue;

        TRACE_SCOPE(tracing::RenderHUDGauge);

        gauge->resetClip();
        gauge->setFont();
        gauge->render(frametime);  // <-- Individual gauge rendering
    }

    // Clean up cockpit display render target
    if ( cockpit_display_num >= 0 ) {
        ship_end_render_cockpit_display(cockpit_display_num);
        if ( gr_screen.rendering_to_texture != -1 ) {
            bm_set_render_target(-1);
        }
    }
}
```

---

## 3. HUD Gauge System

### HudGauge Base Class

**File:** `code/hud/hud.h:214-385`

The `HudGauge` class provides the foundation for all HUD elements. Key members:

```cpp
class HudGauge
{
protected:
    int position[2];           // Screen position (x, y)
    int base_w, base_h;        // Base resolution for scaling
    color gauge_color;         // RGBA color for this gauge
    int gauge_type;            // HUD_* type identifier
    int font_num;              // Font to use for text

    float tabled_origin[2];    // Origin point for positioning
    int tabled_offset[2];      // Offset from origin
    float aspect_quotient;     // Aspect ratio correction factor

    bool reticle_follow;       // Follow reticle movement
    bool active;               // Is gauge active/visible
    bool pop_up;               // Pop-up behavior enabled

    // Render-to-texture parameters
    char texture_target_fname[MAX_FILENAME_LEN];
    int texture_target;
    int canvas_w, canvas_h;    // Virtual canvas size
    int target_w, target_h;    // Physical texture size

public:
    // Rendering methods
    void renderBitmap(int x, int y, float scale = 1.0f, bool config = false) const;
    void renderBitmap(int frame, int x, int y, float scale = 1.0f, bool config = false) const;
    void renderBitmapColor(int frame, int x, int y, float scale = 1.0f, bool config = false) const;
    void renderString(int x, int y, const char *str, float scale = 1.0f, bool config = false);
    void renderLine(int x1, int y1, int x2, int y2, bool config = false) const;
    void renderRect(int x, int y, int w, int h, bool config = false) const;
    void renderCircle(int x, int y, int diameter, bool filled = true, bool config = false) const;

    // State management
    void setGaugeColor(int bright_index = HUD_C_NONE, bool config = false);
    void setFont();
    void setClip(int x, int y, int w, int h);
    void resetClip();
};
```

### Gauge Rendering Methods

**File:** `code/hud/hud.cpp:1052-1092`

#### renderBitmap()

Renders a bitmap using `gr_aabitmap()` with proper scaling and EMP jitter:

```cpp
void HudGauge::renderBitmap(int x, int y, float scale, bool config) const
{
    int nx = 0, ny = 0;

    // EMP effect can disable gauge rendering
    if( !emp_should_blit_gauge() ) return;

    // Apply EMP jitter effect
    emp_hud_jitter(&x, &y);

    int resize = GR_RESIZE_FULL;

    if (!config) {
        if (gr_screen.rendering_to_texture != -1) {
            // Render-to-texture mode: use canvas/target dimensions
            gr_set_screen_scale(canvas_w, canvas_h, -1, -1,
                               target_w, target_h, target_w, target_h, true);
        } else {
            if (reticle_follow) {
                // Apply reticle offset for HUD elements that follow nose position
                nx = HUD_nose_x;
                ny = HUD_nose_y;
                gr_resize_screen_pos(&nx, &ny);
                gr_set_screen_scale(base_w, base_h);
                gr_unsize_screen_pos(&nx, &ny);
            } else {
                gr_set_screen_scale(base_w, base_h);
            }
        }
    } else {
        resize = HC_resize_mode;  // HUD config preview mode
    }

    gr_aabitmap(x + nx, y + ny, resize, false, scale);
    gr_reset_screen_scale();
}
```

#### renderString()

**File:** `code/hud/hud.cpp:899-932`

Renders text with optional shadow effect:

```cpp
void HudGauge::renderString(int x, int y, const char *str, float scale, bool config)
{
    int nx = 0, ny = 0;
    int resize = GR_RESIZE_FULL;

    // [Screen scale setup - same as renderBitmap]

    // Optional shadow rendering for better readability
    if (HUD_shadows) {
        color cur = gr_screen.current_color;
        gr_set_color_fast(&Color_black);
        gr_string(x + nx + 1, y + ny + 1, str, resize, scale);  // Shadow offset (1,1)
        gr_set_color_fast(&cur);
    }

    gr_string(x + nx, y + ny, str, resize, scale);  // Main text
    gr_reset_screen_scale();
}
```

---

## 4. Vulkan 2D/Interface Rendering

### gr_vulkan_render_primitives()

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:1065-1358`

This is the core rendering function that handles all material-based primitive rendering, including HUD elements.

#### Function Signature

```cpp
void gr_vulkan_render_primitives(
    material* material_info,        // Contains all pipeline state
    primitive_type prim_type,       // Triangle list, strip, etc.
    vertex_layout* layout,          // Vertex attribute layout
    int offset,                     // Vertex offset in buffer
    int n_verts,                    // Number of vertices
    gr_buffer_handle buffer_handle, // Vertex buffer handle
    size_t buffer_offset = 0        // Byte offset in buffer
);
```

#### Interface Shader Data Path

When `shaderType == SDR_TYPE_INTERFACE` (line 1196):

```cpp
if (shaderType == SDR_TYPE_INTERFACE) {
    // Interface shader: 40-byte layout with color at offset 0
    interfaceData.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};
    interfaceData.baseMapIndex = baseMapIndex;
    interfaceData.alphaTexture = alphaTexture;    // 1 for AA bitmaps
    interfaceData.noTexturing = noTexturing;       // 1 if no texture
    interfaceData.srgb = 1;                        // Enable sRGB conversion
    interfaceData.intensity = intensity;           // Color multiplier
    interfaceData.alphaThreshold = 0.f;           // Alpha test threshold

    genericDataPtr = &interfaceData;
    genericDataSize = sizeof(genericData_interface_frag);
}
```

#### Uniform Buffer Layout

The interface shader uses two uniform buffer bindings:

**Binding 0 - Matrix Data (128 bytes):**
```cpp
layout (binding = 0, std140) uniform matrixData {
    mat4 modelViewMatrix;  // 64 bytes
    mat4 projMatrix;       // 64 bytes
};
```

**Binding 1 - Generic Data (40 bytes, std140 aligned):**
```cpp
layout (binding = 1, std140) uniform genericData {
    vec4 color;            // 16 bytes - RGBA color
    int baseMapIndex;      // 4 bytes  - Texture array index
    int alphaTexture;      // 4 bytes  - Is this an alpha texture?
    int noTexturing;       // 4 bytes  - Disable texturing?
    int srgb;              // 4 bytes  - Apply sRGB conversion?
    float intensity;       // 4 bytes  - Color intensity multiplier
    float alphaThreshold;  // 4 bytes  - Alpha test threshold
};
```

---

## 5. Interface Shader Pipeline

### Vertex Shader

**File:** `code/graphics/shaders/interface.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Vertex attributes - no color, just position and texcoord
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

**Key Design Notes:**
- No vertex color input (unlike `SDR_TYPE_DEFAULT_MATERIAL`)
- Color comes from uniform buffer, not per-vertex
- Uses standard 2D projection matrix from `gr_set_2d_matrix()`

### Fragment Shader

**File:** `code/graphics/shaders/interface.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"  // sRGB conversion functions

layout (location = 0) in vec2 fragTexCoord;
layout (location = 0) out vec4 fragOut0;

layout (binding = 1, std140) uniform genericData {
    vec4 color;
    int baseMapIndex;
    int alphaTexture;
    int noTexturing;
    int srgb;
    float intensity;
    float alphaThreshold;
};

layout(binding = 2) uniform sampler2DArray baseMap;

void main()
{
    vec4 baseColor = texture(baseMap, vec3(fragTexCoord, float(baseMapIndex)));

    // AA bitmaps (fonts, etc.) are uploaded as single-channel (R8) textures
    // where the mask lives in .r. Don't use .a in that case (it will be 1.0 for R8).
    float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
    if (alphaThreshold > coverage) discard;

    // Convert texture from sRGB if needed
    // For alpha textures, baseColor is a mask, not color data
    baseColor.rgb = (srgb == 1 && alphaTexture == 0)
                    ? srgb_to_linear(baseColor.rgb)
                    : baseColor.rgb;

    // Uniform color (no vertex color multiplication)
    vec4 blendColor = (srgb == 1)
                      ? vec4(srgb_to_linear(color.rgb), color.a)
                      : color;

    // Mix based on alpha texture mode and no-texturing mode
    fragOut0 = mix(
        mix(baseColor * blendColor,
            vec4(blendColor.rgb, baseColor.r * blendColor.a),
            float(alphaTexture)),
        blendColor,
        float(noTexturing)
    ) * intensity;
}
```

### Gamma/sRGB Conversion

**File:** `code/def_files/data/effects/gamma.sdr`

```glsl
const float SRGB_GAMMA = 2.2;
const float SRGB_GAMMA_INVERSE = 1.0 / SRGB_GAMMA;

float srgb_to_linear(float val) {
    return pow(val, SRGB_GAMMA);
}

vec3 srgb_to_linear(vec3 val) {
    return pow(val, vec3(SRGB_GAMMA));
}

float linear_to_srgb(float val) {
    return pow(val, SRGB_GAMMA_INVERSE);
}
```

---

## 6. Alpha Blending and Blend Modes

### Blend Mode Enumeration

**File:** `code/graphics/grinternal.h:60-67`

```cpp
typedef enum gr_alpha_blend {
    ALPHA_BLEND_NONE,                  // 1*SrcPixel + 0*DestPixel (opaque)
    ALPHA_BLEND_ADDITIVE,              // 1*SrcPixel + 1*DestPixel (glow effects)
    ALPHA_BLEND_ALPHA_ADDITIVE,        // Alpha*SrcPixel + 1*DestPixel
    ALPHA_BLEND_ALPHA_BLEND_ALPHA,     // Alpha*SrcPixel + (1-Alpha)*DestPixel  <-- HUD DEFAULT
    ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR, // Alpha*SrcPixel + (1-SrcPixel)*DestPixel
    ALPHA_BLEND_PREMULTIPLIED          // 1*SrcPixel + (1-Alpha)*DestPixel
} gr_alpha_blend;
```

### HUD Default Blend Mode

HUD elements typically use `ALPHA_BLEND_ALPHA_BLEND_ALPHA`:

```cpp
// Standard alpha blending formula:
// FinalColor = SrcAlpha * SrcColor + (1 - SrcAlpha) * DstColor
```

This is set in material initialization:

**File:** `code/graphics/render.cpp:180-181`

```cpp
material render_mat;
render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);  // HUD has no depth testing
```

### Dynamic Blend State

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:1330-1334`

Blend mode is set dynamically per draw call using extended dynamic state:

```cpp
if (ctxBase.renderer.supportsExtendedDynamicState3()) {
    const auto& caps = ctxBase.renderer.getExtendedDynamicState3Caps();
    if (caps.colorBlendEnable) {
        vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE)
                                 ? VK_TRUE : VK_FALSE;
        cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
    }
}
```

---

## 7. AA Bitmap Handling and Masking

### What Are AA Bitmaps?

AA (Anti-Aliased) bitmaps are single-channel textures where the grayscale value represents coverage/opacity. They are primarily used for:

- Font glyphs (VFNT format)
- HUD element masks
- Anti-aliased UI elements

### Texture Cache Type

**File:** `code/graphics/grinternal.h:50`

```cpp
#define TCACHE_TYPE_AABITMAP  0  // HUD bitmap. All Alpha.
```

### gr_aabitmap() Implementation

**File:** `code/graphics/render.cpp:199-278`

```cpp
void gr_aabitmap(int x, int y, int resize_mode, bool mirror, float scale_factor) {
    GR_DEBUG_SCOPE("Draw AA-bitmap");

    int w, h;
    bm_get_info(gr_screen.current_bitmap, &w, &h);

    if (scale_factor != 1.0f) {
        w = static_cast<int>(w * scale_factor);
        h = static_cast<int>(h * scale_factor);
    }

    // Clipping calculations...

    // Call internal bitmap renderer with aabitmap=true
    bitmap_ex_internal(dx1, dy1, (dx2 - dx1 + 1), (dy2 - dy1 + 1),
                       sx, sy, resize_mode, true, mirror, &GR_CURRENT_COLOR,
                       scale_factor);
}
```

### AA Bitmap Material Setup

**File:** `code/graphics/render.cpp:179-194`

```cpp
static void bitmap_ex_internal(..., bool aabitmap, ...) {
    material render_mat;
    render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
    render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);
    render_mat.set_texture_map(TM_BASE_TYPE, gr_screen.current_bitmap);
    render_mat.set_color(clr->red, clr->green, clr->blue, clr->alpha);
    render_mat.set_cull_mode(false);

    if (aabitmap) {
        render_mat.set_texture_type(material::TEX_TYPE_AABITMAP);
    } else {
        // Determine type based on texture properties
        if (bm_has_alpha_channel(gr_screen.current_bitmap)) {
            render_mat.set_texture_type(material::TEX_TYPE_XPARENT);
        } else {
            render_mat.set_texture_type(material::TEX_TYPE_NORMAL);
        }
    }

    draw_textured_quad(&render_mat, x1, y1, u0, v0, x2, y2, u1, v1);
}
```

### Shader Handling of AA Textures

**File:** `code/graphics/shaders/interface.frag:28-31`

```glsl
// AA bitmaps are R8 format - coverage is in the red channel
float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
if (alphaThreshold > coverage) discard;

// For AA textures, output uses the mask to modulate alpha
fragOut0 = mix(
    mix(baseColor * blendColor,
        vec4(blendColor.rgb, baseColor.r * blendColor.a),  // AA path
        float(alphaTexture)),
    blendColor,
    float(noTexturing)
) * intensity;
```

---

## 8. Screen-Space Coordinate Systems

### Coordinate System Overview

FreeSpace 2 uses a top-left origin coordinate system with Y increasing downward:

```
(0,0) -----------------> X (gr_screen.max_w)
  |
  |
  |
  v
  Y (gr_screen.max_h)
```

### Viewport Setup (Vulkan Y-Flip)

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:138-148`

Vulkan uses a bottom-left origin, so the viewport is flipped:

```cpp
vk::Viewport createFullScreenViewport()
{
    vk::Viewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(gr_screen.max_h);  // Start at bottom
    viewport.width = static_cast<float>(gr_screen.max_w);
    viewport.height = -static_cast<float>(gr_screen.max_h);  // Negative = flip Y
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    return viewport;
}
```

### Scissor Rectangle

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:150-158`

The scissor respects the `gr_screen.clip_*` values for clipping:

```cpp
vk::Rect2D createClipScissor()
{
    auto clip = getClipScissorFromScreen(gr_screen);
    clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{clip.x, clip.y};
    scissor.extent = vk::Extent2D{clip.width, clip.height};
    return scissor;
}
```

### Screen Scaling System

HUD gauges use a virtual coordinate system that gets scaled to the actual screen resolution:

**File:** `code/hud/hud.cpp:906-918`

```cpp
if (gr_screen.rendering_to_texture != -1) {
    // Render-to-texture: use canvas dimensions
    gr_set_screen_scale(canvas_w, canvas_h, -1, -1,
                       target_w, target_h, target_w, target_h, true);
} else {
    if (reticle_follow) {
        // Reticle follow mode: apply nose offset
        nx = HUD_nose_x;
        ny = HUD_nose_y;
        gr_resize_screen_pos(&nx, &ny);
        gr_set_screen_scale(base_w, base_h);
        gr_unsize_screen_pos(&nx, &ny);
    } else {
        // Standard mode: scale from base resolution
        gr_set_screen_scale(base_w, base_h);
    }
}
```

### Resize Modes

**File:** `code/graphics/2d.h`

```cpp
#define GR_RESIZE_NONE     0   // No scaling
#define GR_RESIZE_FULL     1   // Scale to fit screen
#define GR_RESIZE_FULL_CENTER  2   // Scale and center
#define GR_RESIZE_MENU     3   // Menu-specific scaling
#define GR_RESIZE_MENU_ZOOMED  4   // Menu with zoom
#define GR_RESIZE_MENU_NO_OFFSET  5  // Menu without offset
```

---

## 9. Font and Glyph Management

### Font Types

FreeSpace 2 supports two font rendering systems:

1. **VFNT (Legacy Bitmap Fonts)** - Pre-rendered bitmap glyphs
2. **NanoVG (TrueType Fonts)** - Vector-based rendering

### VFNT Text Rendering

**File:** `code/graphics/render.cpp:567-739`

```cpp
static void gr_string_old(float sx, float sy, const char* s, const char* end,
                          font::font* fontData, float height,
                          bool canAutoScale, bool canScale,
                          int resize_mode, float scaleMultiplier)
{
    GR_DEBUG_SCOPE("Render VFNT string");

    material render_mat;
    // VFNT uses DEFAULT_MATERIAL because it needs vertex colors
    render_mat.set_shader_type(SDR_TYPE_DEFAULT_MATERIAL);
    render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
    render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);
    render_mat.set_texture_map(TM_BASE_TYPE, fontData->bitmap_id);
    render_mat.set_color(GR_CURRENT_COLOR.red, GR_CURRENT_COLOR.green,
                        GR_CURRENT_COLOR.blue, GR_CURRENT_COLOR.alpha);
    render_mat.set_texture_type(material::TEX_TYPE_AABITMAP);

    // Vertex layout with position, texcoord, AND color
    vertex_layout vert_def;
    vert_def.add_vertex_component(vertex_format_data::POSITION2, sizeof(v4), offsetof(v4, x));
    vert_def.add_vertex_component(vertex_format_data::TEX_COORD2, sizeof(v4), offsetof(v4, u));
    vert_def.add_vertex_component(vertex_format_data::COLOR4F, sizeof(v4), offsetof(v4, r));

    // Build vertex buffer for all characters
    while (s < end) {
        // Get glyph UV coordinates from font data
        int u = fontData->bm_u[letter];
        int v = fontData->bm_v[letter];

        // Generate 6 vertices (2 triangles) per character
        String_render_buff[buffer_offset++] = {x1, y1, u0, v0, cr, cg, cb, ca};
        String_render_buff[buffer_offset++] = {x1, y2, u0, v1, cr, cg, cb, ca};
        String_render_buff[buffer_offset++] = {x2, y1, u1, v0, cr, cg, cb, ca};
        String_render_buff[buffer_offset++] = {x1, y2, u0, v1, cr, cg, cb, ca};
        String_render_buff[buffer_offset++] = {x2, y1, u1, v0, cr, cg, cb, ca};
        String_render_buff[buffer_offset++] = {x2, y2, u1, v1, cr, cg, cb, ca};

        // Flush buffer if full
        if (buffer_offset == MAX_VERTS_PER_DRAW) {
            gr_render_primitives_immediate(&render_mat, PRIM_TYPE_TRIS,
                                          &vert_def, buffer_offset,
                                          String_render_buff, sizeof(v4) * buffer_offset);
            buffer_offset = 0;
        }

        x += raw_spacing * scale_factor;
    }

    // Render remaining characters
    if (buffer_offset) {
        gr_render_primitives_immediate(&render_mat, PRIM_TYPE_TRIS,
                                      &vert_def, buffer_offset,
                                      String_render_buff, sizeof(v4) * buffer_offset);
    }
}
```

### NanoVG Text Rendering

NanoVG is used for TrueType fonts and requires stencil buffer support:

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:1406-1535`

```cpp
void gr_vulkan_render_nanovg(nanovg_material* material_info, ...)
{
    // NanoVG requires stencil buffer
    Assertion(rt.depthFormat != vk::Format::eUndefined,
              "render_nanovg requires a depth/stencil attachment");
    Assertion(ctxBase.renderer.renderTargets()->depthHasStencil(),
              "render_nanovg requires a stencil-capable depth format");

    // Use SDR_TYPE_NANOVG shader
    ShaderModules shaderModules = ctxBase.renderer.getShaderModules(SDR_TYPE_NANOVG);

    // Set up stencil operations for path rendering
    pipelineKey.stencil_test_enable = material_info->is_stencil_enabled();
    // ... stencil configuration ...

    cmd.setDepthTestEnable(VK_FALSE);
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setStencilTestEnable(material_info->is_stencil_enabled() ? VK_TRUE : VK_FALSE);
}
```

### Font Selection

**File:** `code/hud/hud.h:224`

```cpp
int font_num;  // Member of HudGauge class
```

Fonts are set before rendering:

```cpp
void HudGauge::setFont() {
    font::set_font(font_num);
}
```

---

## 10. Render-to-Texture and Cockpit Displays

### Cockpit Display System

Some HUD gauges render to textures that are then applied to 3D cockpit models.

**File:** `code/ship/ship.cpp:8724-8750`

```cpp
int ship_start_render_cockpit_display(size_t cockpit_display_num)
{
    // Validate cockpit model exists
    if ( Ship_info[Player_ship->ship_info_index].cockpit_model_num < 0 ) {
        return -1;
    }

    if ( Player_cockpit_textures == nullptr ) {
        return -1;
    }

    // Get cockpit display configuration
    cockpit_display* display = &Player_displays[cockpit_display_num];

    // Set render target to the cockpit texture
    bm_set_render_target(display->target);

    // Clear the texture
    gr_clear();

    return display->target;
}
```

### Gauge Render Canvas Setup

**File:** `code/hud/hud.cpp:1453-1466`

This function determines whether the current gauge should render to the specified render target. It checks the gauge's configured texture target against the requested render target:

```cpp
bool HudGauge::setupRenderCanvas(int render_target)
{
    if (texture_target_fname[0] != '\0') {
        // Gauge has a texture target configured
        if ( render_target >= 0 && render_target == texture_target ) {
            return true;  // This is the target we're looking for
        }
    } else {
        // Gauge does not target a texture
        if ( render_target < 0 ) {
            return true;  // This matches (no texture target)
        }
    }

    return false;
}
```

**Note**: Actual screen scale and 2D matrix setup occurs in the individual gauge rendering methods (`renderBitmap()`, `renderString()`, etc.), not in `setupRenderCanvas()`. This function only validates whether a gauge should render to a given target.

---

## 11. Dynamic State and Pipeline Configuration

### Pipeline Key Structure

Vulkan pipelines are cached based on a key that includes all relevant state:

```cpp
struct PipelineKey {
    shader_type type;           // SDR_TYPE_INTERFACE, etc.
    uint variant_flags;         // Shader variant flags
    VkFormat color_format;      // Render target color format
    VkFormat depth_format;      // Render target depth format
    VkSampleCountFlagBits sample_count;
    uint32_t color_attachment_count;
    gr_alpha_blend blend_mode;  // Alpha blend mode
    size_t layout_hash;         // Vertex layout hash
    // ... additional state ...
};
```

### Dynamic State Commands

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:1307-1355`

```cpp
// Set dynamic state per draw call
cmd.setPrimitiveTopology(convertPrimitiveType(prim_type));
cmd.setCullMode(material_info->get_cull_mode() ?
                vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone);
cmd.setFrontFace(vk::FrontFace::eClockwise);

// Depth state - HUD typically has no depth testing
gr_zbuffer_type zbufferMode = material_info->get_depth_mode();
bool depthTest = (zbufferMode == ZBUFFER_TYPE_READ || zbufferMode == ZBUFFER_TYPE_FULL);
bool depthWrite = (zbufferMode == ZBUFFER_TYPE_WRITE || zbufferMode == ZBUFFER_TYPE_FULL);

// No depth if no attachment
if (!hasDepthAttachment) {
    depthTest = false;
    depthWrite = false;
}

cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
cmd.setStencilTestEnable(VK_FALSE);

// Extended dynamic state 3 (blend, color mask, polygon mode)
if (ctxBase.renderer.supportsExtendedDynamicState3()) {
    // ... dynamic blend enable, color write mask, etc.
}

// Viewport and scissor
vk::Viewport viewport = createFullScreenViewport();
cmd.setViewport(0, 1, &viewport);

vk::Rect2D scissor = createClipScissor();  // Respects gr_screen.clip_*
cmd.setScissor(0, 1, &scissor);
```

---

## 12. Known Issues and Edge Cases

### EMP Effect Handling

EMP effects can disable HUD gauge rendering and add jitter:

**File:** `code/hud/hud.h:88-89`

```cpp
#define GR_AABITMAP(a, b, c) {
    int jx, jy;
    if(emp_should_blit_gauge()) {
        gr_set_bitmap(a);
        jx = b; jy = c;
        emp_hud_jitter(&jx, &jy);  // Apply random offset
        gr_aabitmap(jx, jy);
    }
}
```

### Scissor Clipping Edge Cases

The scissor rectangle is clamped to the framebuffer dimensions to avoid Vulkan validation errors:

**File:** `code/graphics/vulkan/VulkanGraphics.cpp:150-158`

```cpp
vk::Rect2D createClipScissor()
{
    auto clip = getClipScissorFromScreen(gr_screen);
    // Clamp to framebuffer to prevent negative extents or overflow
    clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
    // ...
}
```

### Shader Type Mismatch

VFNT text rendering requires `SDR_TYPE_DEFAULT_MATERIAL` (which has vertex color support), while bitmap rendering uses `SDR_TYPE_INTERFACE` (uniform color only):

**File:** `code/graphics/render.cpp:584`

```cpp
// Text rendering - needs vertex color
render_mat.set_shader_type(SDR_TYPE_DEFAULT_MATERIAL);
```

**File:** `code/graphics/render.cpp:180`

```cpp
// Bitmap rendering - uniform color
// (No explicit set_shader_type, defaults to SDR_TYPE_INTERFACE for interface materials)
```

### Render Target Switching

When switching between cockpit display rendering and main screen rendering, the render target must be properly reset:

**File:** `code/hud/hud.cpp:2157-2164`

```cpp
if ( cockpit_display_num >= 0 ) {
    ship_end_render_cockpit_display(cockpit_display_num);

    if ( gr_screen.rendering_to_texture != -1 ) {
        // Safety check: reset render target if still set
        bm_set_render_target(-1);
    }
}
```

### Supernova Effect

HUD rendering is completely disabled during supernova events:

**File:** `code/hud/hud.cpp:2102-2105`

```cpp
if( supernova_stage() >= SUPERNOVA_STAGE::HIT) {
    return;  // Don't render HUD during supernova
}
```

---

## Appendix: Quick Reference

### Material Setup for HUD Elements

```cpp
material mat;
mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
mat.set_depth_mode(ZBUFFER_TYPE_NONE);
mat.set_texture_map(TM_BASE_TYPE, bitmap_id);
mat.set_color(r, g, b, a);
mat.set_cull_mode(false);
mat.set_texture_type(material::TEX_TYPE_AABITMAP);  // For fonts/masks
```

### Key File Locations

| Purpose | File Path |
|---------|-----------|
| HUD gauge base class | `code/hud/hud.h:214-385` |
| HUD rendering entry point | `code/hud/hud.cpp:2064-2165` |
| Vulkan primitive rendering | `code/graphics/vulkan/VulkanGraphics.cpp:1065-1358` |
| Interface fragment shader | `code/graphics/shaders/interface.frag` |
| Interface vertex shader | `code/graphics/shaders/interface.vert` |
| Material class | `code/graphics/material.h:32-165` |
| Blend mode definitions | `code/graphics/grinternal.h:60-67` |
| Font rendering | `code/graphics/render.cpp:567-739` |
| gr_aabitmap | `code/graphics/render.cpp:199-278` |

### Shader Type Reference

| Shader Type | Use Case | Vertex Color | Stencil |
|-------------|----------|--------------|---------|
| `SDR_TYPE_INTERFACE` | HUD bitmaps, UI | No (uniform) | No |
| `SDR_TYPE_DEFAULT_MATERIAL` | VFNT text | Yes | No |
| `SDR_TYPE_NANOVG` | TrueType text | No | Yes |
| `SDR_TYPE_BATCHED_BITMAP` | Particles | No | No |
| `SDR_TYPE_ROCKET_UI` | RocketUI menus | No | No |
