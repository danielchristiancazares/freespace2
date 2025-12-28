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

**Related Documentation**: See `VULKAN_CAPABILITY_TOKENS.md` for detailed coverage of `DeferredGeometryCtx` and `DeferredLightingCtx` tokens.

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
| `code/graphics/vulkan/VulkanDeferredLights.h` | Light UBO structures (`DeferredMatrixUBO`, `DeferredLightUBO`) and light variant types |
| `code/graphics/vulkan/VulkanRenderingSession.cpp` | G-buffer target management and layout transitions |
| `code/graphics/vulkan/VulkanRenderTargets.h` | G-buffer image/view creation and accessors (`kGBufferCount`, `kGBufferEmissiveIndex`) |
| `code/graphics/vulkan/VulkanPhaseContexts.h` | Capability tokens (`DeferredGeometryCtx`, `DeferredLightingCtx`) |
| `code/graphics/vulkan/VulkanGraphics.cpp` | Engine API bridge (`gr_vulkan_deferred_lighting_*`) |
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

The deferred lighting pipeline uses typestate tokens to enforce correct API sequencing.

```
[Frame Start]
    |
    +-- gr_vulkan_deferred_lighting_begin(clearNonColorBufs)       [Engine API]
    |   +-- VulkanRenderer::deferredLightingBegin()                [Public API - returns DeferredGeometryCtx]
    |       +-- beginDeferredLighting()                            [Internal implementation]
    |           +-- Detect current target (scene HDR or swapchain)
    |           +-- [If scene HDR active] Capture scene HDR -> emissive G-buffer
    |           +-- [Else if swapchain + TRANSFER_SRC] Capture swapchain -> emissive G-buffer
    |           +-- beginDeferredPass(clearNonColorBufs, preserveEmissive)
    |           |   +-- Set m_gbufferLoadOps: eClear for all, eLoad for emissive if preserved
    |           |   +-- Set m_target = DeferredGBufferTarget
    |           +-- ensureRenderingStartedRecording() -> Begin dynamic rendering
    |
    +-- [Geometry Rendering - DeferredGeometryCtx is active]
    |   +-- Bind model pipeline (SDR_TYPE_MODEL, 5 color attachments)
    |   +-- Draw models using vertex pulling + bindless textures
    |   +-- Shader writes: albedo, normal, position, specular, emissive
    |   +-- [Optional: Decal pass via gr_start_decal_pass()]
    |
    +-- gr_vulkan_deferred_lighting_end()                          [Engine API]
    |   +-- VulkanRenderer::deferredLightingEnd()                  [Consumes DeferredGeometryCtx]
    |       +-- endDeferredGeometry()
    |       |   +-- endActivePass() -> End dynamic rendering
    |       |   +-- transitionGBufferToShaderRead(cmd)
    |       |       +-- Transition all 5 G-buffer images to SHADER_READ_ONLY_OPTIMAL
    |       |       +-- Transition depth to DEPTH_READ_ONLY_OPTIMAL (or DEPTH_STENCIL_READ_ONLY)
    |       |   +-- Set m_target = SwapchainNoDepthTarget (default)
    |       +-- [If scene texture active] Override to SceneHdrNoDepthTarget
    |       +-- Return DeferredLightingCtx
    |
    +-- gr_vulkan_deferred_lighting_finish()                       [Engine API]
    |   +-- VulkanRenderer::deferredLightingFinish()               [Consumes DeferredLightingCtx]
    |       +-- buildDeferredLights()                              [Boundary: engine lights -> variants]
    |       |   +-- Synthetic ambient light (fullscreen, blend-off, MUST be first)
    |       |   +-- Directional lights (fullscreen, additive)
    |       |   +-- Point lights (sphere volume, additive)
    |       |   +-- Cone lights (sphere volume, additive)
    |       |   +-- Tube lights (cylinder volume, additive)
    |       |
    |       +-- ensureRenderingStartedRecording() -> Begin lighting pass
    |       +-- recordDeferredLighting()
    |       |   +-- Set fullscreen viewport/scissor, disable depth test
    |       |   +-- Get deferred pipeline (SDR_TYPE_DEFERRED_LIGHTING)
    |       |   +-- Bind G-buffer descriptors (set=1) from frame.globalDescriptorSet()
    |       |   +-- For each light:
    |       |       +-- Bind pipeline (ambient uses blend-off, others additive)
    |       |       +-- Push UBO descriptors (set=0): matrices + light params
    |       |       +-- Draw volume mesh (fullscreen, sphere, or cylinder)
    |       |
    |       +-- Restore scissor
    |       +-- requestMainTargetWithDepth() -> Resume normal rendering
    |
    +-- [Continue with transparent objects, HUD, UI - deferred state is std::monostate]
```

**Typestate Enforcement**: The `std::variant<std::monostate, DeferredGeometryCtx, DeferredLightingCtx>` in `g_backend->deferred` ensures calls occur in sequence. Attempting to call `end()` without `begin()` triggers an assertion.

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

**Public Entry Point**: `VulkanRenderer::deferredLightingBegin(RecordingFrame& rec, bool clearNonColorBufs)`
**Internal Implementation**: `VulkanRenderer::beginDeferredLighting(RecordingFrame& rec, bool clearNonColorBufs)`

The geometry pass begins by:

1. **Preserving scissor state** - The clip scissor is captured via `getClipScissorFromScreen()` and stored in `restoreScissor`. This is restored after internal fullscreen copy passes.

2. **Detecting current render target** - The code checks `m_renderingSession->targetIsSceneHdr()` and `targetIsSwapchain()` to determine the capture source for emissive preservation.

3. **Determining emissive preservation** - Pre-deferred content (stars, nebulae) must be captured before clearing the G-buffer:
   - If scene HDR target is active: suspend rendering, transition scene HDR to shader-read, copy to emissive via `recordPreDeferredSceneHdrCopy()`
   - Else if swapchain is active and supports `TRANSFER_SRC`: capture swapchain via `captureSwapchainColorToSceneCopy()`, then copy to emissive via `recordPreDeferredSceneColorCopy()`
   - Otherwise: `preserveEmissive = false` (emissive buffer cleared)

4. **Beginning the deferred pass**:
   ```cpp
   m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
   ```

5. **Starting dynamic rendering** - Even if no geometry draws occur, this ensures clear operations execute:
   ```cpp
   (void)ensureRenderingStartedRecording(rec);
   ```

6. **Returning the geometry token**:
   ```cpp
   return DeferredGeometryCtx{m_frameCounter};
   ```

### 4.2 G-Buffer Target Setup

**Function**: `VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive)`

```cpp
void VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive) {
  endActivePass();
  // Vulkan mirrors OpenGL's deferred begin semantics:
  // - pre-deferred swapchain color is captured and copied into the emissive buffer (handled by VulkanRenderer)
  // - the remaining G-buffer attachments are cleared here by loadOp=CLEAR
  (void)clearNonColorBufs; // parameter retained for API parity; Vulkan always clears non-emissive G-buffer attachments.

  // Depth is shared across swapchain and deferred targets. Always clear it when entering deferred geometry so
  // pre-deferred draws (treated as emissive background) cannot occlude deferred geometry.
  m_clearOps = m_clearOps.withDepthStencilClear();

  m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eClear);
  if (preserveEmissive) {
    m_gbufferLoadOps[VulkanRenderTargets::kGBufferEmissiveIndex] = vk::AttachmentLoadOp::eLoad;
  }
  m_target = std::make_unique<DeferredGBufferTarget>();
}
```

**Note**: The `clearNonColorBufs` parameter is retained for API parity with OpenGL but Vulkan always clears non-emissive G-buffer attachments. The depth buffer is always cleared to prevent pre-deferred draws from occluding geometry.

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

### 4.4 Decal Pass (Optional)

During the geometry phase, decals can be rendered via `gr_start_decal_pass()`. This prepares the depth buffer for sampling while maintaining the G-buffer as the active render target.

The decal pass:
1. Copies the current depth buffer to a cockpit depth snapshot (if needed)
2. Binds the depth texture for shader sampling
3. Allows decals to read depth for projection while writing to G-buffer

**Precondition**: `DeferredGeometryCtx` must be active (enforced by assertion).

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

**Public Entry Point**: `VulkanRenderer::deferredLightingEnd(RecordingFrame& rec, DeferredGeometryCtx&& geometry)`
**Internal Helper**: `VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd)`

The transition from geometry to lighting consumes the `DeferredGeometryCtx` token and returns a `DeferredLightingCtx`:

```cpp
DeferredLightingCtx VulkanRenderer::deferredLightingEnd(RecordingFrame& rec, DeferredGeometryCtx&& geometry) {
  Assertion(geometry.frameIndex == m_frameCounter, ...);  // Validate token matches current frame
  vk::CommandBuffer cmd = rec.cmd();

  endDeferredGeometry(cmd);
  if (m_sceneTexture.has_value()) {
    // Deferred lighting output should land in the scene HDR target during scene texture mode.
    m_renderingSession->requestSceneHdrNoDepthTarget();
  }
  return DeferredLightingCtx{m_frameCounter};
}
```

**Session-Level Transition** (`VulkanRenderingSession::endDeferredGeometry`):

```cpp
void VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd) {
  Assertion(dynamic_cast<DeferredGBufferTarget*>(m_target.get()) != nullptr,
            "endDeferredGeometry called when not in deferred gbuffer target");

  endActivePass();                        // End dynamic rendering
  transitionGBufferToShaderRead(cmd);     // All 5 G-buffer + depth to SHADER_READ_ONLY
  m_target = std::make_unique<SwapchainNoDepthTarget>();  // Default lighting target
}
```

**Layout Transition Details**: `transitionGBufferToShaderRead()` issues a single barrier batch for all 6 images (5 G-buffer + 1 depth):

```cpp
// Creates barriers for kGBufferCount + 1 images
std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount + 1> barriers{};
// G-buffer: COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
// Depth: DEPTH_ATTACHMENT_OPTIMAL -> depthReadLayout() (DEPTH_READ_ONLY or DEPTH_STENCIL_READ_ONLY)
```

**Important**: The lighting target has **no depth attachment**. Depth is sampled from the G-buffer depth texture for lighting calculations if needed (currently unused by the shader).

### 6.2 Building Lights

**Function**: `buildDeferredLights(VulkanFrame& frame, vk::Buffer uniformBuffer, const matrix4& viewMatrix, const matrix4& projMatrix, uint32_t uniformAlignment)`

**Location**: `code/graphics/vulkan/VulkanDeferredLights.cpp`

This is a boundary function that converts engine light data to Vulkan-specific variants. Lights are built from the engine's global `Lights` array. Each light produces one of three variant types:

**Light Type Constants** (from `lighting.glsl`):
```glsl
const int LT_DIRECTIONAL = 0;  // A light like a sun
const int LT_POINT       = 1;  // A point light, like an explosion
const int LT_TUBE        = 2;  // A tube light, like a fluorescent light
const int LT_CONE        = 3;  // A cone light, like a flood light
const int LT_AMBIENT     = 4;  // Directionless ambient light
```

**UBO Structures** (from `VulkanDeferredLights.h`):
```cpp
// Must match deferred.vert layout(set=0, binding=0)
struct alignas(16) DeferredMatrixUBO {
  matrix4 modelViewMatrix;
  matrix4 projMatrix;
};

// Must match deferred.vert/frag layout(set=0, binding=1), std140 layout
struct alignas(16) DeferredLightUBO {
  float diffuseLightColor[3];  float coneAngle;
  float lightDir[3];           float coneInnerAngle;
  float coneDir[3];            uint32_t dualCone;
  float scale[3];              float lightRadius;
  int32_t lightType;           uint32_t enable_shadows;
  float sourceRadius;          float _pad;
};
```

**Ambient Light** (synthetic, always first):
```cpp
FullscreenLight ambient{};
ambient.isAmbient = true;
ambient.light.lightType = LT_AMBIENT_SHADER;  // Value: 4
// Identity matrices (fullscreen quad renders in clip space)
// Color from gr_get_ambient_light()
```

**Directional Lights**:
```cpp
FullscreenLight directional{};
directional.isAmbient = false;
directional.light.lightType = LT_DIRECTIONAL;  // Value: 0
// Direction transformed to view space via viewMatrix
// Identity model matrix (fullscreen quad)
```

**Point Lights**:
```cpp
SphereLight point{};
point.light.lightType = LT_POINT;  // Value: 1
// Model matrix: translation to light position in world space
// modelViewMatrix = viewMatrix * modelMatrix
// Scale: radius * 1.05 (5% oversize ensures edge coverage)
```

**Cone Lights**:
```cpp
SphereLight cone{};
cone.light.lightType = LT_CONE;  // Value: 3
// Same sphere volume mesh as point (conservative bounding)
// Cone direction transformed to view space and stored in coneDir
// coneAngle, coneInnerAngle, dualCone passed in UBO for shader culling
```

**Tube Lights**:
```cpp
CylinderLight tube{};
tube.light.lightType = LT_TUBE;  // Value: 2
// Model matrix: translation to start position + rotation aligning local -Z with tube direction
// Scale: radius * 1.05 for X/Y, full length for Z
// Beam direction derived in shader from: modelViewMatrix * vec4(0, 0, -scale.z, 0)
```

**Cockpit Lighting Mode**: When `Lighting_mode == lighting_mode::COCKPIT`, both intensity and radius are modified by the current lighting profile's cockpit modifiers via `lp->cockpit_light_intensity_modifier.handle()` and `lp->cockpit_light_radius_modifier.handle()`.

### 6.3 Rendering Lights

**Function**: `VulkanRenderer::recordDeferredLighting(const RenderCtx& render, vk::Buffer uniformBuffer, vk::DescriptorSet globalSet, const std::vector<DeferredLight>& lights)`

**Setup Phase** (executed once before lights):
```cpp
// Fullscreen viewport/scissor, depth disabled
cmd.setViewport(0, 1, &viewport);      // Negative height for Y-flip
cmd.setScissor(0, 1, &scissor);        // Full swapchain extent
cmd.setDepthTestEnable(VK_FALSE);
cmd.setDepthWriteEnable(VK_FALSE);
cmd.setCullMode(vk::CullModeFlagBits::eNone);

// Get pipelines: additive blend and blend-off (for ambient)
vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, deferredLayout);
vk::Pipeline ambientPipeline = m_pipelineManager->getPipeline(ambientKey, modules, deferredLayout);

// Bind G-buffer descriptor set (set=1) once for all lights
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.layout, 1, 1, &globalSet, 0, nullptr);
```

**Per-Light Rendering** via `std::visit` on `DeferredLight` variant:

```cpp
for (const auto& light : lights) {
  std::visit([&](const auto& l) {
    using T = std::decay_t<decltype(l)>;
    if constexpr (std::is_same_v<T, FullscreenLight>) {
      l.record(ctx, fullscreenVB);
    } else if constexpr (std::is_same_v<T, SphereLight>) {
      l.record(ctx, sphereVB, sphereIB, m_sphereMesh.indexCount);
    } else if constexpr (std::is_same_v<T, CylinderLight>) {
      l.record(ctx, cylinderVB, cylinderIB, m_cylinderMesh.indexCount);
    }
  }, light);
}
```

**Light Record Implementation** (e.g., `FullscreenLight::record`):
1. **Bind pipeline**: Ambient uses `ambientPipeline` (blend-off); others use `pipeline` (additive)
2. **Set dynamic blend enable** (if `VK_EXT_extended_dynamic_state3` available via `setColorBlendEnableEXT`)
3. **Push descriptors (set=0)**: Matrix and light UBOs via `pushDescriptorSetKHR()`
4. **Bind vertex buffer** and draw (fullscreen: 3 vertices; volumes: indexed)

```cpp
void FullscreenLight::record(const DeferredDrawContext& ctx, vk::Buffer fullscreenVB) const {
  vk::Pipeline pipe = isAmbient ? ctx.ambientPipeline : ctx.pipeline;
  ctx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe);

  if (ctx.dynamicBlendEnable) {
    vk::Bool32 blendEnable = isAmbient ? VK_FALSE : VK_TRUE;
    ctx.cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
  }

  pushLightDescriptors(ctx.cmd, ctx.layout, ctx.uniformBuffer, matrixOffset, lightOffset);
  ctx.cmd.bindVertexBuffers(0, 1, &fullscreenVB, &offset);
  ctx.cmd.draw(3, 1, 0, 0);  // Fullscreen triangle
}
```

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

The deferred lighting system is exposed via the graphics API in `VulkanGraphics.cpp`:

```cpp
void gr_vulkan_deferred_lighting_begin(bool clearNonColorBufs);
void gr_vulkan_deferred_lighting_end();
void gr_vulkan_deferred_lighting_finish();
```

**Bridge Implementation** (`VulkanGraphics.cpp`):
```cpp
void gr_vulkan_deferred_lighting_begin(bool clearNonColorBufs) {
  Assertion(std::holds_alternative<std::monostate>(g_backend->deferred), ...);
  auto& renderer = currentRenderer();
  g_backend->deferred = renderer.deferredLightingBegin(*g_backend->recording, clearNonColorBufs);
}

void gr_vulkan_deferred_lighting_end() {
  auto* geometry = std::get_if<DeferredGeometryCtx>(&g_backend->deferred);
  Assertion(geometry != nullptr, "...called without a matching begin");
  g_backend->deferred = currentRenderer().deferredLightingEnd(*g_backend->recording, std::move(*geometry));
}

void gr_vulkan_deferred_lighting_finish() {
  auto* lighting = std::get_if<DeferredLightingCtx>(&g_backend->deferred);
  Assertion(lighting != nullptr, "...called without a matching end");
  vk::Rect2D scissor = createClipScissor();
  currentRenderer().deferredLightingFinish(*g_backend->recording, std::move(*lighting), scissor);
  g_backend->deferred = std::monostate{};  // Reset to idle state
}
```

**State Machine** (`g_backend->deferred`):
```
std::variant<std::monostate, DeferredGeometryCtx, DeferredLightingCtx>

  monostate ──begin()──> DeferredGeometryCtx ──end()──> DeferredLightingCtx ──finish()──> monostate
```

**Decal Integration**: During the geometry phase, decals can be rendered via `gr_start_decal_pass()`, which asserts that `DeferredGeometryCtx` is active:
```cpp
Assertion(std::holds_alternative<DeferredGeometryCtx>(g_backend->deferred),
          "Vulkan decal pass requires deferred geometry to be active");
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

The deferred lighting shader uses a two-set layout with push descriptors for per-light data:

**Set 0** (Push Descriptors via `pushDescriptorSetKHR`, per-light):

| Binding | Type | Content | GLSL Declaration |
|---------|------|---------|------------------|
| 0 | Uniform Buffer | `DeferredMatrixUBO` | `layout(binding = 0, std140) uniform matrixData` |
| 1 | Uniform Buffer | `DeferredLightUBO` | `layout(binding = 1, std140) uniform lightData` |

**Set 1** (Global descriptor set `frame.globalDescriptorSet()`, bound once):

| Binding | Type | Content | GLSL Declaration |
|---------|------|---------|------------------|
| 0 | Combined Image Sampler | G-buffer color (albedo) | `layout(set = 1, binding = 0) uniform sampler2D ColorBuffer` |
| 1 | Combined Image Sampler | G-buffer normal | `layout(set = 1, binding = 1) uniform sampler2D NormalBuffer` |
| 2 | Combined Image Sampler | G-buffer position | `layout(set = 1, binding = 2) uniform sampler2D PositionBuffer` |
| 3 | Combined Image Sampler | G-buffer depth | `layout(set = 1, binding = 3) uniform sampler2D DepthBuffer` |
| 4 | Combined Image Sampler | G-buffer specular | `layout(set = 1, binding = 4) uniform sampler2D SpecularBuffer` |
| 5 | Combined Image Sampler | G-buffer emissive | `layout(set = 1, binding = 5) uniform sampler2D EmissiveBuffer` |

**Note**: The G-buffer descriptors are populated by `bindDeferredGlobalDescriptors()` which writes to the frame's global descriptor set. This happens once per frame when deferred lighting is used.

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
- `deferredLightingBegin()` not called before geometry rendering
- Emissive preserved incorrectly (stale content loaded)
- `beginDeferredPass()` not setting `loadOp::eClear` for G-buffer attachments

**Fix**: Ensure `gr_vulkan_deferred_lighting_begin()` is called at the start of each deferred frame. Verify the `clearNonColorBufs` parameter is appropriate for the use case.

### Issue 2: Lighting Output to Wrong Target

**Symptoms**: Deferred lighting renders to swapchain instead of scene HDR (or vice versa).

**Causes**:
- `scene_texture_begin()` not called before `gr_vulkan_deferred_lighting_begin()`
- `m_sceneTexture` not set when `deferredLightingEnd()` checks it
- Target override in `deferredLightingEnd()` not triggering due to missing scene texture state

**Fix**: Verify scene texture mode is active before entering deferred lighting if HDR output is desired. Check that `beginSceneTexture()` was called and `m_sceneTexture.has_value()` is true.

### Issue 3: Emissive Not Preserved

**Symptoms**: Background stars/nebulae missing from final image.

**Causes**:
- `preserveEmissive = false` when it should be true
- Swapchain does not support `TRANSFER_SRC` usage (check swapchain creation)
- Scene HDR target not active and swapchain capture path disabled
- Copy shader (`SDR_TYPE_COPY`) not correctly recording

**Debugging**:
```cpp
// In beginDeferredLighting():
bool canCaptureSwapchain = (m_vulkanDevice->swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc)
                           != vk::ImageUsageFlags{};
bool sceneHdrTarget = m_renderingSession->targetIsSceneHdr();
bool swapchainTarget = m_renderingSession->targetIsSwapchain();
mprintf(("canCaptureSwapchain=%d, sceneHdr=%d, swapchain=%d\n",
         canCaptureSwapchain, sceneHdrTarget, swapchainTarget));
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

**Symptoms**: Vulkan validation errors about image layout mismatches (e.g., "VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL" when shader expected "VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL").

**Causes**:
- G-buffer sampled before `transitionGBufferToShaderRead()` was called in `endDeferredGeometry()`
- Layout tracking out of sync after error recovery
- `deferredLightingEnd()` skipped or early-returned without calling `endDeferredGeometry()`

**Fix**: Ensure `gr_vulkan_deferred_lighting_end()` is called before `gr_vulkan_deferred_lighting_finish()`. Check that no early returns skip the transition. The typestate tokens should prevent this at compile time, but early returns in bridge functions can bypass the enforcement.

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

**Note**: Line numbers are approximate and may drift as the codebase evolves. Use the function names to locate the current implementation.

| File | Function | Line | Description |
|------|----------|------|-------------|
| `VulkanRenderer.cpp` | `beginDeferredLighting()` | ~571 | Internal geometry pass setup |
| `VulkanRenderer.cpp` | `deferredLightingBegin()` | ~1017 | Public API - returns `DeferredGeometryCtx` |
| `VulkanRenderer.cpp` | `deferredLightingEnd()` | ~1023 | Public API - returns `DeferredLightingCtx` |
| `VulkanRenderer.cpp` | `deferredLightingFinish()` | ~1039 | Public API - consumes `DeferredLightingCtx` |
| `VulkanRenderer.cpp` | `bindDeferredGlobalDescriptors()` | ~1065 | G-buffer descriptor binding |
| `VulkanRenderer.cpp` | `recordDeferredLighting()` | ~3503 | Per-light draw recording loop |
| `VulkanDeferredLights.cpp` | `buildDeferredLights()` | ~122 | Light construction from engine state |
| `VulkanDeferredLights.cpp` | `FullscreenLight::record()` | ~64 | Fullscreen light draw recording |
| `VulkanDeferredLights.cpp` | `SphereLight::record()` | ~82 | Point/cone light draw recording |
| `VulkanDeferredLights.cpp` | `CylinderLight::record()` | ~100 | Tube light draw recording |
| `VulkanRenderingSession.cpp` | `beginDeferredPass()` | ~248 | G-buffer target setup |
| `VulkanRenderingSession.cpp` | `endDeferredGeometry()` | ~439 | Transition to lighting phase |
| `VulkanGraphics.cpp` | `gr_vulkan_deferred_lighting_*()` | - | Engine API bridge functions |
| `VulkanRenderTargets.h` | G-buffer constants | ~15-22 | `kGBufferCount`, `kGBufferEmissiveIndex` |
| `VulkanPhaseContexts.h` | `DeferredGeometryCtx`, `DeferredLightingCtx` | ~50-74 | Capability token definitions |

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
