# Dynamic Buffer Management in the Vulkan Renderer

This document provides comprehensive technical documentation for the dynamic buffer subsystem in the FreeSpace 2 Open Vulkan renderer. It covers buffer lifecycle management, allocation strategies, GPU synchronization, and integration with the rendering pipeline.

## Table of Contents

1. [Quick Start](#1-quick-start)
2. [Architecture Overview](#2-architecture-overview)
3. [Buffer Types and Usage Hints](#3-buffer-types-and-usage-hints)
4. [Ring Buffer System](#4-ring-buffer-system)
5. [VulkanBufferManager: Long-Lived Buffers](#5-vulkanbuffermanager-long-lived-buffers)
6. [Buffer Orphaning and GPU Synchronization](#6-buffer-orphaning-and-gpu-synchronization)
7. [Frame Lifecycle and Resource Recycling](#7-frame-lifecycle-and-resource-recycling)
8. [Deferred Release Queue](#8-deferred-release-queue)
9. [Memory Mapping and Host-Device Coherence](#9-memory-mapping-and-host-device-coherence)
10. [Common Usage Patterns](#10-common-usage-patterns)
11. [Thread Safety](#11-thread-safety)
12. [Performance Considerations](#12-performance-considerations)
13. [Debugging and Troubleshooting](#13-debugging-and-troubleshooting)
14. [Appendices](#appendices)

---

## 1. Quick Start

### Choosing the Right Buffer Strategy

| Scenario | Use This | Lifetime |
|----------|----------|----------|
| Per-draw uniforms, immediate geometry | Ring buffer (`frame.uniformBuffer()`) | Single frame |
| Model geometry, static meshes | `VulkanBufferManager` with `Static` hint | Application |
| Frequently updated uniforms | `VulkanBufferManager` with `Dynamic` hint | Application |
| Per-frame vertex data | Ring buffer (`frame.vertexBuffer()`) | Single frame |
| Texture upload staging | Ring buffer (`frame.stagingBuffer()`) | Single frame |

### Minimal Example: Per-Draw Uniform Allocation

```cpp
// Allocate from the current frame's uniform ring
VulkanFrame& frame = ctx.frame();
auto alloc = frame.uniformBuffer().allocate(sizeof(MatrixUniforms), uboAlignment);

// Write data directly (host-coherent, no flush needed)
std::memcpy(alloc.mapped, &matrices, sizeof(matrices));

// Bind via descriptor with the returned offset
vk::DescriptorBufferInfo info{};
info.buffer = frame.uniformBuffer().buffer();
info.offset = alloc.offset;
info.range = sizeof(matrices);
```

### Minimal Example: Managed Buffer Creation

```cpp
// Create a persistent vertex buffer
gr_buffer_handle handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);

// Upload data (creates the underlying Vulkan buffer on first call)
gr_update_buffer_data(handle, dataSize, dataPtr);

// Use the buffer for rendering...

// Cleanup when done
gr_delete_buffer(handle);
```

---

## 2. Architecture Overview

The Vulkan renderer employs a two-tier buffer management architecture:

| Layer | Class | Purpose | Lifetime |
|-------|-------|---------|----------|
| **Per-Frame Transient** | `VulkanRingBuffer` | Immediate-mode allocations (uniforms, vertices, staging) | Single frame |
| **Long-Lived Managed** | `VulkanBufferManager` | Persistent buffers (model geometry, static uniforms) | Application lifetime |

### Data Flow Diagram

```
                         +---------------------+
                         |   Application Code  |
                         +----------+----------+
                                    |
              +---------------------+---------------------+
              |                                           |
              v                                           v
  +------------------------+               +---------------------------+
  |  Per-Frame Ring Buffer |               |  VulkanBufferManager      |
  |  (VulkanRingBuffer)    |               |  (Long-Lived Buffers)     |
  +------------------------+               +---------------------------+
  | - Uniform Ring (512KB) |               | - Static (device-local)   |
  | - Vertex Ring (1MB)    |               | - Dynamic (host-visible)  |
  | - Staging Ring (12MB)  |               | - Streaming (host-visible)|
  +------------------------+               +---------------------------+
              |                                           |
              v                                           v
  +------------------------+               +---------------------------+
  | Host-Visible Memory    |               | Device-Local or           |
  | (coherent, mapped)     |               | Host-Visible Memory       |
  +------------------------+               +---------------------------+
              |                                           |
              +---------------------+---------------------+
                                    |
                                    v
                         +---------------------+
                         |   GPU Execution     |
                         +---------------------+
```

### Key Source Files

| File | Purpose |
|------|---------|
| `VulkanRingBuffer.h` | Ring buffer class definition |
| `VulkanRingBuffer.cpp` | Ring buffer implementation (allocation, reset) |
| `VulkanBufferManager.h` | Managed buffer interface and `VulkanBuffer` struct |
| `VulkanBufferManager.cpp` | Buffer creation, update, orphaning, staging uploads |
| `VulkanFrame.h` | Per-frame resource container (ring buffers, sync primitives) |
| `VulkanFrame.cpp` | Frame initialization, `wait_for_gpu()`, `reset()` |
| `VulkanDeferredRelease.h` | Serial-gated deferred destruction queue |
| `VulkanConstants.h` | `kFramesInFlight` and bindless texture constants |

---

## 3. Buffer Types and Usage Hints

### 3.1 Buffer Types

Defined in `code/graphics/2d.h`:

```cpp
enum class BufferType {
    Vertex,
    Index,
    Uniform
};
```

Each type maps to specific Vulkan usage flags:

| BufferType | Vulkan Usage Flags | Purpose |
|------------|-------------------|---------|
| `Vertex` | `eVertexBuffer \| eStorageBuffer \| eTransferDst` | Vertex data, also usable as storage buffer for transforms |
| `Index` | `eIndexBuffer \| eTransferDst` | Index data for indexed draw calls |
| `Uniform` | `eUniformBuffer \| eTransferDst` | Uniform buffer objects |

### 3.2 Usage Hints

Defined in `code/graphics/2d.h`:

```cpp
enum class BufferUsageHint { Static, Dynamic, Streaming, PersistentMapping };
```

Memory placement strategy:

| UsageHint | Memory Properties | Behavior | Typical Use |
|-----------|------------------|----------|-------------|
| `Static` | `eDeviceLocal` | Uploaded via staging buffer; grow-only | Model geometry, static meshes |
| `Dynamic` | `eHostVisible \| eHostCoherent` | Orphaned on every update | Frequently updated uniforms |
| `Streaming` | `eHostVisible \| eHostCoherent` | Orphaned on every update | Per-frame vertex data, NanoVG |
| `PersistentMapping` | `eHostVisible \| eHostCoherent` | Persistently mapped; caller writes directly | Persistent mapped buffers |

### 3.3 Usage Hint Selection Guidance

```
Need maximum GPU performance?
  |
  +--YES--> Is data updated rarely or never?
  |           |
  |           +--YES--> Use Static
  |           |
  |           +--NO---> How often is it updated?
  |                       |
  |                       +--Every frame --> Use Streaming or ring buffer
  |                       |
  |                       +--Occasionally --> Use Dynamic
  |
  +--NO---> Need persistent CPU access?
              |
              +--YES--> Use PersistentMapping
              |
              +--NO---> Use Dynamic or Streaming
```

---

## 4. Ring Buffer System

### 4.1 Ring Buffer Architecture

Each `VulkanFrame` contains three ring buffers for per-frame transient allocations:

```cpp
VulkanRingBuffer& uniformBuffer() { return m_uniformRing; }
VulkanRingBuffer& vertexBuffer()  { return m_vertexRing; }
VulkanRingBuffer& stagingBuffer() { return m_stagingRing; }
```

### 4.2 Ring Buffer Sizes

Defined in `VulkanRenderer.h`:

```cpp
static constexpr vk::DeviceSize UNIFORM_RING_SIZE  = 512 * 1024;       // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE   = 1024 * 1024;      // 1 MB
static constexpr vk::DeviceSize STAGING_RING_SIZE  = 12 * 1024 * 1024; // 12 MB
```

| Ring Buffer | Size | Purpose | Typical Usage |
|-------------|------|---------|---------------|
| Uniform Ring | 512 KB | Per-draw uniform blocks, matrices, generic data | 100-200 KB/frame |
| Vertex Ring | 1 MB | Immediate-mode geometry, transform buffer uploads | 200-400 KB/frame |
| Staging Ring | 12 MB | Texture uploads, buffer-to-image copies | 2-8 MB/frame |

### 4.3 Ring Buffer Class Structure

From `VulkanRingBuffer.h`:

```cpp
class VulkanRingBuffer {
public:
    struct Allocation {
        vk::DeviceSize offset{0};   // Offset into the underlying buffer
        void* mapped{nullptr};       // CPU-accessible pointer for writing
    };

    // Allocate from the ring; throws std::runtime_error if exhausted
    Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);

    // Try to allocate; returns nullopt if insufficient space
    std::optional<Allocation> try_allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);

    // Reset cursor to beginning (call only after GPU completion)
    void reset();

    // Accessors
    vk::Buffer buffer() const { return m_buffer.get(); }
    vk::DeviceSize size() const { return m_size; }
    vk::DeviceSize remaining() const;

private:
    vk::Device m_device;
    vk::UniqueBuffer m_buffer;
    vk::UniqueDeviceMemory m_memory;
    void* m_mapped = nullptr;        // Persistent host mapping

    vk::DeviceSize m_size = 0;       // Total capacity
    vk::DeviceSize m_alignment = 1;  // Minimum allocation alignment (from device limits)
    vk::DeviceSize m_offset = 0;     // Current write cursor
};
```

### 4.4 Allocation Algorithm

From `VulkanRingBuffer.cpp`:

```cpp
std::optional<VulkanRingBuffer::Allocation> VulkanRingBuffer::try_allocate(
    vk::DeviceSize requestSize,
    vk::DeviceSize alignmentOverride)
{
    // Never allow an override to weaken the ring's baseline alignment
    const vk::DeviceSize align = std::max(m_alignment,
        alignmentOverride ? alignmentOverride : vk::DeviceSize{0});
    vk::DeviceSize alignedOffset = ((m_offset + align - 1) / align) * align;

    // Do not wrap within a frame - this would overwrite in-flight GPU reads
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

1. **No Wrap-Around**: Allocations never wrap within a frame to prevent write-after-read hazards. If the ring is exhausted, allocation fails rather than corrupting in-flight data.

2. **Alignment Enforcement**: The ring's baseline alignment (derived from device limits like `minUniformBufferOffsetAlignment`) is never weakened by caller-provided overrides. Callers can only request stricter alignment.

3. **Failure Semantics**: `try_allocate()` returns `std::nullopt` if exhausted; `allocate()` throws `std::runtime_error`.

4. **Zero-Allocation Fast Path**: The ring buffer performs no Vulkan allocations during frame rendering. All memory is pre-allocated at frame construction.

### 4.5 Per-Frame Reset

At frame start, all ring cursors reset to zero:

```cpp
void VulkanFrame::reset()
{
    m_device.resetCommandPool(m_commandPool.get());
    m_uniformRing.reset();   // Cursor -> 0
    m_vertexRing.reset();    // Cursor -> 0
    m_stagingRing.reset();   // Cursor -> 0
}
```

**Why This Is Safe:**

1. `reset()` is only called after `wait_for_gpu()` confirms the frame's GPU work has completed
2. The double-buffered frame pipeline (`kFramesInFlight = 2`) ensures no overlap between CPU writes and GPU reads
3. Each frame has exclusive ownership of its ring buffers during recording

---

## 5. VulkanBufferManager: Long-Lived Buffers

### 5.1 Buffer Handle System

Buffers are identified by opaque, type-safe handles (`code/graphics/2d.h`):

```cpp
struct gr_buffer_handle_tag {};
using gr_buffer_handle = ::util::ID<gr_buffer_handle_tag, int, -1>;
```

The handle is an index into `VulkanBufferManager::m_buffers`. The invalid handle value is `-1`.

### 5.2 Buffer Record Structure

From `VulkanBufferManager.h`:

```cpp
struct VulkanBuffer {
    vk::UniqueBuffer buffer;          // Vulkan buffer handle (RAII)
    vk::UniqueDeviceMemory memory;    // Bound memory (RAII)
    BufferType type;                  // Vertex, Index, or Uniform
    BufferUsageHint usage;            // Static, Dynamic, Streaming, PersistentMapping
    vk::DeviceSize size = 0;          // Current allocation size (0 = not yet allocated)
    void* mapped = nullptr;           // Mapped pointer (for host-visible buffers)
    bool isPersistentMapped = false;  // Whether buffer uses persistent mapping
};
```

### 5.3 Buffer Creation

From `VulkanBufferManager.cpp`:

```cpp
gr_buffer_handle VulkanBufferManager::createBuffer(BufferType type, BufferUsageHint usage)
{
    VulkanBuffer buffer;
    buffer.type = type;
    buffer.usage = usage;
    buffer.size = 0;  // Not yet allocated

    // Buffer will be created/resized on first updateBufferData call
    m_buffers.push_back(std::move(buffer));
    return gr_buffer_handle(static_cast<int>(m_buffers.size() - 1));
}
```

**Lazy Allocation**: The underlying Vulkan buffer is not created until the first data upload via `updateBufferData()`. This matches OpenGL's `glBufferData` semantics where you can create a buffer object without immediately allocating storage.

### 5.4 Buffer Update Paths

From `VulkanBufferManager.cpp`:

```cpp
void VulkanBufferManager::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
    auto& buffer = m_buffers[handle.value()];

    // Match OpenGL semantics: Dynamic/Streaming buffers always orphan
    if (buffer.usage == BufferUsageHint::Dynamic ||
        buffer.usage == BufferUsageHint::Streaming) {
        resizeBuffer(handle, size);  // Always recreate storage (orphaning)
    } else {
        ensureBuffer(handle, static_cast<vk::DeviceSize>(size));  // Grow if needed
    }

    auto& updatedBuffer = m_buffers[handle.value()];

    if (data == nullptr) {
        return;  // Allocation-only path for persistent mapping
    }

    // Upload data
    if (updatedBuffer.mapped) {
        // Host-visible: direct memcpy (coherent, no flush needed)
        memcpy(updatedBuffer.mapped, data, size);
    } else {
        // Device-local: stage and copy via transfer queue
        uploadToDeviceLocal(updatedBuffer, 0, static_cast<vk::DeviceSize>(size), data);
    }
}
```

### 5.5 Update Path Decision Tree

```
updateBufferData(handle, size, data)
         |
         v
    Is usage Dynamic or Streaming?
         |
    +----+----+
    |         |
   YES        NO
    |         |
    v         v
  resizeBuffer()   ensureBuffer()
  (always orphan)  (grow only if needed)
         |         |
         +---------+
                |
                v
          Is data null?
                |
           +----+----+
           |         |
          YES        NO
           |         |
           v         v
         return   Is buffer mapped?
         (alloc      |
          only)  +---+---+
                 |       |
                YES      NO
                 |       |
                 v       v
              memcpy   uploadToDeviceLocal()
              (fast)   (staging + transfer)
```

---

## 6. Buffer Orphaning and GPU Synchronization

### 6.1 The Orphaning Problem

When updating a buffer that the GPU may still be reading from a previous frame, naive overwrites cause **write-after-read (WAR) hazards**:

```
Frame N:   CPU writes to Buffer A
           GPU reads from Buffer A (in-flight)

Frame N+1: CPU overwrites Buffer A  <-- HAZARD! GPU may still be reading
```

OpenGL solves this with "buffer orphaning": calling `glBufferData` with the same size implicitly allocates new storage, leaving the old storage valid until the GPU finishes.

### 6.2 Vulkan Orphaning Implementation

From `VulkanBufferManager.cpp`:

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
        // Dynamic/Streaming: proceed with orphaning even for same size
    }

    // Retire the old buffer for deferred deletion
    if (buffer.buffer) {
        if (buffer.mapped) {
            m_device.unmapMemory(buffer.memory.get());
            buffer.mapped = nullptr;
        }

        // Defer destruction until GPU completes
        const uint64_t retireSerial = m_safeRetireSerial + 1;
        m_deferredReleases.enqueue(retireSerial,
            [oldBuf = std::move(buffer.buffer),
             oldMem = std::move(buffer.memory)]() mutable {
                // Destructor runs when lambda is destroyed
            });
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

**Critical Insight**: The old buffer is moved into a deferred-release closure. The buffer and memory remain valid until the closure executes, which only happens after `collect()` is called with a serial indicating GPU completion.

### 6.3 Static Buffer Updates via Staging

For device-local buffers, updates go through a staging buffer:

```cpp
void VulkanBufferManager::uploadToDeviceLocal(const VulkanBuffer& buffer,
    vk::DeviceSize dstOffset, vk::DeviceSize size, const void* data)
{
    // 1. Create temporary staging buffer in host-visible memory
    vk::BufferCreateInfo stagingInfo{};
    stagingInfo.size = size;
    stagingInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
    auto stagingBuffer = m_device.createBufferUnique(stagingInfo);
    // ... allocate host-visible memory, bind, map ...

    // 2. Copy data to staging buffer
    std::memcpy(mapped, data, static_cast<size_t>(size));
    m_device.unmapMemory(stagingMemory.get());

    // 3. Record copy command
    cmd.copyBuffer(stagingBuffer.get(), buffer.buffer.get(), 1, &copy);

    // 4. Insert pipeline barrier for correct synchronization
    vk::BufferMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = dstStage;   // Depends on buffer type
    barrier.dstAccessMask = dstAccess; // Depends on buffer type
    cmd.pipelineBarrier2(depInfo);

    // 5. Submit with fence and wait synchronously
    m_transferQueue.submit(submit, fence.get());
    m_device.waitForFences(1, &fenceHandle, VK_TRUE, UINT64_MAX);

    // 6. Staging resources destroyed automatically (RAII)
}
```

**Pipeline Barrier Destination Stages by Buffer Type:**

| BufferType | Destination Stage | Destination Access |
|------------|------------------|-------------------|
| `Vertex` | `eVertexInput \| eVertexShader` | `eVertexAttributeRead \| eShaderRead` |
| `Index` | `eVertexInput` | `eIndexRead` |
| `Uniform` | `eVertexShader \| eFragmentShader` | `eUniformRead` |

---

## 7. Frame Lifecycle and Resource Recycling

### 7.1 Double-Buffered Frame Pipeline

From `VulkanConstants.h`:

```cpp
constexpr uint32_t kFramesInFlight = 2;
```

The renderer maintains two independent frames that rotate through the pipeline:

```
Time --->

Frame 0: [Recording] -----> [GPU Executing] -----> [Available]
                                    |
Frame 1:              [Recording] --+--> [GPU Executing] -----> [Available]
                                                    |
Frame 0:                            [Recording] ----+--> [GPU Executing] -->
```

### 7.2 Frame State Machine

```
                    +------------------+
                    |    Available     |
                    |  (ring buffers   |
                    |   reset, ready)  |
                    +--------+---------+
                             |
                   acquireAvailableFrame()
                             |
                             v
                    +------------------+
                    |    Recording     |
                    |  (ring buffers   |
                    |   allocating,    |
                    |   cmds recording)|
                    +--------+---------+
                             |
                   submitRecordedFrame()
                             |
                             v
                    +------------------+
                    |    In-Flight     |
                    |  (GPU executing, |
                    |   CPU waiting)   |
                    +--------+---------+
                             |
                   recycleOneInFlight()
                   [wait_for_gpu()]
                             |
                             v
                    +------------------+
                    |    Available     |
                    +------------------+
```

### 7.3 Frame Preparation and Recycling

From `VulkanRenderer.cpp`:

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

### 7.4 Frame Advance Flow

From `VulkanRenderer.cpp`:

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

## 8. Deferred Release Queue

### 8.1 Purpose

GPU resources cannot be destroyed while still referenced by in-flight commands. The deferred release queue delays destruction until the GPU has completed all work using the resource.

### 8.2 Implementation

From `VulkanDeferredRelease.h`:

```cpp
class DeferredReleaseQueue {
public:
    struct Entry {
        uint64_t retireSerial = 0;     // Wait until this serial completes
        MoveOnlyFunction release;       // Destructor closure (captures ownership)
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

### 8.3 Serial Management

From `VulkanBufferManager.h`:

```cpp
// Set before recording; resources retired during this frame use serial+1
void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }

// Called after GPU completes work up to completedSerial
void collect(uint64_t completedSerial) { m_deferredReleases.collect(completedSerial); }
```

The retirement serial is set conservatively. Resources orphaned during frame N's recording are assigned `serial = N + 1`, ensuring they remain valid through the current frame's GPU execution.

### 8.4 MoveOnlyFunction

The queue uses a custom `MoveOnlyFunction` type-erased wrapper (`VulkanDeferredRelease.h`) because `std::function` requires copyable callables, but Vulkan RAII handles (`vk::UniqueBuffer`, `vk::UniqueDeviceMemory`) are move-only.

---

## 9. Memory Mapping and Host-Device Coherence

### 9.1 Persistent Mapping

Ring buffers and host-visible managed buffers use persistent mapping:

From `VulkanRingBuffer.cpp`:
```cpp
m_mapped = m_device.mapMemory(m_memory.get(), 0, size);
```

From `VulkanBufferManager.cpp`:
```cpp
if (getMemoryProperties(buffer.usage) & vk::MemoryPropertyFlagBits::eHostVisible) {
    buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);
}
```

The mapping persists for the buffer's lifetime. No `mapMemory`/`unmapMemory` calls occur during frame rendering.

### 9.2 Host-Coherent Memory

All host-visible buffers use `eHostCoherent` memory:

```cpp
case BufferUsageHint::Dynamic:
case BufferUsageHint::Streaming:
case BufferUsageHint::PersistentMapping:
    return vk::MemoryPropertyFlagBits::eHostVisible |
           vk::MemoryPropertyFlagBits::eHostCoherent;
```

**Implications:**

| Property | Benefit | Trade-off |
|----------|---------|-----------|
| No explicit flush | Simpler API, fewer errors | Slightly slower writes on some hardware |
| Automatic visibility | CPU writes visible to GPU immediately | No cache control |
| Universal support | Works on all Vulkan implementations | May use system RAM on discrete GPUs |

### 9.3 Zero-Copy Data Path

For host-visible buffers, data flows directly from CPU to GPU without intermediate copies:

```
CPU: std::memcpy(alloc.mapped, data, size)
     |
     | (automatic coherence - no barrier/flush needed)
     v
GPU: vkCmdBindVertexBuffers / vkCmdBindDescriptorSets
     |
     v
GPU: shader reads data
```

### 9.4 flushMappedBuffer Is a No-Op

From `VulkanBufferManager.cpp`:

```cpp
void VulkanBufferManager::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size)
{
    // For host-coherent memory, flush is a no-op
    // For non-coherent, we'd need vkFlushMappedMemoryRanges
    // Since we use HOST_COHERENT for all host-visible buffers, this does nothing
}
```

---

## 10. Common Usage Patterns

### 10.1 Per-Draw Uniform Allocation

From `VulkanGraphics.cpp`:

```cpp
// Allocate from per-frame uniform ring with required alignment
auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, uboAlignment);
auto* uniformBase = static_cast<char*>(uniformAlloc.mapped);

// Write uniform data directly to mapped memory
std::memcpy(uniformBase, &matrices, sizeof(matrices));
std::memcpy(uniformBase + genericOffset, genericDataPtr, genericDataSize);

// Build descriptor with ring buffer offset
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;
matrixInfo.range = sizeof(matrices);
```

### 10.2 Transform Buffer Upload

From `VulkanGraphics.cpp`:

```cpp
void gr_vulkan_update_transform_buffer(void* data, size_t size)
{
    VulkanFrame& frame = ctxBase.frame();

    // Allocate from vertex ring (used as storage buffer for transforms)
    auto& ring = frame.vertexBuffer();

    // Respect device alignment requirements for storage buffers
    const auto minAlign = static_cast<vk::DeviceSize>(
        ctxBase.renderer.vulkanDevice()->properties().limits.minStorageBufferOffsetAlignment);
    vk::DeviceSize alignment = std::max(minAlign, vk::DeviceSize{16});

    auto allocOpt = ring.try_allocate(static_cast<vk::DeviceSize>(size), alignment);
    Assertion(allocOpt.has_value(),
        "Transform buffer upload of %zu bytes exceeds per-frame vertex ring remaining %zu bytes. "
        "Increase VERTEX_RING_SIZE or reduce batched transforms.",
        size, static_cast<size_t>(ring.remaining()));

    const auto alloc = *allocOpt;
    std::memcpy(alloc.mapped, data, size);

    // Track offset for model descriptor binding
    frame.modelTransformDynamicOffset = static_cast<uint32_t>(alloc.offset);
    frame.modelTransformSize = size;
}
```

### 10.3 Texture Staging Upload

From `VulkanTextureManager.cpp`:

```cpp
// Try to allocate staging space for this mip level
auto allocOpt = frame.stagingBuffer().try_allocate(layerSize);
if (!allocOpt) {
    // Staging buffer exhausted - defer upload to next frame
    bm_unlock(frameHandle);
    remaining.push_back(frameHandle);
    continue;
}

// Copy texture data to staging buffer
std::memcpy(allocOpt->mapped, pixelData, layerSize);

// Record buffer-to-image copy command
cmd.copyBufferToImage(frame.stagingBuffer().buffer(),
    record.gpu.image.get(),
    vk::ImageLayout::eTransferDstOptimal,
    regions);
```

### 10.4 NanoVG HUD Rendering

```cpp
// Allocate uniform block for HUD rendering
auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, uboAlignment);
std::memcpy(uniformAlloc.mapped, &matrices, sizeof(matrices));
std::memcpy(static_cast<char*>(uniformAlloc.mapped) + genericOffset, &generic, sizeof(generic));

// Bind via push descriptors
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;
matrixInfo.range = sizeof(matrices);
```

---

## 11. Thread Safety

### 11.1 Single-Threaded Design

The Vulkan renderer's buffer management is designed for single-threaded access:

| Component | Thread Safety | Notes |
|-----------|---------------|-------|
| `VulkanRingBuffer` | Not thread-safe | Each frame has its own ring buffers |
| `VulkanBufferManager` | Not thread-safe | All buffer operations on main thread |
| `DeferredReleaseQueue` | Not thread-safe | `enqueue()` and `collect()` on main thread |

### 11.2 Frame Isolation

Thread safety is achieved through frame isolation rather than locks:

- Each `VulkanFrame` owns its ring buffers exclusively during recording
- Only one frame is in the "Recording" state at any time
- The frame pipeline ensures CPU and GPU never access the same frame's resources simultaneously

### 11.3 Safe Patterns

```cpp
// SAFE: Each draw call operates on the current recording frame
void draw(VulkanFrame& frame) {
    auto alloc = frame.uniformBuffer().allocate(size, alignment);
    // Write to alloc.mapped...
}

// UNSAFE: Accessing a different frame's resources
void unsafeDraw(VulkanFrame& currentFrame, VulkanFrame& otherFrame) {
    auto alloc = otherFrame.uniformBuffer().allocate(size, alignment);  // WRONG!
}
```

---

## 12. Performance Considerations

### 12.1 Ring Buffer Sizing

Current sizes are tuned for typical FreeSpace 2 workloads:

| Ring | Size | Typical Usage | Headroom Factor |
|------|------|---------------|-----------------|
| Uniform | 512 KB | ~100-200 KB per frame | 2.5-5x |
| Vertex | 1 MB | ~200-400 KB (transforms, immediate geometry) | 2.5-5x |
| Staging | 12 MB | ~2-8 MB (texture streaming) | 1.5-6x |

### 12.2 Allocation Failure Handling

Ring buffer exhaustion triggers assertions with diagnostic information:

```cpp
Assertion(allocOpt.has_value(),
    "Transform buffer upload of %zu bytes exceeds per-frame vertex ring "
    "remaining %zu bytes. Increase VERTEX_RING_SIZE or reduce batched transforms.",
    size, static_cast<size_t>(ring.remaining()));
```

### 12.3 Memory Coherence Trade-offs

| Using `HOST_COHERENT` | Alternative (non-coherent) |
|----------------------|---------------------------|
| No explicit flush calls needed | Must call `vkFlushMappedMemoryRanges` |
| Simpler, less error-prone code | More control over cache behavior |
| Works on all implementations | May offer better performance on some hardware |
| May use system RAM on discrete GPUs | Can use BAR memory with explicit management |

### 12.4 Double-Buffering Benefits

With `kFramesInFlight = 2`:

| Benefit | Explanation |
|---------|-------------|
| No CPU/GPU Stalls | CPU can record frame N+1 while GPU executes frame N |
| Safe Ring Reuse | Each frame has exclusive access to its ring buffers |
| Deferred Release Safety | Resources guaranteed unused after one frame boundary |
| Latency | One frame of latency between input and display |

### 12.5 Orphaning vs. Suballocation

| Strategy | Use Case | Overhead | When to Use |
|----------|----------|----------|-------------|
| Orphaning (`resizeBuffer`) | Dynamic/Streaming buffers | New allocation per update | Infrequent, variable-size updates |
| Suballocation (ring buffer) | Per-frame transient data | Zero allocations | Frequent, small allocations |
| Grow-only (`ensureBuffer`) | Static buffers | Occasional reallocation | Rare updates, append patterns |

---

## 13. Debugging and Troubleshooting

### 13.1 Common Issues and Solutions

| Symptom | Likely Cause | Solution |
|---------|-------------|----------|
| Assertion: "ring remaining" | Ring buffer exhausted | Increase `*_RING_SIZE` constant or reduce allocation frequency |
| Corruption/flickering | WAR hazard | Ensure Dynamic/Streaming buffers are used, or use ring buffers |
| Validation error: "buffer in use" | Premature destruction | Check deferred release serial; ensure `collect()` called with correct serial |
| Crash in `mapBuffer` | Wrong usage hint | Ensure buffer was created with `PersistentMapping` hint |
| Slow uploads | Using staging for host-visible | Check if `Static` hint is appropriate for your use case |

### 13.2 Debugging Tips

**Enable Vulkan Validation Layers:**
Validation layers will catch many synchronization and lifetime errors. Ensure they are enabled during development.

**Track Ring Buffer Usage:**
Add logging to track high-water marks:

```cpp
// In allocate() or try_allocate()
static vk::DeviceSize maxUsed = 0;
if (m_offset > maxUsed) {
    maxUsed = m_offset;
    mprintf(("Ring buffer high water: %zu / %zu bytes\n",
        static_cast<size_t>(maxUsed), static_cast<size_t>(m_size)));
}
```

**Verify Frame Isolation:**
Assertions in frame accessors can catch illegal cross-frame access:

```cpp
Assertion(&currentRecordingFrame == &frame,
    "Attempted to allocate from non-recording frame");
```

### 13.3 RenderDoc Integration

When capturing with RenderDoc:

- Ring buffer allocations appear as a single large buffer per frame
- Look for the offset in descriptor bindings to identify specific allocations
- Managed buffers appear as separate resources; deferred destruction may show "unused" buffers that were orphaned

---

## Appendices

### Appendix A: Key Constants Reference

From various source files:

```cpp
// VulkanConstants.h
constexpr uint32_t kFramesInFlight = 2;

// VulkanRenderer.h
static constexpr vk::DeviceSize UNIFORM_RING_SIZE  = 512 * 1024;       // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE   = 1024 * 1024;      // 1 MB
static constexpr vk::DeviceSize STAGING_RING_SIZE  = 12 * 1024 * 1024; // 12 MB

// VulkanConstants.h
constexpr uint32_t kMaxBindlessTextures = 1024;
```

### Appendix B: File Reference

| File | Key Contents |
|------|--------------|
| `VulkanRingBuffer.h` | Ring buffer class, `Allocation` struct |
| `VulkanRingBuffer.cpp` | Constructor, `allocate`, `try_allocate`, `reset`, `remaining` |
| `VulkanBufferManager.h` | `VulkanBuffer` struct, manager interface |
| `VulkanBufferManager.cpp` | Buffer creation, update, resize, orphaning, staging uploads |
| `VulkanFrame.h` | Per-frame resources, ring buffer accessors, sync primitives |
| `VulkanFrame.cpp` | Frame initialization, `wait_for_gpu`, `reset` |
| `VulkanDeferredRelease.h` | `MoveOnlyFunction`, `DeferredReleaseQueue` |
| `VulkanRenderer.cpp` | `prepareFrameForReuse`, `recycleOneInFlight`, `advanceFrame` |
| `VulkanGraphics.cpp` | `gr_vulkan_update_transform_buffer`, buffer management hooks |
| `VulkanConstants.h` | `kFramesInFlight`, bindless texture slot constants |
| `code/graphics/2d.h` | `BufferType`, `BufferUsageHint` enums, `gr_buffer_handle` |

### Appendix C: Synchronization Guarantees

#### Frame-Level Guarantees

1. **Ring Buffer Isolation**: Each frame's ring buffers are exclusively owned during recording
2. **Fence Wait Before Reuse**: `wait_for_gpu()` blocks until the frame's commands complete
3. **Deferred Release Collection**: Old resources destroyed only after GPU completion
4. **Timeline Semaphores**: Per-frame timeline values enable precise GPU progress tracking

#### Buffer Update Guarantees

1. **Static Buffers**: Staging copy with explicit barrier ensures visibility before use
2. **Dynamic/Streaming Buffers**: Always orphan; old buffer deferred until GPU completion
3. **Ring Allocations**: No wrap-around prevents overwriting in-flight data

#### Memory Visibility Guarantees

1. **Host Coherent**: CPU writes immediately visible to GPU (no explicit flush required)
2. **Device Local**: Pipeline barriers in staging copy ensure correct visibility ordering
3. **Submission Order**: Commands within a frame execute in recorded order

### Appendix D: OpenGL Compatibility Notes

The buffer system is designed to match OpenGL semantics where the engine relies on them:

| OpenGL Pattern | Vulkan Implementation |
|----------------|----------------------|
| `glBufferData` with same size | `resizeBuffer` with orphaning for Dynamic/Streaming |
| `glBufferSubData` | `updateBufferDataOffset` with staging or direct copy |
| `glMapBuffer` | Persistent mapping; `mapBuffer` returns pre-mapped pointer |
| `glFlushMappedBufferRange` | No-op (host-coherent memory) |
| Implicit synchronization | Explicit deferred release with serial tracking |

---

*Last updated: December 2024*
*Applies to: FreeSpace 2 Open Vulkan Renderer*
