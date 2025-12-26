# HUD Rendering Pipeline - Comprehensive Technical Documentation

This document provides a complete technical reference for the HUD (Heads-Up Display) rendering system in FreeSpace 2's Vulkan graphics backend. It covers the rendering architecture, shader pipeline, alpha blending, coordinate systems, and font/glyph management.

## Prerequisites

Before reading this document, you should be familiar with:

- Basic Vulkan concepts (pipelines, shaders, descriptors, render passes)
- FreeSpace 2's graphics abstraction layer (`gr_*` functions)
- C++ class hierarchies and virtual dispatch
- GLSL shader programming

## Target Audience

- Graphics engine developers modifying or debugging HUD rendering
- Mod developers creating custom HUD gauges
- Contributors working on Vulkan backend features

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
12. [Debugging and Performance](#12-debugging-and-performance)
13. [Known Issues and Edge Cases](#13-known-issues-and-edge-cases)

---

## 1. Architecture Overview

The HUD rendering system bridges the legacy immediate-mode graphics API (`gr_*` functions) with the modern Vulkan backend. The architecture follows a material-driven pipeline where all rendering state (blending, depth testing, texturing) is encapsulated in `material` objects.

### High-Level Data Flow

```
+-------------------+     +------------------+     +---------------------+
|   HudGauge        |     |   material       |     |   Vulkan Backend    |
|   (Game Logic)    | --> |   (State Bundle) | --> |   (GPU Commands)    |
+-------------------+     +------------------+     +---------------------+
        |                         |                         |
        v                         v                         v
  renderBitmap()          set_blend_mode()        vkCmdBindPipeline()
  renderString()          set_texture_map()       vkCmdPushConstants()
  renderLine()            set_color()             vkCmdDraw()
```

### Key Components

| Component | File | Purpose |
|-----------|------|---------|
| `HudGauge` | `code/hud/hud.h:214` | Base class for all HUD gauges with render methods |
| `material` | `code/graphics/material.h:32` | Encapsulates all pipeline state for a draw call |
| `interface_material` | `code/graphics/material.h` | Specialized material for HUD/UI rendering |
| `gr_vulkan_render_primitives` | `code/graphics/vulkan/VulkanGraphics.cpp` | Core Vulkan primitive rendering function |
| `interface.frag` | `code/graphics/shaders/interface.frag` | Fragment shader for HUD rendering |
| `interface.vert` | `code/graphics/shaders/interface.vert` | Vertex shader for HUD rendering |

### Shader Type Hierarchy

The HUD system uses several shader types depending on the content being rendered:

```
SDR_TYPE_INTERFACE (2d.h:201)
    Purpose: HUD bitmaps, UI elements, gr_aabitmap()
    Shader files: interface.vert, interface.frag
    Color source: Uniform buffer (not per-vertex)
    Vertex format: Position + TexCoord only

SDR_TYPE_DEFAULT_MATERIAL (2d.h:200)
    Purpose: Legacy text rendering (VFNT bitmap fonts)
    Shader files: default-material.vert, default-material.frag
    Color source: Per-vertex color attribute
    Vertex format: Position + TexCoord + Color

SDR_TYPE_NANOVG (2d.h:202)
    Purpose: NanoVG text rendering (TrueType fonts)
    Requirement: Stencil buffer for path rendering
    Note: More complex pipeline with fill/stroke operations

SDR_TYPE_BATCHED_BITMAP (2d.h:199)
    Purpose: Particle systems, batched effects
    Note: Optimized for many similar draw calls

SDR_TYPE_ROCKET_UI (2d.h:206)
    Purpose: RocketUI/RmlUI menu system
    Note: Separate UI framework with its own rendering
```

---

## 2. Frame Rendering Flow

HUD rendering occurs after all 3D scene rendering is complete but before the frame is presented. This ensures HUD elements overlay the scene correctly.

### Rendering Sequence Diagram

```
game_render_frame()
|
+-- g3_start_frame()
|       Set up 3D projection matrices
|
+-- game_render_frame_setup()
|       Configure camera, view matrices
|
+-- [3D Scene Rendering]
|   +-- obj_render_queue_all()
|   +-- gr_deferred_lighting_begin/end()
|   +-- scene.render_all()
|
+-- g3_end_frame()
|       End 3D rendering context, flush batches
|
+-- hud_render_all(frametime)              <-- HUD RENDERING ENTRY POINT
|   +-- hud_render_gauges(-1, frametime)   Render screen-space HUD gauges
|   +-- for each cockpit_display:
|   |       hud_render_gauges(i, frametime) Render to cockpit textures
|   +-- hud_clear_msg_buffer()
|   +-- font::set_font(FONT1)              Restore default font
|
+-- game_flash_diminish(frametime)
        Decay screen flash effects
```

### Entry Point: hud_render_all()

**File:** `code/hud/hud.cpp:2064`

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

### Gauge Rendering Loop: hud_render_gauges()

**File:** `code/hud/hud.cpp:2081`

This function iterates through all gauges and renders them. The `cockpit_display_num` parameter determines whether rendering targets the screen (`-1`) or a cockpit texture (`>= 0`).

```cpp
void hud_render_gauges(int cockpit_display_num, float frametime)
{
    ship_info* sip = &Ship_info[Player_ship->ship_info_index];
    int render_target = -1;

    // Setup phase: configure render target
    if ( cockpit_display_num >= 0 ) {
        // Validate cockpit model exists
        if ( sip->cockpit_model_num < 0 ) return;
        if ( !sip->hud_enabled ) return;

        // Activate render-to-texture target
        render_target = ship_start_render_cockpit_display(cockpit_display_num);
        if ( render_target < 0 ) return;
    } else {
        // Screen rendering - skip during supernova
        if( supernova_stage() >= SUPERNOVA_STAGE::HIT) return;
    }

    // Determine gauge source (ship-specific or default)
    size_t num_gauges = sip->hud_enabled ? sip->hud_gauges.size()
                                         : default_hud_gauges.size();

    // Render each gauge
    for(size_t j = 0; j < num_gauges; j++) {
        GR_DEBUG_SCOPE("Render HUD gauge");

        HudGauge* gauge = sip->hud_enabled ? sip->hud_gauges[j].get()
                                           : default_hud_gauges[j].get();

        // Preprocess only for screen rendering (3D calculations)
        if ( cockpit_display_num < 0 ) {
            gauge->preprocess();
        }

        gauge->onFrame(frametime);

        // Check if this gauge should render to current target
        if ( !gauge->setupRenderCanvas(render_target) ) continue;

        // Check visibility conditions (active, not popped down, etc.)
        if ( !gauge->canRender() ) continue;

        TRACE_SCOPE(tracing::RenderHUDGauge);

        gauge->resetClip();
        gauge->setFont();
        gauge->render(frametime);  // Virtual call to specific gauge implementation
    }

    // Cleanup phase: reset render target
    if ( cockpit_display_num >= 0 ) {
        ship_end_render_cockpit_display(cockpit_display_num);
        if ( gr_screen.rendering_to_texture != -1 ) {
            bm_set_render_target(-1);  // Safety reset
        }
    }
}
```

---

## 3. HUD Gauge System

### HudGauge Base Class

**File:** `code/hud/hud.h:214`

The `HudGauge` class provides the foundation for all HUD elements. All specific gauge types (radar, target brackets, messages, etc.) inherit from this class.

```cpp
class HudGauge
{
protected:
    // Position and sizing
    int position[2];           // Screen position (x, y) in base resolution
    int base_w, base_h;        // Base resolution for scaling (e.g., 1024x768)
    float aspect_quotient;     // Aspect ratio correction factor

    // Appearance
    color gauge_color;         // RGBA color for this gauge
    int font_num;              // Font index for text rendering

    // Origin-based positioning (alternative to absolute position)
    float tabled_origin[2];    // Origin point (0.0-1.0, screen relative)
    int tabled_offset[2];      // Pixel offset from origin

    // Behavior flags
    bool reticle_follow;       // Follow ship nose/reticle movement
    bool active;               // Is gauge currently active/visible
    bool pop_up;               // Pop-up behavior enabled
    bool sexp_override;        // SEXP script has control

    // Render-to-texture parameters (for cockpit displays)
    char texture_target_fname[MAX_FILENAME_LEN];  // Target texture filename
    int texture_target;        // Resolved texture handle
    int canvas_w, canvas_h;    // Virtual canvas size (design resolution)
    int target_w, target_h;    // Physical texture size (pixels)

public:
    // Primary rendering methods
    void renderBitmap(int x, int y, float scale = 1.0f, bool config = false) const;
    void renderBitmap(int frame, int x, int y, float scale = 1.0f, bool config = false) const;
    void renderBitmapColor(int frame, int x, int y, float scale = 1.0f, bool config = false) const;
    void renderBitmapEx(int frame, int x, int y, int w, int h, int sx, int sy,
                        float scale = 1.0f, bool config = false) const;

    // Text rendering methods
    void renderString(int x, int y, const char *str, float scale = 1.0f, bool config = false);
    void renderString(int x, int y, int gauge_id, const char *str, float scale = 1.0f, bool config = false);
    void renderStringAlignCenter(int x, int y, int area_width, const char *s,
                                 float scale = 1.0f, bool config = false);
    void renderPrintf(int x, int y, float scale, bool config, const char* format, ...);

    // Primitive rendering methods
    void renderLine(int x1, int y1, int x2, int y2, bool config = false) const;
    void renderGradientLine(int x1, int y1, int x2, int y2, bool config = false) const;
    void renderRect(int x, int y, int w, int h, bool config = false) const;
    void renderCircle(int x, int y, int diameter, bool filled = true, bool config = false) const;

    // State management
    void setGaugeColor(int bright_index = HUD_C_NONE, bool config = false);
    void setFont();
    void setClip(int x, int y, int w, int h);
    void resetClip();

    // Coordinate transformation
    void unsize(int *x, int *y);   // Convert from scaled to base resolution
    void unsize(float *x, float *y);
    void resize(int *x, int *y);   // Convert from base to scaled resolution
    void resize(float *x, float *y);
};
```

### Derived Gauge Classes

The `HudGauge3DAnchor` class is used for gauges that take their position from 3D world coordinates (e.g., target brackets, lead indicators):

```cpp
// Use this instead of HudGauge for elements anchored in 3D space
// These MUST NEVER slew (follow reticle) because their position comes from
// g3_rotate_vertex/g3_project_vertex calculations
class HudGauge3DAnchor : public HudGauge {
public:
    void initSlew(bool /*slew*/) override {}  // Disable slewing
};
```

### Gauge Rendering Methods

#### renderBitmap()

**File:** `code/hud/hud.cpp:1052`

Renders a bitmap using `gr_aabitmap()` with proper scaling and EMP jitter effects:

```cpp
void HudGauge::renderBitmap(int x, int y, float scale, bool config) const
{
    int nx = 0, ny = 0;

    // EMP effect can disable gauge rendering entirely
    if( !emp_should_blit_gauge() ) {
        return;
    }

    // Apply EMP jitter effect (random position offset)
    emp_hud_jitter(&x, &y);

    int resize = GR_RESIZE_FULL;

    if (!config) {
        if (gr_screen.rendering_to_texture != -1) {
            // Render-to-texture mode: scale for cockpit display canvas
            gr_set_screen_scale(canvas_w, canvas_h, -1, -1,
                               target_w, target_h, target_w, target_h, true);
        } else {
            if (reticle_follow) {
                // Apply nose/reticle offset for slewing gauges
                nx = HUD_nose_x;
                ny = HUD_nose_y;
                gr_resize_screen_pos(&nx, &ny);
                gr_set_screen_scale(base_w, base_h);
                gr_unsize_screen_pos(&nx, &ny);
            } else {
                // Standard screen scaling
                gr_set_screen_scale(base_w, base_h);
            }
        }
    } else {
        resize = HC_resize_mode;  // HUD configuration preview mode
    }

    gr_aabitmap(x + nx, y + ny, resize, false, scale);
    gr_reset_screen_scale();
}
```

#### renderString()

**File:** `code/hud/hud.cpp:899`

Renders text with optional shadow effect for readability:

```cpp
void HudGauge::renderString(int x, int y, const char *str, float scale, bool config)
{
    int nx = 0, ny = 0;
    int resize = GR_RESIZE_FULL;

    // Screen scale setup (same pattern as renderBitmap)
    if (!config) {
        if (gr_screen.rendering_to_texture != -1) {
            gr_set_screen_scale(canvas_w, canvas_h, -1, -1,
                               target_w, target_h, target_w, target_h, true);
        } else {
            if (reticle_follow) {
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
        resize = HC_resize_mode;
    }

    // Optional shadow rendering for better readability against scene
    if (HUD_shadows) {
        color cur = gr_screen.current_color;
        gr_set_color_fast(&Color_black);
        gr_string(x + nx + 1, y + ny + 1, str, resize, scale);  // Shadow at (1,1) offset
        gr_set_color_fast(&cur);
    }

    gr_string(x + nx, y + ny, str, resize, scale);  // Main text
    gr_reset_screen_scale();
}
```

---

## 4. Vulkan 2D/Interface Rendering

### gr_vulkan_render_primitives()

**File:** `code/graphics/vulkan/VulkanGraphics.cpp`

This is the core rendering function that handles all material-based primitive rendering, including HUD elements. It translates the abstract `material` state into Vulkan commands.

#### Function Signature

```cpp
void gr_vulkan_render_primitives(
    material* material_info,        // Contains all pipeline state
    primitive_type prim_type,       // PRIM_TYPE_TRIS, PRIM_TYPE_TRISTRIP, etc.
    vertex_layout* layout,          // Vertex attribute layout
    int offset,                     // Vertex offset in buffer
    int n_verts,                    // Number of vertices to draw
    gr_buffer_handle buffer_handle, // Vertex buffer handle
    size_t buffer_offset = 0        // Byte offset in buffer
);
```

#### Interface Shader Data Path

When the shader type is `SDR_TYPE_INTERFACE`, the function populates a specialized uniform buffer:

```cpp
if (shaderType == SDR_TYPE_INTERFACE) {
    // Interface shader uses uniform color (no vertex color input)
    interfaceData.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};
    interfaceData.baseMapIndex = baseMapIndex;      // Texture array slice
    interfaceData.alphaTexture = alphaTexture;      // 1 for AA bitmaps (fonts, masks)
    interfaceData.noTexturing = noTexturing;        // 1 if solid color (no texture)
    interfaceData.srgb = 1;                         // Enable sRGB conversion
    interfaceData.intensity = intensity;            // Color multiplier (brightness)
    interfaceData.alphaThreshold = 0.f;             // Alpha test threshold (0 = no discard)

    genericDataPtr = &interfaceData;
    genericDataSize = sizeof(genericData_interface_frag);
}
```

### Uniform Buffer Layouts

The interface shader uses two uniform buffer bindings:

**Binding 0 - Matrix Data (128 bytes, std140):**

```glsl
layout (binding = 0, std140) uniform matrixData {
    mat4 modelViewMatrix;  // 64 bytes - Identity for screen-space, transform for 3D
    mat4 projMatrix;       // 64 bytes - Orthographic projection for 2D
};
```

**Binding 1 - Generic Data (40 bytes, std140 aligned):**

```glsl
layout (binding = 1, std140) uniform genericData {
    vec4 color;            // 16 bytes - RGBA color (linear space if srgb=1)
    int baseMapIndex;      // 4 bytes  - Texture array index
    int alphaTexture;      // 4 bytes  - Is this an alpha/mask texture?
    int noTexturing;       // 4 bytes  - Disable texturing entirely?
    int srgb;              // 4 bytes  - Apply sRGB-to-linear conversion?
    float intensity;       // 4 bytes  - Color intensity multiplier
    float alphaThreshold;  // 4 bytes  - Alpha test threshold (discard if < threshold)
};
```

---

## 5. Interface Shader Pipeline

### Vertex Shader

**File:** `code/graphics/shaders/interface.vert`

The interface vertex shader is minimal, handling only position transformation and texture coordinate passthrough:

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

// Vertex attributes - position and texcoord only (no vertex color)
layout (location = 0) in vec4 vertPosition;
layout (location = 2) in vec4 vertTexCoord;

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;

// Matrix uniforms - shared binding layout with other shaders for compatibility
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
- Color comes from uniform buffer, enabling efficient batching of same-colored elements
- Uses standard orthographic projection from `gr_set_2d_matrix()`
- Location 1 is intentionally skipped (reserved for normal in other shaders)

### Fragment Shader

**File:** `code/graphics/shaders/interface.frag`

The fragment shader handles texture sampling, alpha testing, sRGB conversion, and color blending:

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

    // AA bitmaps (fonts, masks) are uploaded as R8 textures.
    // Coverage/alpha lives in .r channel, not .a (which would be 1.0 for R8).
    float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
    if (alphaThreshold > coverage) discard;

    // Convert texture from sRGB if needed.
    // For alpha textures, baseColor is a mask (not color data), so skip conversion.
    baseColor.rgb = (srgb == 1 && alphaTexture == 0)
                    ? srgb_to_linear(baseColor.rgb)
                    : baseColor.rgb;

    // Convert uniform color from sRGB if needed
    vec4 blendColor = (srgb == 1)
                      ? vec4(srgb_to_linear(color.rgb), color.a)
                      : color;

    // Final output: mix based on texture mode
    // - noTexturing=1: Output pure blendColor (solid color)
    // - alphaTexture=1: Use texture.r as alpha mask over blendColor
    // - Otherwise: Multiply texture color by blend color
    fragOut0 = mix(
        mix(baseColor * blendColor,
            vec4(blendColor.rgb, baseColor.r * blendColor.a),
            float(alphaTexture)),
        blendColor,
        float(noTexturing)
    ) * intensity;
}
```

### Fragment Shader Output Modes

The shader handles three distinct rendering modes:

| Mode | `noTexturing` | `alphaTexture` | Output |
|------|---------------|----------------|--------|
| **Solid Color** | 1 | - | `blendColor * intensity` |
| **Alpha Mask** | 0 | 1 | `vec4(blendColor.rgb, baseColor.r * blendColor.a) * intensity` |
| **Textured** | 0 | 0 | `baseColor * blendColor * intensity` |

### Gamma/sRGB Conversion

**File:** `code/def_files/data/effects/gamma.sdr`

The gamma conversion functions use a simplified 2.2 gamma curve (not the full sRGB piecewise function):

```glsl
const float SRGB_GAMMA = 2.2;
const float SRGB_GAMMA_INVERSE = 1.0 / SRGB_GAMMA;

// Convert from sRGB to linear color space (for shader math)
float srgb_to_linear(float val) {
    return pow(val, SRGB_GAMMA);
}
vec2 srgb_to_linear(vec2 val) {
    return pow(val, vec2(SRGB_GAMMA));
}
vec3 srgb_to_linear(vec3 val) {
    return pow(val, vec3(SRGB_GAMMA));
}
vec4 srgb_to_linear(vec4 val) {
    return pow(val, vec4(SRGB_GAMMA));
}

// Convert from linear to sRGB color space (for display)
float linear_to_srgb(float val) {
    return pow(val, SRGB_GAMMA_INVERSE);
}
vec2 linear_to_srgb(vec2 val) {
    return pow(val, vec2(SRGB_GAMMA_INVERSE));
}
vec3 linear_to_srgb(vec3 val) {
    return pow(val, vec3(SRGB_GAMMA_INVERSE));
}
vec4 linear_to_srgb(vec4 val) {
    return pow(val, vec4(SRGB_GAMMA_INVERSE));
}
```

**Note:** The 2.2 power curve is an approximation. The true sRGB transfer function uses a linear segment for dark values. This approximation is standard for real-time graphics.

---

## 6. Alpha Blending and Blend Modes

### Blend Mode Enumeration

**File:** `code/graphics/grinternal.h:60`

```cpp
typedef enum gr_alpha_blend {
    ALPHA_BLEND_NONE,                  // 1*Src + 0*Dst  (opaque overwrite)
    ALPHA_BLEND_ADDITIVE,              // 1*Src + 1*Dst  (glow/light effects)
    ALPHA_BLEND_ALPHA_ADDITIVE,        // A*Src + 1*Dst  (soft glow with transparency)
    ALPHA_BLEND_ALPHA_BLEND_ALPHA,     // A*Src + (1-A)*Dst  (standard transparency) <-- HUD DEFAULT
    ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR, // A*Src + (1-Src)*Dst  (color-based blend)
    ALPHA_BLEND_PREMULTIPLIED          // 1*Src + (1-A)*Dst  (premultiplied alpha)
} gr_alpha_blend;
```

### Blend Mode Formulas

| Blend Mode | Vulkan Blend Factors | Formula |
|------------|---------------------|---------|
| `ALPHA_BLEND_NONE` | Src=ONE, Dst=ZERO | `Final = Src` |
| `ALPHA_BLEND_ADDITIVE` | Src=ONE, Dst=ONE | `Final = Src + Dst` |
| `ALPHA_BLEND_ALPHA_ADDITIVE` | Src=SRC_ALPHA, Dst=ONE | `Final = Src*A + Dst` |
| `ALPHA_BLEND_ALPHA_BLEND_ALPHA` | Src=SRC_ALPHA, Dst=ONE_MINUS_SRC_ALPHA | `Final = Src*A + Dst*(1-A)` |
| `ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR` | Src=SRC_ALPHA, Dst=ONE_MINUS_SRC_COLOR | `Final = Src*A + Dst*(1-Src)` |
| `ALPHA_BLEND_PREMULTIPLIED` | Src=ONE, Dst=ONE_MINUS_SRC_ALPHA | `Final = Src + Dst*(1-A)` |

### HUD Default Blend Mode

HUD elements typically use `ALPHA_BLEND_ALPHA_BLEND_ALPHA` for standard transparency:

```cpp
// Standard alpha blending formula:
// FinalColor = SrcAlpha * SrcColor + (1 - SrcAlpha) * DstColor

// Material setup for HUD rendering
material render_mat;
render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);  // HUD has no depth testing
```

### Dynamic Blend State (Vulkan)

Blend mode is set dynamically per draw call using Vulkan extended dynamic state:

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

AA (Anti-Aliased) bitmaps are single-channel (grayscale) textures where the value represents coverage or opacity. They are primarily used for:

- **Font glyphs** - VFNT bitmap fonts store glyphs as coverage masks
- **HUD element masks** - Shaped gauges with smooth edges
- **Anti-aliased primitives** - Smooth lines and circles

### Texture Cache Types

**File:** `code/graphics/grinternal.h:50`

```cpp
#define TCACHE_TYPE_AABITMAP    0  // Single-channel alpha/coverage texture
#define TCACHE_TYPE_NORMAL      1  // Standard RGB texture, no transparency
#define TCACHE_TYPE_XPARENT     2  // RGB with transparency (green=transparent or alpha channel)
#define TCACHE_TYPE_INTERFACE   3  // Interface graphics (special filtering)
#define TCACHE_TYPE_COMPRESSED  4  // Compressed format (DXT1, DXT3, DXT5)
#define TCACHE_TYPE_CUBEMAP     5  // Cubemap texture
#define TCACHE_TYPE_3DTEX       6  // True 3D volume texture
```

### gr_aabitmap() Implementation

**File:** `code/graphics/render.cpp:199`

```cpp
void gr_aabitmap(int x, int y, int resize_mode, bool mirror, float scale_factor)
{
    if (gr_screen.mode == GR_STUB) {
        return;
    }

    GR_DEBUG_SCOPE("Draw AA-bitmap");

    int w, h, do_resize;

    bm_get_info(gr_screen.current_bitmap, &w, &h);

    // Apply scale factor to dimensions
    if (scale_factor != 1.0f) {
        w = static_cast<int>(w * scale_factor);
        h = static_cast<int>(h * scale_factor);
    }

    // Determine if resize mode applies
    if (resize_mode != GR_RESIZE_NONE &&
        (gr_screen.custom_size || (gr_screen.rendering_to_texture != -1))) {
        do_resize = 1;
    } else {
        do_resize = 0;
    }

    // Calculate destination rectangle
    int dx1 = x, dx2 = x + w - 1;
    int dy1 = y, dy2 = y + h - 1;
    int sx = 0, sy = 0;

    // Perform clipping against current clip region
    int clip_left = ((do_resize) ? gr_screen.clip_left_unscaled : gr_screen.clip_left);
    int clip_right = ((do_resize) ? gr_screen.clip_right_unscaled : gr_screen.clip_right);
    int clip_top = ((do_resize) ? gr_screen.clip_top_unscaled : gr_screen.clip_top);
    int clip_bottom = ((do_resize) ? gr_screen.clip_bottom_unscaled : gr_screen.clip_bottom);

    // Early out if completely clipped
    if ((dx1 > clip_right) || (dx2 < clip_left)) return;
    if ((dy1 > clip_bottom) || (dy2 < clip_top)) return;

    // Clip left/top edges
    if (dx1 < clip_left) { sx = clip_left - dx1; dx1 = clip_left; }
    if (dy1 < clip_top)  { sy = clip_top - dy1;  dy1 = clip_top; }

    // Clip right/bottom edges
    if (dx2 > clip_right)  dx2 = clip_right;
    if (dy2 > clip_bottom) dy2 = clip_bottom;

    // Validate source coordinates
    if ((sx < 0) || (sy < 0)) return;
    if ((sx >= w) || (sy >= h)) return;

    // Render the clipped bitmap
    bitmap_ex_internal(dx1, dy1, (dx2 - dx1 + 1), (dy2 - dy1 + 1),
                       sx, sy, resize_mode, true, mirror,
                       &GR_CURRENT_COLOR, scale_factor);
}
```

### AA Bitmap Material Setup

**File:** `code/graphics/render.cpp:179`

```cpp
static void bitmap_ex_internal(..., bool aabitmap, ...)
{
    material render_mat;
    render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
    render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);
    render_mat.set_texture_map(TM_BASE_TYPE, gr_screen.current_bitmap);
    render_mat.set_color(clr->red, clr->green, clr->blue, clr->alpha);
    render_mat.set_cull_mode(false);

    if (aabitmap) {
        // Single-channel texture: coverage in R channel
        render_mat.set_texture_type(material::TEX_TYPE_AABITMAP);
    } else {
        // Determine transparency based on texture properties
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
// For RGBA textures, use the alpha channel as normal
float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;

// Alpha test: discard fragments below threshold
if (alphaThreshold > coverage) discard;

// Final output for AA textures uses mask to modulate uniform color's alpha
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

FreeSpace 2 uses a top-left origin coordinate system with Y increasing downward, consistent with most 2D graphics systems:

```
(0,0) ---------------------------------> X+ (gr_screen.max_w)
  |
  |     +------------------+
  |     |   HUD Element    |
  |     |   at (100, 50)   |
  |     +------------------+
  |
  v
  Y+ (gr_screen.max_h)
```

### Vulkan Y-Flip

Vulkan natively uses a bottom-left origin with Y increasing upward. To maintain compatibility with the existing coordinate system, the viewport is configured with a negative height:

**File:** `code/graphics/vulkan/VulkanGraphics.cpp`

```cpp
vk::Viewport createFullScreenViewport()
{
    vk::Viewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(gr_screen.max_h);  // Start at bottom
    viewport.width = static_cast<float>(gr_screen.max_w);
    viewport.height = -static_cast<float>(gr_screen.max_h);  // Negative flips Y
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    return viewport;
}
```

This flipped viewport means:
- OpenGL/Direct3D-style coordinates work without modification
- No changes needed to projection matrices or vertex data
- Scissor rectangles use standard top-left coordinates

### Scissor Rectangle Setup

The scissor rectangle respects the current clip region from `gr_screen`:

```cpp
vk::Rect2D createClipScissor()
{
    auto clip = getClipScissorFromScreen(gr_screen);
    // Clamp to framebuffer bounds to prevent validation errors
    clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{clip.x, clip.y};
    scissor.extent = vk::Extent2D{clip.width, clip.height};
    return scissor;
}
```

### Screen Scaling System

HUD gauges use a virtual coordinate system based on a reference resolution (typically 1024x768 or 640x480). These coordinates are scaled to match the actual screen resolution at render time.

**File:** `code/hud/hud.cpp:906`

```cpp
if (gr_screen.rendering_to_texture != -1) {
    // Render-to-texture: scale from canvas to physical texture size
    gr_set_screen_scale(canvas_w, canvas_h,      // Source resolution
                       -1, -1,                   // Center offset (not used)
                       target_w, target_h,       // Target resolution
                       target_w, target_h,       // Clip region
                       true);                    // Force 2D matrix update
} else {
    if (reticle_follow) {
        // Reticle follow mode: apply nose offset with proper scaling
        nx = HUD_nose_x;
        ny = HUD_nose_y;
        gr_resize_screen_pos(&nx, &ny);      // Scale to current resolution
        gr_set_screen_scale(base_w, base_h);
        gr_unsize_screen_pos(&nx, &ny);      // Convert back to base coords
    } else {
        // Standard mode: scale from base resolution to screen
        gr_set_screen_scale(base_w, base_h);
    }
}
```

### Resize Modes

**File:** `code/graphics/2d.h`

```cpp
#define GR_RESIZE_NONE           0  // No scaling, 1:1 pixel mapping
#define GR_RESIZE_FULL           1  // Scale to fit screen, maintain aspect
#define GR_RESIZE_FULL_CENTER    2  // Scale and center (letterbox/pillarbox)
#define GR_RESIZE_MENU           3  // Menu-specific scaling (4:3 safe area)
#define GR_RESIZE_MENU_ZOOMED    4  // Menu with zoom effect
#define GR_RESIZE_MENU_NO_OFFSET 5  // Menu without centering offset
```

---

## 9. Font and Glyph Management

### Font Types

FreeSpace 2 supports two font rendering systems:

| System | Format | Rendering | Use Case |
|--------|--------|-----------|----------|
| VFNT | Bitmap atlas | `SDR_TYPE_DEFAULT_MATERIAL` | Legacy fonts, fixed sizes |
| NanoVG | TrueType/OpenType | `SDR_TYPE_NANOVG` | Scalable fonts, Unicode |

### VFNT Bitmap Font Structure

VFNT fonts are pre-rendered bitmap atlases with per-character metrics:

```cpp
struct font_data {
    int bitmap_id;          // Texture atlas handle
    int num_chars;          // Number of characters in font
    int first_ascii;        // ASCII code of first character
    int w, h;               // Character cell dimensions
    int* char_widths;       // Per-character advance widths
    int* bm_u;              // U texture coordinates per character
    int* bm_v;              // V texture coordinates per character
};
```

### VFNT Text Rendering Path

**File:** `code/graphics/render.cpp:567`

VFNT rendering builds a vertex buffer of quads (2 triangles each) and submits them in batches:

```cpp
static void gr_string_old(float sx, float sy, const char* s, const char* end,
                          font::font* fontData, float height,
                          bool canAutoScale, bool canScale,
                          int resize_mode, float scaleMultiplier)
{
    GR_DEBUG_SCOPE("Render VFNT string");

    // VFNT uses DEFAULT_MATERIAL because it requires per-vertex colors
    material render_mat;
    render_mat.set_shader_type(SDR_TYPE_DEFAULT_MATERIAL);
    render_mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);
    render_mat.set_depth_mode(ZBUFFER_TYPE_NONE);
    render_mat.set_texture_map(TM_BASE_TYPE, fontData->bitmap_id);
    render_mat.set_color(GR_CURRENT_COLOR.red, GR_CURRENT_COLOR.green,
                        GR_CURRENT_COLOR.blue, GR_CURRENT_COLOR.alpha);
    render_mat.set_texture_type(material::TEX_TYPE_AABITMAP);

    // Vertex layout: position, texcoord, AND per-vertex color
    vertex_layout vert_def;
    vert_def.add_vertex_component(vertex_format_data::POSITION2, sizeof(v4), offsetof(v4, x));
    vert_def.add_vertex_component(vertex_format_data::TEX_COORD2, sizeof(v4), offsetof(v4, u));
    vert_def.add_vertex_component(vertex_format_data::COLOR4F, sizeof(v4), offsetof(v4, r));

    int buffer_offset = 0;

    // Build vertex buffer for all characters
    while (s < end) {
        // Get glyph UV coordinates from font atlas
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
        if (buffer_offset >= MAX_VERTS_PER_DRAW) {
            gr_render_primitives_immediate(&render_mat, PRIM_TYPE_TRIS,
                                          &vert_def, buffer_offset,
                                          String_render_buff, sizeof(v4) * buffer_offset);
            buffer_offset = 0;
        }

        x += raw_spacing * scale_factor;
        s++;
    }

    // Render remaining characters
    if (buffer_offset > 0) {
        gr_render_primitives_immediate(&render_mat, PRIM_TYPE_TRIS,
                                      &vert_def, buffer_offset,
                                      String_render_buff, sizeof(v4) * buffer_offset);
    }
}
```

### NanoVG Text Rendering

NanoVG is used for TrueType fonts and provides scalable, high-quality text rendering. It requires stencil buffer support for its fill algorithm.

**File:** `code/graphics/vulkan/VulkanGraphics.cpp`

```cpp
void gr_vulkan_render_nanovg(nanovg_material* material_info, ...)
{
    // NanoVG requires stencil buffer for its path filling algorithm
    Assertion(rt.depthFormat != vk::Format::eUndefined,
              "render_nanovg requires a depth/stencil attachment");
    Assertion(ctxBase.renderer.renderTargets()->depthHasStencil(),
              "render_nanovg requires a stencil-capable depth format");

    // Use SDR_TYPE_NANOVG shader
    ShaderModules shaderModules = ctxBase.renderer.getShaderModules(SDR_TYPE_NANOVG);

    // Configure stencil operations for path rendering
    pipelineKey.stencil_test_enable = material_info->is_stencil_enabled();
    // ... additional stencil configuration ...

    // Disable depth operations (2D rendering)
    cmd.setDepthTestEnable(VK_FALSE);
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setStencilTestEnable(material_info->is_stencil_enabled() ? VK_TRUE : VK_FALSE);
}
```

### Font Selection in HUD Gauges

Each gauge can specify its preferred font via the `font_num` member:

```cpp
void HudGauge::setFont() {
    font::set_font(font_num);
}
```

Common font constants:
- `font::FONT1` - Primary HUD font
- `font::FONT2` - Secondary/smaller HUD font
- `font::FONT3` - Large display font

---

## 10. Render-to-Texture and Cockpit Displays

### Cockpit Display System Overview

Some HUD gauges render to textures that are then applied to 3D cockpit models, creating in-universe displays (MFDs, radar screens, etc.).

```
+------------------+     +-------------------+     +------------------+
|  HUD Gauge       |     |  Render Target    |     |  Cockpit Model   |
|  (2D content)    | --> |  (RTT texture)    | --> |  (3D mesh)       |
+------------------+     +-------------------+     +------------------+
                               ^
                               |
              gr_set_screen_scale(canvas_w, canvas_h, ...)
```

### Starting Cockpit Display Rendering

**File:** `code/ship/ship.cpp`

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

    // Clear the texture to prepare for new content
    gr_clear();

    return display->target;
}
```

### Gauge Render Canvas Setup

**File:** `code/hud/hud.cpp:1453`

This function determines whether a gauge should render to the current target:

```cpp
bool HudGauge::setupRenderCanvas(int render_target)
{
    if (texture_target_fname[0] != '\0') {
        // Gauge is configured to render to a specific texture
        if ( render_target >= 0 && render_target == texture_target ) {
            return true;  // Match: render to this cockpit display
        }
        return false;  // Wrong target, skip this gauge
    } else {
        // Gauge renders to screen (no texture target)
        if ( render_target < 0 ) {
            return true;  // Match: render to screen
        }
        return false;  // We're rendering to a texture, but this gauge is screen-only
    }
}
```

**Important:** The actual screen scale and 2D matrix setup occurs in individual gauge rendering methods (`renderBitmap()`, `renderString()`, etc.), not in `setupRenderCanvas()`. This function only validates whether rendering should proceed.

### Ending Cockpit Display Rendering

```cpp
if ( cockpit_display_num >= 0 ) {
    ship_end_render_cockpit_display(cockpit_display_num);

    // Safety check: ensure render target is reset
    if ( gr_screen.rendering_to_texture != -1 ) {
        bm_set_render_target(-1);
    }
}
```

---

## 11. Dynamic State and Pipeline Configuration

### Pipeline Key Structure

Vulkan pipelines are cached based on a key that captures all relevant state. This avoids redundant pipeline creation:

```cpp
struct PipelineKey {
    // Shader identification
    shader_type type;                    // SDR_TYPE_INTERFACE, etc.
    uint variant_flags;                  // Shader variant flags

    // Render target format
    VkFormat color_format;               // e.g., VK_FORMAT_B8G8R8A8_SRGB
    VkFormat depth_format;               // e.g., VK_FORMAT_D32_SFLOAT_S8_UINT
    VkSampleCountFlagBits sample_count;  // MSAA sample count
    uint32_t color_attachment_count;     // Number of color outputs

    // Fixed pipeline state (when not using dynamic state)
    gr_alpha_blend blend_mode;           // Alpha blend mode
    size_t layout_hash;                  // Vertex layout hash

    // Additional state...
};
```

### Dynamic State Commands

The Vulkan backend uses dynamic state extensively to reduce pipeline permutations:

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

// Disable depth if no attachment present
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
    const auto& caps = ctxBase.renderer.getExtendedDynamicState3Caps();

    if (caps.colorBlendEnable) {
        vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE)
                                 ? VK_TRUE : VK_FALSE;
        cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
    }

    if (caps.colorWriteMask) {
        // Set color write mask from material
        vk::ColorComponentFlags mask = convertColorMask(material_info->get_color_mask());
        cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(1, &mask));
    }
}

// Viewport and scissor (always dynamic)
vk::Viewport viewport = createFullScreenViewport();
cmd.setViewport(0, 1, &viewport);

vk::Rect2D scissor = createClipScissor();  // Respects gr_screen.clip_*
cmd.setScissor(0, 1, &scissor);
```

### Z-Buffer Type Enumeration

**File:** `code/graphics/grinternal.h:69`

```cpp
typedef enum gr_zbuffer_type {
    ZBUFFER_TYPE_NONE,     // No depth testing or writing (HUD default)
    ZBUFFER_TYPE_READ,     // Read depth buffer, don't write
    ZBUFFER_TYPE_WRITE,    // Write depth buffer, don't test
    ZBUFFER_TYPE_FULL,     // Both read and write
    ZBUFFER_TYPE_DEFAULT   // Use current default mode
} gr_zbuffer_type;
```

---

## 12. Debugging and Performance

### Debug Scopes

The HUD system uses debug scopes for profiling and debugging:

```cpp
GR_DEBUG_SCOPE("Render HUD gauge");     // GPU debug marker
TRACE_SCOPE(tracing::RenderHUDGauge);   // CPU profiling scope
```

These integrate with:
- RenderDoc (GPU frame capture)
- Tracy/Optick (CPU profiling)
- Vulkan validation layers

### Common Issues and Diagnostics

#### HUD Not Rendering

1. Check `hud_disabled()` return value
2. Verify gauge's `canRender()` returns true
3. Check `setupRenderCanvas()` for target mismatch
4. Verify blend mode is not `ALPHA_BLEND_NONE` with zero alpha

#### Incorrect Colors

1. Verify sRGB conversion is enabled (`srgb = 1` in uniform)
2. Check `intensity` multiplier value
3. For AA textures, ensure `alphaTexture = 1`

#### Clipping Issues

1. Check `gr_screen.clip_*` values
2. Verify scissor is not clamped to zero extent
3. Check screen scale setup in rendering methods

### Performance Considerations

**Batching**: The HUD system benefits from batching similar draw calls. Gauges using the same texture and shader can be rendered efficiently.

**Texture Atlases**: VFNT fonts and HUD graphics often share atlases to reduce texture binds.

**Dynamic State**: Using dynamic state reduces pipeline permutations but may have minor overhead on some hardware.

**Render-to-Texture**: Cockpit displays add overhead. Consider reducing resolution for distant cockpits.

---

## 13. Known Issues and Edge Cases

### EMP Effect Handling

EMP effects can disable HUD gauge rendering and add random jitter to positions:

**File:** `code/hud/hud.h:88`

```cpp
#define GR_AABITMAP(a, b, c) {                  \
    int jx, jy;                                  \
    if(emp_should_blit_gauge()) {                \
        gr_set_bitmap(a);                        \
        jx = b; jy = c;                          \
        emp_hud_jitter(&jx, &jy);                \
        gr_aabitmap(jx, jy);                     \
    }                                            \
}
```

**Workaround**: Custom gauges should call `emp_should_blit_gauge()` before rendering and apply `emp_hud_jitter()` to coordinates.

### Scissor Clipping Edge Cases

The scissor rectangle is clamped to framebuffer dimensions to prevent Vulkan validation errors:

```cpp
vk::Rect2D createClipScissor()
{
    auto clip = getClipScissorFromScreen(gr_screen);
    // Clamp to framebuffer to prevent negative extents or overflow
    clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
    // ...
}
```

**Issue**: Extremely large or negative clip values could cause unexpected behavior before clamping was added.

### Shader Type Mismatch

VFNT text rendering requires `SDR_TYPE_DEFAULT_MATERIAL` (vertex color support), while bitmap rendering uses `SDR_TYPE_INTERFACE` (uniform color only). Mixing these incorrectly causes rendering artifacts.

| Content Type | Required Shader | Color Source |
|--------------|----------------|--------------|
| VFNT text | `SDR_TYPE_DEFAULT_MATERIAL` | Per-vertex |
| HUD bitmaps | `SDR_TYPE_INTERFACE` | Uniform |
| NanoVG text | `SDR_TYPE_NANOVG` | Uniform |

### Render Target Switching

When switching between cockpit display rendering and main screen rendering, the render target must be properly reset:

```cpp
if ( cockpit_display_num >= 0 ) {
    ship_end_render_cockpit_display(cockpit_display_num);

    if ( gr_screen.rendering_to_texture != -1 ) {
        // Safety check: reset render target if still set (shouldn't happen)
        bm_set_render_target(-1);
    }
}
```

**Issue**: Failing to reset the render target can cause subsequent gauges to render to the wrong surface.

### Supernova Effect

HUD rendering is completely disabled during supernova events to enhance the dramatic effect:

**File:** `code/hud/hud.cpp:2102`

```cpp
if( supernova_stage() >= SUPERNOVA_STAGE::HIT) {
    return;  // Skip all HUD rendering during supernova
}
```

### Resolution Scaling Artifacts

When gauges are designed for a specific resolution (e.g., 1024x768) but rendered at a different resolution, scaling can introduce:
- Sub-pixel positioning (fuzzy edges)
- Aspect ratio distortion
- Off-by-one pixel errors

**Mitigation**: Use the `aspect_quotient` correction factor and design gauges at common resolutions.

---

## Appendix A: Quick Reference

### Material Setup for HUD Elements

```cpp
material mat;
mat.set_blend_mode(ALPHA_BLEND_ALPHA_BLEND_ALPHA);  // Standard transparency
mat.set_depth_mode(ZBUFFER_TYPE_NONE);              // No depth testing
mat.set_texture_map(TM_BASE_TYPE, bitmap_id);       // Set texture
mat.set_color(r, g, b, a);                          // Set color (0-255 range)
mat.set_cull_mode(false);                           // Disable backface culling
mat.set_texture_type(material::TEX_TYPE_AABITMAP);  // For fonts/masks
```

### Texture Type Reference

| Type | Purpose | Alpha Source |
|------|---------|--------------|
| `TEX_TYPE_NORMAL` | Opaque textures | None (alpha = 1) |
| `TEX_TYPE_XPARENT` | Transparent textures | Alpha channel |
| `TEX_TYPE_AABITMAP` | Fonts, masks | Red channel |
| `TEX_TYPE_INTERFACE` | UI graphics | Alpha channel |

---

## Appendix B: Key File Locations

| Purpose | File Path |
|---------|-----------|
| HUD gauge base class | `code/hud/hud.h:214` |
| HUD rendering entry point | `code/hud/hud.cpp:2064` |
| Vulkan primitive rendering | `code/graphics/vulkan/VulkanGraphics.cpp` |
| Interface fragment shader | `code/graphics/shaders/interface.frag` |
| Interface vertex shader | `code/graphics/shaders/interface.vert` |
| Material class | `code/graphics/material.h:32` |
| Blend mode definitions | `code/graphics/grinternal.h:60` |
| Font rendering | `code/graphics/render.cpp:567` |
| gr_aabitmap implementation | `code/graphics/render.cpp:199` |
| Shader type enumeration | `code/graphics/2d.h:199` |
| Gamma conversion functions | `code/def_files/data/effects/gamma.sdr` |

---

## Appendix C: Shader Type Matrix

| Shader Type | Use Case | Vertex Color | Stencil | Typical Material |
|-------------|----------|--------------|---------|------------------|
| `SDR_TYPE_INTERFACE` | HUD bitmaps, UI | No (uniform) | No | `interface_material` |
| `SDR_TYPE_DEFAULT_MATERIAL` | VFNT text | Yes | No | `material` |
| `SDR_TYPE_NANOVG` | TrueType text | No | Yes | `nanovg_material` |
| `SDR_TYPE_BATCHED_BITMAP` | Particles | No | No | `particle_material` |
| `SDR_TYPE_ROCKET_UI` | RocketUI menus | No | No | `rocket_material` |

---

## Revision History

| Date | Author | Changes |
|------|--------|---------|
| 2024-12 | - | Initial comprehensive documentation |
| 2025-01 | - | Added debugging section, improved accuracy of code references |
