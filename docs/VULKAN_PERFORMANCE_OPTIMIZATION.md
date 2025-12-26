# Vulkan Performance Optimization Guide

This document provides performance optimization strategies, profiling techniques, and best practices for the Vulkan renderer. It is intended for developers working on performance-critical paths or diagnosing frame time regressions.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Pipeline Management](#2-pipeline-management)
3. [Descriptor Optimization](#3-descriptor-optimization)
4. [Memory Management](#4-memory-management)
5. [Command Buffer Recording](#5-command-buffer-recording)
6. [Synchronization Optimization](#6-synchronization-optimization)
7. [Profiling Techniques](#7-profiling-techniques)
8. [Common Bottlenecks](#8-common-bottlenecks)
9. [Performance Checklist](#9-performance-checklist)
10. [Quick Reference Tables](#10-quick-reference-tables)

---

## 1. Overview

Performance optimization in the Vulkan renderer centers on minimizing CPU overhead and GPU stalls while maximizing parallelism. The core strategies are:

| Strategy | Benefit | Primary Cost |
|----------|---------|--------------|
| Pipeline caching | Eliminates shader compilation stalls | Disk I/O at startup/shutdown |
| Bindless textures | Removes per-draw descriptor updates | Initial full-array descriptor write |
| Ring buffers | Lock-free per-frame allocations | Fixed memory footprint per frame |
| Dynamic state | Reduces pipeline permutations | Minor command buffer overhead |
| Timeline semaphores | Simplified cross-frame sync | Slightly higher driver overhead vs binary |

**Key Principle**: Measure first, optimize second. Profile before assuming a bottleneck.

**Recommended Tools**:

| Tool | Use Case | Platform |
|------|----------|----------|
| RenderDoc | Frame capture, GPU timeline, resource inspection | Cross-platform |
| AMD Radeon GPU Profiler (RGP) | GPU occupancy, wavefront analysis, barrier inspection | AMD GPUs |
| NVIDIA Nsight Graphics | GPU trace, shader profiling, memory bandwidth | NVIDIA GPUs |
| Intel GPA | Frame analysis, shader optimization | Intel GPUs |
| Vulkan validation layers | `VK_LAYER_KHRONOS_validation` with `VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT` | All |

---

## 2. Pipeline Management

### 2.1 Pipeline Cache Persistence

The renderer persists the Vulkan pipeline cache to disk, dramatically reducing pipeline creation time for subsequent runs.

**Implementation**: `VulkanDevice.cpp` (functions `createPipelineCache` and `savePipelineCache`)

**Cache File**: `vulkan_pipeline.cache` in the working directory

**Cache Validation**: Before loading cached data, the implementation validates:
- Header length matches expected structure size
- Vendor ID matches current GPU
- Device ID matches current GPU
- Pipeline cache UUID matches (changes on driver updates)

If validation fails, a fresh cache is created. This prevents crashes or corruption from stale cache data after driver updates.

**Performance Impact**:
- Uncached pipeline creation: ~5-15 ms per pipeline (shader compilation)
- Cached pipeline creation: <1 ms per pipeline

**Best Practices**:
- Ensure the cache file is writable at shutdown
- Delete the cache file when testing pipeline changes during development
- The cache is invalidated automatically on driver updates (UUID mismatch)

### 2.2 Dynamic State to Reduce Pipeline Variants

The renderer leverages Vulkan dynamic state to reduce the number of unique pipelines needed.

**Core Dynamic States** (Vulkan 1.3 core, always available):

| Dynamic State | Effect |
|---------------|--------|
| `eViewport` | Viewport dimensions set per-frame |
| `eScissor` | Scissor rect set per-frame |
| `eLineWidth` | Line width for line primitives |
| `eCullMode` | Front/back/none culling |
| `eFrontFace` | Clockwise/counter-clockwise winding |
| `ePrimitiveTopology` | Triangle list/strip/fan, line list/strip |
| `eDepthTestEnable` | Depth testing on/off |
| `eDepthWriteEnable` | Depth writes on/off |
| `eDepthCompareOp` | Less/greater/equal comparison |
| `eStencilTestEnable` | Stencil testing on/off |

**Extended Dynamic State 3** (`VK_EXT_extended_dynamic_state3`, optional):

The renderer queries device capabilities and conditionally uses these dynamic states:

| Dynamic State | Capability Flag | Usage |
|---------------|-----------------|-------|
| `eColorBlendEnableEXT` | `caps.colorBlendEnable` | Deferred lights toggle additive blend |
| `eColorWriteMaskEXT` | `caps.colorWriteMask` | Selective channel writes |
| `ePolygonModeEXT` | `caps.polygonMode` | Wireframe debugging |
| `eRasterizationSamplesEXT` | `caps.rasterizationSamples` | MSAA sample count |

**Implementation**: `VulkanPipelineManager::BuildDynamicStateList` builds the dynamic state list based on device support.

**Impact**: Reduces pipeline count from potentially thousands of permutations to approximately 100-200 unique pipelines for typical gameplay.

### 2.3 Pipeline Binding Patterns

**Optimal Pattern**: Bind a pipeline once, issue multiple draws.

```cpp
// GOOD: Bind once, draw many (sorted by pipeline)
vk::Pipeline currentPipeline = VK_NULL_HANDLE;
for (const auto& batch : sortedBatches) {
    if (batch.pipeline != currentPipeline) {
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, batch.pipeline);
        currentPipeline = batch.pipeline;
    }
    cmd.drawIndexed(...);
}

// BAD: Unconditional rebind (wastes GPU cycles)
for (const auto& model : models) {
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, model.pipeline);
    cmd.drawIndexed(...);
}
```

**Cost of Pipeline Switches**: Each `vkCmdBindPipeline` can cause:
- Shader constant reprogramming
- Render state flushes
- Cache invalidations on some architectures

Sorting draw calls by pipeline is a standard optimization.

---

## 3. Descriptor Optimization

### 3.1 Bindless Texture Array

The renderer uses a bindless texture array to avoid per-draw descriptor set updates for model textures.

**Configuration** (from `VulkanConstants.h`):

```cpp
constexpr uint32_t kMaxBindlessTextures = 1024;
```

**Reserved Slots**:

| Slot | Purpose | Content |
|------|---------|---------|
| 0 | Fallback (safety) | Black texture (prevents sampling destroyed images) |
| 1 | Default base/diffuse | Neutral gray |
| 2 | Default normal | Flat normal (128, 128, 255) |
| 3 | Default specular | Neutral specular |
| 4+ | Dynamic textures | Assigned at runtime |

**Incremental Updates**: The renderer avoids updating all 1024 descriptors every frame by maintaining a per-frame cache of descriptor contents.

**Algorithm** (from `VulkanRenderer::updateModelDescriptors`):

1. Build the desired descriptor info array for the current frame
2. Compare against the cached info from the previous use of this frame index
3. Identify contiguous ranges of changed slots
4. Issue `vkUpdateDescriptorSets` only for changed ranges

**Implementation Pattern**:

```cpp
// Pseudo-code for incremental update
uint32_t i = 0;
while (i < kMaxBindlessTextures) {
    if (cache[i] == desired[i]) { ++i; continue; }

    uint32_t start = i;
    while (i < kMaxBindlessTextures && cache[i] != desired[i]) {
        cache[i] = desired[i];
        ++i;
    }
    uint32_t count = i - start;

    // Single write covering [start, start+count)
    writes.push_back(makeDescriptorWrite(start, count, &desired[start]));
}
```

**Performance Impact**: Reduces descriptor updates from 1024 per frame to typically 10-50, depending on texture churn.

### 3.2 Push Descriptors

For per-draw data where bindless is impractical (e.g., post-processing passes, deferred lights), the renderer uses push descriptors (`VK_KHR_push_descriptor`).

**Advantages**:
- No descriptor set allocation
- No descriptor pool management
- Low overhead for infrequent draws

**Usage Pattern**:

```cpp
// Push descriptors for deferred light pass
std::array<vk::WriteDescriptorSet, 2> writes = { uniformWrite, textureWrite };
cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,
                         layout,
                         0,  // set index
                         writes);
```

**Trade-off**: Push descriptors have per-draw overhead. Use them for passes with few draws (post-processing, fullscreen effects), not for high-volume model rendering.

### 3.3 Descriptor Set Binding Strategy

The model descriptor set is bound once per frame with dynamic offsets for per-draw uniform data.

**Binding Layout**:

| Binding | Type | Content | Update Frequency |
|---------|------|---------|------------------|
| 0 | Storage Buffer | Vertex heap (SSBO) | Once per frame |
| 1 | Combined Image Sampler (array) | Bindless textures (1024 slots) | Incremental per frame |
| 2 | Uniform Buffer Dynamic | Model uniforms | Dynamic offset per draw |
| 3 | Storage Buffer Dynamic | Batched transforms | Dynamic offset per draw |

**Pattern**: Bind descriptor set once, update dynamic offsets at draw time.

---

## 4. Memory Management

### 4.1 Ring Buffer Architecture

Each frame-in-flight owns three ring buffers for CPU-to-GPU data transfer without synchronization.

**Ring Buffer Sizes** (from `VulkanRenderer.h`):

| Buffer | Size | Purpose |
|--------|------|---------|
| Uniform Ring | 512 KB | Per-frame uniform allocations |
| Vertex Ring | 1 MB | Dynamic vertex/index data (2D, HUD) |
| Staging Ring | 12 MB | Texture uploads, buffer updates |

**Allocation Pattern**:

```cpp
VulkanRingBuffer::Allocation alloc = frame.uniformBuffer().allocate(size, alignment);
memcpy(alloc.mapped, data, size);
// alloc.offset is the GPU-visible offset within the ring buffer
```

**Ring Reset**: Rings are reset when the frame is recycled (after GPU fence wait).

**Sizing Guidelines**:
- **Staging buffer too small**: Uploads are deferred, causing stuttering
- **Staging buffer too large**: Wastes host-visible memory
- Profile texture upload patterns to tune staging size

### 4.2 Uniform Buffer Alignment

Uniform buffer dynamic offsets must be aligned to `minUniformBufferOffsetAlignment`.

**Typical Values**: 64-256 bytes depending on GPU vendor.

**Query Method**: `VulkanDevice::minUniformBufferOffsetAlignment()`

**Current Implementation**: The renderer queries this at device creation and passes it to ring buffer allocation.

**Optimization Opportunity**: Pack multiple small uniforms into single aligned allocations to reduce allocation overhead.

### 4.3 Vertex Heap (SSBO-based Vertex Pulling)

Model vertices are stored in a large SSBO ("vertex heap") rather than traditional vertex buffers.

**Benefits**:
- Flexible vertex layouts via shader vertex pulling
- Single buffer binding regardless of model count
- Simplified memory management

**Memory Type**: Device-local (fast GPU access), populated via staging buffer transfers.

**Implementation**: Managed by `GPUMemoryHeap`, registered with renderer via `setModelVertexHeapHandle()`.

### 4.4 VMA Integration

The renderer uses Vulkan Memory Allocator (VMA) for memory management. Key considerations:

- Prefer `VMA_MEMORY_USAGE_AUTO` for automatic memory type selection
- Use `VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT` for staging buffers
- Dedicated allocations for large render targets

---

## 5. Command Buffer Recording

### 5.1 Recording Overhead

Command buffer recording is CPU-bound. The goal is to minimize command count and state changes.

**Target**: Command buffer recording should complete in <1 ms per frame.

**Optimization Strategies**:
- Sort draws by pipeline to minimize bind calls
- Sort draws by descriptor set to minimize rebinds
- Use instancing for repeated geometry
- Use indirect draws for GPU-driven culling (future enhancement)

### 5.2 Secondary Command Buffers

**Current Status**: Not implemented. All recording occurs in primary command buffers.

**Potential Use Cases**:
- Static scene geometry (record once, replay each frame)
- UI rendering (isolated recording, parallel preparation)

**Trade-off**: Secondary command buffers add execution overhead. Only beneficial when:
- Contents can be reused across frames
- Recording can be parallelized across threads

### 5.3 Command Pool Reset Strategy

**Current Implementation**: Each frame owns a command pool, reset entirely when the frame is recycled.

```cpp
void VulkanFrame::reset() {
    m_device.resetCommandPool(m_commandPool.get());
    // Ring buffers reset, per-frame bindings cleared
}
```

**Alternative**: Per-command-buffer reset (`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`) allows recycling individual buffers. Not used currently due to small pool size per frame.

---

## 6. Synchronization Optimization

### 6.1 Frames-in-Flight Model

**Current Configuration**: 2 frames in flight (`kFramesInFlight` in `VulkanConstants.h`).

**Mechanism**:
1. Frame N records commands
2. Frame N-1 may still be executing on GPU
3. CPU proceeds without blocking until frame N-2's fence is waited

**Memory Implications**: Each frame-in-flight requires its own:
- Command pool and buffer
- Ring buffers (uniform, vertex, staging)
- Semaphores
- Descriptor set allocations

**Tuning**:
- **Increase to 3 frames**: More GPU parallelism, higher input latency, more memory
- **Stay at 2 frames**: Balance between latency and throughput

**Measurement**: GPU utilization should be 90-100% with 2 frames. If GPU is idle waiting for CPU, consider increasing frames-in-flight.

### 6.2 Semaphore Strategy

**Binary Semaphores**: Used for swapchain synchronization.

| Semaphore | Purpose |
|-----------|---------|
| `imageAvailable` | Signals when swapchain image is ready |
| `renderFinished` | Signals when rendering is complete for present |

**Timeline Semaphore**: Used for serial-based resource lifetime tracking.

| Semaphore | Purpose |
|-----------|---------|
| `m_submitTimeline` | Global completion serial for deferred resource release |

**Benefit**: Timeline semaphores reduce total semaphore count and simplify cross-frame dependency tracking.

### 6.3 Pipeline Barrier Optimization

**Cost**: Pipeline barriers synchronize GPU work and can cause stalls.

**Current Implementation**: Sync2 barriers (`pipelineBarrier2`) with explicit stage/access masks.

**Best Practices**:

1. **Batch Barriers**: Combine multiple image/buffer barriers into single `pipelineBarrier2` call:

```cpp
std::vector<vk::ImageMemoryBarrier2> barriers;
barriers.push_back(colorBarrier);
barriers.push_back(depthBarrier);

vk::DependencyInfo dep{};
dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
dep.pImageMemoryBarriers = barriers.data();
cmd.pipelineBarrier2(dep);
```

2. **Precise Stage Masks**: Avoid `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` except when truly needed. Use specific stages:

| Operation | Recommended Stage Mask |
|-----------|------------------------|
| Color attachment write | `eColorAttachmentOutput` |
| Depth attachment write | `eEarlyFragmentTests` or `eLateFragmentTests` |
| Shader read (sampled image) | `eFragmentShader` |
| Transfer | `eTransfer` |

3. **Layout Transitions**: The renderer uses helper functions like `stageAccessForLayout()` to derive correct masks from target layouts.

---

## 7. Profiling Techniques

### 7.1 RenderDoc Profiling

**Capture Workflow**:
1. Launch game with RenderDoc attached
2. Press capture hotkey during representative gameplay
3. Open capture in RenderDoc

**Key Metrics to Examine**:

| Metric | Location | Target |
|--------|----------|--------|
| GPU time per pass | Event Browser -> Timeline | Identify hotspots |
| Draw call count | Statistics -> Draw Calls | Minimize |
| Pipeline switches | Event Browser -> filter by `vkCmdBindPipeline` | Minimize |
| Descriptor updates | Event Browser -> filter by `vkUpdateDescriptorSets` | Should be batched |
| Barrier count | Event Browser -> filter by `vkCmdPipelineBarrier` | Minimize |

### 7.2 CPU Profiling

**Tools**: Visual Studio Profiler, Intel VTune, perf (Linux)

**Target Metrics**:

| Operation | Target Time |
|-----------|-------------|
| Command buffer recording | < 1 ms |
| Descriptor updates (total) | < 0.5 ms |
| Pipeline creation (cached) | < 0.1 ms |
| Ring buffer allocations | < 0.1 ms |
| Draw call setup (per call) | < 10 us |

**Hotspot Analysis**: Look for:
- Excessive time in `vkUpdateDescriptorSets` (descriptor churn)
- Slow hash computations (pipeline key hashing)
- Lock contention (should be minimal with ring buffers)

### 7.3 GPU Profiling

**Tools**: AMD RGP, NVIDIA Nsight Graphics, Intel GPA

**Key Metrics**:

| Metric | Target | Action if Exceeded |
|--------|--------|-------------------|
| GPU utilization | > 90% | Check for CPU bottleneck |
| Memory bandwidth | < 80% of peak | Compress textures, reduce overdraw |
| Shader occupancy | High (varies by GPU) | Reduce register pressure |
| Texture cache hit rate | > 80% | Improve texture locality |
| Barrier stalls | Minimal | Batch barriers, use precise masks |

### 7.4 Validation Layer Performance Hints

Enable best practices validation:

```cpp
VkValidationFeatureEnableEXT enables[] = {
    VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT
};
VkValidationFeaturesEXT features = { ... };
features.enabledValidationFeatureCount = 1;
features.pEnabledValidationFeatures = enables;
```

This reports performance anti-patterns at runtime.

---

## 8. Common Bottlenecks

### Bottleneck 1: Pipeline Creation Stalls

**Symptoms**: Frame drops on first render of new pipeline; hitching when loading new assets.

**Cause**: Pipeline creation includes shader compilation (~5-15 ms uncached).

**Diagnosis**: Profile `vkCreateGraphicsPipelines` calls; check pipeline cache hit rate.

**Solutions**:
- Ensure pipeline cache is loaded at startup and saved at shutdown (implemented)
- Pre-warm common pipelines during loading screens
- Maximize dynamic state usage to reduce unique pipeline count

### Bottleneck 2: Descriptor Update Overhead

**Symptoms**: High CPU time in descriptor update code paths; `vkUpdateDescriptorSets` in profiler hotspots.

**Cause**: Updating large descriptor arrays every frame without caching.

**Diagnosis**: Count descriptor writes per frame; check for full-array updates.

**Solutions**:
- Use incremental bindless updates (implemented via per-frame cache)
- Reduce texture count through atlasing
- Use push descriptors for infrequent draws

### Bottleneck 3: Command Buffer Recording

**Symptoms**: High CPU time before `vkQueueSubmit`; recording exceeds 1 ms.

**Cause**: Too many draw calls; excessive state changes between draws.

**Diagnosis**: Count draw calls; profile state change frequency.

**Solutions**:
- Sort draws by pipeline and descriptor set
- Batch small draws using instancing
- Reduce unique material count

### Bottleneck 4: GPU Synchronization Stalls

**Symptoms**: Low GPU utilization despite heavy frame; frame pacing issues; GPU timeline shows idle gaps.

**Cause**: Overly conservative barriers; CPU waiting on GPU unnecessarily.

**Diagnosis**: Examine barrier timeline in RenderDoc; check fence wait frequency.

**Solutions**:
- Use precise stage/access masks (not `ALL_COMMANDS`)
- Batch barriers into single calls
- Increase frames-in-flight if CPU is bottleneck

### Bottleneck 5: Memory Bandwidth Saturation

**Symptoms**: GPU metrics show high memory bandwidth usage; slowdowns with high-resolution textures.

**Cause**: Uncompressed textures; excessive texture sampling; large render targets.

**Diagnosis**: Check memory bandwidth in GPU profiler; examine texture formats.

**Solutions**:
- Compress textures (BC1/BC3/BC7)
- Implement texture streaming for distant/low-priority textures
- Reduce render target resolution where acceptable

### Bottleneck 6: Ring Buffer Exhaustion

**Symptoms**: Assert failures in ring buffer allocation; stuttering during heavy upload frames.

**Cause**: Ring buffer too small for frame's allocation needs.

**Diagnosis**: Monitor ring buffer usage via debug counters; log allocation failures.

**Solutions**:
- Increase ring buffer size (trade-off: memory usage)
- Spread uploads across multiple frames
- Prioritize uploads based on visibility

---

## 9. Performance Checklist

Use this checklist when reviewing performance-critical code or diagnosing issues.

### Frame Recording
- [ ] Pipeline bound once per batch (draws sorted by pipeline)
- [ ] Descriptor sets bound once per frame where possible (dynamic offsets for per-draw data)
- [ ] Push constants updated only when changed
- [ ] Draw calls batched (minimize state changes)
- [ ] Command buffer recording time < 1 ms

### Descriptors
- [ ] Bindless textures use incremental updates (per-frame cache)
- [ ] Push descriptors used for per-draw data in low-frequency passes
- [ ] Descriptor writes batched (single `vkUpdateDescriptorSets` call per pass)
- [ ] Total descriptor update time < 0.5 ms

### Memory
- [ ] Staging buffer sized for typical frame (12 MB default)
- [ ] Uniform buffers aligned to `minUniformBufferOffsetAlignment`
- [ ] Vertex heap uses device-local memory
- [ ] Ring buffers reset per-frame (no leaks)

### Synchronization
- [ ] Pipeline barriers batched (single `pipelineBarrier2` call when possible)
- [ ] Precise stage/access masks used (not `ALL_COMMANDS`)
- [ ] Timeline semaphore for resource lifetime tracking
- [ ] Frame-in-flight count appropriate (2-3)

### Pipelines
- [ ] Pipeline cache loaded at startup
- [ ] Pipeline cache saved at shutdown
- [ ] Extended dynamic state used where supported
- [ ] Pipeline creation time < 0.1 ms (cached)

---

## 10. Quick Reference Tables

### Ring Buffer Sizes

| Buffer | Size | Defined In |
|--------|------|------------|
| `UNIFORM_RING_SIZE` | 512 KB | `VulkanRenderer.h` |
| `VERTEX_RING_SIZE` | 1 MB | `VulkanRenderer.h` |
| `STAGING_RING_SIZE` | 12 MB | `VulkanRenderer.h` |

### Key Constants

| Constant | Value | Defined In |
|----------|-------|------------|
| `kFramesInFlight` | 2 | `VulkanConstants.h` |
| `kMaxBindlessTextures` | 1024 | `VulkanConstants.h` |
| `kBindlessFirstDynamicTextureSlot` | 4 | `VulkanConstants.h` |

### Target Performance Metrics

| Metric | Target | Acceptable | Poor |
|--------|--------|------------|------|
| Command buffer recording | < 1 ms | 1-2 ms | > 2 ms |
| Descriptor updates | < 0.5 ms | 0.5-1 ms | > 1 ms |
| Pipeline creation (cached) | < 0.1 ms | 0.1-1 ms | > 1 ms |
| GPU utilization | > 90% | 80-90% | < 80% |

---

## References

### Source Files

| File | Contents |
|------|----------|
| `code/graphics/vulkan/VulkanDevice.cpp` | Pipeline cache persistence (`createPipelineCache`, `savePipelineCache`) |
| `code/graphics/vulkan/VulkanPipelineManager.cpp` | Pipeline creation, dynamic state list (`BuildDynamicStateList`) |
| `code/graphics/vulkan/VulkanRenderer.cpp` | Descriptor management (`updateModelDescriptors`), ring buffer usage |
| `code/graphics/vulkan/VulkanRenderer.h` | Ring buffer size constants |
| `code/graphics/vulkan/VulkanRenderingSession.cpp` | Barrier management, layout transitions |
| `code/graphics/vulkan/VulkanConstants.h` | Frame count, bindless array size |
| `code/graphics/vulkan/VulkanFrame.h` | Per-frame resources and ring buffers |
| `code/graphics/vulkan/VulkanRingBuffer.h` | Ring buffer allocator |

### Related Documentation

| Document | Topic |
|----------|-------|
| `VULKAN_ARCHITECTURE_OVERVIEW.md` | High-level renderer structure |
| `VULKAN_SYNCHRONIZATION.md` | Fences, semaphores, barriers |
| `VULKAN_DESCRIPTOR_SETS.md` | Descriptor layout and binding strategy |
| `VULKAN_PIPELINE_MANAGEMENT.md` | Pipeline key, caching details |
| `VULKAN_RENDER_PASS_STRUCTURE.md` | Target transitions, dynamic rendering |

### External Resources

| Resource | URL |
|----------|-----|
| Vulkan Best Practices | https://github.com/KhronosGroup/Vulkan-Guide |
| RenderDoc | https://renderdoc.org/ |
| AMD Radeon GPU Profiler | https://gpuopen.com/rgp/ |
| NVIDIA Nsight Graphics | https://developer.nvidia.com/nsight-graphics |
