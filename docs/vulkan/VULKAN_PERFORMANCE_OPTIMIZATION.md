# Vulkan Performance Optimization Guide

This document provides performance optimization strategies, profiling techniques, and best practices for the Vulkan renderer.

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

---

## 1. Overview

Performance optimization in the Vulkan renderer focuses on:

- **Pipeline Caching**: Minimize pipeline creation overhead
- **Descriptor Batching**: Reduce descriptor update costs
- **Memory Efficiency**: Optimize buffer allocations and layouts
- **Command Recording**: Minimize CPU overhead
- **Synchronization**: Reduce GPU stalls

**Key Principle**: Measure first, optimize second. Use profiling to identify bottlenecks.

**Tools**:
- RenderDoc: Frame capture and analysis
- Vulkan validation layers: Performance warnings
- GPU profilers: AMD RGP, Nsight Graphics, Intel GPA

---

## 2. Pipeline Management

### 2.1 Pipeline Cache

**Current Implementation**: Pipeline cache persisted to disk (`VulkanDevice.cpp`)

**Cache Key**: Pipeline key hash (`PipelineKeyHasher` functor)

**Optimization**: Pipeline cache reduces pipeline creation time from ~10ms to <1ms for cached pipelines.

**Best Practices**:
- Load pipeline cache at startup
- Save pipeline cache at shutdown
- Invalidate cache on driver updates

### 2.2 Pipeline Key Minimization

**Current State**: Pipeline keys include all state (blend mode, formats, etc.)

**Optimization**: Use dynamic state features to reduce pipeline variants:
- Core Vulkan 1.3: Cull mode, depth test, primitive topology (always available)
- EDS3 (`VK_EXT_extended_dynamic_state3`): Blend enable, color write mask, polygon mode

**Impact**: Reduces pipeline count from ~1000s to ~100s

**Current Usage**:
- Core dynamic state: `VulkanPipelineManager.cpp:261-272`
- EDS3 blend enable: `VulkanDeferredLights.cpp:71-74`, `VulkanRenderingSession.cpp:1419-1426`

### 2.3 Pipeline Binding Frequency

**Pattern**: Bind pipeline once per draw batch

**Anti-Pattern**: Rebinding pipeline for every draw

**Example**:
```cpp
// GOOD: Bind once, draw many
cmd.bindPipeline(pipeline);
for (const auto& model : models) {
    cmd.drawIndexed(...);
}

// BAD: Rebind for every draw
for (const auto& model : models) {
    cmd.bindPipeline(model.pipeline);  // Expensive!
    cmd.drawIndexed(...);
}
```

---

## 3. Descriptor Optimization

### 3.1 Bindless Textures

**Current Implementation**: 1024-slot bindless texture array

**Benefit**: Eliminates per-draw descriptor updates

**Cost**: Full array update on first frame (~1000 descriptors)

**Optimization**: Incremental updates (`VulkanRenderer.cpp:2973-3093`):
- Track previous frame's descriptor contents
- Detect changed ranges
- Update only changed slots

**Impact**: Reduces descriptor updates from 1024 to ~10-50 per frame

### 3.2 Push Descriptors

**Current Usage**: Per-draw textures (post-processing, deferred lighting)

**Benefit**: No descriptor set allocation, low overhead

**Cost**: Descriptor write per draw (acceptable for infrequent draws)

**Best Practice**: Use push descriptors for per-draw data, bindless for per-model textures

### 3.3 Descriptor Set Binding

**Pattern**: Bind descriptor set once per frame, use dynamic offsets

**Current Implementation**: Model descriptor set bound once, dynamic offsets per draw

**Optimization**: Batch draws with same descriptor set to minimize binding overhead

---

## 4. Memory Management

### 4.1 Ring Buffer Usage

**Staging Ring Buffer**: 12 MB per frame (`VulkanRenderer.h:199`)

**Optimization**: Size based on texture upload requirements
- Too small: Uploads deferred, stuttering
- Too large: Wasted memory

**Current Sizing**: 12 MB sufficient for typical frame (verify with profiling)

### 4.2 Uniform Buffer Alignment

**Device Alignment**: Typically 256 bytes (`minUniformBufferOffsetAlignment`)

**Optimization**: Pack multiple uniforms into single aligned allocation

**Current Implementation**: Per-model uniform allocation (`VulkanRenderer.cpp:2907-2950`)

**Future Optimization**: Batch uniforms to reduce allocations

### 4.3 Vertex Heap

**Type**: Large SSBO (vertex pulling)

**Optimization**: 
- Use device-local memory (fast GPU access)
- Minimize CPU-GPU synchronization
- Batch vertex uploads

**Current Implementation**: Managed by `GPUMemoryHeap`, uploaded on allocation/resize

---

## 5. Command Buffer Recording

### 5.1 Recording Overhead

**Cost**: Command buffer recording is CPU-bound

**Optimization**: Minimize command count
- Batch draws
- Use instancing (if applicable)
- Reduce state changes

**Measurement**: Profile command buffer recording time (should be <1ms per frame)

### 5.2 Secondary Command Buffers

**Current Status**: Not used (all recording in primary command buffer)

**Potential Optimization**: Use secondary command buffers for:
- Static geometry (record once, reuse)
- UI rendering (record per frame, but isolated)

**Trade-off**: Secondary buffers add overhead, only beneficial if reused

### 5.3 Command Buffer Reset

**Pattern**: Reset command pool per frame (`VulkanFrame.cpp`)

**Optimization**: Reset individual command buffers (if supported) to reduce overhead

**Current Implementation**: Reset entire pool (acceptable for small pool size)

---

## 6. Synchronization Optimization

### 6.1 Frame-In-Flight

**Current Configuration**: 2 frames in flight (`VulkanConstants.h:8`)

**Optimization**: Increase to 3 frames if GPU is bottleneck
- More GPU parallelism
- Higher latency
- More memory usage

**Measurement**: GPU utilization should be ~90-100% with 2 frames

### 6.2 Pipeline Barriers

**Cost**: Pipeline barriers cause GPU stalls

**Optimization**: Minimize barriers, batch barriers

**Current Implementation**: Barriers for layout transitions (`VulkanRenderingSession.cpp`)

**Best Practice**: 
- Batch multiple barriers into single `pipelineBarrier2()` call
- Use `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT` sparingly

### 6.3 Semaphore Usage

**Current Implementation**: Binary semaphores for swapchain, timeline for submissions

**Optimization**: Timeline semaphores reduce semaphore count (already implemented)

**Cost**: Binary semaphores are simpler, timeline semaphores more flexible

---

## 7. Profiling Techniques

### 7.1 RenderDoc Profiling

**Setup**: Capture frame in RenderDoc, analyze GPU timeline

**Metrics**:
- GPU time per pass
- Draw call count
- Pipeline switches
- Descriptor updates

**Optimization Targets**:
- Reduce GPU time per pass
- Minimize pipeline switches
- Batch draw calls

### 7.2 CPU Profiling

**Tools**: Visual Studio Profiler, Intel VTune, perf

**Metrics**:
- Command buffer recording time
- Descriptor update time
- Pipeline creation time
- Memory allocation time

**Optimization Targets**:
- Command recording <1ms
- Descriptor updates <0.5ms
- Pipeline creation <0.1ms (cached)

### 7.3 GPU Profiling

**Tools**: AMD RGP, Nsight Graphics, Intel GPA

**Metrics**:
- GPU utilization
- Memory bandwidth
- Shader execution time
- Texture cache hit rate

**Optimization Targets**:
- GPU utilization >90%
- Memory bandwidth <80% of peak
- Shader execution balanced across stages

---

## 8. Common Bottlenecks

### Bottleneck 1: Pipeline Creation

**Symptoms**: Frame drops on first render of new pipeline

**Cause**: Pipeline creation is expensive (~10ms uncached)

**Fix**: 
- Use pipeline cache (already implemented)
- Pre-warm common pipelines
- Use EDS3 to reduce variants

### Bottleneck 2: Descriptor Updates

**Symptoms**: High CPU time in descriptor update paths

**Cause**: Updating 1024 bindless descriptors every frame

**Fix**: 
- Incremental updates (already implemented)
- Reduce texture count
- Use texture atlasing

### Bottleneck 3: Command Buffer Recording

**Symptoms**: High CPU time in draw call recording

**Cause**: Too many draw calls, excessive state changes

**Fix**:
- Batch draws
- Reduce state changes
- Use instancing (if applicable)

### Bottleneck 4: GPU Synchronization

**Symptoms**: Low GPU utilization, frame pacing issues

**Cause**: Excessive pipeline barriers, CPU-GPU synchronization

**Fix**:
- Minimize barriers
- Batch barriers
- Increase frames in flight (if memory allows)

### Bottleneck 5: Memory Bandwidth

**Symptoms**: High memory bandwidth usage, texture upload stalls

**Cause**: Large texture uploads, inefficient layouts

**Fix**:
- Compress textures (BC1/BC3/BC7)
- Use texture streaming
- Reduce texture sizes

---

## Appendix: Performance Checklist

### Frame Recording
- [ ] Pipeline bound once per batch
- [ ] Descriptor set bound once per frame (with dynamic offsets)
- [ ] Push constants updated only when changed
- [ ] Draw calls batched (minimize state changes)
- [ ] Command buffer recording <1ms

### Descriptors
- [ ] Bindless textures use incremental updates
- [ ] Push descriptors used for per-draw data
- [ ] Descriptor updates <0.5ms per frame

### Memory
- [ ] Staging buffer sized appropriately (12 MB)
- [ ] Uniform buffers aligned to device requirements
- [ ] Vertex heap uses device-local memory

### Synchronization
- [ ] Pipeline barriers batched
- [ ] Semaphores minimized (timeline where possible)
- [ ] Frame-in-flight count appropriate (2-3)

### Pipelines
- [ ] Pipeline cache persisted
- [ ] EDS3 used to reduce variants
- [ ] Pipeline creation <0.1ms (cached)

---

## References

- `code/graphics/vulkan/VulkanDevice.cpp:874-949` - Pipeline cache persistence
- `code/graphics/vulkan/VulkanPipelineManager.cpp` - Pipeline creation and caching
- `code/graphics/vulkan/VulkanRenderer.cpp:2973-3093` - Descriptor optimization
- `code/graphics/vulkan/VulkanRenderingSession.cpp` - Barrier management
- `code/graphics/vulkan/VulkanConstants.h` - Frame-in-flight and bindless array sizes
- Vulkan Best Practices: https://github.com/KhronosGroup/Vulkan-Guide
- RenderDoc: https://renderdoc.org/

