# Dynamic Buffer Management in the Vulkan Renderer

This document provides comprehensive technical documentation for the dynamic buffer subsystem in the FreeSpace 2 Open Vulkan renderer. It covers buffer lifecycle management, allocation strategies, GPU synchronization, and integration with the rendering pipeline.

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [Buffer Types and Usage Hints](#2-buffer-types-and-usage-hints)
3. [Ring Buffer System](#3-ring-buffer-system)
4. [VulkanBufferManager: Long-Lived Buffers](#4-vulkanbuffermanager-long-lived-buffers)
5. [Buffer Orphaning and GPU Synchronization](#5-buffer-orphaning-and-gpu-synchronization)
6. [Frame Lifecycle and Resource Recycling](#6-frame-lifecycle-and-resource-recycling)
7. [Deferred Release Queue](#7-deferred-release-queue)
8. [Memory Mapping and Host-Device Coherence](#8-memory-mapping-and-host-device-coherence)
9. [Common Usage Patterns](#9-common-usage-patterns)
10. [Performance Considerations](#10-performance-considerations)

---

## 1. Architecture Overview

The Vulkan renderer employs a two-tier buffer management architecture:

| Layer | Class | Purpose | Lifetime |
|-------|-------|---------|----------|
| **Per-Frame Transient** | `VulkanRingBuffer` | Immediate-mode allocations (uniforms, vertices, staging) | Single frame |
| **Long-Lived Managed** | `VulkanBufferManager` | Persistent buffers (model geometry, static uniforms) | Application lifetime |

**Key Files:**

| File | Purpose |
|------|---------|
| `VulkanRingBuffer.h` | Ring buffer class definition |
| `VulkanRingBuffer.cpp:10-97` | Ring buffer implementation |
| `VulkanBufferManager.h` | Managed buffer interface |
| `VulkanBufferManager.cpp:1-404` | Buffer creation, update, and orphaning logic |
| `VulkanFrame.h:26-98` | Per-frame resource container |
| `VulkanFrame.cpp:13-96` | Frame initialization and reset |
| `VulkanDeferredRelease.h:56-98` | Serial-gated deferred destruction |

---

## 2. Buffer Types and Usage Hints

### 2.1 Buffer Types

Defined in `code/graphics/2d.h:654-658`:

```cpp
enum class BufferType {
    Vertex,
    Index,
    Uniform
};
```

Each type maps to specific Vulkan usage flags in `VulkanBufferManager.cpp:40-52`:

| BufferType | Vulkan Usage Flags |
|------------|-------------------|
| `Vertex` | `eVertexBuffer \| eStorageBuffer \| eTransferDst` |
| `Index` | `eIndexBuffer \| eTransferDst` |
| `Uniform` | `eUniformBuffer \| eTransferDst` |

### 2.2 Usage Hints

Defined in `code/graphics/2d.h:660`:

```cpp
enum class BufferUsageHint { Static, Dynamic, Streaming, PersistentMapping };
```

Memory placement strategy from `VulkanBufferManager.cpp:55-69`:

| UsageHint | Memory Properties | Typical Use |
|-----------|------------------|-------------|
| `Static` | `eDeviceLocal` | Model geometry, static meshes |
| `Dynamic` | `eHostVisible \| eHostCoherent` | Frequently updated uniforms |
| `Streaming` | `eHostVisible \| eHostCoherent` | Per-frame vertex data, NanoVG |
| `PersistentMapping` | `eHostVisible \| eHostCoherent` | Persistent mapped buffers |

---

## 3. Ring Buffer System

### 3.1 Ring Buffer Architecture

Each `VulkanFrame` contains three ring buffers for per-frame transient allocations (`VulkanFrame.h:45-47`):

```cpp
VulkanRingBuffer& uniformBuffer() { return m_uniformRing; }
VulkanRingBuffer& vertexBuffer() { return m_vertexRing; }
VulkanRingBuffer& stagingBuffer() { return m_stagingRing; }
```

### 3.2 Ring Buffer Sizes

Defined in `VulkanRenderer.h:191-193`:

```cpp
static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;      // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;      // 1 MB
static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024; // 12 MB
```

| Ring Buffer | Size | Purpose |
|-------------|------|---------|
| Uniform Ring | 512 KB | Per-draw uniform blocks, matrices, generic data |
| Vertex Ring | 1 MB | Immediate-mode geometry, transform buffer uploads |
| Staging Ring | 12 MB | Texture uploads, buffer-to-image copies |

### 3.3 Ring Buffer Structure

From `VulkanRingBuffer.h:12-53`:

```cpp
class VulkanRingBuffer {
public:
    struct Allocation {
        vk::DeviceSize offset{0};   // Offset into the buffer
        void* mapped{nullptr};       // CPU-accessible pointer
    };

    Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
    std::optional<Allocation> try_allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
    void reset();

    vk::Buffer buffer() const { return m_buffer.get(); }
    vk::DeviceSize remaining() const;

private:
    vk::UniqueBuffer m_buffer;
    vk::UniqueDeviceMemory m_memory;
    void* m_mapped = nullptr;        // Persistent host mapping

    vk::DeviceSize m_size = 0;       // Total capacity
    vk::DeviceSize m_alignment = 1;  // Minimum allocation alignment
    vk::DeviceSize m_offset = 0;     // Current write cursor
};
```

### 3.4 Allocation Algorithm

From `VulkanRingBuffer.cpp:48-67`:

```cpp
std::optional<VulkanRingBuffer::Allocation> VulkanRingBuffer::try_allocate(
    vk::DeviceSize requestSize,
    vk::DeviceSize alignmentOverride)
{
    // Never allow override to weaken baseline alignment
    const vk::DeviceSize align = std::max(m_alignment,
        alignmentOverride ? alignmentOverride : vk::DeviceSize{0});
    vk::DeviceSize alignedOffset = ((m_offset + align - 1) / align) * align;

    // Do not wrap within a frame - would overwrite in-flight GPU reads
    if (alignedOffset + requestSize > m_size) {
        return std::nullopt;
    }

    Allocation result;
    result.offset = alignedOffset;
    result.mapped = static_cast<uint8_t*>(m_mapped) + alignedOffset;

    m_offset = alignedOffset + requestSize;
    return result;
}
```

**Key Properties:**

1. **No Wrap-Around**: Allocations never wrap within a frame to prevent write-after-read hazards
2. **Alignment Enforcement**: The ring's baseline alignment (from device limits) is never weakened
3. **Failure Semantics**: `try_allocate()` returns `nullopt` if exhausted; `allocate()` throws

### 3.5 Per-Frame Reset

At frame start, all ring cursors reset to zero (`VulkanFrame.cpp:79-93`):

```cpp
void VulkanFrame::reset()
{
    m_device.resetCommandPool(m_commandPool.get());
    m_uniformRing.reset();   // Cursor -> 0
    m_vertexRing.reset();    // Cursor -> 0
    m_stagingRing.reset();   // Cursor -> 0
}
```

This is safe because:
1. `reset()` is only called after `wait_for_gpu()` confirms GPU completion
2. The double-buffered frame pipeline ensures no overlap between CPU writes and GPU reads

---

## 4. VulkanBufferManager: Long-Lived Buffers

### 4.1 Buffer Handle System

Buffers are identified by opaque handles (`code/graphics/2d.h:364-366`):

```cpp
struct gr_buffer_handle_tag {};
using gr_buffer_handle = ::util::ID<gr_buffer_handle_tag, int, -1>;
```

### 4.2 Buffer Record Structure

From `VulkanBufferManager.h:12-20`:

```cpp
struct VulkanBuffer {
    vk::UniqueBuffer buffer;
    vk::UniqueDeviceMemory memory;
    BufferType type;
    BufferUsageHint usage;
    vk::DeviceSize size = 0;
    void* mapped = nullptr;         // For host-visible buffers
    bool isPersistentMapped = false;
};
```

### 4.3 Buffer Creation

From `VulkanBufferManager.cpp:171-183`:

```cpp
gr_buffer_handle VulkanBufferManager::createBuffer(BufferType type, BufferUsageHint usage)
{
    VulkanBuffer buffer;
    buffer.type = type;
    buffer.usage = usage;
    buffer.size = 0;

    // Buffer is created lazily on first updateBufferData call
    m_buffers.push_back(std::move(buffer));
    return gr_buffer_handle(static_cast<int>(m_buffers.size() - 1));
}
```

**Lazy Allocation**: The Vulkan buffer is not created until the first data upload, matching OpenGL's `glBufferData` semantics.

### 4.4 Buffer Update Paths

From `VulkanBufferManager.cpp:210-243`:

```cpp
void VulkanBufferManager::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
    auto& buffer = m_buffers[handle.value()];

    // Match OpenGL semantics: Dynamic/Streaming buffers always orphan
    if (buffer.usage == BufferUsageHint::Dynamic ||
        buffer.usage == BufferUsageHint::Streaming) {
        resizeBuffer(handle, size);  // Always recreate storage
    } else {
        ensureBuffer(handle, static_cast<vk::DeviceSize>(size));  // Grow if needed
    }

    auto& updatedBuffer = m_buffers[handle.value()];

    if (data == nullptr) {
        return;  // Allocation-only path for persistent mapping
    }

    // Upload data
    if (updatedBuffer.mapped) {
        // Host-visible: direct memcpy
        memcpy(updatedBuffer.mapped, data, size);
    } else {
        // Device-local: stage and copy
        uploadToDeviceLocal(updatedBuffer, 0, static_cast<vk::DeviceSize>(size), data);
    }
}
```

---

## 5. Buffer Orphaning and GPU Synchronization

### 5.1 The Orphaning Problem

When updating a buffer that the GPU may still be reading from a previous frame, naive overwrites cause **write-after-read (WAR) hazards**. OpenGL solves this with "buffer orphaning" - `glBufferData` with the same size implicitly allocates new storage.

### 5.2 Vulkan Orphaning Implementation

From `VulkanBufferManager.cpp:318-371`:

```cpp
void VulkanBufferManager::resizeBuffer(gr_buffer_handle handle, size_t size)
{
    auto& buffer = m_buffers[handle.value()];

    // OpenGL treats "resize to same size" as orphaning hint for Dynamic/Streaming
    if (size == buffer.size) {
        if (buffer.usage != BufferUsageHint::Dynamic &&
            buffer.usage != BufferUsageHint::Streaming) {
            return;  // Static buffers: no-op for same size
        }
    }

    // Retire the old buffer for deferred deletion
    if (buffer.buffer) {
        if (buffer.mapped) {
            m_device.unmapMemory(buffer.memory.get());
            buffer.mapped = nullptr;
        }

        const uint64_t retireSerial = m_safeRetireSerial + 1;
        m_deferredReleases.enqueue(retireSerial,
            [oldBuf = std::move(buffer.buffer),
             oldMem = std::move(buffer.memory)]() mutable {});
    }

    // Create new buffer with fresh storage
    vk::BufferCreateInfo bufferInfo;
    bufferInfo.size = size;
    bufferInfo.usage = getVkUsageFlags(buffer.type);
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;

    buffer.buffer = m_device.createBufferUnique(bufferInfo);
    // ... memory allocation and binding ...

    if (getMemoryProperties(buffer.usage) & vk::MemoryPropertyFlagBits::eHostVisible) {
        buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);
    }

    buffer.size = size;
}
```

**Critical Insight**: The old buffer is moved into a deferred-release closure, ensuring it remains valid until the GPU completes all commands referencing it.

### 5.3 Static Buffer Updates via Staging

For device-local buffers, updates go through a staging buffer (`VulkanBufferManager.cpp:71-169`):

1. Create temporary staging buffer in host-visible memory
2. Map staging buffer and copy data
3. Record `vkCmdCopyBuffer` in a one-shot command buffer
4. Submit with a fence and wait synchronously
5. Insert pipeline barrier for correct synchronization
6. Destroy staging resources

```cpp
void VulkanBufferManager::uploadToDeviceLocal(const VulkanBuffer& buffer,
    vk::DeviceSize dstOffset, vk::DeviceSize size, const void* data)
{
    // Create staging buffer
    vk::BufferCreateInfo stagingInfo{};
    stagingInfo.size = size;
    stagingInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;

    auto stagingBuffer = m_device.createBufferUnique(stagingInfo);
    // ... allocate, bind, map, copy data ...

    // Record copy command
    cmd.copyBuffer(stagingBuffer.get(), buffer.buffer.get(), 1, &copy);

    // Pipeline barrier based on buffer type
    vk::BufferMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = dstStage;  // Vertex/Index/Uniform dependent
    barrier.dstAccessMask = dstAccess;
    cmd.pipelineBarrier2(depInfo);

    // Wait synchronously for transfer
    m_transferQueue.submit(submit, fence.get());
    m_device.waitForFences(1, &fenceHandle, VK_TRUE, UINT64_MAX);
}
```

---

## 6. Frame Lifecycle and Resource Recycling

### 6.1 Double-Buffered Frame Pipeline

From `VulkanConstants.h:8`:

```cpp
constexpr uint32_t kFramesInFlight = 2;
```

The renderer maintains two independent frames that rotate through the pipeline:

```
Frame 0: [Recording] -> [Submitted/GPU] -> [Available]
Frame 1: [Available] -> [Recording] -> [Submitted/GPU]
```

### 6.2 Frame State Machine

```
                    +------------------+
                    |   Available      |
                    | (ring reset,     |
                    |  ready to use)   |
                    +--------+---------+
                             |
                   acquireAvailableFrame()
                             |
                             v
                    +------------------+
                    |   Recording      |
                    | (ring allocating,|
                    |  cmd recording)  |
                    +--------+---------+
                             |
                   submitRecordedFrame()
                             |
                             v
                    +------------------+
                    |   In-Flight      |
                    | (GPU executing)  |
                    +--------+---------+
                             |
                   recycleOneInFlight()
                             |
                             v
                    +------------------+
                    |   Available      |
                    +------------------+
```

### 6.3 Frame Preparation and Recycling

From `VulkanRenderer.cpp:420-448`:

```cpp
void VulkanRenderer::prepareFrameForReuse(VulkanFrame& frame, uint64_t completedSerial)
{
    // Collect deferred releases that have completed
    m_bufferManager->collect(completedSerial);
    m_textureManager->collect(completedSerial);

    // Reset ring buffer cursors to 0
    frame.reset();
}

void VulkanRenderer::recycleOneInFlight()
{
    InFlightFrame inflight = std::move(m_inFlightFrames.front());
    m_inFlightFrames.pop_front();

    VulkanFrame& f = inflight.ref();

    // Block until GPU completes this frame's work
    f.wait_for_gpu();

    const uint64_t completed = queryCompletedSerial();
    m_completedSerial = std::max(m_completedSerial, completed);

    prepareFrameForReuse(f, m_completedSerial);
    m_availableFrames.push_back(AvailableFrame{ &f, m_completedSerial });
}
```

### 6.4 Frame Advance Flow

From `VulkanRenderer.cpp:472-484`:

```cpp
RecordingFrame VulkanRenderer::advanceFrame(RecordingFrame prev)
{
    endFrame(prev);
    auto submit = submitRecordedFrame(prev);

    m_inFlightFrames.emplace_back(prev.ref(), submit);

    logFrameCounters();
    m_frameModelDraws = 0;
    m_framePrimDraws = 0;

    return beginRecording();
}
```

---

## 7. Deferred Release Queue

### 7.1 Purpose

GPU resources cannot be destroyed while still referenced by in-flight commands. The deferred release queue delays destruction until the GPU has completed all work using the resource.

### 7.2 Implementation

From `VulkanDeferredRelease.h:56-98`:

```cpp
class DeferredReleaseQueue {
public:
    struct Entry {
        uint64_t retireSerial = 0;     // Wait until this serial is complete
        MoveOnlyFunction release;       // Destructor closure (moves ownership)
    };

    template <typename F>
    void enqueue(uint64_t retireSerial, F&& releaseFn)
    {
        Entry e;
        e.retireSerial = retireSerial;
        e.release = MoveOnlyFunction(std::forward<F>(releaseFn));
        m_entries.push_back(std::move(e));
    }

    void collect(uint64_t completedSerial)
    {
        size_t writeIdx = 0;
        for (auto& e : m_entries) {
            if (e.retireSerial <= completedSerial) {
                e.release();  // Invoke destructor closure
            } else {
                m_entries[writeIdx++] = std::move(e);  // Keep for later
            }
        }
        m_entries.resize(writeIdx);
    }
};
```

### 7.3 Serial Management

From `VulkanBufferManager.h:45-48`:

```cpp
// Set before recording; resources retired during this frame use serial+1
void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }

// Called after GPU completes work up to completedSerial
void collect(uint64_t completedSerial) { m_deferredReleases.collect(completedSerial); }
```

The retirement serial is set conservatively (`VulkanRenderer.cpp:313-314`):

```cpp
// Resources retired during this frame's recording may still be referenced
m_bufferManager->setSafeRetireSerial(m_submitSerial + 1);
```

---

## 8. Memory Mapping and Host-Device Coherence

### 8.1 Persistent Mapping

Ring buffers and host-visible managed buffers use persistent mapping:

From `VulkanRingBuffer.cpp:35`:
```cpp
m_mapped = m_device.mapMemory(m_memory.get(), 0, size);
```

From `VulkanBufferManager.cpp:366-368`:
```cpp
if (getMemoryProperties(buffer.usage) & vk::MemoryPropertyFlagBits::eHostVisible) {
    buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);
}
```

### 8.2 Host-Coherent Memory

All host-visible buffers use `eHostCoherent` memory (`VulkanBufferManager.cpp:61-64`):

```cpp
case BufferUsageHint::Dynamic:
case BufferUsageHint::Streaming:
case BufferUsageHint::PersistentMapping:
    return vk::MemoryPropertyFlagBits::eHostVisible |
           vk::MemoryPropertyFlagBits::eHostCoherent;
```

**Implications:**

1. **No Explicit Flush**: CPU writes are automatically visible to the GPU
2. **Simplified API**: `flushMappedBuffer()` is a no-op (`VulkanBufferManager.cpp:286-300`)
3. **Portability**: Works correctly on all Vulkan implementations

### 8.3 Zero-Copy Data Path

For host-visible buffers, data flows directly from CPU to GPU:

```
CPU: memcpy(buffer.mapped, data, size)
     |
     | (automatic coherence - no barrier needed)
     v
GPU: vkCmdBindVertexBuffers / vkCmdBindDescriptorSets
     |
     v
GPU: shader reads
```

---

## 9. Common Usage Patterns

### 9.1 Per-Draw Uniform Allocation

From `VulkanGraphics.cpp:1250-1264`:

```cpp
// Allocate from per-frame uniform ring
auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, uboAlignment);
auto* uniformBase = static_cast<char*>(uniformAlloc.mapped);

// Write uniform data directly
std::memcpy(uniformBase, &matrices, sizeof(matrices));
std::memcpy(uniformBase + genericOffset, genericDataPtr, genericDataSize);

// Build descriptor with ring buffer offset
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;
matrixInfo.range = sizeof(matrices);
```

### 9.2 Transform Buffer Upload

From `VulkanGraphics.cpp:400-450`:

```cpp
void gr_vulkan_update_transform_buffer(void* data, size_t size)
{
    VulkanFrame& frame = ctxBase.frame();

    // Allocate from vertex ring (used as storage buffer)
    auto& ring = frame.vertexBuffer();
    const auto minAlign = props.limits.minStorageBufferOffsetAlignment;

    auto allocOpt = ring.try_allocate(static_cast<vk::DeviceSize>(size), alignment);
    Assertion(allocOpt.has_value(),
        "Transform buffer upload of %zu bytes exceeds per-frame vertex ring. "
        "Increase VERTEX_RING_SIZE or reduce batched transforms.",
        size, ring.remaining());

    const auto alloc = *allocOpt;
    std::memcpy(alloc.mapped, data, size);

    // Track for model descriptor binding
    frame.modelTransformDynamicOffset = static_cast<uint32_t>(alloc.offset);
    frame.modelTransformSize = size;
}
```

### 9.3 Texture Staging Upload

From `VulkanTextureManager.cpp:775-883`:

```cpp
// Try to allocate staging space for this mip level
auto allocOpt = frame.stagingBuffer().try_allocate(layerSize);
if (!allocOpt) {
    // Staging buffer exhausted - defer to next frame
    bm_unlock(frameHandle);
    remaining.push_back(frameHandle);
    continue;
}

// Copy texture data to staging
std::memcpy(allocOpt->mapped, pixelData, layerSize);

// Record buffer-to-image copy
cmd.copyBufferToImage(frame.stagingBuffer().buffer(),
    record.gpu.image.get(),
    vk::ImageLayout::eTransferDstOptimal,
    regions);
```

### 9.4 NanoVG Rendering

From `VulkanGraphics.cpp:1605-1619`:

```cpp
// Allocate uniform block for HUD rendering
auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, uboAlignment);
std::memcpy(uniformAlloc.mapped, &matrices, sizeof(matrices));
std::memcpy(uniformAlloc.mapped + genericOffset, &generic, sizeof(generic));

// Bind via push descriptors
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;
matrixInfo.range = sizeof(matrices);
```

---

## 10. Performance Considerations

### 10.1 Ring Buffer Sizing

Current sizes are tuned for typical FreeSpace 2 workloads:

| Ring | Size | Typical Usage | Headroom |
|------|------|---------------|----------|
| Uniform | 512 KB | ~100-200 KB per frame | 2-5x |
| Vertex | 1 MB | ~200-400 KB (transforms, immediate geometry) | 2-5x |
| Staging | 12 MB | ~2-8 MB (texture streaming) | 1.5-6x |

### 10.2 Allocation Failure Handling

Ring buffer exhaustion triggers assertions with diagnostic information (`VulkanGraphics.cpp:432-435`):

```cpp
Assertion(allocOpt.has_value(),
    "Transform buffer upload of %zu bytes exceeds per-frame vertex ring "
    "remaining %zu bytes. Increase VERTEX_RING_SIZE or reduce batched transforms.",
    size, static_cast<size_t>(ring.remaining()));
```

### 10.3 Memory Coherence Trade-offs

Using `HOST_COHERENT` memory:

| Advantage | Disadvantage |
|-----------|--------------|
| No explicit flush calls | Slightly slower writes on some hardware |
| Simpler code path | No control over cache behavior |
| Correct on all implementations | May use system RAM instead of BAR memory |

### 10.4 Double-Buffering Benefits

With `kFramesInFlight = 2`:

1. **No CPU/GPU Stalls**: CPU can record frame N+1 while GPU executes frame N
2. **Safe Ring Reuse**: Each frame has exclusive access to its ring buffers
3. **Deferred Release Safety**: Resources are guaranteed unused after one frame boundary

### 10.5 Orphaning vs. Suballocation

| Strategy | Use Case | Overhead |
|----------|----------|----------|
| Orphaning (`resizeBuffer`) | Dynamic/Streaming buffers | New allocation per update |
| Suballocation (ring buffer) | Per-frame transient data | Zero allocations |
| Grow-only (`ensureBuffer`) | Static buffers | Occasional reallocation |

---

## Appendix A: Key Constants Reference

From various source files:

```cpp
// VulkanConstants.h:8
constexpr uint32_t kFramesInFlight = 2;

// VulkanRenderer.h:191-193
static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;      // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;      // 1 MB
static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024; // 12 MB
```

## Appendix B: File Reference

| File | Lines | Key Contents |
|------|-------|--------------|
| `VulkanRingBuffer.h` | 1-57 | Ring buffer class, Allocation struct |
| `VulkanRingBuffer.cpp` | 1-97 | Constructor, allocate, try_allocate, reset, remaining |
| `VulkanBufferManager.h` | 1-69 | VulkanBuffer struct, manager interface |
| `VulkanBufferManager.cpp` | 1-404 | Buffer creation, update, resize, orphaning, staging uploads |
| `VulkanFrame.h` | 1-102 | Per-frame resources, ring buffer accessors |
| `VulkanFrame.cpp` | 1-97 | Frame initialization, wait_for_gpu, reset |
| `VulkanDeferredRelease.h` | 1-102 | MoveOnlyFunction, DeferredReleaseQueue |
| `VulkanRenderer.cpp` | 420-484 | prepareFrameForReuse, recycleOneInFlight, advanceFrame |
| `VulkanGraphics.cpp` | 400-450 | gr_vulkan_update_transform_buffer |
| `VulkanConstants.h` | 1-24 | kFramesInFlight, bindless texture slots |
| `code/graphics/2d.h` | 654-660 | BufferType, BufferUsageHint enums |

---

## Appendix C: Synchronization Guarantees

### Frame-Level Guarantees

1. **Ring Buffer Isolation**: Each frame's ring buffers are exclusively owned during recording
2. **Fence Wait Before Reuse**: `wait_for_gpu()` blocks until the frame's commands complete
3. **Deferred Release Collection**: Old resources are destroyed only after GPU completion

### Buffer Update Guarantees

1. **Static Buffers**: Staging copy with explicit barrier ensures visibility
2. **Dynamic/Streaming Buffers**: Always orphan, old buffer deferred until safe
3. **Ring Allocations**: No wrap-around prevents overwriting in-flight data

### Memory Visibility Guarantees

1. **Host Coherent**: CPU writes immediately visible to GPU (no flush needed)
2. **Device Local**: Pipeline barriers in staging copy ensure correct ordering
