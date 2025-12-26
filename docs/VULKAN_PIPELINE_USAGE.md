# Vulkan Pipeline Usage Guide

This document provides practical guidance for constructing pipeline keys, binding pipelines, and integrating with the rendering system in the Vulkan renderer. It covers common patterns, pitfalls, and debugging techniques for developers working with the pipeline system.

**Prerequisites**: Familiarity with Vulkan concepts (pipelines, render passes, descriptor sets) and the FreeSpace 2 graphics architecture.

**Related Documentation**:
- [Pipeline Management](VULKAN_PIPELINE_MANAGEMENT.md) - Pipeline architecture, lifecycle, and state matrix
- [Vulkan Architecture](VULKAN_ARCHITECTURE_OVERVIEW.md) - Overall renderer architecture
- [Render Pass Structure](VULKAN_RENDER_PASS_STRUCTURE.md) - Dynamic rendering details
- [Descriptor Sets](VULKAN_DESCRIPTOR_SETS.md) - Descriptor set management

---

## Table of Contents

1. [Overview](#1-overview)
2. [Pipeline Key Construction](#2-pipeline-key-construction)
3. [Render Target to Pipeline Key Mapping](#3-render-target-to-pipeline-key-mapping)
4. [Pipeline Binding Patterns](#4-pipeline-binding-patterns)
5. [Shader Type Reference](#5-shader-type-reference)
6. [Blend Mode Reference](#6-blend-mode-reference)
7. [Stencil State Configuration](#7-stencil-state-configuration)
8. [Dynamic State](#8-dynamic-state)
9. [Error Handling](#9-error-handling)
10. [Debugging Pipeline Issues](#10-debugging-pipeline-issues)

---

## 1. Overview

### Purpose

The pipeline system manages the creation, caching, and binding of Vulkan graphics pipelines. Pipelines are identified by a composite `PipelineKey` that captures all state affecting pipeline identity. The system:

- Caches pipelines in memory to avoid redundant GPU compilation
- Validates pipeline keys against shader requirements
- Automatically selects the correct pipeline layout based on shader type
- Uses Vulkan 1.3 dynamic rendering (no render pass objects)

### Key Concepts

| Concept | Description |
|---------|-------------|
| `PipelineKey` | Composite key identifying a unique pipeline configuration |
| `RenderTargetInfo` | Format and attachment information for the current render target |
| `ShaderLayoutSpec` | Specifies pipeline layout kind and vertex input mode for each shader type |
| Vertex Pulling | Shader fetches vertex data from storage buffers instead of using vertex attributes |

### Thread Safety

All pipeline operations occur on the main rendering thread. The pipeline cache is not thread-safe and must not be accessed concurrently.

### Source Files

| File | Purpose |
|------|---------|
| `VulkanPipelineManager.h` | `PipelineKey` structure, `VulkanPipelineManager` class |
| `VulkanPipelineManager.cpp` | Pipeline creation, vertex layout conversion, blend state |
| `VulkanLayoutContracts.h` | `ShaderLayoutSpec`, `PipelineLayoutKind`, `VertexInputMode` enums |
| `VulkanLayoutContracts.cpp` | Shader-to-layout mapping table |
| `VulkanRenderTargetInfo.h` | `RenderTargetInfo` structure |
| `VulkanRenderingSession.h` | Render target request methods |

---

## 2. Pipeline Key Construction

### PipelineKey Structure

The complete `PipelineKey` structure from `VulkanPipelineManager.h`:

```cpp
struct PipelineKey {
  shader_type type = SDR_TYPE_NONE;
  uint32_t variant_flags = 0;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits sample_count{VK_SAMPLE_COUNT_1_BIT};
  uint32_t color_attachment_count{1};
  gr_alpha_blend blend_mode{ALPHA_BLEND_NONE};
  size_t layout_hash = 0;
  uint32_t color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  // Stencil state (see Section 7)
  bool stencil_test_enable = false;
  VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;
  uint32_t stencil_compare_mask = 0xFF;
  uint32_t stencil_write_mask = 0xFF;
  uint32_t stencil_reference = 0;
  VkStencilOp front_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp front_depth_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp front_pass_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_depth_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_pass_op = VK_STENCIL_OP_KEEP;
};
```

### Standard Pattern

The most common pattern constructs a `PipelineKey` from the current render target's format information:

```cpp
// Obtain render context from active render pass
const auto& render = renderCtx; // RenderCtx from ensureRenderingStarted()

// Construct pipeline key
PipelineKey key{};
key.type = shader_type::SDR_TYPE_POST_PROCESS_BLUR;
key.variant_flags = variantFlags;                                      // Shader-specific flags
key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
key.color_attachment_count = render.targetInfo.colorAttachmentCount;
key.blend_mode = ALPHA_BLEND_NONE;
key.layout_hash = fsLayout.hash();                                     // Required for vertex attribute shaders

// Obtain and bind pipeline
vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
```

### Pipeline Key Fields

| Field | Type | Source | Required For |
|-------|------|--------|--------------|
| `type` | `shader_type` | Shader type enum | All pipelines |
| `variant_flags` | `uint32_t` | Shader-specific flags | Variant shaders |
| `color_format` | `VkFormat` | `render.targetInfo.colorFormat` | All pipelines |
| `depth_format` | `VkFormat` | `render.targetInfo.depthFormat` | Depth-enabled targets |
| `sample_count` | `VkSampleCountFlagBits` | `getSampleCount()` | All pipelines |
| `color_attachment_count` | `uint32_t` | `render.targetInfo.colorAttachmentCount` | All pipelines |
| `blend_mode` | `gr_alpha_blend` | Blend mode enum | All pipelines |
| `layout_hash` | `size_t` | `layout.hash()` | Vertex attribute shaders only |
| `color_write_mask` | `uint32_t` | Color component flags | When masking output |
| Stencil fields | Various | See Section 7 | Stencil-enabled effects |

### Vertex Pulling Pattern

Shaders using vertex pulling (currently only `SDR_TYPE_MODEL`) fetch vertex data from storage buffers. The `layout_hash` field is ignored for these shaders:

```cpp
PipelineKey key{};
key.type = shader_type::SDR_TYPE_MODEL;
key.variant_flags = material_info->get_shader_flags();
key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
key.color_attachment_count = render.targetInfo.colorAttachmentCount;  // 5 for G-buffer
key.blend_mode = material_info->get_blend_mode();
// layout_hash is ignored for vertex pulling shaders

vertex_layout emptyLayout;  // Empty layout - vertex data fetched from storage buffer
vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, emptyLayout);
```

### Pipeline Key Equality

The `PipelineKey::operator==` compares all fields except `layout_hash` for vertex-pulling shaders. The `usesVertexPulling()` helper determines whether to ignore the layout hash:

```cpp
bool operator==(const PipelineKey& other) const
{
  if (type != other.type) return false;
  const bool ignoreLayout = usesVertexPulling(type);
  return variant_flags == other.variant_flags &&
         color_format == other.color_format &&
         // ... all other fields ...
         (ignoreLayout || layout_hash == other.layout_hash);
}
```

### Common Mistakes

**Mistake 1: Missing `layout_hash` for vertex attribute shaders**

```cpp
// WRONG: layout_hash not set for vertex attribute shader
PipelineKey key{};
key.type = shader_type::SDR_TYPE_INTERFACE;
key.color_format = ...;
// Missing: key.layout_hash = layout.hash();
// Result: Runtime exception in getPipeline()
```

**Mistake 2: Stale render target info**

```cpp
// WRONG: Using captured target info after target change
const auto& rt = renderCtx.targetInfo;  // Captured before target change
// ... render target changes via requestSwapchainTarget() ...
PipelineKey key{};
key.color_format = static_cast<VkFormat>(rt.colorFormat);  // STALE
// Result: Pipeline incompatible with current render target
```

**Correct approach**: Always query `render.targetInfo` immediately before constructing the key, after any render target changes.

**Mistake 3: Wrong color attachment count**

```cpp
// WRONG: Using 1 attachment for G-buffer target (which has 5)
key.color_attachment_count = 1;  // G-buffer has 5 attachments
// Result: Validation error - pipeline incompatible with render target
```

**Mistake 4: Zero color attachment count**

```cpp
// WRONG: color_attachment_count is 0
key.color_attachment_count = 0;
// Result: std::runtime_error from createPipeline()
```

The pipeline manager validates that `color_attachment_count >= 1`.

---

## 3. Render Target to Pipeline Key Mapping

### RenderTargetInfo Structure

From `VulkanRenderTargetInfo.h`:

```cpp
struct RenderTargetInfo {
  vk::Format colorFormat = vk::Format::eUndefined;
  uint32_t colorAttachmentCount = 1;
  vk::Format depthFormat = vk::Format::eUndefined;  // eUndefined => no depth attachment
};
```

### Render Target Request Methods

All render target request methods are defined in `VulkanRenderingSession`:

| Target Type | Request Method | Color Format | Depth Format | Attachments |
|-------------|----------------|--------------|--------------|-------------|
| Swapchain | `requestSwapchainTarget()` | Surface format | Device depth format | 1 |
| Swapchain (no depth) | `requestSwapchainNoDepthTarget()` | Surface format | `eUndefined` | 1 |
| Scene HDR | `requestSceneHdrTarget()` | `R16G16B16A16_SFLOAT` | Device depth format | 1 |
| Scene HDR (no depth) | `requestSceneHdrNoDepthTarget()` | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| G-buffer | `beginDeferredPass()` | `R16G16B16A16_SFLOAT` | Device depth format | 5 |
| G-buffer (restore) | `requestDeferredGBufferTarget()` | `R16G16B16A16_SFLOAT` | Device depth format | 5 |
| G-buffer emissive | `requestGBufferEmissiveTarget()` | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| G-buffer attachment | `requestGBufferAttachmentTarget(n)` | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| Post LDR | `requestPostLdrTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| Post Luminance | `requestPostLuminanceTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| Bloom mip | `requestBloomMipTarget(idx, mip)` | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| SMAA edges | `requestSmaaEdgesTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| SMAA blend | `requestSmaaBlendTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| SMAA output | `requestSmaaOutputTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| Bitmap RTT | `requestBitmapTarget(handle, face)` | Varies by bitmap | `eUndefined` | 1 |

### G-Buffer Attachment Layout

The G-buffer consists of 5 attachments (defined in `VulkanRenderTargets.h`):

| Index | Name | Content |
|-------|------|---------|
| 0 | Color | Albedo/diffuse color |
| 1 | Normal | World-space normals |
| 2 | Position | World-space position |
| 3 | Specular | Specular color and intensity |
| 4 | Emissive | Emissive color (`kGBufferEmissiveIndex`) |

### Depth Format Selection

The device depth format is selected at initialization from (in preference order):
1. `D32_SFLOAT_S8_UINT` - 32-bit depth with 8-bit stencil
2. `D24_UNORM_S8_UINT` - 24-bit depth with 8-bit stencil
3. `D32_SFLOAT` - 32-bit depth without stencil

Query the selected format via `VulkanRenderTargets::depthFormat()`.

**Surface format**: Negotiated with the display surface; typically `B8G8R8A8_SRGB`. Query via `VulkanDevice::swapchainFormat()`.

### Obtaining Current Target Info

Render target info is obtained from the `RenderCtx` returned by `ensureRenderingStarted()`:

```cpp
auto renderCtx = ensureRenderingStarted(frameCtx);
const auto& rt = renderCtx.targetInfo;

// Use these values in pipeline key
key.color_format = static_cast<VkFormat>(rt.colorFormat);
key.depth_format = static_cast<VkFormat>(rt.depthFormat);
key.color_attachment_count = rt.colorAttachmentCount;
```

**Important**: The `targetInfo` reflects the current render target at the time `ensureRenderingStarted()` was called. If you change targets (via `requestSwapchainTarget()`, etc.), call `ensureRenderingStarted()` again to get updated info.

### Compatibility Rules

A pipeline is compatible with a render target when all of these match:

1. `key.color_format` equals `render.targetInfo.colorFormat`
2. `key.depth_format` equals `render.targetInfo.depthFormat` (or both are `eUndefined`)
3. `key.color_attachment_count` equals `render.targetInfo.colorAttachmentCount`
4. `key.sample_count` equals the attachment sample count

Mismatches cause validation layer errors or undefined behavior.

---

## 4. Pipeline Binding Patterns

### When to Rebind Pipelines

Rebind the pipeline when any of these change:

| Change | Reason |
|--------|--------|
| Render target | Format or attachment count may differ |
| Shader type | Different shader modules and pipeline layout |
| Blend mode | Blend state is baked into pipeline |
| Vertex layout | For vertex attribute shaders only |
| Stencil configuration | Stencil ops are baked into pipeline |

### Pipeline Layout Compatibility

Pipeline layout is determined by shader type. When switching between shader families, descriptor sets must be rebound with the matching layout:

```cpp
// Standard pipeline (push descriptors)
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, standardPipeline);
cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, standardPipelineLayout, 0, ...);

// Model pipeline (bindless + dynamic uniform)
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, modelPipeline);
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, modelPipelineLayout, 0, ...);
```

You cannot use a Standard pipeline with Model descriptor sets or vice versa.

### Post-Processing Chain Example

Post-processing passes typically rebind pipelines for each pass due to different shader types and potentially different render targets:

```cpp
// Bright pass - extract bright pixels
m_renderingSession->requestBloomMipTarget(0, 0);
auto brightCtx = ensureRenderingStarted(frameCtx);
PipelineKey brightKey = buildKey(SDR_TYPE_POST_PROCESS_BRIGHTPASS, brightCtx.targetInfo);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, getPipeline(brightKey, ...));
// ... draw fullscreen quad ...

// Horizontal blur pass
m_renderingSession->suspendRendering();
m_renderingSession->requestBloomMipTarget(1, 0);
auto blurHCtx = ensureRenderingStarted(frameCtx);
PipelineKey blurHKey = buildKey(SDR_TYPE_POST_PROCESS_BLUR, blurHCtx.targetInfo);
blurHKey.variant_flags = SDR_FLAG_BLUR_HORIZONTAL;
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, getPipeline(blurHKey, ...));
// ... draw fullscreen quad ...

// Vertical blur pass
m_renderingSession->suspendRendering();
m_renderingSession->requestBloomMipTarget(0, 1);
auto blurVCtx = ensureRenderingStarted(frameCtx);
PipelineKey blurVKey = buildKey(SDR_TYPE_POST_PROCESS_BLUR, blurVCtx.targetInfo);
blurVKey.variant_flags = SDR_FLAG_BLUR_VERTICAL;
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, getPipeline(blurVKey, ...));
// ... draw fullscreen quad ...
```

### Deferred Geometry Pass Example

Model rendering uses a single pipeline for all models in the G-buffer pass:

```cpp
// Begin deferred geometry pass
m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
auto gbufferCtx = ensureRenderingStarted(frameCtx);

// Single pipeline for all model draws
PipelineKey modelKey{};
modelKey.type = shader_type::SDR_TYPE_MODEL;
modelKey.color_format = static_cast<VkFormat>(gbufferCtx.targetInfo.colorFormat);
modelKey.depth_format = static_cast<VkFormat>(gbufferCtx.targetInfo.depthFormat);
modelKey.color_attachment_count = gbufferCtx.targetInfo.colorAttachmentCount;  // 5
modelKey.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());

vk::Pipeline modelPipeline = getPipeline(modelKey, modules, emptyLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, modelPipeline);

// Draw all models with same pipeline
for (const auto& model : models) {
    // Update push constants and dynamic uniform buffer
    cmd.pushConstants(...);
    cmd.drawIndexed(...);  // No pipeline rebind needed
}
```

### Performance Considerations

- **Pipeline rebinding** has minimal CPU cost but may cause GPU pipeline stalls
- **Batch draws** that use the same pipeline together when possible
- **Pipeline cache** ensures each unique configuration is compiled only once per session
- **First-use cost**: Initial pipeline creation incurs shader compilation (typically 10-100ms)

---

## 5. Shader Type Reference

### Shader Layout Specifications

Each shader type has a defined pipeline layout kind and vertex input mode. The mapping is defined in `VulkanLayoutContracts.cpp`:

| Shader Type | Pipeline Layout | Vertex Input | Description |
|-------------|-----------------|--------------|-------------|
| `SDR_TYPE_MODEL` | Model | VertexPulling | 3D model rendering (G-buffer output) |
| `SDR_TYPE_DEFERRED_LIGHTING` | Deferred | VertexAttributes | Lighting pass for deferred rendering |
| `SDR_TYPE_EFFECT_PARTICLE` | Standard | VertexAttributes | Particle effects |
| `SDR_TYPE_EFFECT_DISTORTION` | Standard | VertexAttributes | Distortion effects |
| `SDR_TYPE_POST_PROCESS_MAIN` | Standard | VertexAttributes | Main post-processing |
| `SDR_TYPE_POST_PROCESS_BLUR` | Standard | VertexAttributes | Gaussian blur (H/V variants) |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | Standard | VertexAttributes | Bloom composition |
| `SDR_TYPE_POST_PROCESS_BRIGHTPASS` | Standard | VertexAttributes | Bright pass extraction |
| `SDR_TYPE_POST_PROCESS_FXAA` | Standard | VertexAttributes | FXAA anti-aliasing |
| `SDR_TYPE_POST_PROCESS_FXAA_PREPASS` | Standard | VertexAttributes | FXAA prepass |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | Standard | VertexAttributes | God rays effect |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | Standard | VertexAttributes | HDR to LDR conversion |
| `SDR_TYPE_POST_PROCESS_SMAA_EDGE` | Standard | VertexAttributes | SMAA edge detection |
| `SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT` | Standard | VertexAttributes | SMAA blending weights |
| `SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING` | Standard | VertexAttributes | SMAA final blend |
| `SDR_TYPE_DEFERRED_CLEAR` | Standard | VertexAttributes | G-buffer clear |
| `SDR_TYPE_VIDEO_PROCESS` | Standard | VertexAttributes | Video playback |
| `SDR_TYPE_PASSTHROUGH_RENDER` | Standard | VertexAttributes | Passthrough rendering |
| `SDR_TYPE_SHIELD_DECAL` | Standard | VertexAttributes | Shield impact decals |
| `SDR_TYPE_BATCHED_BITMAP` | Standard | VertexAttributes | Batched bitmap rendering |
| `SDR_TYPE_DEFAULT_MATERIAL` | Standard | VertexAttributes | Default material shader |
| `SDR_TYPE_INTERFACE` | Standard | VertexAttributes | 2D UI rendering |
| `SDR_TYPE_NANOVG` | Standard | VertexAttributes | NanoVG vector graphics |
| `SDR_TYPE_DECAL` | Standard | VertexAttributes | Decal rendering |
| `SDR_TYPE_SCENE_FOG` | Standard | VertexAttributes | Scene fog effect |
| `SDR_TYPE_VOLUMETRIC_FOG` | Standard | VertexAttributes | Volumetric fog |
| `SDR_TYPE_ROCKET_UI` | Standard | VertexAttributes | Rocket UI library |
| `SDR_TYPE_COPY` | Standard | VertexAttributes | Texture copy |
| `SDR_TYPE_COPY_WORLD` | Standard | VertexAttributes | World texture copy |
| `SDR_TYPE_MSAA_RESOLVE` | Standard | VertexAttributes | MSAA resolve |
| `SDR_TYPE_ENVMAP_SPHERE_WARP` | Standard | VertexAttributes | Environment map sphere warp |
| `SDR_TYPE_IRRADIANCE_MAP_GEN` | Standard | VertexAttributes | Irradiance map generation |
| `SDR_TYPE_FLAT_COLOR` | Standard | VertexAttributes | Flat color rendering |

### Pipeline Layout Kinds

| Kind | Descriptor Strategy | Use Case |
|------|---------------------|----------|
| `Standard` | Push descriptors + global set | 2D, UI, particles, post-processing |
| `Model` | Bindless textures + push constants | 3D model rendering |
| `Deferred` | Push descriptors + G-buffer set | Deferred lighting pass |

### Shader Variant Flags

Shader variant flags are defined in `graphics/2d.h`. Some shader types support variants selected via `variant_flags`:

| Shader Type | Flag | Value | Description |
|-------------|------|-------|-------------|
| `SDR_TYPE_POST_PROCESS_BLUR` | `SDR_FLAG_BLUR_HORIZONTAL` | `1<<0` | Horizontal blur direction |
| `SDR_TYPE_POST_PROCESS_BLUR` | `SDR_FLAG_BLUR_VERTICAL` | `1<<1` | Vertical blur direction |
| `SDR_TYPE_EFFECT_PARTICLE` | `SDR_FLAG_PARTICLE_POINT_GEN` | `1<<0` | Point sprite generation |
| `SDR_TYPE_NANOVG` | `SDR_FLAG_NANOVG_EDGE_AA` | `1<<0` | Edge anti-aliasing |
| `SDR_TYPE_DECAL` | `SDR_FLAG_DECAL_USE_NORMAL_MAP` | `1<<0` | Normal map enabled |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_4` | `1<<0` | 4x MSAA |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_8` | `1<<1` | 8x MSAA |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_16` | `1<<2` | 16x MSAA |
| `SDR_TYPE_VOLUMETRIC_FOG` | `SDR_FLAG_VOLUMETRICS_DO_EDGE_SMOOTHING` | `1<<0` | Edge smoothing |
| `SDR_TYPE_VOLUMETRIC_FOG` | `SDR_FLAG_VOLUMETRICS_NOISE` | `1<<1` | Noise enabled |
| `SDR_TYPE_COPY` | `SDR_FLAG_COPY_FROM_ARRAY` | `1<<0` | Copy from texture array |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | `SDR_FLAG_TONEMAPPING_LINEAR_OUT` | `1<<0` | Linear output (no sRGB) |
| `SDR_TYPE_ENVMAP_SPHERE_WARP` | `SDR_FLAG_ENV_MAP` | `1<<0` | Environment map enabled |

Model shaders use a separate flag system defined in `model_shader_flags.h` with `MODEL_SDR_FLAG_*` constants.

### Vertex Attribute Locations

For vertex attribute shaders, the following location mapping is used (defined in `VulkanPipelineManager.cpp`):

| Location | Vertex Format | Vulkan Format |
|----------|---------------|---------------|
| 0 | POSITION2/3/4, SCREEN_POS | R32G32Sfloat to R32G32B32A32Sfloat |
| 1 | COLOR3/4/4F | R8G8B8Unorm to R32G32B32A32Sfloat |
| 2 | TEX_COORD2/4 | R32G32Sfloat to R32G32B32A32Sfloat |
| 3 | NORMAL | R32G32B32Sfloat |
| 4 | TANGENT | R32G32B32A32Sfloat |
| 5 | MODEL_ID | R32Sfloat |
| 6 | RADIUS | R32Sfloat |
| 7 | UVEC | R32G32B32Sfloat |
| 8-11 | MATRIX4 | R32G32B32A32Sfloat (4 vec4s) |

**Note**: Vulkan allows gaps in vertex attribute locations. A layout with position (0) and texcoord (2) but no color (1) is valid.

---

## 6. Blend Mode Reference

### Blend Modes

The `gr_alpha_blend` enum is defined in `graphics/grinternal.h`:

| Mode | Formula | Use Case |
|------|---------|----------|
| `ALPHA_BLEND_NONE` | `1*Src + 0*Dst` | Opaque geometry |
| `ALPHA_BLEND_ADDITIVE` | `1*Src + 1*Dst` | Additive glow effects |
| `ALPHA_BLEND_ALPHA_ADDITIVE` | `Alpha*Src + 1*Dst` | Alpha-weighted additive |
| `ALPHA_BLEND_ALPHA_BLEND_ALPHA` | `Alpha*Src + (1-Alpha)*Dst` | Standard alpha blending |
| `ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR` | `Alpha*Src + (1-SrcColor)*Dst` | Source color-weighted blend |
| `ALPHA_BLEND_PREMULTIPLIED` | `1*Src + (1-Alpha)*Dst` | Premultiplied alpha (NanoVG) |

### Blend State in Pipeline

Blend state is baked into the pipeline and applied per-attachment. For multi-attachment targets (like G-buffer), all attachments use the same blend state by default:

```cpp
auto colorBlendAttachment = buildBlendAttachment(key.blend_mode, colorWriteMask);
std::vector<vk::PipelineColorBlendAttachmentState> attachments(
    key.color_attachment_count, colorBlendAttachment);
```

### Common Blend Patterns

| Effect Type | Typical Blend Mode |
|-------------|-------------------|
| Opaque models | `ALPHA_BLEND_NONE` |
| Transparent UI | `ALPHA_BLEND_ALPHA_BLEND_ALPHA` |
| Particle effects | `ALPHA_BLEND_ALPHA_ADDITIVE` |
| Glow/bloom | `ALPHA_BLEND_ADDITIVE` |
| NanoVG | `ALPHA_BLEND_PREMULTIPLIED` |
| Post-processing | `ALPHA_BLEND_NONE` (usually) |

---

## 7. Stencil State Configuration

### Stencil State in Pipeline Key

Stencil state is baked into the pipeline and affects the cache key:

```cpp
struct PipelineKey {
    // ... other fields ...
    bool stencil_test_enable = false;
    VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;
    uint32_t stencil_compare_mask = 0xFF;
    uint32_t stencil_write_mask = 0xFF;
    uint32_t stencil_reference = 0;
    VkStencilOp front_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp front_depth_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp front_pass_op = VK_STENCIL_OP_KEEP;
    VkStencilOp back_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp back_depth_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp back_pass_op = VK_STENCIL_OP_KEEP;
};
```

### Stencil Availability

Stencil operations require a depth format with stencil component. The helper `formatHasStencil()` in `VulkanPipelineManager.cpp` checks this:

```cpp
static bool formatHasStencil(vk::Format format)
{
  switch (format) {
  case vk::Format::eD32SfloatS8Uint:
  case vk::Format::eD24UnormS8Uint:
    return true;
  default:
    return false;
  }
}
```

If the depth format has no stencil, the stencil attachment format is set to `eUndefined`.

### State Behavior

| Field | Baked | Dynamic | Notes |
|-------|:-----:|:-------:|-------|
| `stencil_test_enable` | Yes | Yes | Can be overridden via `cmd.setStencilTestEnable()` |
| `stencil_compare_op` | Yes | No | Always baked |
| `stencil_compare_mask` | Yes | No | Always baked |
| `stencil_write_mask` | Yes | No | Always baked |
| `stencil_reference` | Yes | No | Always baked |
| `front_*_op` / `back_*_op` | Yes | No | Always baked |

**Note**: While `stencil_test_enable` is a dynamic state (set via `cmd.setStencilTestEnable()`), the value in the pipeline key affects caching. Different stencil configurations create different pipelines.

### Shield Decal Example

Shield decals use stencil to mark shield regions:

```cpp
PipelineKey decalKey{};
decalKey.type = shader_type::SDR_TYPE_SHIELD_DECAL;
decalKey.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
decalKey.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
// ... other fields ...

// Configure stencil for shield marking
decalKey.stencil_test_enable = true;
decalKey.stencil_compare_op = VK_COMPARE_OP_EQUAL;
decalKey.stencil_reference = 1;
decalKey.stencil_compare_mask = 0xFF;
decalKey.stencil_write_mask = 0xFF;
decalKey.front_pass_op = VK_STENCIL_OP_KEEP;
decalKey.front_fail_op = VK_STENCIL_OP_KEEP;
decalKey.front_depth_fail_op = VK_STENCIL_OP_KEEP;
// Back face ops typically mirror front

vk::Pipeline pipeline = getPipeline(decalKey, modules, layout);
```

### Default Stencil State

Most pipelines use default stencil state (`stencil_test_enable = false`). Only special effects like shield decals configure stencil operations.

---

## 8. Dynamic State

### Core Dynamic States

The following dynamic states are always enabled (Vulkan 1.3 core or Extended Dynamic State 1):

| State | Command | Notes |
|-------|---------|-------|
| Viewport | `cmd.setViewport()` | Required before draw |
| Scissor | `cmd.setScissor()` | Required before draw |
| Line width | `cmd.setLineWidth()` | For line primitives |
| Cull mode | `cmd.setCullMode()` | Front/back/none |
| Front face | `cmd.setFrontFace()` | CW/CCW |
| Primitive topology | `cmd.setPrimitiveTopology()` | Triangle list, etc. |
| Depth test enable | `cmd.setDepthTestEnable()` | On/off |
| Depth write enable | `cmd.setDepthWriteEnable()` | On/off |
| Depth compare op | `cmd.setDepthCompareOp()` | Less, LessOrEqual, etc. |
| Stencil test enable | `cmd.setStencilTestEnable()` | On/off |

### Extended Dynamic State 3

Extended Dynamic State 3 is an optional Vulkan extension that allows additional state to be set dynamically rather than baked into the pipeline. Capabilities are stored in `ExtendedDynamicState3Caps`:

```cpp
struct ExtendedDynamicState3Caps {
    bool colorBlendEnable = false;
    bool colorWriteMask = false;
    bool polygonMode = false;
    bool rasterizationSamples = false;  // Rarely available
};
```

When EDS3 is supported, these states can be set per-draw:

| State | Dynamic Method | Fallback |
|-------|----------------|----------|
| Color blend enable | `cmd.setColorBlendEnableEXT()` | Baked into pipeline |
| Color write mask | `cmd.setColorWriteMaskEXT()` | Baked into pipeline |
| Polygon mode | `cmd.setPolygonModeEXT()` | Baked into pipeline |
| Rasterization samples | N/A | Always baked |

### Building the Dynamic State List

The `BuildDynamicStateList()` static method constructs the dynamic state list based on device capabilities:

```cpp
std::vector<vk::DynamicState> VulkanPipelineManager::BuildDynamicStateList(
    bool supportsExtendedDynamicState3,
    const ExtendedDynamicState3Caps& caps)
{
  std::vector<vk::DynamicState> dynamicStates = {
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor,
    vk::DynamicState::eLineWidth,
    vk::DynamicState::eCullMode,
    vk::DynamicState::eFrontFace,
    vk::DynamicState::ePrimitiveTopology,
    vk::DynamicState::eDepthTestEnable,
    vk::DynamicState::eDepthWriteEnable,
    vk::DynamicState::eDepthCompareOp,
    vk::DynamicState::eStencilTestEnable,
  };

  if (supportsExtendedDynamicState3) {
    if (caps.colorBlendEnable)
      dynamicStates.push_back(vk::DynamicState::eColorBlendEnableEXT);
    if (caps.colorWriteMask)
      dynamicStates.push_back(vk::DynamicState::eColorWriteMaskEXT);
    if (caps.polygonMode)
      dynamicStates.push_back(vk::DynamicState::ePolygonModeEXT);
    if (caps.rasterizationSamples)
      dynamicStates.push_back(vk::DynamicState::eRasterizationSamplesEXT);
  }

  return dynamicStates;
}
```

### Pipeline Key Impact

The `color_write_mask` field is always part of the pipeline key. When EDS3 is not available, changing this value requires a different pipeline. When EDS3 is available, the baked value serves as a default that can be overridden dynamically.

---

## 9. Error Handling

### Error Sources

Pipeline creation can fail at several points:

| Stage | Error Type | Cause |
|-------|------------|-------|
| Key validation | `std::runtime_error` | `layout_hash` mismatch for vertex attribute shader |
| Key validation | `std::runtime_error` | `color_attachment_count` is 0 |
| Shader loading | `std::runtime_error` | SPIR-V file not found or invalid |
| Divisor check | `std::runtime_error` | Divisor > 1 requested but extension unavailable |
| Pipeline creation | `std::runtime_error` | `vkCreateGraphicsPipelines()` fails |

### Validation in getPipeline()

The `getPipeline()` function validates that the pipeline key's `layout_hash` matches the provided `vertex_layout` for shaders using vertex attributes:

```cpp
vk::Pipeline VulkanPipelineManager::getPipeline(
    const PipelineKey& key,
    const ShaderModules& modules,
    const vertex_layout& layout)
{
  PipelineKey adjustedKey = key;
  const auto& layoutSpec = getShaderLayoutSpec(key.type);

  if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
    const size_t expectedHash = layout.hash();
    if (adjustedKey.layout_hash != expectedHash) {
      adjustedKey.layout_hash = expectedHash;
      throw std::runtime_error(
          "PipelineKey.layout_hash mismatches provided vertex_layout "
          "for VertexAttributes shader");
    }
  }
  // ... cache lookup and creation ...
}
```

### Common Failure Modes

**Shader module not found**:
```
Error: Failed to open shader module code/def_files/shaders/blur_h.frag.spv
```
Verify shader files exist and are compiled to SPIR-V.

**Pipeline creation failed**:
```
Error: Failed to create Vulkan graphics pipeline.
```
Check validation layer output for specific errors. Usually indicates format incompatibility or device limit exceeded.

**Validation layer error**:
```
Validation Error: Pipeline not compatible with render pass
```
Verify `PipelineKey` formats match `render.targetInfo` values.

**Vertex attribute divisor error**:
```
Error: vertexAttributeInstanceRateDivisor not enabled but divisor > 1 requested in vertex layout.
```
The vertex layout requires instance rate divisor > 1 but the `VK_EXT_vertex_attribute_divisor` extension is not enabled.

### Current Behavior

Pipeline creation failures throw `std::runtime_error` and terminate rendering. No fallback shader system currently exists.

---

## 10. Debugging Pipeline Issues

### Diagnostic Checklist

**Nothing renders**:

1. Is `ensureRenderingStarted()` called? (Render pass must be active)
2. Is pipeline bound? (`cmd.bindPipeline()`)
3. Are descriptor sets bound and compatible with pipeline layout?
4. Is dynamic state set? (Viewport, scissor, cull mode, etc.)
5. Are draw calls issued? (`cmd.drawIndexed()`, etc.)

**Wrong colors or artifacts**:

1. Check format mismatch between pipeline and render target
2. Verify blend mode is correct for the effect
3. Check color write mask settings

**Validation errors**:

1. "Pipeline not compatible" - Format or attachment count mismatch
2. "Descriptor set not bound" - Missing bind call or layout mismatch
3. "Dynamic state not set" - Missing `setViewport()`, `setScissor()`, etc.

### Debugging Output

Add logging to track pipeline key construction:

```cpp
Warning(LOCATION, "Creating pipeline: type=%d color=%s depth=%s count=%u",
        static_cast<int>(key.type),
        vk::to_string(static_cast<vk::Format>(key.color_format)).c_str(),
        vk::to_string(static_cast<vk::Format>(key.depth_format)).c_str(),
        key.color_attachment_count);
```

Log render target info for comparison:

```cpp
Warning(LOCATION, "Target: color=%s depth=%s count=%u",
        vk::to_string(rt.colorFormat).c_str(),
        vk::to_string(rt.depthFormat).c_str(),
        rt.colorAttachmentCount);
```

### RenderDoc Integration

Debug regions are inserted for GPU capture correlation:

```cpp
cmd.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{"Model Rendering"});
// ... draw calls ...
cmd.endDebugUtilsLabelEXT();
```

In RenderDoc, inspect:
- Bound pipeline state
- Pipeline format compatibility with attachments
- Descriptor set contents
- Draw call parameters

### Validation Layers

Enable `VK_LAYER_KHRONOS_validation` for detailed error messages during development. The validation layer catches:
- Format mismatches
- Missing descriptor bindings
- Unset dynamic state
- Pipeline/render target incompatibility

---

## Appendix: Quick Reference

### Pipeline Key Construction Checklist

- [ ] Set `type` to the shader type
- [ ] Set `variant_flags` if the shader supports variants
- [ ] Set `color_format` from `render.targetInfo.colorFormat`
- [ ] Set `depth_format` from `render.targetInfo.depthFormat`
- [ ] Set `sample_count` from `getSampleCount()`
- [ ] Set `color_attachment_count` from `render.targetInfo.colorAttachmentCount`
- [ ] Set `blend_mode` appropriate for the rendering pass
- [ ] Set `layout_hash` from `layout.hash()` (vertex attribute shaders only)
- [ ] Set stencil state fields if using stencil operations
- [ ] Query `render.targetInfo` immediately before key construction

### Pipeline Binding Checklist

- [ ] Render target is set via `request*Target()` or `beginDeferredPass()`
- [ ] Render pass is active via `ensureRenderingStarted()`
- [ ] Pipeline key matches current `render.targetInfo`
- [ ] Pipeline obtained via `getPipeline(key, modules, layout)`
- [ ] Pipeline bound via `cmd.bindPipeline()`
- [ ] Descriptor sets bound and compatible with pipeline layout
- [ ] Dynamic state set (viewport, scissor, cull mode, depth test, etc.)
- [ ] Draw calls issued

### Common Render Target Formats

| Target | Color Format | Depth Format | Attachments |
|--------|--------------|--------------|-------------|
| Swapchain | `B8G8R8A8_SRGB` (typical) | Device depth format | 1 |
| Scene HDR | `R16G16B16A16_SFLOAT` | Device depth format | 1 |
| G-buffer | `R16G16B16A16_SFLOAT` | Device depth format | 5 |
| Post LDR | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| Bloom mip | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| SMAA targets | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
