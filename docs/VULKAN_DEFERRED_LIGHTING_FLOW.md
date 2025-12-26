# Vulkan Deferred Lighting Complete Flow

This document describes the complete deferred lighting pipeline in the Vulkan renderer, from geometry pass through light volumes to final composite. It covers G-buffer rendering, emissive preservation, light volume meshes, and the complete sequence.

---

## Table of Contents

1. [Prerequisites](#1-prerequisites)
2. [Overview](#2-overview)
3. [Complete Flow Diagram](#3-complete-flow-diagram)
4. [Geometry Pass](#4-geometry-pass)
5. [Emissive Preservation](#5-emissive-preservation)
6. [Lighting Pass](#6-lighting-pass)
7. [Light Volume Rendering](#7-light-volume-rendering)
8. [Shader Implementation Details](#8-shader-implementation-details)
9. [Integration Points](#9-integration-points)
10. [Performance Considerations](#10-performance-considerations)
11. [Common Issues](#11-common-issues)
12. [References](#12-references)

---

## 1. Prerequisites

Before reading this document, you should be familiar with:

- **Deferred shading** - A rendering technique that decouples geometry rasterization from lighting calculations by storing material properties in intermediate buffers (the G-buffer).
- **Vulkan dynamic rendering** - The `VK_KHR_dynamic_rendering` extension used in this renderer (see `VULKAN_RENDER_PASS_STRUCTURE.md`).
- **Capability tokens** - The typestate pattern used to enforce correct API sequencing (see `VULKAN_CAPABILITY_TOKENS.md`).
- **Descriptor sets and push descriptors** - How textures and uniforms are bound to shaders (see `VULKAN_DESCRIPTOR_SETS.md`).

**Key Terminology**:

| Term | Description |
|------|-------------|
| **G-buffer** | Geometry buffer; a set of render targets storing per-pixel material properties |
| **Light volume** | A 3D mesh (sphere, cylinder) representing the spatial extent of a light source |
| **Push descriptors** | `VK_KHR_push_descriptor` extension allowing per-draw descriptor updates without pre-allocated sets |
| **Vertex pulling** | Technique where vertex data is fetched from SSBOs in the vertex shader rather than via vertex input bindings |
| **HDR scene target** | 16-bit float offscreen render target used for high dynamic range rendering before tonemapping |

---

## 2. Overview

### Why Deferred Lighting?

Deferred lighting separates geometry rendering from lighting calculations, providing these benefits:

1. **Decoupled light count** - Lighting cost scales with screen pixels lit, not geometry complexity times light count.
2. **Consistent shading** - All geometry uses the same lighting code path via fullscreen/volume passes.
3. **Simplified material pipeline** - The geometry pass writes material properties; lighting reads them uniformly.

The pipeline consists of three phases:

1. **Geometry Pass** - Renders material properties to G-buffer (5 color attachments + depth)
2. **Lighting Pass** - Samples G-buffer and applies lights using fullscreen quads or light volume meshes
3. **Composite** - Final result written to swapchain or scene HDR target for post-processing

### Key Files

| File | Purpose |
|------|---------|
| `code/graphics/vulkan/VulkanRenderer.cpp` | Deferred lighting orchestration and command recording |
| `code/graphics/vulkan/VulkanDeferredLights.cpp` | Light data building and per-light draw recording |
| `code/graphics/vulkan/VulkanDeferredLights.h` | Light UBO structures and light type definitions |
| `code/graphics/vulkan/VulkanRenderingSession.cpp` | G-buffer target management and layout transitions |
| `code/graphics/vulkan/VulkanRenderTargets.h` | G-buffer image/view creation and accessors |
| `code/graphics/shaders/deferred.vert` | Light volume vertex transformation |
| `code/graphics/shaders/deferred.frag` | G-buffer sampling and PBR lighting calculations |
| `code/graphics/shaders/lighting.glsl` | Shared light type constants and BRDF functions |

### G-Buffer Attachments

The G-buffer uses 5 color attachments plus a shared depth buffer:

| Index | Name | Format | Content |
|-------|------|--------|---------|
| 0 | Color | `R16G16B16A16_SFLOAT` | Base texture color (RGB) + alpha (A) |
| 1 | Normal | `R16G16B16A16_SFLOAT` | View-space normal (RGB) + gloss placeholder (A=1.0) |
| 2 | Position | `R16G16B16A16_SFLOAT` | View-space position (RGB) + AO placeholder (A=1.0) |
| 3 | Specular | `R16G16B16A16_SFLOAT` | Specular color (RGB) + fresnel factor (A) |
| 4 | Emissive | `R16G16B16A16_SFLOAT` | Glow/emissive color (RGBA) |
| - | Depth | Runtime-selected | `D32_SFLOAT_S8_UINT` preferred; fallback to `D24_UNORM_S8_UINT` or `D32_SFLOAT` |

**Note**: The 16-bit float format (`R16G16B16A16_SFLOAT`) is used for all G-buffer attachments to preserve precision for HDR lighting calculations.

---

## 3. Complete Flow Diagram

### High-Level Sequence

```
[Frame Start]
    |
    +-- beginDeferredLighting()
    |   +-- [If scene texture active] Capture HDR scene -> emissive
    |   +-- [Else if swapchain supports TRANSFER_SRC] Capture swapchain -> emissive
    |   +-- beginDeferredPass(clearNonColorBufs, preserveEmissive)
    |   |   +-- Transition G-buffer -> COLOR_ATTACHMENT_OPTIMAL
    |   |   +-- Transition depth -> DEPTH_ATTACHMENT_OPTIMAL
    |   |   +-- Set loadOp: eClear for all G-buffer, eLoad for emissive if preserved
    |   +-- ensureRenderingStartedRecording() -> Begin dynamic rendering
    |
    +-- [Geometry Rendering]
    |   +-- Bind model pipeline (SDR_TYPE_MODEL, 5 color attachments)
    |   +-- Draw models using vertex pulling + bindless textures
    |   +-- Shader writes: albedo, normal, position, specular, emissive
    |
    +-- deferredLightingEnd()
    |   +-- endDeferredGeometry()
    |   |   +-- endActivePass() -> End dynamic rendering
    |   |   +-- transitionGBufferToShaderRead(cmd)
    |   |   +-- Select SwapchainNoDepthTarget (default)
    |   +-- [If scene texture active] Override to SceneHdrNoDepthTarget
    |   +-- Return DeferredLightingCtx
    |
    +-- deferredLightingFinish()
    |   +-- bindDeferredGlobalDescriptors()
    |   |   +-- Bind G-buffer textures to set=1: albedo(0), normal(1), position(2),
    |   |       depth(3), specular(4), emissive(5)
    |   |
    |   +-- buildDeferredLights()
    |   |   +-- Build ambient light (fullscreen, blend-off, MUST be first)
    |   |   +-- Build directional lights (fullscreen, additive)
    |   |   +-- Build point lights (sphere volume, additive)
    |   |   +-- Build cone lights (sphere volume, additive)
    |   |   +-- Build tube lights (cylinder volume, additive)
    |   |
    |   +-- ensureRenderingStartedRecording() -> Begin lighting pass
    |   +-- recordDeferredLighting()
    |   |   +-- For each light: bind pipeline, push descriptors, draw volume
    |   |
    |   +-- Restore scissor, requestMainTargetWithDepth()
    |
    +-- [Continue with transparent objects, HUD, UI]
```

### Image Layout Transitions

```
G-Buffer Images (all 5 attachments):
    UNDEFINED
        | beginDeferredPass()
        v
    COLOR_ATTACHMENT_OPTIMAL (geometry writes)
        | transitionGBufferToShaderRead()
        v
    SHADER_READ_ONLY_OPTIMAL (lighting samples)
        | [Next frame or target switch]
        v
    COLOR_ATTACHMENT_OPTIMAL (or UNDEFINED if cleared)

Depth Buffer:
    UNDEFINED (or previous layout)
        | beginDeferredPass()
        v
    DEPTH_ATTACHMENT_OPTIMAL (geometry writes)
        | transitionGBufferToShaderRead()
        v
    DEPTH_READ_ONLY_OPTIMAL (lighting samples)
        | requestMainTargetWithDepth()
        v
    DEPTH_ATTACHMENT_OPTIMAL (subsequent draws)
```

---

## 4. Geometry Pass

### 4.1 Beginning the Pass

**Entry Point**: `VulkanRenderer::beginDeferredLighting(RecordingFrame& rec, bool clearNonColorBufs)`

The geometry pass begins by:

1. **Preserving scissor state** - The clip scissor is captured and restored after internal fullscreen passes.

2. **Determining emissive preservation** - Pre-deferred content (stars, nebulae) must be captured before clearing the G-buffer:
   - If `m_sceneTexture` is active: copy from HDR scene target
   - Else if swapchain supports `TRANSFER_SRC`: copy from swapchain
   - Otherwise: no preservation (emissive buffer cleared)

3. **Beginning the deferred pass**:
   ```cpp
   m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
   ```

4. **Starting dynamic rendering** - Even if no geometry draws occur, this ensures clear operations execute.

### 4.2 G-Buffer Target Setup

**Function**: `VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive)`

```cpp
// Depth is always cleared when entering deferred geometry
m_clearOps = m_clearOps.withDepthStencilClear();

// All G-buffer attachments clear by default
m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eClear);

// Emissive attachment: preserve if pre-deferred content was captured
if (preserveEmissive) {
    m_gbufferLoadOps[VulkanRenderTargets::kGBufferEmissiveIndex] = vk::AttachmentLoadOp::eLoad;
}

m_target = std::make_unique<DeferredGBufferTarget>();
```

**Note**: The `clearNonColorBufs` parameter is retained for API parity with OpenGL but Vulkan always clears non-emissive G-buffer attachments.

### 4.3 Geometry Rendering

**Pipeline Requirements**:
- Shader type: `SDR_TYPE_MODEL`
- Color format: `R16G16B16A16_SFLOAT`
- Color attachment count: 5
- Depth format: Runtime-selected (matches G-buffer depth)

**Shader Outputs** (from `model.frag`):
```glsl
layout(location = 0) out vec4 outColor;     // Base texture color
layout(location = 1) out vec4 outNormal;    // View-space normal, A=1.0
layout(location = 2) out vec4 outPosition;  // View-space position, A=1.0
layout(location = 3) out vec4 outSpecular;  // Specular texture sample
layout(location = 4) out vec4 outEmissive;  // Glow texture (additive with preserved content)
```

**Rendering Characteristics**:
- **Vertex pulling**: Vertex data fetched from SSBO, not via vertex input bindings
- **Bindless textures**: Material texture indices passed via push constants
- **Dynamic uniform offsets**: Per-model transform data via uniform ring buffer

---

## 5. Emissive Preservation

### 5.1 Purpose

Emissive preservation captures pre-deferred scene content (background stars, nebulae, skybox) and stores it in the G-buffer emissive attachment. This content is then composited during the ambient lighting pass.

### 5.2 Capture Scenarios

| Scenario | Source | Method | Notes |
|----------|--------|--------|-------|
| Scene HDR active | HDR offscreen target | Fullscreen copy shader (`SDR_TYPE_COPY`) | Scene rendered to HDR before deferred |
| Swapchain direct | Swapchain image | Blit to intermediate, then copy shader | Requires `TRANSFER_SRC` usage flag |
| No pre-deferred content | N/A | G-buffer emissive cleared | `preserveEmissive = false` |

### 5.3 Capture Sequence

**For HDR Scene Target**:
```cpp
// 1. End active rendering, transition scene HDR to shader-readable
m_renderingSession->suspendRendering();
m_renderingSession->transitionSceneHdrToShaderRead(cmd);

// 2. Request single-attachment emissive target
m_renderingSession->requestGBufferEmissiveTarget();

// 3. Fullscreen copy pass
const auto emissiveRender = ensureRenderingStartedRecording(rec);
recordPreDeferredSceneHdrCopy(emissiveRender);  // SDR_TYPE_COPY shader
```

**Key Point**: Capture happens **before** the geometry pass, so model emissive (glow maps) additively blends with preserved background content.

---

## 6. Lighting Pass

### 6.1 Ending Geometry, Beginning Lighting

**Function**: `VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd)`

```cpp
// Verify we are in G-buffer target
Assertion(dynamic_cast<DeferredGBufferTarget*>(m_target.get()) != nullptr, ...);

endActivePass();                        // End dynamic rendering
transitionGBufferToShaderRead(cmd);     // All 5 G-buffer + depth to SHADER_READ_ONLY
m_target = std::make_unique<SwapchainNoDepthTarget>();  // Default lighting target
```

**Target Override**: If scene texture mode is active, `deferredLightingEnd()` overrides the target:
```cpp
if (m_sceneTexture.has_value()) {
    m_renderingSession->requestSceneHdrNoDepthTarget();
}
```

**Important**: The lighting target has **no depth attachment**. Depth is sampled from the G-buffer position/depth textures for lighting calculations.

### 6.2 Building Lights

**Function**: `buildDeferredLights(VulkanFrame& frame, vk::Buffer uniformBuffer, const matrix4& viewMatrix, const matrix4& projMatrix, uint32_t uniformAlignment)`

Lights are built from the engine's global `Lights` array. Each light type produces a specific light variant:

**Ambient Light** (synthetic, always first):
```cpp
FullscreenLight ambient{};
ambient.isAmbient = true;
ambient.light.lightType = LT_AMBIENT;  // Value: 4
// Identity matrices (fullscreen quad in clip space)
// Color from gr_get_ambient_light()
```

**Directional Lights**:
```cpp
FullscreenLight directional{};
directional.isAmbient = false;
directional.light.lightType = LT_DIRECTIONAL;  // Value: 0
// Direction transformed to view space
// Identity matrices (fullscreen quad)
```

**Point Lights**:
```cpp
SphereLight point{};
point.light.lightType = LT_POINT;  // Value: 1
// Model matrix: translation to light position in world space
// Scale: radius * 1.05 (5% oversize ensures edge coverage)
```

**Cone Lights**:
```cpp
SphereLight cone{};
cone.light.lightType = LT_CONE;  // Value: 3
// Same sphere volume as point (conservative bounding)
// Cone direction and angles passed in UBO for shader culling
```

**Tube Lights**:
```cpp
CylinderLight tube{};
tube.light.lightType = LT_TUBE;  // Value: 2
// Model matrix: translation + rotation aligning -Z with tube direction
// Scale: radius * 1.05 for X/Y, full length for Z
```

**Cockpit Lighting Mode**: When `Lighting_mode == lighting_mode::COCKPIT`, light intensity and radius are modified by the current lighting profile's cockpit modifiers.

### 6.3 Rendering Lights

**Function**: `VulkanRenderer::recordDeferredLighting(...)`

Each light is rendered by its `record()` method:

```cpp
for (const auto& light : lights) {
    std::visit([&](const auto& l) {
        l.record(ctx, meshVB, meshIB, indexCount);
    }, light);
}
```

**Per-Light Recording**:
1. **Bind pipeline**: Ambient uses blend-off pipeline; others use additive blend
2. **Set dynamic blend enable** (if `VK_EXT_extended_dynamic_state3` available)
3. **Push descriptors**: Matrix and light UBOs via `pushDescriptorSetKHR()`
4. **Draw**: Fullscreen triangle (3 vertices) or indexed volume mesh

---

## 7. Light Volume Rendering

### 7.1 Volume Mesh Types

| Mesh | Geometry | Draw Method | Used For |
|------|----------|-------------|----------|
| Fullscreen | 3-vertex triangle covering clip space | `draw(3, 1, 0, 0)` | Ambient, Directional |
| Sphere | Indexed icosphere | `drawIndexed(indexCount, ...)` | Point, Cone |
| Cylinder | Indexed cylinder | `drawIndexed(indexCount, ...)` | Tube |

Meshes are created at renderer initialization and stored in `VolumeMesh` structures:
```cpp
struct VolumeMesh {
    gr_buffer_handle vbo;       // Vertex buffer
    gr_buffer_handle ibo{};     // Index buffer (optional)
    uint32_t indexCount = 0;    // For indexed draws
};
```

### 7.2 Volume Scaling

Light volumes are scaled to ensure complete coverage:

- **Point/Cone lights**: Sphere scaled by `radius * 1.05` (5% oversize)
- **Tube lights**: Cylinder radius scaled by `radius * 1.05`; length unscaled

The 5% oversize margin prevents edge artifacts where the volume mesh clips the light's influence region.

### 7.3 Blend Modes

| Light Type | Blend Mode | Reason |
|------------|------------|--------|
| Ambient | Disabled (overwrite) | Initializes render target; writes emissive + ambient |
| All others | Additive (src + dst) | Accumulates light contributions |

The ambient light **must** be rendered first with blending disabled to initialize the render target with a defined value. Subsequent lights use additive blending.

---

## 8. Shader Implementation Details

### 8.1 Vertex Shader (deferred.vert)

The vertex shader handles both fullscreen and volume rendering:

```glsl
void main() {
    if (lightType == LT_DIRECTIONAL || lightType == LT_AMBIENT) {
        // Fullscreen: pass through clip-space coordinates
        gl_Position = vec4(vertPosition.xyz, 1.0);
    } else {
        // Volume: transform scaled geometry through MVP
        gl_Position = projMatrix * modelViewMatrix * vec4(vertPosition.xyz * scale, 1.0);
    }
}
```

### 8.2 Fragment Shader (deferred.frag)

**G-Buffer Sampling**:
```glsl
layout(set = 1, binding = 0) uniform sampler2D ColorBuffer;
layout(set = 1, binding = 1) uniform sampler2D NormalBuffer;
layout(set = 1, binding = 2) uniform sampler2D PositionBuffer;
layout(set = 1, binding = 3) uniform sampler2D DepthBuffer;  // Currently unused
layout(set = 1, binding = 4) uniform sampler2D SpecularBuffer;
layout(set = 1, binding = 5) uniform sampler2D EmissiveBuffer;
```

**Screen UV Calculation**:
```glsl
vec2 getScreenUV() {
    ivec2 sz = textureSize(ColorBuffer, 0);
    return gl_FragCoord.xy / vec2(sz);
}
```

**Background Pixel Handling**:
Background pixels (no geometry) have position buffer at clear value (0,0,0,0). The shader handles this:

```glsl
if (dot(position, position) < 1.0e-8) {
    if (lightType == LT_AMBIENT) {
        // Output preserved emissive for background
        fragOut0 = vec4(texture(EmissiveBuffer, screenPos).rgb, 1.0);
        return;
    }
    discard;  // Other lights: skip background pixels
}
```

**Ambient Pass**:
```glsl
if (lightType == LT_AMBIENT) {
    float ao = position_buffer.w;  // Currently 1.0 (placeholder)
    vec3 emissive = texture(EmissiveBuffer, screenPos).rgb;
    outRgb = diffuseLightColor * diffColor * ao + emissive;
}
```

**Non-Ambient Lighting** (PBR path):
```glsl
vec4 specColor = texture(SpecularBuffer, screenPos);
float gloss = normalData.a;              // Currently 1.0 (placeholder)
float roughness = clamp(1.0 - gloss, 0.0, 1.0);
float alpha = roughness * roughness;

vec3 eyeDir = normalize(-position);
vec3 reflectDir = reflect(-eyeDir, normal);

vec3 L;
float attenuation;
float area_norm;
GetLightInfo(position, alpha, reflectDir, L, attenuation, area_norm);

// GGX BRDF computation
outRgb = computeLighting(specColor.rgb, diffColor, L, normal, halfVec, eyeDir,
                          roughness, fresnel, NdotL)
         * diffuseLightColor * attenuation * area_norm;
```

### 8.3 Light Type Calculations

**Directional**:
- Infinite distance, no attenuation
- Direction from UBO (pre-transformed to view space)

**Point**:
- Position extracted from `modelViewMatrix[3].xyz`
- Distance attenuation: `(1 - sqrt(dist / radius))^2`
- Area light expansion for `sourceRadius > 0`
- Fragments beyond `lightRadius` are discarded

**Cone**:
- Same position/attenuation as point
- Cone angle test: `dot(normalize(-lightDir), coneDir)` vs `coneAngle`
- Smooth falloff from `coneAngle` to `coneInnerAngle`
- `dualCone` option for bidirectional cones

**Tube**:
- Beam direction derived from model matrix: `modelViewMatrix * vec4(0, 0, -scale.z, 0)`
- Nearest point on beam segment calculation
- Same attenuation model as point lights
- Area light energy conservation (linear, not squared)

### 8.4 Area Light Energy Conservation

For spherical/tube area lights, the shader uses energy conservation from "Real Shading in Unreal Engine 4" (Brian Karis, Epic Games):

```glsl
vec3 ExpandLightSize(in vec3 lightDir, in vec3 reflectDir) {
    vec3 centerToRay = max(dot(lightDir, reflectDir), sourceRadius) * reflectDir - lightDir;
    return lightDir + centerToRay * clamp(sourceRadius / length(centerToRay), 0.0, 1.0);
}
```

---

## 9. Integration Points

### 9.1 Engine API

The deferred lighting system is exposed via the graphics API:

```cpp
// VulkanGraphics.cpp
void gr_vulkan_deferred_lighting_begin(bool clearNonColorBufs);
void gr_vulkan_deferred_lighting_end();
void gr_vulkan_deferred_lighting_finish();
```

**State Machine**:
```cpp
// Stored in g_backend->deferred variant
std::variant<std::monostate, DeferredGeometryCtx, DeferredLightingCtx>

// begin -> returns DeferredGeometryCtx
// end   -> consumes DeferredGeometryCtx, returns DeferredLightingCtx
// finish -> consumes DeferredLightingCtx, returns std::monostate
```

### 9.2 Scene Texture Integration

When HDR scene rendering is active (`scene_texture_begin()` was called):

1. **Geometry pass**: Outputs to G-buffer (same as swapchain path)
2. **Lighting pass**: Outputs to scene HDR target (not swapchain)
3. **Post-processing**: Reads scene HDR for bloom, tonemapping, AA

```cpp
// In deferredLightingEnd():
if (m_sceneTexture.has_value()) {
    m_renderingSession->requestSceneHdrNoDepthTarget();
}
```

### 9.3 Descriptor Binding Layout

**Set 0** (Push Descriptors, per-light):
| Binding | Type | Content |
|---------|------|---------|
| 0 | Uniform Buffer | `DeferredMatrixUBO` (modelView + projection matrices) |
| 1 | Uniform Buffer | `DeferredLightUBO` (light parameters) |

**Set 1** (Global, bound once per lighting pass):
| Binding | Type | Content |
|---------|------|---------|
| 0 | Combined Image Sampler | G-buffer color (albedo) |
| 1 | Combined Image Sampler | G-buffer normal |
| 2 | Combined Image Sampler | G-buffer position |
| 3 | Combined Image Sampler | G-buffer depth |
| 4 | Combined Image Sampler | G-buffer specular |
| 5 | Combined Image Sampler | G-buffer emissive |

---

## 10. Performance Considerations

### 10.1 G-Buffer Bandwidth

The G-buffer uses 5x `R16G16B16A16_SFLOAT` attachments (8 bytes per pixel each) plus depth:
- **Total per-pixel bandwidth (write)**: 40+ bytes
- **Lighting pass sampling**: 48+ bytes per pixel (6 textures)

This bandwidth cost is the primary overhead of deferred rendering. Mitigations:
- Early depth testing in geometry pass
- Light volume culling (fragments outside volume are not shaded)
- Fragment discard for pixels beyond light radius

### 10.2 Light Volume Overdraw

Each light volume potentially samples the entire G-buffer for covered pixels. Strategies to minimize overdraw:

- **Sphere volumes**: 5% oversize is minimal; tighter meshes risk missing edge pixels
- **Fullscreen lights**: Ambient and directional touch every pixel once
- **Future optimization**: Frustum and distance culling before building light list

### 10.3 Memory Layout

UBO alignment requirements vary by device. The uniform ring buffer respects `minUniformBufferOffsetAlignment`:

```cpp
uint32_t uniformAlignment = getMinUniformBufferAlignment();
auto alloc = frame.uniformBuffer().allocate(size, uniformAlignment);
```

---

## 11. Common Issues

### Issue 1: G-Buffer Not Cleared

**Symptoms**: Previous frame's G-buffer content visible; ghosting artifacts.

**Causes**:
- `beginDeferredPass()` not called before geometry rendering
- Emissive preserved incorrectly (stale content loaded)

**Fix**: Ensure `beginDeferredLighting()` is called at the start of each deferred frame.

### Issue 2: Lighting Output to Wrong Target

**Symptoms**: Deferred lighting renders to swapchain instead of scene HDR (or vice versa).

**Causes**:
- `scene_texture_begin()` not called before `deferredLightingBegin()`
- Target override in `deferredLightingEnd()` not triggering

**Fix**: Verify scene texture mode is active before entering deferred lighting if HDR output is desired.

### Issue 3: Emissive Not Preserved

**Symptoms**: Background stars/nebulae missing from final image.

**Causes**:
- `preserveEmissive = false` when it should be true
- Swapchain does not support `TRANSFER_SRC` usage (check swapchain creation)
- Capture shader (`SDR_TYPE_COPY`) not correctly bound

**Debugging**:
```cpp
bool canCapture = (m_vulkanDevice->swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc)
                  != vk::ImageUsageFlags{};
Warning(LOCATION, "canCaptureSwapchain: %s, preserveEmissive: %s",
        canCapture ? "true" : "false", preserveEmissive ? "true" : "false");
```

### Issue 4: Lights Not Appearing

**Symptoms**: Scene is dark except for emissive/ambient; point/cone/tube lights missing.

**Causes**:
- Light volumes not rendering (mesh not bound, draw call failing)
- G-buffer not bound to global descriptor set
- Light radius is zero or negative
- Blend mode incorrect (ambient overwrites instead of additives)

**Debugging**:
```cpp
Warning(LOCATION, "Lights built: %zu", lights.size());
for (const auto& light : lights) {
    std::visit([](const auto& l) {
        Warning(LOCATION, "  type=%d radius=%f", l.light.lightType, l.light.lightRadius);
    }, light);
}
```

### Issue 5: Validation Errors on Layout Transitions

**Symptoms**: Vulkan validation errors about image layout mismatches.

**Causes**:
- G-buffer sampled before `transitionGBufferToShaderRead()` called
- Layout tracking out of sync after error recovery

**Fix**: Ensure `endDeferredGeometry()` is called before `recordDeferredLighting()`. Check that no early returns skip the transition.

### Issue 6: Ambient Light Trails / Undefined Swapchain

**Symptoms**: Trails or garbage visible in background areas between frames.

**Cause**: The ambient pass uses `loadOp::eLoad` for the render target, but non-geometry pixels were never written.

**Fix**: The deferred fragment shader handles this by outputting emissive color for background pixels during the ambient pass:
```glsl
if (dot(position, position) < 1.0e-8 && lightType == LT_AMBIENT) {
    fragOut0 = vec4(emissive, 1.0);
    return;
}
```

---

## 12. References

### Source Files (Line References)

| File | Function | Lines | Description |
|------|----------|-------|-------------|
| `VulkanRenderer.cpp` | `beginDeferredLighting()` | 593-639 | Entry point for geometry pass |
| `VulkanRenderer.cpp` | `deferredLightingBegin/End/Finish()` | 1044-1099 | Public API wrappers |
| `VulkanRenderer.cpp` | `bindDeferredGlobalDescriptors()` | 1101+ | G-buffer descriptor binding |
| `VulkanDeferredLights.cpp` | `buildDeferredLights()` | 130-348 | Light construction from engine state |
| `VulkanDeferredLights.cpp` | `FullscreenLight::record()` | 66-84 | Fullscreen light draw recording |
| `VulkanDeferredLights.cpp` | `SphereLight::record()` | 86-104 | Point/cone light draw recording |
| `VulkanDeferredLights.cpp` | `CylinderLight::record()` | 106-124 | Tube light draw recording |
| `VulkanRenderingSession.cpp` | `beginDeferredPass()` | 202-218 | G-buffer target setup |
| `VulkanRenderingSession.cpp` | `endDeferredGeometry()` | 410-417 | Transition to lighting phase |
| `VulkanRenderTargets.h` | G-buffer constants | 15-22 | `kGBufferCount`, `kGBufferEmissiveIndex` |

### Related Documentation

- `VULKAN_RENDER_PASS_STRUCTURE.md` - Target types and dynamic rendering
- `VULKAN_CAPABILITY_TOKENS.md` - `RenderCtx`, `DeferredGeometryCtx`, `DeferredLightingCtx`
- `VULKAN_DESCRIPTOR_SETS.md` - Push descriptors and global descriptor sets
- `VULKAN_SYNCHRONIZATION.md` - Layout transitions and barrier rules
- `VULKAN_POST_PROCESSING.md` - What happens after deferred lighting (bloom, tonemap, AA)
- `VULKAN_MODEL_RENDERING_PIPELINE.md` - Geometry pass shader and vertex pulling details

### External References

- "Real Shading in Unreal Engine 4" (Brian Karis, Epic Games) - Area light energy conservation
- Vulkan `VK_KHR_dynamic_rendering` specification - Dynamic rendering API
- Vulkan `VK_KHR_push_descriptor` specification - Per-draw descriptor updates

---

## Appendix: G-Buffer Channel Usage Summary

| Attachment | Channel | Written By | Read By Shader As |
|------------|---------|------------|-------------------|
| **0: Color** | RGB | `model.frag` baseColor | Diffuse albedo for BRDF |
| | A | `model.frag` alpha | Material alpha (transparency) |
| **1: Normal** | RGB | `model.frag` TBN-transformed normal | Surface normal for NdotL, reflection |
| | A | 1.0 (placeholder) | Gloss factor (roughness = 1 - gloss) |
| **2: Position** | RGB | `model.frag` view-space position | Light direction calculation, distance |
| | A | 1.0 (placeholder) | Ambient occlusion multiplier |
| **3: Specular** | RGB | `model.frag` specular texture | Specular F0 color for BRDF |
| | A | `model.frag` fresnel | Fresnel blend factor |
| **4: Emissive** | RGB | `model.frag` glow + preserved background | Added during ambient pass only |
| | A | unused | - |

**Future Expansion**: The placeholder alpha channels (normal.a for gloss, position.a for AO) are reserved for PBR material enhancements. Current code writes 1.0; shaders read these values but the defaults produce no visual effect.
