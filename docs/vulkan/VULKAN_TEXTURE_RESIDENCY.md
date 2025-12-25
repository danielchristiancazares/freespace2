# Vulkan Texture Residency State Machine

This document describes the texture residency system in the Vulkan renderer, covering state transitions, upload batching, bindless slot assignment, and fallback handling.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Residency States](#2-residency-states)
3. [State Transitions](#3-state-transitions)
4. [Upload Batching](#4-upload-batching)
5. [Bindless Slot Assignment](#5-bindless-slot-assignment)
6. [Fallback Texture Handling](#6-fallback-texture-handling)
7. [Usage Patterns](#7-usage-patterns)
8. [Common Issues](#8-common-issues)

---

## 1. Overview

The texture residency system manages GPU texture resources using a **state-as-location** pattern. Textures exist in one of three containers:

- **`m_residentTextures`**: GPU-resident textures ready for use
- **`m_pendingUploads`**: Textures queued for upload to GPU
- **`m_unavailableTextures`**: Textures that cannot be uploaded (invalid handle, unsupported format, etc.)

**Key Principle**: Texture state is determined by container membership, not by flags or enums. A texture is resident if it exists in `m_residentTextures`.

**Files**:
- `code/graphics/vulkan/VulkanTextureManager.h` - Residency tracking structures
- `code/graphics/vulkan/VulkanTextureManager.cpp` - State transition and upload logic

---

## 2. Residency States

### 2.1 Resident Texture

**Container**: `std::unordered_map<int, ResidentTexture> m_residentTextures`

**Structure** (`VulkanTextureManager.h:113-117`):
```cpp
struct ResidentTexture {
    VulkanTexture gpu;                    // GPU image, memory, view, sampler
    uint32_t lastUsedFrame = 0;          // LRU tracking: frame index
    uint64_t lastUsedSerial = 0;         // Serial of most recent submission
};
```

**Properties**:
- GPU resources (`VkImage`, `VkDeviceMemory`, `VkImageView`) are allocated
- Image layout is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
- Can be bound to descriptors immediately
- Has a bindless slot assignment (if requested)

**Lifetime**: Texture remains resident until explicitly deleted or retired due to LRU eviction.

### 2.2 Pending Upload

**Container**: `std::vector<int> m_pendingUploads`

**Properties**:
- Texture handle is queued for upload
- No GPU resources allocated yet
- Upload occurs during `flushPendingUploads()` at frame start
- May be deferred to next frame if staging buffer is exhausted

**Transition Triggers**:
- `queueTextureUpload()` - Explicitly queue a texture
- `preloadTexture()` - Preload texture immediately (may queue if staging fails)

### 2.3 Unavailable Texture

**Container**: `std::unordered_map<int, UnavailableTexture> m_unavailableTextures`

**Structure** (`VulkanTextureManager.h:119-121`):
```cpp
struct UnavailableTexture {
    UnavailableReason reason = UnavailableReason::InvalidHandle;
};
```

**Unavailable Reasons** (`VulkanTextureManager.h:105-111`):
- `InvalidHandle` - Bitmap handle is invalid or not found
- `InvalidArray` - Array texture has invalid frame count
- `BmpLockFailed` - Failed to lock bitmap data
- `TooLargeForStaging` - Texture exceeds staging buffer capacity
- `UnsupportedFormat` - Format not supported by Vulkan

**Properties**:
- Never retried (permanently unavailable)
- Does not consume bindless slots
- Returns fallback descriptor when sampled

---

## 3. State Transitions

### 3.1 Transition Diagram

```
                    +------------------+
                    |   Not Tracked    |
                    |  (no container)  |
                    +--------+---------+
                             |
                    queueTextureUpload()
                             |
                             v
                    +------------------+
                    |  Pending Upload  |
                    +--------+---------+
                             |
                    flushPendingUploads()
                             |
                +-------------+-------------+
                |                           |
        [Success]                    [Failure]
                |                           |
                v                           v
    +------------------+      +------------------+
    |   Resident        |      |   Unavailable    |
    |   (GPU ready)     |      |   (permanent)    |
    +------------------+      +------------------+
                |
        deleteTexture() /
        releaseBitmap()
                |
                v
    +------------------+
    |   Deferred       |
    |   Release        |
    |   (serial-gated) |
    +------------------+
```

### 3.2 Queue Upload

**Function**: `VulkanTextureManager::queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex, const SamplerKey& samplerKey)`

**Behavior** (`VulkanTextureManager.cpp:1583-1612`):
1. Resolve base frame handle (for animated textures)
2. Check if already resident -> skip
3. Check if unavailable -> skip
4. Warm sampler cache for later descriptor requests
5. Add to `m_pendingUploads` vector if not already queued

**Key Point**: Queuing is idempotent. Multiple calls for the same handle are safe.

### 3.3 Flush Pending Uploads

**Function**: `VulkanTextureManager::flushPendingUploads(const UploadCtx& ctx)`

**Called At**: Frame start, after fence wait, before descriptor sync (`VulkanRenderer.cpp:327`)

**Process** (`VulkanTextureManager.cpp:649-924`):

1. **Process Retirements**: Clean up textures retired in previous frames
2. **Retry Bindless Slots**: Resolve any pending bindless slot requests
3. **Iterate Pending Uploads**:
   - Skip if already resident
   - Skip if unavailable
   - Validate bitmap handle
   - Check staging buffer capacity
   - Allocate staging space
   - Lock bitmap data
   - Create GPU image
   - Record copy commands
   - Transition to shader-read layout
   - Move to `m_residentTextures`

4. **Handle Failures**:
   - Staging buffer exhausted → defer to next frame
   - Invalid handle → mark unavailable
   - Lock failed → mark unavailable
   - Format unsupported → mark unavailable

**Staging Buffer Management**:
- Uses per-frame staging ring buffer (`frame.stagingBuffer()`)
- Tracks staging usage: `stagingUsed += layerSize`
- Defers uploads that exceed remaining capacity
- Remaining uploads stay in `m_pendingUploads` for next frame

### 3.4 Mark Unavailable

**Function**: Internal lambda `markUnavailable()` in `flushPendingUploads()`

**Behavior** (`VulkanTextureManager.cpp:669-682`, defined inside `flushPendingUploads`):
1. Insert into `m_unavailableTextures`
2. Remove from `m_pendingBindlessSlots`
3. Release bindless slot if assigned
4. Add slot to free list if dynamic

**Key Point**: Unavailable textures never retry. They remain unavailable until handle is released and recreated.

### 3.5 Delete Texture

**Function**: `VulkanTextureManager::deleteTexture(int bitmapHandle)`

**Behavior** (`VulkanTextureManager.cpp:1402-1417`):
1. Remove from `m_unavailableTextures`
2. Remove from `m_pendingBindlessSlots`
3. Remove from `m_pendingUploads`
4. Add to `m_pendingRetirements` (slot reuse deferred to frame-start safe point)

**Deferred Retirement**: The actual slot release and GPU resource destruction are deferred to `processPendingRetirements()` at the next frame start, ensuring safe slot reuse.

### 3.6 Release Bitmap

**Function**: `VulkanTextureManager::releaseBitmap(int bitmapHandle)`

**Called At**: When bmpman slot becomes `BM_TYPE_NONE` (handle reuse)

**Behavior** (`VulkanTextureManager.cpp:1419-1457`):
1. Remove from `m_unavailableTextures`, `m_pendingBindlessSlots`, `m_pendingRetirements`, `m_pendingUploads`
2. If resident: call `retireTexture()` which defers GPU resource destruction via `DeferredReleaseQueue`
3. If not resident but has bindless slot: release slot immediately to free list

**Critical**: Must be called before handle reuse to prevent texture collisions. Unlike `deleteTexture()`, this is a hard lifecycle boundary that drops all cache state immediately since bmpman may reuse the handle on the next call.

---

## 4. Upload Batching

### 4.1 Upload Timing

Uploads are batched at frame start to minimize GPU stalls:

**Frame Start Sequence** (`VulkanRenderer.cpp:310-338`):
```
beginFrame()
+-- reset command pool
+-- begin command buffer
+-- setSafeRetireSerial() / setCurrentFrameIndex()
+-- flushPendingUploads()        <- All pending uploads processed here
+-- beginModelDescriptorSync()   <- Descriptors updated after uploads
+-- renderingSession->beginFrame()
```

**Why Frame Start**:
- Staging buffer is reset (fresh capacity)
- Command buffer is empty (no pipeline stalls)
- Descriptors can be updated after uploads complete
- GPU can process uploads while CPU records rendering

### 4.2 Staging Buffer Allocation

**Staging Ring Buffer**: Per-frame, 12 MB capacity (`VulkanRenderer.h:199`, `STAGING_RING_SIZE`)

**Allocation Pattern** (`VulkanTextureManager.cpp:775-887`):
```cpp
// Try to allocate staging space
auto allocOpt = frame.stagingBuffer().try_allocate(static_cast<vk::DeviceSize>(layerSize));
if (!allocOpt) {
    // Staging exhausted - defer to next frame
    remaining.push_back(baseFrame);
    continue;
}
auto& alloc = *allocOpt;

// Copy texture data to staging (format-dependent expansion may occur)
std::memcpy(alloc.mapped, pixelData, layerSize);

// Record buffer-to-image copy
cmd.copyBufferToImage(
    frame.stagingBuffer().buffer(),
    record.gpu.image.get(),
    vk::ImageLayout::eTransferDstOptimal,
    static_cast<uint32_t>(regions.size()),
    regions.data());
```

**Staging Budget Tracking**:
- Tracks `stagingUsed` against `stagingBudget`
- Defers uploads that exceed remaining capacity
- Remaining uploads stay in `m_pendingUploads` for next frame

### 4.3 Upload Command Recording

**In-Frame Uploads** (`VulkanTextureManager::flushPendingUploads()`):
- Recorded into main command buffer (not separate transfer queue)
- Uses `VK_PIPELINE_STAGE_2` barriers for synchronization
- Transitions: `UNDEFINED` -> `TRANSFER_DST` -> `SHADER_READ_ONLY`

**Immediate Uploads** (`VulkanTextureManager::uploadImmediate()`, lines 346-577):
- Creates dedicated command buffer on transfer queue
- Synchronous wait (`m_transferQueue.waitIdle()`)
- Used for preload operations (textures needed before first frame)

---

## 5. Bindless Slot Assignment

### 5.1 Slot Types

**Reserved Slots** (`VulkanConstants.h:14-18`):
- Slot 0: Fallback texture (black, always valid)
- Slot 1: Default base texture (white)
- Slot 2: Default normal texture (flat normal)
- Slot 3: Default specular texture (dielectric F0)
- Slots 4+: Dynamic texture assignments

**Dynamic Slots**: Assigned from free list (`m_freeBindlessSlots`)

### 5.2 Assignment Algorithm

**Function**: `VulkanTextureManager::getBindlessSlotIndex(int textureHandle)`

**Process** (`VulkanTextureManager.cpp:1482-1515`):

1. **Check Unavailability**:
   ```cpp
   if (m_unavailableTextures.find(textureHandle) != m_unavailableTextures.end()) {
       return 0;  // Fallback slot
   }
   ```

2. **Try Slot Assignment** (via `tryAssignBindlessSlot()`):
   - If already has slot -> return it
   - If no free slots -> try reclaiming from non-resident textures
   - Assigns from `m_freeBindlessSlots` if available

3. **Handle Assignment Failure**:
   - Add to `m_pendingBindlessSlots` for retry at next frame start
   - Queue for upload if not resident and not already queued
   - Return slot 0 (fallback) until assignment succeeds

4. **Update LRU Tracking**:
   - If resident, update `lastUsedFrame` and `lastUsedSerial`

**Key Function**: `tryAssignBindlessSlot()` (`VulkanTextureManager.cpp:1274-1345`) handles the actual slot allocation logic, including reclaiming slots from non-resident textures under pressure.

### 5.3 Slot Assignment on Upload

**Function**: `VulkanTextureManager::onTextureResident(int textureHandle)`

**Called At**: After successful upload in `flushPendingUploads()`, when texture becomes resident

**Behavior** (`VulkanTextureManager.cpp:1474-1480`):
- Currently an assertion-only function that verifies the texture is in `m_residentTextures`
- Slot assignment is handled separately by `getBindlessSlotIndex()` when the texture is first requested

**Retry Logic** (`VulkanTextureManager.cpp:1256-1272`):
- `retryPendingBindlessSlots()` called at frame start (line 658 in `flushPendingUploads`)
- Iterates `m_pendingBindlessSlots` and attempts assignment via `tryAssignBindlessSlot()`
- Removes unavailable textures from the pending set
- Uses `allowResidentEvict=true` to allow LRU eviction of resident textures under slot pressure

### 5.4 Slot Release

**Slots Released When**:
- Texture deleted (`deleteTexture()`)
- Texture marked unavailable (`markUnavailable()`)
- Bitmap released (`releaseBitmap()`)

**Release Process**:
1. Remove from `m_bindlessSlots` map
2. If dynamic slot → add to `m_freeBindlessSlots`
3. Descriptor remains pointing to fallback until next update

---

## 6. Fallback Texture Handling

### 6.1 Fallback Texture

**Purpose**: Prevents accessing destroyed `VkImage`/`VkImageView` resources

**Creation** (`VulkanTextureManager.cpp:580-586`):
- 1x1 black texture (`RGBA: 0,0,0,255`)
- Created at initialization
- Always valid, never destroyed

**Usage**: Returned when:
- Texture handle is invalid
- Texture is unavailable
- Texture not yet resident
- Bindless slot exhausted

### 6.2 Default Textures

**Default Base** (`VulkanTextureManager.cpp:588-593`):
- 1x1 white texture (`RGBA: 255,255,255,255`)
- Used for untextured draws

**Default Normal** (`VulkanTextureManager.cpp:595-600`):
- 1x1 flat normal (`RGBA: 128,128,255,255`)
- Tangent-space normal (0,0,1) remapped to [0,1]

**Default Specular** (`VulkanTextureManager.cpp:602-607`):
- 1x1 dielectric F0 (`RGBA: 10,10,10,0`)
- Default specular reflectance (~0.04)

### 6.3 Fallback Descriptor

**Function**: `VulkanTextureManager::fallbackDescriptor(const SamplerKey& samplerKey)`

**Returns**: `vk::DescriptorImageInfo` pointing to fallback texture with requested sampler

**Usage**: Used in bindless descriptor updates when texture is unavailable or not resident

---

## 7. Usage Patterns

### 7.1 Queue Texture for Upload

```cpp
// Queue texture for upload (will be processed at frame start)
textureManager->queueTextureUpload(bitmapHandle, currentFrameIndex, samplerKey);

// Upload happens automatically during flushPendingUploads()
```

### 7.2 Preload Texture Immediately

```cpp
// Preload texture synchronously (may queue if staging fails)
bool success = textureManager->preloadTexture(bitmapHandle, isAABitmap);
if (!success) {
    // Out of memory or texture unavailable
}
```

### 7.3 Get Bindless Slot

```cpp
// Get bindless slot index (may return fallback slot 0)
uint32_t slot = textureManager->getBindlessSlotIndex(textureHandle);

// Use slot in model push constants
pushConstants.baseMapIndex = slot;
```

### 7.4 Check Residency

```cpp
// Check if texture has a bindless slot assigned
bool hasSlot = textureManager->hasBindlessSlot(baseFrameHandle);
// Note: A slot can be assigned before the texture is resident (slot points to fallback).
// To check true residency, examine m_residentTextures directly (internal) or use
// the return value from getBindlessSlotIndex() combined with descriptor contents.
```

### 7.5 Get Texture Descriptor

```cpp
// Get descriptor for resident texture
vk::DescriptorImageInfo info = textureManager->getTextureDescriptorInfo(
    textureHandle, 
    samplerKey);

// Use in push descriptor or bindless update
```

---

## 8. Common Issues

### Issue 1: Texture Never Uploads

**Symptoms**: Texture remains in `m_pendingUploads` across frames

**Causes**:
- Staging buffer consistently exhausted
- Texture too large for staging buffer
- Upload deferred indefinitely

**Debugging**:
```cpp
// Check staging buffer usage (VulkanRingBuffer::remaining() at line 69)
vk::DeviceSize remaining = frame.stagingBuffer().remaining();
Warning(LOCATION, "Staging buffer remaining: %zu bytes", static_cast<size_t>(remaining));

// Check pending uploads
Warning(LOCATION, "Pending uploads: %zu", m_pendingUploads.size());
```

**Solutions**:
- Increase `STAGING_RING_SIZE` in `VulkanRenderer.h` (currently 12 MB at line 199)
- Reduce texture sizes
- Preload critical textures

### Issue 2: Bindless Slot Exhaustion

**Symptoms**: All textures return slot 0 (fallback)

**Causes**:
- More than 1020 dynamic textures (1024 - 4 reserved)
- Slots not released when textures deleted

**Debugging**:
```cpp
// Check free slots
Warning(LOCATION, "Free bindless slots: %zu", m_freeBindlessSlots.size());
Warning(LOCATION, "Assigned slots: %zu", m_bindlessSlots.size());
```

**Solutions**:
- Increase `kMaxBindlessTextures` (requires device limit check)
- Ensure textures are deleted when no longer needed
- Use texture atlasing to reduce slot count

### Issue 3: Texture Unavailable Forever

**Symptoms**: Texture marked unavailable, never retries

**Causes**:
- Invalid bitmap handle
- Unsupported format
- Permanent failure (lock failed, etc.)

**Debugging**:
```cpp
// Check unavailable reason
auto it = m_unavailableTextures.find(baseFrame);
if (it != m_unavailableTextures.end()) {
    Warning(LOCATION, "Texture unavailable: reason=%d", 
            static_cast<int>(it->second.reason));
}
```

**Solutions**:
- Fix bitmap handle
- Convert unsupported formats
- Release and recreate texture handle

### Issue 4: Stale Descriptors

**Symptoms**: Texture deleted but descriptor still points to old image

**Causes**:
- Descriptor not updated after texture deletion
- Deferred release not collected

**Solutions**:
- Ensure `collect(completedSerial)` called at frame start
- Verify descriptor updates happen after texture state changes

---

## Appendix: Key Functions Reference

| Function | Purpose | When Called |
|----------|---------|-------------|
| `queueTextureUpload()` | Queue texture for upload | When texture is needed |
| `preloadTexture()` | Upload texture immediately | Preload critical textures |
| `flushPendingUploads()` | Process all pending uploads | Frame start |
| `getBindlessSlotIndex()` | Get bindless slot for texture | During model rendering |
| `deleteTexture()` | Delete texture and release slot | When texture no longer needed |
| `releaseBitmap()` | Release texture (handle reuse) | When bmpman slot freed |
| `collect()` | Clean up retired textures | Frame start, after GPU completion |

---

## References

- `code/graphics/vulkan/VulkanTextureManager.h` - Residency structures and API
- `code/graphics/vulkan/VulkanTextureManager.cpp` - State transition and upload logic
- `code/graphics/vulkan/VulkanConstants.h` - Bindless slot constants (`kMaxBindlessTextures`, reserved slots)
- `code/graphics/vulkan/VulkanRenderer.h` - Staging buffer size (`STAGING_RING_SIZE`)
- `code/graphics/vulkan/VulkanRingBuffer.h` - Staging ring buffer API
- `docs/DESIGN_PHILOSOPHY.md` - State-as-location pattern

