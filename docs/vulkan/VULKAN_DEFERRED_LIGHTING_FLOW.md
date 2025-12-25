# Vulkan Deferred Lighting Complete Flow

This document describes the complete deferred lighting pipeline in the Vulkan renderer, from geometry pass through light volumes to final composite. It covers G-buffer rendering, emissive preservation, light volume meshes, and the complete sequence.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Complete Flow Diagram](#2-complete-flow-diagram)
3. [Geometry Pass](#3-geometry-pass)
4. [Emissive Preservation](#4-emissive-preservation)
5. [Lighting Pass](#5-lighting-pass)
6. [Light Volume Rendering](#6-light-volume-rendering)
7. [Integration Points](#7-integration-points)
8. [Common Issues](#8-common-issues)

---

## 1. Overview

The deferred lighting pipeline separates geometry rendering from lighting calculations:

1. **Geometry Pass**: Renders material properties to G-buffer (5 attachments)
2. **Lighting Pass**: Samples G-buffer and applies lights using light volume meshes
3. **Composite**: Final result written to swapchain or scene HDR target

**Key Files**:
- `code/graphics/vulkan/VulkanRenderer.cpp` - Deferred lighting orchestration
- `code/graphics/vulkan/VulkanDeferredLights.cpp` - Light building and rendering
- `code/graphics/vulkan/VulkanDeferredLights.h` - Light structures and types
- `code/graphics/vulkan/VulkanRenderingSession.cpp` - G-buffer target management

**G-Buffer Attachments** (`VulkanRenderTargets.h:15-21`):
- **Attachment 0**: Color (RGBA from base texture)
- **Attachment 1**: Normal (RGB view-space normal, A = 1.0 placeholder for gloss)
- **Attachment 2**: Position (RGB view-space position, A = 1.0 placeholder for AO)
- **Attachment 3**: Specular (RGBA from specular texture)
- **Attachment 4**: Emissive (RGBA from glow texture)
- **Depth**: Shared depth buffer (format selected at runtime: `D32_SFLOAT_S8_UINT` preferred, fallback to `D24_UNORM_S8_UINT` or `D32_SFLOAT`)

---

## 2. Complete Flow Diagram

### High-Level Sequence

```
[Frame Start]
    │
    ├── beginDeferredLighting()
    │   ├── Capture pre-deferred scene color (if needed)
    │   ├── beginDeferredPass(clearNonColorBufs, preserveEmissive)
    │   │   ├── Transition G-buffer → COLOR_ATTACHMENT
    │   │   ├── Transition depth → DEPTH_ATTACHMENT
    │   │   └── Begin dynamic rendering (G-buffer target)
    │   └── Return DeferredGeometryCtx
    │
    ├── [Geometry Rendering]
    │   ├── Bind model pipeline (G-buffer output)
    │   ├── Draw models to G-buffer
    │   └── G-buffer contains: albedo, normal, position, specular, emissive
    │
    ├── deferredLightingEnd()
    │   ├── endDeferredGeometry()
    │   │   ├── Suspend G-buffer rendering
    │   │   ├── Transition G-buffer → SHADER_READ_ONLY
    │   │   ├── Transition depth → SHADER_READ_ONLY
    │   │   └── Request swapchain-no-depth target
    │   └── Return DeferredLightingCtx
    │
    ├── deferredLightingFinish()
    │   ├── bindDeferredGlobalDescriptors()
    │   │   ├── Update global descriptor set (G-buffer textures)
    │   │   └── Bindings: 0=albedo, 1=normal, 2=position, 3=depth, 4=specular, 5=emissive
    │   │
    │   ├── buildDeferredLights()
    │   │   ├── Build ambient light (fullscreen, blend-off)
    │   │   ├── Build directional lights (fullscreen)
    │   │   ├── Build point lights (sphere volumes)
    │   │   ├── Build cone lights (sphere volumes)
    │   │   └── Build tube lights (cylinder volumes)
    │   │
    │   ├── recordDeferredLighting()
    │   │   ├── Bind deferred lighting pipeline
    │   │   ├── Bind global descriptor set (G-buffer)
    │   │   ├── Render ambient light (fullscreen, blend-off)
    │   │   ├── Render directional lights (fullscreen, additive blend)
    │   │   ├── Render point/cone lights (sphere volumes, additive blend)
    │   │   └── Render tube lights (cylinder volumes, additive blend)
    │   │
    │   └── Restore scissor, request main target with depth
    │
    └── [Continue with HUD/UI rendering]
```

### Detailed State Transitions

```
G-Buffer Images:
    UNDEFINED
        │
        v beginDeferredPass()
    COLOR_ATTACHMENT_OPTIMAL (writing)
        │
        v endDeferredGeometry()
    SHADER_READ_ONLY_OPTIMAL (sampling)
        │
        v [Lighting pass reads G-buffer]
        │
        v [Next frame or target switch]
    COLOR_ATTACHMENT_OPTIMAL (or UNDEFINED if cleared)

Depth Buffer:
    UNDEFINED (or previous layout)
        │
        v beginDeferredPass()
    DEPTH_ATTACHMENT_OPTIMAL (writing)
        │
        v endDeferredGeometry()
    SHADER_READ_ONLY_OPTIMAL (sampling for lighting)
        │
        v [Next geometry pass]
    DEPTH_ATTACHMENT_OPTIMAL
```

---

## 3. Geometry Pass

### 3.1 Beginning the Pass

**Function**: `VulkanRenderer::beginDeferredLighting(RecordingFrame& rec, bool clearNonColorBufs)`

**Process** (`VulkanRenderer.cpp:592-638`):

1. **Preserve Emissive Logic**:
   ```cpp
   bool preserveEmissive = false;
   if (m_sceneTexture.has_value()) {
       // Scene rendering: preserve from HDR scene target
       suspendRendering();
       transitionSceneHdrToShaderRead(cmd);
       requestGBufferEmissiveTarget();
       recordPreDeferredSceneHdrCopy(renderCtx);
       preserveEmissive = true;
   } else if (canCaptureSwapchain) {
       // Direct swapchain: capture current swapchain image
       captureSwapchainColorToSceneCopy(cmd, imageIndex);
       requestGBufferEmissiveTarget();
       recordPreDeferredSceneColorCopy(renderCtx, imageIndex);
       preserveEmissive = true;
   }
   ```

2. **Begin Deferred Pass**:
   ```cpp
   m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
   ```

3. **Start Rendering**:
   ```cpp
   ensureRenderingStartedRecording(rec);  // Begins dynamic rendering
   ```

### 3.2 G-Buffer Target Setup

**Function**: `VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive)`

**Process** (`VulkanRenderingSession.cpp:202-218`):

1. **End Active Pass**: Terminates any currently active rendering pass.

2. **Set Clear Operations**:
   ```cpp
   // Depth is always cleared when entering deferred geometry (clearNonColorBufs is for API parity)
   m_clearOps = m_clearOps.withDepthStencilClear();

   // All G-buffer attachments clear by default
   m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eClear);

   // Emissive attachment: preserve if pre-deferred content was copied
   if (preserveEmissive) {
       m_gbufferLoadOps[VulkanRenderTargets::kGBufferEmissiveIndex] = vk::AttachmentLoadOp::eLoad;
   }
   ```

3. **Select Target**: Sets the deferred G-buffer target for subsequent rendering.

**Note**: Layout transitions occur lazily when rendering actually begins via `ensureRenderingStarted()`.

### 3.3 Geometry Rendering

**Pipeline**: Model pipeline (`SDR_TYPE_MODEL`)

**Pipeline Key Requirements**:
- `color_format`: `R16G16B16A16_SFLOAT` (G-buffer format)
- `depth_format`: Runtime-selected (see G-buffer attachments above)
- `color_attachment_count`: 5 (G-buffer attachments)
- `layout_hash`: Ignored (vertex pulling)

**Shader Outputs** (`model.frag`):
- `layout(location = 0) out vec4 outColor;` → G-buffer 0 (base texture color)
- `layout(location = 1) out vec4 outNormal;` → G-buffer 1 (view-space normal, alpha = 1.0)
- `layout(location = 2) out vec4 outPosition;` → G-buffer 2 (view-space position, alpha = 1.0)
- `layout(location = 3) out vec4 outSpecular;` → G-buffer 3 (specular texture)
- `layout(location = 4) out vec4 outEmissive;` → G-buffer 4 (glow texture)

**Model Rendering**:
- Uses bindless textures (slot indices in push constants)
- Vertex pulling from vertex heap SSBO
- Dynamic uniform buffer offsets for per-model data
- All geometry draws to same G-buffer target

---

## 4. Emissive Preservation

### 4.1 Purpose

Emissive preservation captures pre-deferred scene content (e.g., background stars, nebula) and stores it in the G-buffer emissive attachment. This allows deferred lighting to composite with pre-rendered content.

### 4.2 Capture Scenarios

**Scenario 1: Scene HDR Target Active**
- Scene rendered to HDR offscreen target before deferred
- Capture: Copy HDR scene → G-buffer emissive attachment
- Shader: `SDR_TYPE_COPY` fullscreen pass

**Scenario 2: Swapchain Direct Rendering**
- Scene rendered directly to swapchain before deferred
- Capture: Copy swapchain → G-buffer emissive attachment
- Requires: Swapchain supports `TRANSFER_SRC` usage

**Scenario 3: No Pre-Deferred Content**
- `preserveEmissive = false`
- G-buffer emissive cleared normally

### 4.3 Capture Implementation

**Function**: `VulkanRenderer::recordPreDeferredSceneHdrCopy()` / `recordPreDeferredSceneColorCopy()`

**Process**:
1. Request G-buffer emissive target (single attachment)
2. Bind copy shader (`SDR_TYPE_COPY`)
3. Fullscreen quad draw
4. Source: Scene HDR or swapchain (via descriptor)
5. Destination: G-buffer emissive attachment

**Key Point**: Capture happens **before** geometry pass, so emissive attachment contains pre-deferred content when geometry writes to it.

---

## 5. Lighting Pass

### 5.1 Ending Geometry Pass

**Function**: `VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd)`

**Process** (`VulkanRenderingSession.cpp:398-405`):

1. **End Active Pass**:
   ```cpp
   endActivePass();  // Ends dynamic rendering if active
   ```

2. **Transition G-Buffer to Shader Read**:
   ```cpp
   transitionGBufferToShaderRead(cmd);
   // All 5 G-buffer images: COLOR_ATTACHMENT → SHADER_READ_ONLY
   // Depth: DEPTH_ATTACHMENT → DEPTH_READ_ONLY (or DEPTH_STENCIL_READ_ONLY if stencil present)
   ```

3. **Select Lighting Target**:
   ```cpp
   m_target = std::make_unique<SwapchainNoDepthTarget>();  // Default: swapchain
   ```

**Note**: If scene texture is active, `deferredLightingEnd()` overrides this with `requestSceneHdrNoDepthTarget()` at `VulkanRenderer.cpp:1061`.

**Key Point**: Lighting target has **no depth attachment**. Depth is sampled from G-buffer for lighting calculations.

### 5.2 Building Lights

**Function**: `buildDeferredLights(VulkanFrame& frame, vk::Buffer uniformBuffer, ...)`

**Location**: `VulkanDeferredLights.cpp:130-348`

**Process**:

1. **Ambient Light** (always first):
   ```cpp
   FullscreenLight ambient{};
   ambient.isAmbient = true;
   ambient.light.lightType = LT_AMBIENT_SHADER;  // Synthetic type
   // Ambient color from gr_get_ambient_light()
   // Identity matrices (fullscreen)
   ```

2. **Directional Lights**:
   ```cpp
   FullscreenLight directional{};
   directional.isAmbient = false;
   directional.light.lightType = LT_DIRECTIONAL;
   // Direction transformed to view space
   // Identity matrices (fullscreen)
   ```

3. **Point Lights**:
   ```cpp
   SphereLight point{};
   point.light.lightType = LT_POINT;
   // Model matrix: translation to light position
   // Scale: light radius * 1.05 (mesh scale)
   ```

4. **Cone Lights**:
   ```cpp
   SphereLight cone{};
   cone.light.lightType = LT_CONE;
   // Model matrix: translation + orientation
   // Cone direction transformed to view space
   // Scale: light radius * 1.05
   ```

5. **Tube Lights**:
   ```cpp
   CylinderLight tube{};
   tube.light.lightType = LT_TUBE;
   // Model matrix: translation + rotation (aligns -Z with tube direction)
   // Scale: radius for X/Y, length for Z
   ```

**Uniform Upload**: Each light uploads matrix and light data to uniform ring buffer with device alignment.

### 5.3 Lighting Rendering

**Function**: `VulkanRenderer::recordDeferredLighting(const RenderCtx& render, vk::Buffer uniformBuffer, const std::vector<DeferredLight>& lights)`

**Process** (`VulkanRenderer.cpp:3585-3688`):

1. **Pipeline Setup**:
   - Pipeline: `SDR_TYPE_DEFERRED_LIGHTING`
   - Layout: Deferred pipeline layout (G-buffer set + push descriptors)
   - Blend: Additive (except ambient uses blend-off)

2. **Bind Global Descriptors**:
   ```cpp
   cmd.bindDescriptorSets(
       vk::PipelineBindPoint::eGraphics,
       deferredPipelineLayout,
       1, 1,  // Set 1, count 1
       &m_globalDescriptorSet,
       0, nullptr);
   ```

3. **Render Lights**:
   ```cpp
   for (const auto& light : lights) {
       std::visit([&](const auto& l) {
           l.record(ctx, meshVB, meshIB, indexCount);
       }, light);
   }
   ```

**Light Rendering Details** (`VulkanDeferredLights.cpp:66-124` for record methods):

- **Ambient**: Fullscreen quad, blend-off (initializes target)
- **Directional**: Fullscreen quad, additive blend
- **Point/Cone**: Sphere volume mesh, additive blend
- **Tube**: Cylinder volume mesh, additive blend

**Push Descriptors**: Each light pushes matrix and light data via `pushDescriptorSetKHR()`.

---

## 6. Light Volume Rendering

### 6.1 Light Volume Meshes

**Meshes** (`VulkanRenderer.h:46-50`):
```cpp
struct VolumeMesh {
    gr_buffer_handle vbo;       // Vertex buffer
    gr_buffer_handle ibo{};     // Index buffer (optional, default-initialized)
    uint32_t indexCount = 0;    // Index count for indexed draws
};
```

**Mesh Types**:
- **Fullscreen Quad**: 3 vertices, no indices (`draw()`)
- **Sphere**: Indexed mesh (`drawIndexed()`)
- **Cylinder**: Indexed mesh (`drawIndexed()`)

**Creation**: Meshes created at renderer initialization (`VulkanRenderer.cpp:101`)

### 6.2 Light Volume Shader Logic

**Shader**: `deferred.frag`

**Lighting Calculation**:
1. Sample G-buffer at fragment position
2. Read view-space position from G-buffer (stored directly, no reconstruction needed)
3. Calculate light contribution based on light type:
   - **Ambient**: Constant color
   - **Directional**: `dot(normal, lightDir) * lightColor`
   - **Point**: Distance attenuation + `dot(normal, lightDir)`
   - **Cone**: Cone angle attenuation + direction check
   - **Tube**: Distance to line segment + direction check

4. Output: `lightColor * materialColor` (additive blend)

### 6.3 Light Volume Culling

**Current Implementation**: All lights rendered (no culling)

**Future Optimization**: Could cull lights outside frustum or beyond max distance

**Light Radius**: Used for mesh scale (`radius * 1.05`) to ensure coverage

---

## 7. Integration Points

### 7.1 Engine Integration

**Entry Point**: `gr_vulkan_deferred_lighting_begin()` / `end()` / `finish()`

**Location**: `VulkanGraphics.cpp`

**Pattern** (`VulkanGraphics.cpp:639-678`):
```cpp
void gr_vulkan_deferred_lighting_begin(bool clearNonColorBufs) {
    auto& renderer = currentRenderer();
    // Returns DeferredGeometryCtx, stored in g_backend->deferred variant
    g_backend->deferred = renderer.deferredLightingBegin(*g_backend->recording, clearNonColorBufs);
}

void gr_vulkan_deferred_lighting_end() {
    auto* geometry = std::get_if<DeferredGeometryCtx>(&g_backend->deferred);
    auto& renderer = currentRenderer();
    // Returns DeferredLightingCtx, stored in g_backend->deferred variant
    g_backend->deferred = renderer.deferredLightingEnd(*g_backend->recording, std::move(*geometry));
}

void gr_vulkan_deferred_lighting_finish() {
    auto* lighting = std::get_if<DeferredLightingCtx>(&g_backend->deferred);
    auto& renderer = currentRenderer();
    vk::Rect2D scissor = createClipScissor();
    renderer.deferredLightingFinish(*g_backend->recording, std::move(*lighting), scissor);
    g_backend->deferred = std::monostate{};  // Reset deferred state
}
```

### 7.2 Model Rendering Integration

**During Geometry Pass**:
- Models render using model pipeline
- Output to G-buffer attachments
- Uses bindless textures for material maps

**Pipeline Compatibility**:
- Model pipeline must match G-buffer format
- `color_attachment_count = 5` in pipeline key
- Depth format must match G-buffer depth

### 7.3 Scene Texture Integration

**When Scene HDR Active**:
- Geometry pass outputs to G-buffer (same as swapchain path)
- Lighting pass outputs to scene HDR target (not swapchain)
- Post-processing reads scene HDR for tonemapping

**Target Selection** (`VulkanRenderer.cpp:1059-1062` in `deferredLightingEnd()`):
```cpp
// endDeferredGeometry() defaults to SwapchainNoDepthTarget
endDeferredGeometry(cmd);
if (m_sceneTexture.has_value()) {
    // Override to scene HDR target during scene texture mode
    m_renderingSession->requestSceneHdrNoDepthTarget();
}
```

---

## 8. Common Issues

### Issue 1: G-Buffer Not Cleared

**Symptoms**: Previous frame's G-buffer visible in current frame

**Causes**:
- `clearNonColorBufs = false` but depth not cleared
- Emissive preserved but other attachments not cleared

**Fix**: Ensure `clearNonColorBufs = true` on first deferred pass, or manually clear G-buffer attachments

### Issue 2: Lighting Output Wrong Target

**Symptoms**: Lighting renders to wrong target (swapchain vs scene HDR)

**Causes**:
- `endDeferredGeometry()` called before scene texture state set
- Target selection logic incorrect

**Fix**: Ensure `beginSceneTexture()` called before deferred lighting if HDR output desired

### Issue 3: Emissive Not Preserved

**Symptoms**: Pre-deferred content missing from final image

**Causes**:
- `preserveEmissive = false` when it should be true
- Scene capture failed (swapchain doesn't support TRANSFER_SRC)
- Capture shader not bound correctly

**Debugging**:
```cpp
// Check if preserveEmissive was set
Warning(LOCATION, "preserveEmissive: %s", preserveEmissive ? "true" : "false");

// Check swapchain transfer support
bool canTransfer = (swapchainUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
```

### Issue 4: Light Volume Not Rendering

**Symptoms**: Lights don't appear in final image

**Causes**:
- Light volume mesh not bound
- Light uniform data not uploaded
- Blend mode incorrect (ambient should be blend-off, others additive)
- G-buffer not bound to descriptor set

**Debugging**:
```cpp
// Check light count
Warning(LOCATION, "Lights built: %zu", lights.size());

// Check descriptor binding
// Verify m_globalDescriptorSet is bound before lighting draws
```

### Issue 5: G-Buffer Layout Mismatch

**Symptoms**: Validation errors about incompatible layouts

**Causes**:
- G-buffer transitioned incorrectly
- Layout tracking out of sync

**Fix**: Ensure `transitionGBufferToShaderRead()` called before lighting, verify layout tracking

---

## Appendix: G-Buffer Channel Reference

The current implementation uses a simplified G-buffer layout. Alpha channels contain placeholder values (1.0) in most cases.

| Attachment | Channel | Content | Source |
|------------|---------|---------|--------|
| **0: Color** | RGB | Base texture color (linear) | `baseColor.rgb` from base texture |
| | A | Base texture alpha | `baseColor.a` |
| **1: Normal** | RGB | View-space shading normal | TBN-transformed normal map |
| | A | Gloss placeholder | Always 1.0 (reserved for future PBR gloss) |
| **2: Position** | RGB | View-space position | `vPosition` from vertex shader |
| | A | AO placeholder | Always 1.0 (reserved for future AO) |
| **3: Specular** | RGBA | Specular texture sample | Direct `specSample` from spec texture |
| **4: Emissive** | RGBA | Glow texture sample (linear) | `emissive` from glow texture |

**Shader Usage in `deferred.frag`**:
- `ColorBuffer.rgb` → Diffuse color for lighting
- `NormalBuffer.xyz` → Surface normal for dot products
- `NormalBuffer.a` → Gloss (currently 1.0, so roughness = 0)
- `PositionBuffer.xyz` → Position for light direction calculation
- `PositionBuffer.w` → AO multiplier for ambient pass (currently 1.0)
- `SpecularBuffer.rgb` → Specular F0 color
- `SpecularBuffer.a` → Fresnel factor
- `EmissiveBuffer.rgb` → Added in ambient pass

---

## References

- `code/graphics/vulkan/VulkanRenderer.cpp` - Deferred lighting orchestration
  - `beginDeferredLighting()`: lines 592-638
  - `deferredLightingBegin/End/Finish()`: lines 1043-1098
  - `recordDeferredLighting()`: lines 3585-3688
- `code/graphics/vulkan/VulkanDeferredLights.cpp` - Light building and rendering (lines 66-348)
- `code/graphics/vulkan/VulkanDeferredLights.h` - Light structures and types
- `code/graphics/vulkan/VulkanRenderingSession.cpp` - G-buffer target management
  - `beginDeferredPass()`: lines 202-218
  - `endDeferredGeometry()`: lines 398-405
- `code/graphics/vulkan/VulkanRenderTargets.h` - G-buffer definitions (lines 15-21)
- `code/graphics/shaders/deferred.frag` - Deferred lighting shader
- `code/graphics/shaders/model.frag` - G-buffer output shader
- `code/graphics/shaders/lighting.glsl` - Light type constants and PBR functions

