# Vulkan Memory Allocation Patterns

This document describes FSO's Vulkan memory allocation patterns, buffer management strategies, and GPU resource lifetime handling.

## Overview

FSO's Vulkan backend uses **native Vulkan memory allocation** rather than the Vulkan Memory Allocator (VMA) library. While the `vk_mem_alloc.h` header exists in the codebase, it is not currently integrated into the allocation path. All memory allocation flows through direct Vulkan API calls (`vk::allocateMemoryUnique`, `vk::bindBufferMemory`, etc.).

The memory system is organized around these primary components:

| Component | File | Responsibility |
|-----------|------|----------------|
| `VulkanBufferManager` | `VulkanBufferManager.cpp/.h` | Long-lived buffer creation, updates, deferred deletion |
| `VulkanRingBuffer` | `VulkanRingBuffer.cpp/.h` | Per-frame transient allocations (uniform, vertex, staging) |
| `VulkanTextureManager` | `VulkanTextureManager.cpp/.h` | Texture memory, staging uploads, render targets |
| `VulkanMovieManager` | `VulkanMovieManager.cpp/.h` | Movie/video texture memory with deferred release |
| `DeferredReleaseQueue` | `VulkanDeferredRelease.h` | Serial-gated resource destruction queue |

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

### VulkanBuffer Structure

Each managed buffer is represented by `VulkanBuffer` (`VulkanBufferManager.h`):

```cpp
struct VulkanBuffer {
    vk::UniqueBuffer buffer;       // RAII buffer handle
    vk::UniqueDeviceMemory memory; // RAII memory handle
    BufferType type;               // Vertex, Index, or Uniform
    BufferUsageHint usage;         // Static, Dynamic, Streaming, or PersistentMapping
    vk::DeviceSize size = 0;       // Current allocation size (0 = not yet allocated)
    void* mapped = nullptr;        // Non-null for host-visible buffers
    bool isPersistentMapped = false;
};
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

The `VulkanBufferManager` maintains a dedicated transfer command pool with `eTransient | eResetCommandBuffer` flags for these synchronous uploads:

```cpp
vk::CommandPoolCreateInfo poolInfo{};
poolInfo.queueFamilyIndex = transferQueueIndex;
poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
m_transferCommandPool = m_device.createCommandPoolUnique(poolInfo);
```

Memory barriers after the copy are buffer-type-aware, using the appropriate destination stage and access masks:

| Buffer Type | Destination Stage | Destination Access |
|-------------|-------------------|-------------------|
| `Vertex` | `eVertexInput \| eVertexShader` | `eVertexAttributeRead \| eShaderRead` |
| `Index` | `eVertexInput` | `eIndexRead` |
| `Uniform` | `eVertexShader \| eFragmentShader` | `eUniformRead` |

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

// Returns the remaining capacity (accounting for alignment of the next allocation)
vk::DeviceSize remaining() const;
```

### Alignment Enforcement

The ring buffer enforces a baseline alignment (typically `minUniformBufferOffsetAlignment` for uniform rings). Callers can request stricter alignment via `alignmentOverride`, but the override cannot weaken the baseline:

```cpp
const vk::DeviceSize align = std::max(m_alignment, alignmentOverride ? alignmentOverride : vk::DeviceSize{0});
vk::DeviceSize alignedOffset = ((m_offset + align - 1) / align) * align;
```

The `remaining()` method accounts for alignment when reporting available capacity:

```cpp
vk::DeviceSize VulkanRingBuffer::remaining() const {
    const vk::DeviceSize alignedOffset = ((m_offset + m_alignment - 1) / m_alignment) * m_alignment;
    if (alignedOffset >= m_size) {
        return 0;
    }
    return m_size - alignedOffset;
}
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

Ring buffer sizes are defined as constants in `VulkanRenderer` and passed to `VulkanFrame` during construction. The staging ring must accommodate the largest single-frame upload batch:

```cpp
// From VulkanRenderer.h
static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;        // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;        // 1 MB
static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024;  // 12 MiB

// VulkanFrame constructor (VulkanFrame.h)
VulkanFrame(vk::Device device, uint32_t frameIndex, uint32_t queueFamilyIndex,
    const vk::PhysicalDeviceMemoryProperties& memoryProps,
    vk::DeviceSize uniformBufferSize, vk::DeviceSize uniformAlignment,
    vk::DeviceSize vertexBufferSize, vk::DeviceSize vertexAlignment,
    vk::DeviceSize stagingBufferSize, vk::DeviceSize stagingAlignment,
    vk::DescriptorSet globalSet, vk::DescriptorSet modelSet);
```

Alignment parameters are derived from device limits:
- `uniformAlignment`: `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment`
- `vertexAlignment`: Derived from device vertex buffer alignment requirements
- `stagingAlignment`: `VkPhysicalDeviceLimits::optimalBufferCopyOffsetAlignment`

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

    void clear() noexcept;  // Invoke all pending releases immediately
    size_t size() const;    // Number of pending entries
};
```

`MoveOnlyFunction` is a minimal type-erased callable (similar to `std::move_only_function` from C++23) that allows capturing move-only types like `vk::UniqueBuffer` in the release callback.

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

Deferred resource collection happens at two points in the frame lifecycle:

1. **Frame Recycling** (`prepareFrameForReuse`): When a frame is recycled from the in-flight queue, collect is called after the fence wait confirms GPU completion.

2. **Frame Begin** (`beginFrame`): Collection is called again opportunistically at the start of each frame recording.

```cpp
// In prepareFrameForReuse() - called when recycling a frame
void VulkanRenderer::prepareFrameForReuse(VulkanFrame& frame, uint64_t completedSerial) {
    m_bufferManager->collect(completedSerial);
    m_textureManager->collect(completedSerial);
    frame.reset();
}

// In beginFrame() - opportunistic collection at frame start
m_completedSerial = std::max(m_completedSerial, queryCompletedSerial());
if (m_bufferManager) m_bufferManager->collect(m_completedSerial);
if (m_textureManager) m_textureManager->collect(m_completedSerial);
if (m_movieManager) m_movieManager->collect(m_completedSerial);
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

The memory system integrates with the frame lifecycle through a ring of frames managed by `VulkanRenderer`. The actual flow is:

```
flip() -> advanceFrame(currentRecording)
|
+-- endFrame(prev)                        // finish command buffer recording
+-- submitRecordedFrame(prev)             // submit with timeline semaphore signal
+-- add frame to m_inFlightFrames queue
|
+-- beginRecording()
    +-- acquireAvailableFrame()
    |   +-- [if no frames available]:
    |       +-- recycleOneInFlight()
    |           +-- pop from m_inFlightFrames
    |           +-- VulkanFrame::wait_for_gpu()       // fence wait
    |           +-- queryCompletedSerial()            // timeline semaphore query
    |           +-- prepareFrameForReuse()
    |               +-- bufferManager->collect()      // destroy safe resources
    |               +-- textureManager->collect()
    |               +-- frame.reset()                 // reset ring buffer offsets
    |           +-- add to m_availableFrames
    |
    +-- acquireImage(frame)               // swapchain image acquisition
    +-- beginFrame(frame, imageIndex)
        +-- frame.resetPerFrameBindings()
        +-- cmd.begin()                   // start command buffer recording
        +-- collect() again (opportunistic)
        +-- setSafeRetireSerial()         // for resources retired during recording
        +-- flushPendingUploads()         // texture uploads via staging ring
        +-- beginModelDescriptorSync()    // sync bindless descriptors
```

Key invariants:
- Ring buffers are reset only after the corresponding fence signals completion
- Collection happens both at frame recycle and opportunistically at frame begin
- `setSafeRetireSerial(submitSerial + 1)` ensures resources deleted mid-frame survive the upcoming submission

## Key Constants

From `VulkanConstants.h`:

```cpp
constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxBindlessTextures = 1024;

// Bindless texture slot reservations
constexpr uint32_t kBindlessTextureSlotFallback = 0;       // Always-valid black fallback
constexpr uint32_t kBindlessTextureSlotDefaultBase = 1;    // Default base/diffuse texture
constexpr uint32_t kBindlessTextureSlotDefaultNormal = 2;  // Default normal map
constexpr uint32_t kBindlessTextureSlotDefaultSpec = 3;    // Default specular map
constexpr uint32_t kBindlessFirstDynamicTextureSlot = 4;   // First slot for dynamic textures
```

The ring buffer system relies on `kFramesInFlight` to ensure resources from the previous frame are not overwritten while still in use.

Bindless slots 0-3 are reserved defaults that are always populated with valid fallback textures, ensuring shaders never sample destroyed images or need sentinel value routing.

## Best Practices

### For Buffer Users

1. **Prefer `Static` for load-time data** (models, static meshes)
2. **Use `Streaming` for per-frame data** that changes every frame
3. **Use `Dynamic` for data that changes occasionally** but not every frame
4. **Never hold buffer handles past `gr_delete_buffer()`** - the handle becomes invalid

### For Texture Users

1. **Queue uploads early** - `queueTextureUpload()` at draw time; uploads flush at frame start via `flushPendingUploads()`
2. **Expect fallback textures** - bindless sampling uses reserved slots (0-3) until a texture is resident and has a dynamic slot assigned (slot 4+)
3. **Respect staging budget** - textures exceeding the 12 MiB staging buffer are added to `m_permanentlyRejected` and will not be retried automatically
4. **Use `TextureId::tryFromBaseFrame()`** - treat invalid handles as absence rather than using sentinel values

### For Memory Debugging

1. **Enable Vulkan validation layers** - catch use-after-free and binding errors
2. **Monitor memory heaps** via Vulkan device memory properties
3. **Check ring buffer exhaustion** - `try_allocate()` returning `nullopt` or `remaining()` approaching zero indicates budget issues
4. **Review deferred release queue size** - `DeferredReleaseQueue::size()` growing unbounded indicates a serial tracking issue
5. **Use `-vk_hud_debug`** - enables diagnostic logging for texture upload rejections and deferrals

## Related Documentation

- `docs/VULKAN_FRAME_LIFECYCLE.md` - Frame recording and submission flow
- `docs/VULKAN_SYNCHRONIZATION.md` - Fences, semaphores, and barriers
- `docs/VULKAN_TEXTURE_RESIDENCY.md` - Texture upload and bindless slot management
- `docs/VULKAN_DYNAMIC_BUFFERS.md` - Dynamic uniform buffer patterns
