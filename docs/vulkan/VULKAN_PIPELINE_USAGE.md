# Vulkan Pipeline Usage Guide

This document covers practical aspects of using pipelines in the Vulkan renderer: construction patterns, common mistakes, performance considerations, and integration with the rendering system. For pipeline architecture and lifecycle details, see [Pipeline Management](VULKAN_PIPELINE_MANAGEMENT.md).

---

## Table of Contents

1. [Pipeline Key Construction Patterns](#1-pipeline-key-construction-patterns)
2. [Render Target Info to Pipeline Key Mapping](#2-render-target-info-to-pipeline-key-mapping)
3. [Pipeline Binding Patterns and When to Rebind](#3-pipeline-binding-patterns-and-when-to-rebind)
4. [Common Pipeline Key Mistakes and Validation](#4-common-pipeline-key-mistakes-and-validation)
5. [Performance and Cache Behavior](#5-performance-and-cache-behavior)
6. [Stencil State in Pipeline Keys](#6-stencil-state-in-pipeline-keys)
7. [Extended Dynamic State 3 Integration](#7-extended-dynamic-state-3-integration)
8. [Pipeline Creation Error Handling](#8-pipeline-creation-error-handling)
9. [Integration with Render Pass State Machine](#9-integration-with-render-pass-state-machine)
10. [Debugging Pipeline Issues](#10-debugging-pipeline-issues)

---

## 1. Pipeline Key Construction Patterns

### Standard Pattern

The most common pattern for constructing a `PipelineKey` uses the current render target's format information:

```cpp
const auto& render = renderCtx; // RenderCtx from ensureRenderingStarted()
PipelineKey key{};
key.type = shader_type::SDR_TYPE_POST_PROCESS_BLUR;
key.variant_flags = variantFlags; // Shader-specific variant flags
key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
key.color_attachment_count = render.targetInfo.colorAttachmentCount;
key.blend_mode = ALPHA_BLEND_NONE; // or ALPHA_BLEND_ADDITIVE, etc.
key.layout_hash = fsLayout.hash(); // For vertex attribute shaders

vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
```

### Pattern Components

| Component | Source | Notes |
|-----------|--------|-------|
| `type` | Shader type enum | Determines which shader modules to load |
| `variant_flags` | Shader-specific | See [Shader Variant Flags](VULKAN_PIPELINE_MANAGEMENT.md#4-shader-variant-flags) |
| `color_format` | `render.targetInfo.colorFormat` | From active render target |
| `depth_format` | `render.targetInfo.depthFormat` | `eUndefined` if no depth attachment |
| `sample_count` | `getSampleCount()` | Currently always `e1` |
| `color_attachment_count` | `render.targetInfo.colorAttachmentCount` | Usually 1, 5 for G-buffer |
| `blend_mode` | `gr_alpha_blend` enum | Determines blend state |
| `layout_hash` | `vertex_layout::hash()` | **Required** for vertex attribute shaders |

### Vertex Pulling Pattern

For shaders using vertex pulling (e.g., `SDR_TYPE_MODEL`), `layout_hash` is ignored since vertex data is fetched from storage buffers rather than traditional vertex attributes:

```cpp
PipelineKey key{};
key.type = shader_type::SDR_TYPE_MODEL;
key.variant_flags = material_info->get_shader_flags(); // MODEL_SDR_FLAG_* flags
key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
key.color_attachment_count = render.targetInfo.colorAttachmentCount;
key.blend_mode = material_info->get_blend_mode();
// layout_hash is ignored for vertex pulling shaders

vertex_layout emptyLayout; // Empty layout for vertex pulling
vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, emptyLayout);
```

**Note**: Model shaders use shader flags (e.g., `MODEL_SDR_FLAG_LIGHT`, `MODEL_SDR_FLAG_DEFERRED`) passed via push constants and reflected in the pipeline key's `variant_flags`.

### Common Mistakes

**Mistake 1: Forgetting `layout_hash` for vertex attribute shaders**

```cpp
// WRONG: Missing layout_hash
PipelineKey key{};
key.type = shader_type::SDR_TYPE_INTERFACE;
key.color_format = ...;
// Missing: key.layout_hash = layout.hash();
// Result: Runtime exception when getPipeline() validates
```

**Mistake 2: Using wrong `layout_hash`**

```cpp
// WRONG: Using wrong layout
PipelineKey key{};
key.type = shader_type::SDR_TYPE_POST_PROCESS_BLUR;
key.layout_hash = wrongLayout.hash(); // Should be fsLayout.hash()
// Result: Pipeline cache miss, or exception if validation catches it
```

**Mistake 3: Not updating key when render target changes**

```cpp
// WRONG: Using stale target info
const auto& rt = renderCtx.targetInfo; // Captured earlier
// ... render target changes ...
PipelineKey key{};
key.color_format = static_cast<VkFormat>(rt.colorFormat); // Stale!
// Result: Pipeline incompatible with current render target
```

**Correct approach**: Always query `render.targetInfo` immediately before constructing the key.

---

## 2. Render Target Info to Pipeline Key Mapping

### RenderTargetInfo Structure

```cpp
struct RenderTargetInfo {
  vk::Format colorFormat = vk::Format::eUndefined;
  uint32_t colorAttachmentCount = 1;
  vk::Format depthFormat = vk::Format::eUndefined; // eUndefined => no depth
};
```

### Render Target Type to Format Mapping

| Target Type | Method | Color Format | Depth Format | Attachment Count |
|-------------|--------|--------------|--------------|-----------------|
| **Swapchain** | `requestSwapchainTarget()` | Surface format (typically `B8G8R8A8_SRGB`) | Depth format (see note) | 1 |
| **Swapchain (no depth)** | `requestSwapchainNoDepthTarget()` | Surface format | `eUndefined` | 1 |
| **Scene HDR** | `requestSceneHdrTarget()` | `R16G16B16A16_SFLOAT` | Depth format (see note) | 1 |
| **Scene HDR (no depth)** | `requestSceneHdrNoDepthTarget()` | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| **G-buffer** | `beginDeferredPass()` | `R16G16B16A16_SFLOAT` (x5) | Depth format (see note) | 5 |
| **Post LDR** | `requestPostLdrTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| **Bloom mip** | `requestBloomMipTarget()` | `R16G16B16A16_SFLOAT` | `eUndefined` | 1 |
| **SMAA edges** | `requestSmaaEdgesTarget()` | `B8G8R8A8_UNORM` | `eUndefined` | 1 |
| **Bitmap RTT** | `requestBitmapTarget()` | Varies by bitmap | `eUndefined` | 1 |

**Depth format note**: The depth format is device-dependent and selected at runtime from (in preference order): `D32_SFLOAT_S8_UINT`, `D24_UNORM_S8_UINT`, or `D32_SFLOAT`. Use `targets.depthFormat()` to query the actual format.

**Surface format note**: The swapchain color format is negotiated with the surface and is typically `B8G8R8A8_SRGB` but may vary. Use `device.swapchainFormat()` to query the actual format.

### Obtaining Render Target Info

Render target info is obtained from the `RenderCtx` returned by `ensureRenderingStarted()`:

```cpp
auto renderCtx = ensureRenderingStarted(frameCtx);
const auto& rt = renderCtx.targetInfo;

// Use rt.colorFormat, rt.depthFormat, rt.colorAttachmentCount
```

**Important**: The `targetInfo` reflects the **current** render target. If you call `requestSwapchainTarget()` or other target methods after obtaining `RenderCtx`, you must call `ensureRenderingStarted()` again to get updated info.

### Format Compatibility Rules

A pipeline is compatible with a render target if:

1. **Color format matches**: `key.color_format == render.targetInfo.colorFormat`
2. **Depth format matches**: `key.depth_format == render.targetInfo.depthFormat` (or both `eUndefined`)
3. **Attachment count matches**: `key.color_attachment_count == render.targetInfo.colorAttachmentCount`
4. **Sample count matches**: `key.sample_count == attachment.samples`

**Violation**: If any of these mismatch, the pipeline is incompatible and will cause validation errors or undefined behavior.

### Example: Post-Processing Chain

```cpp
// Step 1: Scene HDR target
m_renderingSession->requestSceneHdrTarget();
auto sceneCtx = ensureRenderingStarted(frameCtx);
// sceneCtx.targetInfo: colorFormat=R16G16B16A16_SFLOAT, depthFormat=(device depth format), count=1

// Step 2: Post LDR target (different format!)
m_renderingSession->suspendRendering();
m_renderingSession->requestPostLdrTarget();
auto ldrCtx = ensureRenderingStarted(frameCtx);
// ldrCtx.targetInfo: colorFormat=B8G8R8A8_UNORM, depthFormat=eUndefined, count=1

// Must use different pipeline key for each target
PipelineKey sceneKey{};
sceneKey.color_format = static_cast<VkFormat>(sceneCtx.targetInfo.colorFormat);
// ... use sceneKey for scene rendering

PipelineKey ldrKey{};
ldrKey.color_format = static_cast<VkFormat>(ldrCtx.targetInfo.colorFormat);
// ... use ldrKey for LDR post-processing
```

---

## 3. Pipeline Binding Patterns and When to Rebind

### When Pipelines Must Be Rebound

Pipelines must be rebound when:

1. **Render target changes** (different format/attachment count)
2. **Shader type changes** (different pipeline layout family)
3. **Blend mode changes** (affects pipeline key)
4. **Vertex layout changes** (for vertex attribute shaders)

### Pipeline Layout Compatibility

When switching between pipeline layout families, descriptor sets must be rebound:

```cpp
// Standard pipeline (push descriptors)
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, standardPipeline);
cmd.pushDescriptorSetKHR(standardPipelineLayout, 0, ...);

// Model pipeline (bindless + dynamic uniform)
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, modelPipeline);
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, modelPipelineLayout, ...);
```

**Key Point**: Pipeline layout is baked into the pipeline. You cannot use a Standard pipeline with Model descriptor sets, or vice versa.

### Binding Pattern: Post-Processing Chain

Post-processing passes typically rebind pipelines for each pass:

```cpp
// Bloom bright pass
m_renderingSession->requestBloomMipTarget(0, 0);
auto brightCtx = ensureRenderingStarted(frameCtx);
PipelineKey brightKey = buildKey(SDR_TYPE_POST_PROCESS_BRIGHTPASS, brightCtx.targetInfo);
vk::Pipeline brightPipeline = getPipeline(brightKey, modules, fsLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, brightPipeline);
// ... draw ...

// Blur pass (different variant flags)
m_renderingSession->requestBloomMipTarget(1, 0);
auto blurCtx = ensureRenderingStarted(frameCtx);
PipelineKey blurKey = buildKey(SDR_TYPE_POST_PROCESS_BLUR, blurCtx.targetInfo);
blurKey.variant_flags = SDR_FLAG_BLUR_VERTICAL;
vk::Pipeline blurPipeline = getPipeline(blurKey, modules, fsLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, blurPipeline); // Rebind required
// ... draw ...
```

### Binding Pattern: Model Rendering

Model rendering uses a single pipeline per frame (G-buffer target):

```cpp
// G-buffer target (set once per deferred geometry pass)
m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
auto gbufferCtx = ensureRenderingStarted(frameCtx);

PipelineKey modelKey{};
modelKey.type = shader_type::SDR_TYPE_MODEL;
modelKey.color_format = static_cast<VkFormat>(gbufferCtx.targetInfo.colorFormat);
modelKey.depth_format = static_cast<VkFormat>(gbufferCtx.targetInfo.depthFormat);
modelKey.color_attachment_count = gbufferCtx.targetInfo.colorAttachmentCount; // 5
// ... other fields ...

vk::Pipeline modelPipeline = getPipeline(modelKey, modules, emptyLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, modelPipeline);

// All model draws use the same pipeline (no rebind needed)
for (const auto& model : models) {
  // ... set uniforms, bind descriptors ...
  cmd.drawIndexed(...); // Same pipeline
}
```

### Performance Considerations

**Frequent rebinding**: Rebinding pipelines has minimal CPU cost but can cause GPU pipeline stalls. Batch draws that use the same pipeline.

**Cache locality**: Pipelines are cached by key. If you frequently switch between similar pipelines (e.g., different blend modes), ensure keys are constructed correctly to maximize cache hits.

---

## 4. Common Pipeline Key Mistakes and Validation

### Validation in `getPipeline()`

`VulkanPipelineManager::getPipeline()` validates pipeline keys:

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

### Common Mistakes and Their Consequences

| Mistake | Consequence | Detection |
|---------|-------------|-----------|
| Missing `layout_hash` for vertex attribute shader | Runtime exception | Immediate (validation) |
| Wrong `layout_hash` | Pipeline cache miss or exception | Immediate (validation) |
| Stale `targetInfo` (target changed) | Pipeline incompatible with render target | Validation layer error |
| Wrong `color_attachment_count` (e.g., 1 vs 5) | Pipeline incompatible | Validation layer error |
| Format mismatch (e.g., HDR vs LDR) | Pipeline incompatible | Validation layer error |
| Forgetting to set `variant_flags` | Wrong shader variant loaded | Silent (wrong rendering) |
| Using wrong `blend_mode` | Incorrect blending | Silent (wrong rendering) |

### Debugging Validation Errors

**Error**: "Pipeline not compatible with render pass"

**Cause**: Format or attachment count mismatch between pipeline key and active render target.

**Fix**: Verify `key.color_format == render.targetInfo.colorFormat`, `key.depth_format == render.targetInfo.depthFormat`, and `key.color_attachment_count == render.targetInfo.colorAttachmentCount`.

**Error**: "Descriptor set not bound"

**Cause**: Pipeline layout mismatch. You bound a Standard pipeline but are using Model descriptor sets (or vice versa).

**Fix**: Ensure pipeline layout matches descriptor set layout. Check `getShaderLayoutSpec(key.type).pipelineLayout`.

**Error**: "Dynamic state not set"

**Cause**: Required dynamic state (viewport, scissor, etc.) not set before draw.

**Fix**: Call `cmd.setViewport()`, `cmd.setScissor()`, etc. before drawing. Dynamic state persists until changed or pipeline rebind.

---

## 5. Performance and Cache Behavior

### Cache Hit/Miss Implications

**Cache Hit**: Pipeline lookup returns immediately. No GPU work required.

**Cache Miss**: Pipeline must be created:
- Shader module loading (if not already cached)
- Vertex input state conversion (if not already cached)
- `vkCreateGraphicsPipelines()` call (GPU driver compilation)
- Storage in cache

**First-time cost**: First use of a pipeline incurs shader compilation cost (typically 10-100ms depending on shader complexity).

### Cache Size and Memory Usage

Pipelines are stored in `std::unordered_map<PipelineKey, vk::UniquePipeline>`. Typical usage:

- **2D/UI pipelines**: ~20-50 pipelines (different blend modes, layouts)
- **Post-processing pipelines**: ~10-20 pipelines (different passes, variants)
- **Model pipelines**: ~5-10 pipelines (different targets, blend modes)
- **Total**: ~50-100 pipelines per session

**Memory per pipeline**: ~1-10 KB (driver-dependent). Total cache memory: ~100 KB - 1 MB.

### Cache Invalidation

The pipeline cache is **never invalidated** during normal operation. Pipelines persist for the lifetime of the renderer.

**Invalidation occurs only at**:
- Renderer shutdown
- Device lost recovery (full recreation)

### Performance Best Practices

1. **Prefer render target stability**: Avoid frequent target switches within a frame to minimize pipeline rebinds.

2. **Batch by pipeline**: Group draws that use the same pipeline together.

3. **Cache key construction**: Construct keys correctly to maximize cache hits. Avoid constructing keys with mismatched formats.

4. **Warm-up**: Currently no pipeline warm-up exists. First frame may have pipeline creation stalls. Future improvement: pre-create common pipelines.

### Pipeline Creation Cost

Pipeline creation cost varies by:
- **Shader complexity**: More complex shaders take longer to compile
- **Vertex input state**: Complex vertex layouts add conversion overhead
- **Driver**: Different drivers have different compilation speeds

**Typical costs**:
- Simple 2D shader: ~5-20ms
- Post-processing shader: ~10-50ms
- Model shader: ~20-100ms

**Mitigation**: Cache ensures each pipeline is created only once per session.

---

## 6. Stencil State in Pipeline Keys

### Stencil State Fields

`PipelineKey` includes stencil state fields:

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

### Stencil State Behavior

- **`stencil_test_enable`**: Always dynamic (promoted to core in Vulkan 1.3, part of Extended Dynamic State 1). Set via `cmd.setStencilTestEnable()`.
- **All other stencil fields** (compare op, masks, reference, fail/pass ops): Always baked into the pipeline and affect the cache key.

### Common Use Cases

**Shield Decals**: Use stencil to mark shield regions:

```cpp
PipelineKey decalKey{};
// ... standard fields ...
decalKey.stencil_test_enable = true;
decalKey.stencil_compare_op = VK_COMPARE_OP_EQUAL;
decalKey.stencil_reference = 1;
decalKey.front_pass_op = VK_STENCIL_OP_KEEP;
// ... create pipeline ...
```

**Special Effects**: Stencil can be used for masking or special rendering effects.

### Stencil State Defaults

Most pipelines use default stencil state (`stencil_test_enable = false`). Only special effects pipelines set stencil state.

**Note**: Stencil state is part of the pipeline key, so different stencil configurations create different pipelines.

---

## 7. Extended Dynamic State 3 Integration

### EDS3 Capabilities

Extended Dynamic State 3 (optional extension) allows some state to be set dynamically:

| State | EDS3 Dynamic | Fallback (Baked) |
|-------|--------------|------------------|
| `colorBlendEnable` | `VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT` | Baked into pipeline |
| `colorWriteMask` | `VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT` | Baked into pipeline |
| `polygonMode` | `VK_DYNAMIC_STATE_POLYGON_MODE_EXT` | Baked into pipeline |
| `rasterizationSamples` | `VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT` | Always baked |

### EDS3 Detection

EDS3 capabilities are queried at device initialization:

```cpp
ExtendedDynamicState3Caps extDyn3Caps{};
if (supportsExtendedDynamicState3) {
  // Query capabilities
  extDyn3Caps.colorBlendEnable = ...;
  extDyn3Caps.colorWriteMask = ...;
  extDyn3Caps.polygonMode = ...;
}
```

### Pipeline Key Construction with EDS3

If EDS3 is **not** supported, additional fields must be set in the pipeline key:

```cpp
PipelineKey key{};
// ... standard fields ...

if (!supportsExtendedDynamicState3) {
  // Must bake these into pipeline key
  key.color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | ...;
  // colorBlendEnable and polygonMode are derived from blend_mode
}
```

**Current implementation**: The renderer always sets `color_write_mask` in the pipeline key (defaults to `RGBA`). EDS3 support only affects whether it can be changed dynamically.

### Dynamic State Application

When EDS3 is supported, dynamic state is set via command buffer:

```cpp
if (supportsExtendedDynamicState3 && caps.colorBlendEnable) {
  cmd.setColorBlendEnableEXT(0, 1, &blendEnable);
}
if (supportsExtendedDynamicState3 && caps.colorWriteMask) {
  cmd.setColorWriteMaskEXT(0, 1, &writeMask);
}
```

**Fallback**: If EDS3 is not supported, these states are baked into the pipeline and cannot be changed per-draw.

---

## 8. Pipeline Creation Error Handling

### Error Sources

Pipeline creation can fail at several points:

1. **Shader module loading**: SPIR-V file not found or invalid
2. **Pipeline creation**: `vkCreateGraphicsPipelines()` fails
3. **Validation**: Pipeline incompatible with render target or device limits

### Error Handling in `createPipeline()`

```cpp
auto pipelineResult = m_device.createGraphicsPipelineUnique(m_pipelineCache, pipelineInfo);
if (pipelineResult.result != vk::Result::eSuccess) {
  throw std::runtime_error("Failed to create Vulkan graphics pipeline.");
}
```

**Current behavior**: Fatal exception. No fallback shader system exists.

### Common Failure Modes

**Failure 1: Shader module not found**

```
Error: Failed to load shader module: blur_h.frag.spv
```

**Cause**: Shader file missing or path incorrect.

**Fix**: Verify shader files exist in `code/def_files/shaders/` and are compiled to SPIR-V.

**Failure 2: Pipeline creation failed**

```
Error: Failed to create Vulkan graphics pipeline.
```

**Cause**: Usually format incompatibility or device limits exceeded.

**Debugging**:
1. Check validation layer output for specific error
2. Verify pipeline key formats match render target formats
3. Check device limits (max color attachments, etc.)

**Failure 3: Validation error**

```
Validation Error: Pipeline not compatible with render pass
```

**Cause**: Format or attachment count mismatch.

**Fix**: Ensure `PipelineKey` matches `render.targetInfo`.

### Recovery Strategies

**Current**: No recovery. Fatal exception terminates rendering.

**Future improvement**: Could implement fallback to a simple solid-color shader for graceful degradation.

### Debugging Pipeline Creation Failures

1. **Enable validation layers**: Provides detailed error messages
2. **Log pipeline key**: Print all `PipelineKey` fields before creation
3. **Verify render target**: Ensure `render.targetInfo` matches expected values
4. **Check shader modules**: Verify shader modules are loaded correctly

**Example debug output**:
```cpp
Warning(LOCATION, "Creating pipeline: type=%d, color=%s, depth=%s, count=%u",
        static_cast<int>(key.type),
        vk::to_string(static_cast<vk::Format>(key.color_format)).c_str(),
        vk::to_string(static_cast<vk::Format>(key.depth_format)).c_str(),
        key.color_attachment_count);
```

---

## 9. Integration with Render Pass State Machine

### Render Target State and Pipeline Compatibility

The `VulkanRenderingSession` manages render target state via polymorphic `RenderTargetState` objects. Each state provides `RenderTargetInfo` that pipelines must match.

### Pipeline Compatibility Check

A pipeline is compatible with the active render target if:

1. **Format match**: `key.color_format == render.targetInfo.colorFormat`
2. **Depth match**: `key.depth_format == render.targetInfo.depthFormat`
3. **Count match**: `key.color_attachment_count == render.targetInfo.colorAttachmentCount`

**Enforcement**: Vulkan validation layers will error if formats mismatch. The renderer does not pre-validate compatibility (relies on correct key construction).

### Target Switching and Pipeline Rebinding

When the render target changes, pipelines must be rebound:

```cpp
// Target 1: Scene HDR
m_renderingSession->requestSceneHdrTarget();
auto ctx1 = ensureRenderingStarted(frameCtx);
PipelineKey key1 = buildKey(shaderType, ctx1.targetInfo);
cmd.bindPipeline(..., getPipeline(key1, ...));

// Target 2: Post LDR (different format)
m_renderingSession->suspendRendering();
m_renderingSession->requestPostLdrTarget();
auto ctx2 = ensureRenderingStarted(frameCtx);
PipelineKey key2 = buildKey(shaderType, ctx2.targetInfo); // Different key!
cmd.bindPipeline(..., getPipeline(key2, ...)); // Must rebind
```

### Lazy Render Pass Begin

Render passes begin lazily via `ensureRenderingStarted()`. This means:

1. `requestSwapchainTarget()` sets the target but doesn't begin rendering
2. First `ensureRenderingStarted()` call begins the render pass
3. Pipeline must be compatible with the target set by `request*Target()`

**Gotcha**: If you call `requestSwapchainTarget()` but then construct a pipeline key with HDR format, the pipeline will be incompatible.

### Example: Post-Processing Chain

```cpp
// Step 1: Scene HDR -> Post LDR
m_renderingSession->suspendRendering();
m_renderingSession->transitionSceneHdrToShaderRead(cmd);
m_renderingSession->requestPostLdrTarget();
auto ldrCtx = ensureRenderingStarted(frameCtx);

PipelineKey tonemapKey{};
tonemapKey.type = SDR_TYPE_POST_PROCESS_TONEMAPPING;
tonemapKey.color_format = static_cast<VkFormat>(ldrCtx.targetInfo.colorFormat);
// ... tonemapKey matches ldrCtx.targetInfo ...

// Step 2: Post LDR -> SMAA edges
m_renderingSession->suspendRendering();
m_renderingSession->transitionPostLdrToShaderRead(cmd);
m_renderingSession->requestSmaaEdgesTarget();
auto edgesCtx = ensureRenderingStarted(frameCtx);

PipelineKey edgeKey{};
edgeKey.type = SDR_TYPE_POST_PROCESS_SMAA_EDGE;
edgeKey.color_format = static_cast<VkFormat>(edgesCtx.targetInfo.colorFormat);
// ... edgeKey matches edgesCtx.targetInfo ...
```

Each target switch requires:
1. Suspend rendering (`suspendRendering()`)
2. Transition previous target to shader-read
3. Request new target
4. Get updated `targetInfo` from `ensureRenderingStarted()`
5. Construct new pipeline key matching new target
6. Rebind pipeline

---

## 10. Debugging Pipeline Issues

### Common Issues and Solutions

**Issue 1: Nothing renders**

**Checklist**:
1. Is `ensureRenderingStarted()` called? (Render pass must be active)
2. Is pipeline bound? (`cmd.bindPipeline()`)
3. Are descriptor sets bound? (Check pipeline layout compatibility)
4. Is dynamic state set? (Viewport, scissor, etc.)
5. Are draw calls issued? (`cmd.drawIndexed()`, etc.)

**Issue 2: Wrong colors/format**

**Cause**: Pipeline format mismatch with render target.

**Debug**: Log `render.targetInfo` and `PipelineKey` formats:
```cpp
Warning(LOCATION, "Target: color=%s, depth=%s, count=%u",
        vk::to_string(rt.colorFormat).c_str(),
        vk::to_string(rt.depthFormat).c_str(),
        rt.colorAttachmentCount);
Warning(LOCATION, "Pipeline: color=%s, depth=%s, count=%u",
        vk::to_string(static_cast<vk::Format>(key.color_format)).c_str(),
        vk::to_string(static_cast<vk::Format>(key.depth_format)).c_str(),
        key.color_attachment_count);
```

**Issue 3: Pipeline cache misses**

**Cause**: Incorrect key construction (format mismatch, wrong layout_hash, etc.).

**Debug**: Enable pipeline creation logging to see which keys cause misses.

**Issue 4: Validation errors**

**Enable validation layers** and check output for specific errors:
- "Pipeline not compatible": Format/attachment mismatch
- "Descriptor set not bound": Pipeline layout mismatch
- "Dynamic state not set": Missing `setViewport()`/`setScissor()` calls

### Debugging Tools

**RenderDoc**: Capture frame and inspect pipeline state:
- Check bound pipeline
- Verify pipeline formats match attachments
- Inspect descriptor sets

**Validation Layers**: Enable `VK_LAYER_KHRONOS_validation` for detailed error messages.

**Pipeline Key Logging**: Add logging to `getPipeline()` to track cache hits/misses:

```cpp
auto it = m_pipelines.find(key);
if (it == m_pipelines.end()) {
  Warning(LOCATION, "Pipeline cache miss: type=%d, color=%s",
          static_cast<int>(key.type),
          vk::to_string(static_cast<vk::Format>(key.color_format)).c_str());
}
```

### Best Practices for Debugging

1. **Verify target info**: Always log `render.targetInfo` when constructing keys
2. **Check key construction**: Ensure all required fields are set
3. **Validate compatibility**: Manually verify formats match before binding
4. **Use validation layers**: Enable during development for immediate feedback
5. **Test with RenderDoc**: Visual inspection of pipeline state

---

## Appendix: Quick Reference

### Pipeline Key Construction Checklist

- [ ] Set `type` (shader type)
- [ ] Set `variant_flags` (if shader uses variants)
- [ ] Set `color_format` from `render.targetInfo.colorFormat`
- [ ] Set `depth_format` from `render.targetInfo.depthFormat`
- [ ] Set `sample_count` from `getSampleCount()`
- [ ] Set `color_attachment_count` from `render.targetInfo.colorAttachmentCount`
- [ ] Set `blend_mode` (appropriate for pass)
- [ ] Set `layout_hash` from `layout.hash()` (if vertex attribute shader)
- [ ] Set stencil state (if needed)
- [ ] Query `render.targetInfo` immediately before key construction

### Pipeline Binding Checklist

- [ ] Render target is set (`request*Target()`)
- [ ] Render pass is active (`ensureRenderingStarted()`)
- [ ] Pipeline key matches `render.targetInfo`
- [ ] Pipeline is obtained (`getPipeline(key, modules, layout)`)
- [ ] Pipeline is bound (`cmd.bindPipeline()`)
- [ ] Descriptor sets match pipeline layout
- [ ] Dynamic state is set (viewport, scissor, etc.)
- [ ] Draw calls are issued

### Common Render Target Formats

| Target | Color Format | Depth Format | Count |
|--------|--------------|--------------|-------|
| Swapchain | Surface format (typically `B8G8R8A8_SRGB`) | Device depth format | 1 |
| Scene HDR | `R16G16B16A16_SFLOAT` | Device depth format | 1 |
| G-buffer | `R16G16B16A16_SFLOAT` | Device depth format | 5 |
| Post LDR | `B8G8R8A8_UNORM` | `eUndefined` | 1 |

See [Render Target Type to Format Mapping](#render-target-type-to-format-mapping) for format notes.

---

## Related Documentation

- [Pipeline Management](VULKAN_PIPELINE_MANAGEMENT.md) - Pipeline architecture and lifecycle
- [Vulkan Architecture](VULKAN_ARCHITECTURE.md) - Overall renderer architecture
- [Render Pass Structure](VULKAN_RENDER_PASS_STRUCTURE.md) - Dynamic rendering details
- [Descriptor Sets](VULKAN_DESCRIPTOR_SETS.md) - Descriptor set management


