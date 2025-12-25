# Vulkan Pipeline Management

This document covers pipeline object lifecycle, caching strategy, state management, and the contracts that ensure correct pipeline-target compatibility.

---

## Table of Contents

1. [Pipeline Architecture Overview](#1-pipeline-architecture-overview)
2. [Baked vs Dynamic State Matrix](#2-baked-vs-dynamic-state-matrix)
3. [Pipeline Cache Key Structure](#3-pipeline-cache-key-structure)
4. [Dynamic Rendering Compatibility](#4-dynamic-rendering-compatibility)
5. [Pipeline Lifecycle](#5-pipeline-lifecycle)
6. [Descriptor Update Safety Rules](#6-descriptor-update-safety-rules)
7. [Buffer Orphaning and Descriptor Caching](#7-buffer-orphaning-and-descriptor-caching)
8. [Feature and Extension Requirements](#8-feature-and-extension-requirements)
9. [Failure Modes and Fallbacks](#9-failure-modes-and-fallbacks)

---

## 1. Pipeline Architecture Overview

The Vulkan renderer uses **dynamic state extensively** to minimize pipeline object count while maintaining correct rendering. Pipelines are cached in-memory and reused based on a composite key.

### Key Design Decisions

| Aspect | Approach | Rationale |
|--------|----------|-----------|
| Render Pass | `VK_KHR_dynamic_rendering` | No VkRenderPass/VkFramebuffer management |
| State Model | Maximally dynamic | Fewer pipeline variants, late binding |
| Cache Strategy | In-memory only | Disk persistence not implemented |
| Warm-up | None (lazy compile) | Pipelines created on first use |

### Pipeline Layout Kinds

Three distinct pipeline layouts serve different rendering paths:

| Layout Kind | Use Case | Descriptor Strategy |
|-------------|----------|---------------------|
| `Standard` | 2D, UI, particles, post-process | Push descriptors + global set |
| `Model` | 3D model rendering | Bindless textures + push constants |
| `Deferred` | Deferred lighting | Push descriptors + G-buffer set |

---

## 2. Baked vs Dynamic State Matrix

This table defines which state is baked into the pipeline (affects cache key) versus set dynamically per-draw (does not affect cache key).

### Core State

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Shader modules** | ✓ | - | ✓ | Vertex + fragment modules |
| **Pipeline layout** | ✓ | - | ✓ | Determined by shader type |
| **Vertex input layout** | ✓ | - | ✓ | Attribute bindings and formats |
| **Color attachment format** | ✓ | - | ✓ | From render target |
| **Depth attachment format** | ✓ | - | ✓ | From render target |
| **Sample count** | ✓ | - | ✓ | MSAA samples |
| **Color attachment count** | ✓ | - | ✓ | Number of color attachments |

### Dynamic State (Extended Dynamic State 1/2)

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Viewport** | - | ✓ | - | `setViewport()` per draw |
| **Scissor** | - | ✓ | - | `setScissor()` per draw |
| **Cull mode** | - | ✓ | - | `setCullMode()` per draw |
| **Front face** | - | ✓ | - | `setFrontFace()` per draw |
| **Primitive topology** | - | ✓ | - | `setPrimitiveTopology()` per draw |
| **Depth test enable** | - | ✓ | - | `setDepthTestEnable()` per draw |
| **Depth write enable** | - | ✓ | - | `setDepthWriteEnable()` per draw |
| **Depth compare op** | - | ✓ | - | `setDepthCompareOp()` per draw |
| **Stencil test enable** | - | ✓ | - | `setStencilTestEnable()` per draw |
| **Depth bias enable** | - | ✓ | - | `setDepthBiasEnable()` per draw |

### Extended Dynamic State 3 (Optional)

These features are **extension-only** (not core in Vulkan 1.3) and tracked via `ExtendedDynamicState3Caps`:

| State | Baked | Dynamic | Cache Key | Fallback if Unavailable |
|-------|:-----:|:-------:|:---------:|------------------------|
| **Color blend enable** | ✓* | ✓* | ✓* | Baked into pipeline |
| **Color write mask** | ✓* | ✓* | ✓* | Baked into pipeline |
| **Polygon mode** | ✓* | ✓* | ✓* | Baked into pipeline |
| **Rasterization samples** | ✓ | - | ✓ | Always baked |

*Conditional: Dynamic if extension supported, otherwise baked.

### Alpha Blending

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Blend mode** | ✓ | - | ✓ | `gr_alpha_blend` enum value |
| **Blend factors** | ✓ | - | ✓ | Derived from blend mode |
| **Blend op** | ✓ | - | ✓ | Derived from blend mode |

---

## 3. Pipeline Cache Key Structure

The pipeline cache key captures all state that affects pipeline identity:

```cpp
struct PipelineKey {
    // Shader identity
    shader_type type;
    uint32_t variantFlags;

    // Render target format
    VkFormat colorFormat;
    VkFormat depthFormat;
    VkSampleCountFlagBits sampleCount;
    uint32_t colorAttachmentCount;

    // Fixed state
    gr_alpha_blend blendMode;
    size_t vertexLayoutHash;

    // Extended dynamic state 3 (if not dynamically settable)
    bool colorBlendEnable;      // Only if !caps.colorBlendEnable
    uint32_t colorWriteMask;    // Only if !caps.colorWriteMask
    VkPolygonMode polygonMode;  // Only if !caps.polygonMode
};
```

### Hash Computation

The key is hashed for map lookup:

```cpp
size_t PipelineKeyHasher::operator()(const PipelineKey& key) const
{
    size_t h = 0;
    hash_combine(h, static_cast<uint32_t>(key.type));
    hash_combine(h, key.variantFlags);
    hash_combine(h, static_cast<uint32_t>(key.colorFormat));
    hash_combine(h, static_cast<uint32_t>(key.depthFormat));
    hash_combine(h, static_cast<uint32_t>(key.sampleCount));
    hash_combine(h, key.colorAttachmentCount);
    hash_combine(h, static_cast<uint32_t>(key.blendMode));
    hash_combine(h, key.vertexLayoutHash);
    // ... conditional EDS3 fields
    return h;
}
```

---

## 4. Dynamic Rendering Compatibility

Since the renderer uses `VK_KHR_dynamic_rendering` (no traditional VkRenderPass), pipeline compatibility is determined by matching rendering info at bind time.

### Compatibility Contract

A pipeline is compatible with a rendering session if **all** of these match:

| Property | Pipeline Value | Rendering Info Value |
|----------|----------------|---------------------|
| Color format(s) | `VkPipelineRenderingCreateInfo::pColorAttachmentFormats` | `VkRenderingAttachmentInfo::imageView` format |
| Depth format | `VkPipelineRenderingCreateInfo::depthAttachmentFormat` | `VkRenderingAttachmentInfo::imageView` format |
| Stencil format | `VkPipelineRenderingCreateInfo::stencilAttachmentFormat` | (same as depth if combined) |
| Color attachment count | `colorAttachmentCount` | Number of color attachments in `VkRenderingInfo` |
| Sample count | Pipeline sample count | Attachment image sample count |

### Target Type to Format Mapping

| Target Type | Color Format | Depth Format | Sample Count | Attachments |
|-------------|--------------|--------------|--------------|-------------|
| **Swapchain** | `B8G8R8A8_SRGB` or surface format | `D32_SFLOAT` | 1 | 1 |
| **Scene HDR** | `R16G16B16A16_SFLOAT` | `D32_SFLOAT` | 1 | 1 |
| **G-buffer** | `R16G16B16A16_SFLOAT` (x5) | `D32_SFLOAT` | 1 | 5 |
| **Shadow Map** | None | `D32_SFLOAT` | 1 | 0 |
| **Cockpit Texture** | `B8G8R8A8_UNORM` | None | 1 | 1 |

### Shader Type Restrictions

Some shader types are only valid against specific target types:

| Shader Type | Valid Targets | Notes |
|-------------|---------------|-------|
| `SDR_TYPE_MODEL` | G-buffer | Outputs to all 5 G-buffer attachments |
| `SDR_TYPE_DEFERRED_LIGHTING` | Scene HDR | Reads G-buffer, writes to scene |
| `SDR_TYPE_INTERFACE` | Swapchain, Cockpit | 2D overlay rendering |
| `SDR_TYPE_POST_PROCESS_*` | Swapchain | Final output stage |

---

## 5. Pipeline Lifecycle

### Creation Phase

Pipelines are created **lazily on first use**:

```
Draw call requested
    ↓
Compute pipeline key from current state
    ↓
Check cache: key → VkPipeline
    ↓
[Cache Miss] → Create new pipeline
    ↓
Store in cache
    ↓
Bind pipeline and draw
```

### Thread Safety

Pipeline creation occurs on the **main rendering thread only**. No thread-safety mechanisms are required for the cache.

### Cache Invalidation

The pipeline cache is **never invalidated** during normal operation. It persists for the lifetime of the renderer.

Pipelines are only destroyed at:
- Renderer shutdown
- Device lost recovery (full recreation)

### No Disk Persistence

The current implementation does not persist `VkPipelineCache` to disk. All pipelines are recompiled on each application launch.

**Implication**: First frame of each shader/target combination incurs shader compile cost.

---

## 6. Descriptor Update Safety Rules

Descriptor set updates must respect GPU synchronization to avoid data races.

### The Core Rule

> **A descriptor set must not be updated while any in-flight command buffer references it.**

### Safe Update Conditions

| Scenario | Safe? | Reason |
|----------|:-----:|--------|
| Update before any command buffer submission | ✓ | No references exist |
| Update after `vkQueueWaitIdle()` | ✓ | All work complete |
| Update after fence wait for specific frame | ✓ | Frame's work complete |
| Update between frames with per-frame sets | ✓ | Set not in flight |
| Update to set currently bound in recording | ✓ | Recording != executing |
| Update to set in submitted command buffer | ✗ | **DATA RACE** |

### Per-Frame Descriptor Sets

The renderer uses per-frame model descriptor sets (`kFramesInFlight = 2`):

```
Frame 0: modelDescriptorSet[0] ← Safe to update when frame 0 not in flight
Frame 1: modelDescriptorSet[1] ← Safe to update when frame 1 not in flight
```

**Update timing**: Sets are updated at frame start, after waiting on the previous frame's fence:

```cpp
void prepareFrameForReuse(VulkanFrame& frame)
{
    frame.wait_for_gpu();  // Fence wait - GPU done with this frame
    // NOW safe to update frame's descriptor set
}
```

### Push Descriptors

Push descriptors (`vkCmdPushDescriptorSetKHR`) are inherently safe because they don't reference a persistent descriptor set - data is embedded in the command buffer.

### Global Descriptor Set

The global set (G-buffer textures) is shared across all frames. It is updated:
- Once during deferred lighting setup
- Only when render targets change (resize, etc.)

**Safety**: Updates occur before any command buffer referencing the new values is submitted.

---

## 7. Buffer Orphaning and Descriptor Caching

The renderer uses both buffer orphaning and descriptor caching. These must be coordinated correctly.

### The Problem

Buffer orphaning can replace the underlying `VkBuffer`:

```cpp
void resizeBuffer(handle, size)
{
    // Old buffer retired to deferred release
    m_deferredReleases.enqueue(retireSerial, [oldBuf]() { destroy(oldBuf); });

    // NEW buffer created
    buffer.buffer = createNewBuffer(size);
}
```

Meanwhile, descriptor caching may skip updates if "handle unchanged":

```cpp
if (frame.uniformBinding.bufferHandle == handle) {
    // Skip descriptor update - WRONG if VkBuffer changed!
}
```

### The Invariant

> **Descriptor writes must be keyed on VkBuffer identity, not just handle equality.**

### Implementation

The renderer tracks both handle AND buffer pointer:

```cpp
struct DynamicUniformBinding {
    gr_buffer_handle bufferHandle;
    VkBuffer cachedBuffer;  // Actual VkBuffer for comparison
    uint32_t dynamicOffset;
};

// Update check includes buffer pointer
if (binding.bufferHandle != handle || binding.cachedBuffer != currentBuffer) {
    // Must update descriptor
    writeDescriptor(...);
    binding.cachedBuffer = currentBuffer;
}
```

### Frame Isolation

Each frame has its own `DynamicUniformBinding` state, so orphaning on frame N doesn't affect frame N-1's cached bindings (which reference the old buffer that's still valid via deferred release).

---

## 8. Feature and Extension Requirements

### Required (Core)

| Feature/Extension | Version | Purpose |
|-------------------|---------|---------|
| Vulkan 1.3 | Core | Dynamic rendering, sync2, extended dynamic state |
| `VK_KHR_dynamic_rendering` | Core 1.3 | No render pass objects |
| `VK_KHR_synchronization2` | Core 1.3 | 64-bit stage/access masks |
| `VK_EXT_extended_dynamic_state` | Core 1.3 | Cull mode, depth test, etc. |
| `VK_EXT_extended_dynamic_state2` | Core 1.3 | Depth bias enable, etc. |

### Required (Hard Check)

| Feature | Check | Failure Mode |
|---------|-------|--------------|
| `pushDescriptor` | Vulkan 1.4 feature | Throws exception |
| `shaderSampledImageArrayNonUniformIndexing` | Descriptor indexing | Throws exception |
| `runtimeDescriptorArray` | Descriptor indexing | Throws exception |
| `maxDescriptorSetSampledImages >= 1024` | Device limit | Assertion failure |

### Optional (Graceful Degradation)

| Feature | Fallback |
|---------|----------|
| `VK_EXT_extended_dynamic_state3` | Bake blend/polygon state into pipeline |
| `samplerYcbcrConversion` | Movie playback disabled |
| `G8_B8_R8_3PLANE_420_UNORM` format | Movie playback disabled |

---

## 9. Failure Modes and Fallbacks

### Pipeline Creation Failure

If `vkCreateGraphicsPipelines` fails:

```cpp
auto result = device.createGraphicsPipelines(cache, 1, &pipelineInfo, nullptr, &pipeline);
if (result != vk::Result::eSuccess) {
    // Log error with shader type and key details
    Error(LOCATION, "Pipeline creation failed for shader type %d: %s",
          static_cast<int>(key.type), vk::to_string(result).c_str());
    // Fatal - no fallback shader system
}
```

**Current behavior**: Fatal error. No fallback shader system exists.

**Future improvement**: Could implement fallback to a simple solid-color shader for graceful degradation.

### Shader Module Load Failure

If SPIR-V module cannot be loaded:

```cpp
if (!moduleData.has_value()) {
    Error(LOCATION, "Failed to load shader module: %s", path.c_str());
    // Fatal
}
```

**Current behavior**: Fatal error.

### Descriptor Pool Exhaustion

The model pool is sized for exactly `kFramesInFlight` sets. Exhaustion should be impossible with correct usage:

```cpp
poolInfo.maxSets = kFramesInFlight; // 2
```

If allocation fails, it indicates a logic error (allocating more sets than expected).

---

## Appendix: Debugging Tips

### RenderDoc Integration

Debug regions are inserted for GPU capture correlation:

```cpp
cmd.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{"Model Rendering"});
// ... draw calls ...
cmd.endDebugUtilsLabelEXT();
```

### Validation Layer Messages

Common validation errors related to pipelines:

| Error | Cause | Fix |
|-------|-------|-----|
| "Pipeline not compatible with render pass" | Format mismatch | Check target format in pipeline key |
| "Descriptor set not bound" | Missing bind call | Ensure `bindDescriptorSets` before draw |
| "Dynamic state not set" | Missing `set*` call | Set all required dynamic state before draw |
