# Vulkan Memory Allocation Patterns

This document describes FSO's Vulkan memory allocation patterns, buffer management strategies, and GPU resource lifetime handling.

## Overview

FSO's Vulkan backend uses **native Vulkan memory allocation** rather than the Vulkan Memory Allocator (VMA) library. While the `vk_mem_alloc.h` header exists in the codebase, it is not currently integrated into the allocation path. All memory allocation flows through direct Vulkan API calls (`vk::allocateMemoryUnique`, `vk::bindBufferMemory`, etc.).

The memory system is organized around three primary components:

| Component | File | Responsibility |
|-----------|------|----------------|
| `VulkanBufferManager` | `VulkanBufferManager.cpp/.h` | Long-lived buffer creation, updates, deferred deletion |
| `VulkanRingBuffer` | `VulkanRingBuffer.cpp/.h` | Per-frame transient allocations |
| `VulkanTextureManager` | `VulkanTextureManager.cpp/.h` | Texture memory, staging uploads, render targets |

## Memory Type Selection

FSO selects memory types based on the intended usage pattern, mapping engine buffer hints to Vulkan memory properties.

### Buffer Usage Hints

Defined in `code/graphics/2d.h`:

```cpp
enum class BufferUsageHint { Static, Dynamic, Streaming, PersistentMapping };
```

### Memory Property Mapping

From `VulkanBufferManager::getMemoryProperties()`:

| Usage Hint | Memory Properties | Rationale |
|------------|-------------------|-----------|
| `Static` | `eDeviceLocal` | GPU-only; updates via staging buffer |
| `Dynamic` | `eHostVisible \| eHostCoherent` | CPU-writable; orphaning on each update |
| `Streaming` | `eHostVisible \| eHostCoherent` | CPU-writable; orphaning on each update |
| `PersistentMapping` | `eHostVisible \| eHostCoherent` | Persistently mapped for direct writes |

The `findMemoryType()` helper searches the physical device's memory heaps for a type matching both the buffer's memory requirements and the desired property flags:

```cpp
uint32_t VulkanBufferManager::findMemoryType(uint32_t typeFilter,
    vk::MemoryPropertyFlags properties) const
{
    for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) &&
            (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type.");
}
```

## Buffer Types and Allocation Strategies

### Buffer Type Definitions

Defined in `code/graphics/2d.h`:

```cpp
enum class BufferType { Vertex, Index, Uniform };
```

### Vulkan Usage Flags

From `VulkanBufferManager::getVkUsageFlags()`:

| Buffer Type | Vulkan Usage Flags |
|-------------|-------------------|
| `Vertex` | `eVertexBuffer \| eStorageBuffer \| eTransferDst` |
| `Index` | `eIndexBuffer \| eTransferDst` |
| `Uniform` | `eUniformBuffer \| eTransferDst` |

The `eStorageBuffer` flag on vertex buffers enables vertex-pulling shaders to read vertex data via storage buffer bindings rather than traditional vertex attributes.

### Lazy Buffer Creation

`VulkanBufferManager` employs lazy buffer creation:

1. `createBuffer()` returns a valid handle immediately but does **not** allocate GPU memory
2. `ensureBuffer(handle, minSize)` creates or resizes the buffer on first use
3. `updateBufferData()` calls `ensureBuffer()` internally

This pattern avoids allocating memory for buffers that are created but never populated.

### Orphaning for Dynamic/Streaming Buffers

For `Dynamic` and `Streaming` buffers, `updateBufferData()` always recreates storage even when the size is unchanged. This matches OpenGL's `glBufferData()` orphaning semantics:

```cpp
if (buffer.usage == BufferUsageHint::Dynamic || buffer.usage == BufferUsageHint::Streaming) {
    resizeBuffer(handle, size);  // Always creates new storage
} else {
    ensureBuffer(handle, static_cast<vk::DeviceSize>(size));
}
```

The engine relies on this behavior to avoid overwriting GPU-in-flight data when multiple frames are in flight.

### Static Buffer Uploads

Static buffers use device-local memory for optimal GPU access. Data updates require a staging buffer and transfer commands:

```cpp
void VulkanBufferManager::uploadToDeviceLocal(const VulkanBuffer& buffer,
    vk::DeviceSize dstOffset, vk::DeviceSize size, const void* data)
{
    // 1. Create transient host-visible staging buffer
    // 2. Map, memcpy data, unmap
    // 3. Record copy command (staging -> destination)
    // 4. Insert memory barrier for appropriate destination stage
    // 5. Submit and fence-wait for completion
}
```

The transfer is **synchronous** (fence-wait) to ensure data is available immediately. This is acceptable for static data loaded at startup or level transitions.

## Ring Buffers for Per-Frame Data

### Purpose

Ring buffers provide fast, bump-pointer allocation for data that lives only within a single frame. Each frame in the ring has its own set of ring buffers, eliminating CPU/GPU synchronization hazards.

### Ring Buffer Types

From `VulkanFrame.h`:

| Ring Buffer | Usage | Vulkan Buffer Usage |
|-------------|-------|---------------------|
| `m_uniformRing` | Per-draw uniform data | `eUniformBuffer` |
| `m_vertexRing` | Immediate-mode vertices | `eVertexBuffer \| eStorageBuffer` |
| `m_stagingRing` | Texture upload staging | `eTransferSrc` |

### Memory Allocation

All ring buffers use **host-visible + host-coherent** memory with persistent mapping:

```cpp
VulkanRingBuffer::VulkanRingBuffer(vk::Device device,
    const vk::PhysicalDeviceMemoryProperties& memoryProps,
    vk::DeviceSize size, vk::DeviceSize alignment, vk::BufferUsageFlags usage)
{
    // Create buffer with requested usage flags
    m_buffer = m_device.createBufferUnique(bufferInfo);

    // Allocate host-visible, host-coherent memory
    allocInfo.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        memoryProps);

    // Persistently map for the lifetime of the ring buffer
    m_mapped = m_device.mapMemory(m_memory.get(), 0, size);
}
```

### Allocation API

```cpp
struct Allocation {
    vk::DeviceSize offset{0};   // Offset within the ring buffer
    void* mapped{nullptr};      // CPU-writable pointer
};

// Throws if allocation fails
Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);

// Returns nullopt if allocation would exceed remaining capacity
std::optional<Allocation> try_allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
```

### Alignment Enforcement

The ring buffer enforces a baseline alignment (typically `minUniformBufferOffsetAlignment` for uniform rings). Callers can request stricter alignment via `alignmentOverride`, but the override cannot weaken the baseline:

```cpp
const vk::DeviceSize align = std::max(m_alignment, alignmentOverride ? alignmentOverride : 0);
vk::DeviceSize alignedOffset = ((m_offset + align - 1) / align) * align;
```

### No Mid-Frame Wrap

Ring buffers do **not** wrap within a frame. If an allocation would exceed capacity, `try_allocate()` returns `nullopt` rather than wrapping to offset 0:

```cpp
if (alignedOffset + requestSize > m_size) {
    return std::nullopt;  // Would overwrite in-flight GPU reads
}
```

### Frame Reset

At the start of each frame (after waiting on the frame's fence), the ring buffer's offset is reset to 0:

```cpp
void VulkanRingBuffer::reset() { m_offset = 0; }
```

This reclaims the entire buffer for the new frame's allocations.

### Ring Buffer Sizing

Ring buffer sizes are configured in `VulkanFrame` construction. The staging ring is used for texture uploads and must accommodate the largest single-frame upload batch:

```cpp
VulkanFrame::VulkanFrame(vk::Device device, uint32_t frameIndex,
    uint32_t queueFamilyIndex,
    const vk::PhysicalDeviceMemoryProperties& memoryProps,
    vk::DeviceSize uniformBufferSize, vk::DeviceSize uniformAlignment,
    vk::DeviceSize vertexBufferSize, vk::DeviceSize vertexAlignment,
    vk::DeviceSize stagingBufferSize, vk::DeviceSize stagingAlignment,
    vk::DescriptorSet modelSet)
```

## Texture Memory Allocation

### Image Creation and Memory Binding

Textures use **device-local** memory for optimal sampling performance:

```cpp
auto image = m_device.createImageUnique(imageInfo);
auto imgReqs = m_device.getImageMemoryRequirements(image.get());

vk::MemoryAllocateInfo imgAllocInfo;
imgAllocInfo.allocationSize = imgReqs.size;
imgAllocInfo.memoryTypeIndex = findMemoryType(imgReqs.memoryTypeBits,
    vk::MemoryPropertyFlagBits::eDeviceLocal);

auto imageMem = m_device.allocateMemoryUnique(imgAllocInfo);
m_device.bindImageMemory(image.get(), imageMem.get(), 0);
```

### Staging Upload Flow

Texture uploads use the per-frame staging ring buffer:

1. **Lock bitmap** via bmpman to access pixel data
2. **Allocate staging space** from `frame.stagingBuffer().try_allocate()`
3. **Copy/convert pixel data** to staging (format conversion if needed)
4. **Record transfer commands:**
   - Transition image to `eTransferDstOptimal`
   - Copy buffer regions to image layers
   - Transition image to `eShaderReadOnlyOptimal`

```cpp
for (uint32_t layer = 0; layer < layers; ++layer) {
    auto allocOpt = frame.stagingBuffer().try_allocate(layerSize);
    if (!allocOpt) {
        remaining.push_back(baseFrame);  // Defer to next frame
        break;
    }
    std::memcpy(allocOpt->mapped, pixelData, layerSize);

    regions.push_back({ .bufferOffset = allocOpt->offset, /* ... */ });
}
cmd.copyBufferToImage(frame.stagingBuffer().buffer(), image.get(),
    vk::ImageLayout::eTransferDstOptimal, regions);
```

### Budget-Aware Batching

The texture manager tracks staging budget to avoid exhausting the ring buffer mid-frame:

```cpp
vk::DeviceSize stagingBudget = frame.stagingBuffer().size();
vk::DeviceSize stagingUsed = 0;

for (int baseFrame : m_pendingUploads) {
    if (stagingUsed + totalUploadSize > stagingBudget) {
        remaining.push_back(baseFrame);  // Defer to next frame
        continue;
    }
    // ... upload texture ...
}
m_pendingUploads = std::move(remaining);
```

Textures that cannot fit in a single staging buffer are marked **unavailable** rather than attempting partial uploads.

## Deferred Release via Serial Tracking

### The Problem

Vulkan resources cannot be destroyed while the GPU is still using them. With multiple frames in flight, a buffer deleted on frame N might still be referenced by frame N-1's command buffer.

### Solution: Serial-Gated Deferred Release

FSO tracks GPU progress via **timeline semaphores**. Each frame submission increments the semaphore to a new "serial" value. Resources are tagged with a retire serial and destroyed only after that serial is reached.

### DeferredReleaseQueue

From `VulkanDeferredRelease.h`:

```cpp
class DeferredReleaseQueue {
public:
    struct Entry {
        uint64_t retireSerial = 0;
        MoveOnlyFunction release;  // Destructor callback
    };

    template <typename F>
    void enqueue(uint64_t retireSerial, F&& releaseFn);

    void collect(uint64_t completedSerial);
};
```

### Enqueue Pattern

When deleting a buffer or texture, the resource is moved into a closure and enqueued with a safe retire serial:

```cpp
void VulkanBufferManager::deleteBuffer(gr_buffer_handle handle)
{
    // ... unmap if mapped ...

    if (buffer.buffer) {
        const uint64_t retireSerial = m_safeRetireSerial + 1;
        m_deferredReleases.enqueue(retireSerial,
            [buf = std::move(buffer.buffer), mem = std::move(buffer.memory)]() mutable {
                // buf and mem are destroyed when the closure is invoked
            });
    }
}
```

The closure captures the `vk::UniqueBuffer` and `vk::UniqueDeviceMemory` by move. When `collect()` invokes the closure, the unique handles go out of scope and are destroyed.

### Collect Pattern

Each frame, after confirming the completed serial via timeline semaphore query, managers call `collect()`:

```cpp
void VulkanRenderer::collectDeferredResources(uint64_t completedSerial)
{
    m_bufferManager->collect(completedSerial);
    m_textureManager->collect(completedSerial);
    m_movieManager->collect(completedSerial);
}
```

`collect()` iterates pending entries and invokes release callbacks for entries whose retire serial is <= the completed serial:

```cpp
void DeferredReleaseQueue::collect(uint64_t completedSerial)
{
    size_t writeIdx = 0;
    for (auto& e : m_entries) {
        if (e.retireSerial <= completedSerial) {
            e.release();  // Destroy the captured resources
        } else {
            m_entries[writeIdx++] = std::move(e);  // Keep for later
        }
    }
    m_entries.resize(writeIdx);
}
```

### Safe Retire Serial

The `m_safeRetireSerial` is set by the renderer to indicate the serial of the upcoming (or most recently submitted) frame. Resources retired during recording use `m_safeRetireSerial + 1` to ensure they survive at least one more frame submission.

## Frame Lifecycle Integration

The memory system integrates with the frame lifecycle as follows:

```
flip()
+-- [submit previous frame with timeline semaphore signal]
+-- advance frame index
+-- VulkanFrame::wait_for_gpu()           // fence wait
+-- query timeline semaphore value        // get completedSerial
+-- managers: collect(completedSerial)    // destroy safe-to-delete resources
+-- acquireNextImage()
    +-- beginFrame()
        +-- VulkanFrame::reset()
            +-- m_uniformRing.reset()     // reset ring buffer offsets
            +-- m_vertexRing.reset()
            +-- m_stagingRing.reset()
        +-- flushPendingUploads()         // use staging ring for textures
        +-- [begin recording]
```

## Key Constants

From `VulkanConstants.h`:

```cpp
constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxBindlessTextures = 1024;
```

The ring buffer system relies on `kFramesInFlight` to ensure resources from the previous frame are not overwritten while still in use.

## Best Practices

### For Buffer Users

1. **Prefer `Static` for load-time data** (models, static meshes)
2. **Use `Streaming` for per-frame data** that changes every frame
3. **Use `Dynamic` for data that changes occasionally** but not every frame
4. **Never hold buffer handles past `gr_delete_buffer()`** - the handle becomes invalid

### For Texture Users

1. **Queue uploads early** - `queueTextureUpload()` at draw time; uploads flush at frame start
2. **Expect fallback textures** - `getBindlessSlotIndex()` returns fallback if not resident
3. **Respect staging budget** - very large textures may be marked unavailable

### For Memory Debugging

1. **Enable Vulkan validation layers** - catch use-after-free and binding errors
2. **Monitor memory heaps** via Vulkan device memory properties
3. **Check ring buffer exhaustion** - `try_allocate()` returning `nullopt` indicates budget issues

## Related Documentation

- `docs/VULKAN_FRAME_LIFECYCLE.md` - Frame recording and submission flow
- `docs/VULKAN_SYNCHRONIZATION.md` - Fences, semaphores, and barriers
- `docs/VULKAN_TEXTURE_RESIDENCY.md` - Texture upload and bindless slot management
- `docs/VULKAN_DYNAMIC_BUFFERS.md` - Dynamic uniform buffer patterns
