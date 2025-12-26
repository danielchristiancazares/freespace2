# Vulkan Texture Residency State Machine

This document describes the texture residency system in the Vulkan renderer, covering state transitions, upload batching, bindless slot assignment, fallback handling, and render target management.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Core Data Structures](#2-core-data-structures)
3. [Residency States](#3-residency-states)
4. [State Transitions](#4-state-transitions)
5. [Upload Batching](#5-upload-batching)
6. [Bindless Slot Assignment](#6-bindless-slot-assignment)
7. [Fallback Texture Handling](#7-fallback-texture-handling)
8. [Render Targets](#8-render-targets)
9. [Texture Format Support](#9-texture-format-support)
10. [LRU Eviction Strategy](#10-lru-eviction-strategy)
11. [Thread Safety](#11-thread-safety)
12. [Usage Patterns](#12-usage-patterns)
13. [Common Issues](#13-common-issues)
14. [Performance Considerations](#14-performance-considerations)

---

## 1. Overview

The texture residency system manages GPU texture resources using a **state-as-location** pattern. Textures exist in one of three containers, and their state is determined entirely by container membership:

| Container | State | Description |
|-----------|-------|-------------|
| `m_residentTextures` | Resident | GPU-resident textures ready for use |
| `m_pendingUploads` | Pending | Textures queued for upload to GPU |
| `m_unavailableTextures` | Unavailable | Textures that cannot be uploaded (permanent failure) |

**Key Principle**: Texture state is determined by container membership, not by flags or enums. A texture is resident if and only if it exists in `m_residentTextures`. This eliminates state-flag mismatches by construction.

**Source Files**:

| File | Purpose |
|------|---------|
| `code/graphics/vulkan/VulkanTextureManager.h` | Residency tracking structures and public API |
| `code/graphics/vulkan/VulkanTextureManager.cpp` | State transition and upload logic |
| `code/graphics/vulkan/VulkanConstants.h` | Bindless slot constants |
| `code/graphics/vulkan/VulkanRenderer.h` | Staging buffer size constant |

---

## 2. Core Data Structures

### 2.1 VulkanTexture

The `VulkanTexture` structure holds all GPU resources for a single texture:

```cpp
struct VulkanTexture {
    vk::UniqueImage image;           // GPU image resource
    vk::UniqueDeviceMemory memory;   // Allocated device memory
    vk::UniqueImageView imageView;   // View for shader sampling
    vk::Sampler sampler;             // Borrowed from sampler cache (not owned)
    vk::ImageLayout currentLayout;   // Current image layout (typically eShaderReadOnlyOptimal)
    uint32_t width;                  // Texture width in pixels
    uint32_t height;                 // Texture height in pixels
    uint32_t layers;                 // Array layer count (1 for non-arrays)
    uint32_t mipLevels;              // Mipmap level count
    vk::Format format;               // Vulkan pixel format
};
```

**Ownership**: The `image`, `memory`, and `imageView` are owned via `vk::Unique*` wrappers. The `sampler` is borrowed from the manager's sampler cache and must not be destroyed independently.

### 2.2 ResidentTexture

Wraps `VulkanTexture` with LRU tracking metadata:

```cpp
struct ResidentTexture {
    VulkanTexture gpu;               // GPU resources
    uint32_t lastUsedFrame = 0;      // Frame index when last referenced
    uint64_t lastUsedSerial = 0;     // GPU submission serial of last use
};
```

**LRU Tracking**: The `lastUsedFrame` and `lastUsedSerial` fields enable safe eviction of textures under bindless slot pressure. A texture is safe to evict when `lastUsedSerial <= completedSerial` (GPU has finished all work referencing it).

### 2.3 UnavailableTexture

Records the reason a texture cannot be uploaded:

```cpp
struct UnavailableTexture {
    UnavailableReason reason = UnavailableReason::InvalidHandle;
};
```

### 2.4 SamplerKey

Identifies a unique sampler configuration:

```cpp
struct SamplerKey {
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;
};
```

Samplers are cached by key in `m_samplerCache`. The `getOrCreateSampler()` method returns a cached sampler or creates a new one. All samplers use:
- Trilinear filtering (linear min/mag/mip)
- Anisotropic filtering disabled (can be enabled per-sampler if needed)
- `VK_LOD_CLAMP_NONE` to allow full mipmap chain sampling

---

## 3. Residency States

### 3.1 Resident Texture

**Container**: `std::unordered_map<int, ResidentTexture> m_residentTextures`

**Properties**:
- GPU resources (`VkImage`, `VkDeviceMemory`, `VkImageView`) are allocated
- Image layout is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
- Can be bound to descriptors immediately
- Has a bindless slot assignment (if requested via `getBindlessSlotIndex()`)

**Lifetime**: Texture remains resident until explicitly deleted (`deleteTexture()`), retired due to LRU eviction, or released when bmpman frees the handle (`releaseBitmap()`).

### 3.2 Pending Upload

**Container**: `std::vector<int> m_pendingUploads`

**Properties**:
- Texture handle is queued for upload
- No GPU resources allocated yet
- Upload occurs during `flushPendingUploads()` at frame start
- May be deferred to next frame if staging buffer is exhausted

**Transition Triggers**:
- `queueTextureUpload()` - Explicitly queue a texture for upload
- `queueTextureUploadBaseFrame()` - Queue by base frame handle directly
- `getBindlessSlotIndex()` - Implicitly queues if texture is not resident

### 3.3 Unavailable Texture

**Container**: `std::unordered_map<int, UnavailableTexture> m_unavailableTextures`

**Unavailable Reasons** (defined in `UnavailableReason` enum):

| Reason | Description |
|--------|-------------|
| `InvalidHandle` | Bitmap handle is invalid or not found in bmpman |
| `InvalidArray` | Array texture has mismatched frame dimensions or formats |
| `BmpLockFailed` | Failed to lock bitmap data via `bm_lock()` |
| `TooLargeForStaging` | Texture exceeds staging buffer capacity (12 MB default) |
| `UnsupportedFormat` | Pixel format not supported by Vulkan |

**Properties**:
- Never retried (permanently unavailable under current algorithm)
- Does not consume bindless slots
- Returns fallback descriptor (slot 0) when sampled

---

## 4. State Transitions

### 4.1 Transition Diagram

```
                    +------------------+
                    |   Not Tracked    |
                    |  (no container)  |
                    +--------+---------+
                             |
                    queueTextureUpload()
                    getBindlessSlotIndex()
                             |
                             v
                    +------------------+
                    |  Pending Upload  |
                    |  (m_pendingUploads)
                    +--------+---------+
                             |
                    flushPendingUploads()
                             |
                +------------+------------+
                |                         |
         [Upload Success]          [Upload Failure]
                |                         |
                v                         v
    +------------------+      +------------------+
    |   Resident       |      |   Unavailable    |
    | (m_residentTextures)    | (m_unavailableTextures)
    +--------+---------+      +------------------+
             |
    deleteTexture() /
    releaseBitmap() /
    LRU eviction
             |
             v
    +------------------+
    |   Deferred       |
    |   Release        |
    | (DeferredReleaseQueue)
    +------------------+
             |
    collect(completedSerial)
             |
             v
    +------------------+
    |   Destroyed      |
    +------------------+
```

### 4.2 Queue Upload

**Function**: `VulkanTextureManager::queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex, const SamplerKey& samplerKey)`

**Behavior**:
1. Resolve base frame handle via `bm_get_base_frame()` (for animated textures)
2. Check if already resident - skip if so
3. Check if unavailable - skip if so
4. Warm sampler cache for later descriptor requests
5. Add to `m_pendingUploads` vector if not already queued

**Key Point**: Queuing is idempotent. Multiple calls for the same handle are safe and coalesce to a single upload.

### 4.3 Flush Pending Uploads

**Function**: `VulkanTextureManager::flushPendingUploads(const UploadCtx& ctx)`

**Called At**: Frame start, after fence wait, before descriptor sync (via `VulkanTextureUploader::flushPendingUploads()`)

**Process**:

1. **Process Retirements**: `processPendingRetirements()` handles textures marked for deletion in the previous frame
2. **Retry Bindless Slots**: `retryPendingBindlessSlots()` resolves any pending slot requests from prior frames
3. **Iterate Pending Uploads**:
   - Skip if already resident or unavailable
   - Validate bitmap handle via `bm_get_base_frame()`
   - For array textures, validate all frames have matching dimensions/format
   - Check staging buffer capacity
   - Lock bitmap data via `bm_lock()`
   - Create GPU image, memory, and view
   - Record copy commands to transfer staging data to GPU
   - Transition layout: `UNDEFINED` -> `TRANSFER_DST` -> `SHADER_READ_ONLY`
   - Move to `m_residentTextures`

4. **Handle Failures**:
   - Staging buffer exhausted: defer to next frame (stays in `m_pendingUploads`)
   - Invalid handle: mark unavailable with `InvalidHandle`
   - Lock failed: mark unavailable with `BmpLockFailed`
   - Array mismatch: mark unavailable with `InvalidArray`
   - Exceeds staging capacity: mark unavailable with `TooLargeForStaging`

### 4.4 Mark Unavailable

**Function**: Internal lambda `markUnavailable()` in `flushPendingUploads()`

**Behavior**:
1. Insert into `m_unavailableTextures` with reason
2. Remove from `m_pendingBindlessSlots`
3. Release bindless slot if assigned (return to free list if dynamic)

**Key Point**: Unavailable textures never retry. They remain unavailable until the handle is released by bmpman and potentially reused for a different texture.

### 4.5 Delete Texture

**Function**: `VulkanTextureManager::deleteTexture(int bitmapHandle)`

**Behavior**:
1. Resolve base frame handle
2. Remove from `m_unavailableTextures`, `m_pendingBindlessSlots`, `m_pendingUploads`
3. Add to `m_pendingRetirements` for deferred slot reuse

**Deferred Retirement**: The actual slot release and GPU resource destruction are deferred to `processPendingRetirements()` at the next frame start, ensuring slot reuse happens at a safe synchronization point.

### 4.6 Release Bitmap

**Function**: `VulkanTextureManager::releaseBitmap(int bitmapHandle)`

**Called At**: When bmpman slot becomes `BM_TYPE_NONE` (handle about to be reused)

**Behavior**:
1. Remove from all pending containers immediately
2. If resident: call `retireTexture()` which defers GPU resource destruction via `DeferredReleaseQueue`
3. If not resident but has bindless slot: release slot immediately to free list

**Critical**: Must be called before handle reuse to prevent texture collisions. Unlike `deleteTexture()`, this is a hard lifecycle boundary that drops all cache state immediately since bmpman may reuse the handle on the very next call.

### 4.7 Retire Texture

**Function**: `VulkanTextureManager::retireTexture(int textureHandle, uint64_t retireSerial)`

**Behavior**:
1. Release bindless slot (return to free list if dynamic)
2. Remove from `m_residentTextures` immediately
3. Enqueue GPU resources to `DeferredReleaseQueue` with the retire serial
4. Resources are destroyed when `collect(completedSerial)` is called and `completedSerial >= retireSerial`

---

## 5. Upload Batching

### 5.1 Upload Timing

Uploads are batched at frame start to minimize GPU stalls:

**Frame Start Sequence** (in `VulkanRenderer::beginFrame()`):
```
beginFrame()
+-- reset command pool
+-- begin command buffer
+-- collect() deferred releases
+-- setSafeRetireSerial() / setCurrentFrameIndex()
+-- flushPendingUploads()        <-- All pending uploads processed here
+-- beginModelDescriptorSync()   <-- Descriptors updated after uploads
+-- renderingSession->beginFrame()
```

**Why Frame Start**:
- Staging buffer is reset (fresh capacity available)
- Command buffer is empty (no pipeline stalls from interrupting rendering)
- Descriptors can be updated after uploads complete
- GPU can process uploads in parallel with CPU command recording

### 5.2 Staging Buffer Allocation

**Staging Ring Buffer**: Per-frame, 12 MB capacity (defined as `STAGING_RING_SIZE` in `VulkanRenderer.h`)

**Allocation Pattern**:
```cpp
auto allocOpt = frame.stagingBuffer().try_allocate(layerSize);
if (!allocOpt) {
    // Staging exhausted - defer to next frame
    remaining.push_back(baseFrame);
    continue;
}

// Copy texture data to staging
std::memcpy(alloc.mapped, pixelData, layerSize);

// Record buffer-to-image copy
cmd.copyBufferToImage(
    frame.stagingBuffer().buffer(),
    record.gpu.image.get(),
    vk::ImageLayout::eTransferDstOptimal,
    regions);
```

**Staging Budget Tracking**:
- `stagingUsed` is accumulated as uploads proceed
- Uploads that exceed remaining capacity are deferred
- Remaining uploads stay in `m_pendingUploads` for next frame

### 5.3 Upload Command Recording

**In-Frame Uploads** (via `flushPendingUploads()`):
- Recorded into main command buffer (not separate transfer queue)
- Uses `VK_PIPELINE_STAGE_2` barriers for synchronization
- Layout transitions: `UNDEFINED` -> `TRANSFER_DST` -> `SHADER_READ_ONLY`

**Immediate Uploads** (via `uploadImmediate()`):
- Creates dedicated command buffer on transfer queue
- Synchronous wait (`m_transferQueue.waitIdle()`) after submission
- Used for preload operations (textures needed before first frame)

---

## 6. Bindless Slot Assignment

### 6.1 Slot Layout

**Slot Allocation** (defined in `VulkanConstants.h`):

| Slot | Purpose | Constant |
|------|---------|----------|
| 0 | Fallback texture (black, always valid) | `kBindlessTextureSlotFallback` |
| 1 | Default base texture (white) | `kBindlessTextureSlotDefaultBase` |
| 2 | Default normal texture (flat normal) | `kBindlessTextureSlotDefaultNormal` |
| 3 | Default specular texture (dielectric F0) | `kBindlessTextureSlotDefaultSpec` |
| 4-1023 | Dynamic texture assignments | `kBindlessFirstDynamicTextureSlot` and above |

**Capacity**: 1020 dynamic slots available (1024 total minus 4 reserved)

### 6.2 Assignment Algorithm

**Function**: `VulkanTextureManager::getBindlessSlotIndex(int textureHandle)`

**Process**:

1. **Check Unavailability**: If texture is in `m_unavailableTextures`, return slot 0 (fallback)

2. **Try Slot Assignment** (via `tryAssignBindlessSlot()`):
   - If already has slot assigned, return it
   - If free slots available, pop from `m_freeBindlessSlots` and assign
   - If no free slots, try reclaiming from non-resident textures

3. **Handle Assignment Failure**:
   - Add to `m_pendingBindlessSlots` for retry at next frame start
   - Queue for upload if not resident
   - Return slot 0 (fallback) until assignment succeeds

4. **Update LRU Tracking**: If resident, update `lastUsedFrame` and `lastUsedSerial`

### 6.3 Slot Reclamation

**Function**: `tryAssignBindlessSlot(int textureHandle, bool allowResidentEvict)`

**Non-Resident Reclaim** (always attempted):
- Iterates `m_bindlessSlots` looking for handles not in `m_residentTextures`
- Reclaims the first found slot and returns it to the free list
- Safe during rendering: non-resident slots point to fallback anyway

**Resident Eviction** (only when `allowResidentEvict=true`, i.e., at frame start):
- Finds the oldest texture by `lastUsedFrame` where `lastUsedSerial <= completedSerial`
- Retires that texture, freeing its slot
- Render targets are excluded from eviction (they are long-lived)

### 6.4 Slot Release

**Slots Released When**:
- Texture deleted (`deleteTexture()`)
- Texture marked unavailable (`markUnavailable()`)
- Bitmap released (`releaseBitmap()`)
- Texture retired for LRU eviction

**Release Process**:
1. Remove from `m_bindlessSlots` map
2. If dynamic slot (>= 4), add to `m_freeBindlessSlots`
3. Descriptor remains pointing to fallback until next update

---

## 7. Fallback Texture Handling

### 7.1 Fallback Texture

**Purpose**: Prevents accessing destroyed `VkImage`/`VkImageView` resources when sampling retired or unavailable textures.

**Properties**:
- 1x1 black texture (RGBA: 0, 0, 0, 255)
- Created at `VulkanTextureManager` initialization
- Always valid, never destroyed
- Bound to slot 0 in the bindless descriptor array

**Usage Scenarios**:
- Texture handle is invalid
- Texture is marked unavailable
- Texture is not yet resident (pending upload)
- Bindless slot exhausted (no free slots)

### 7.2 Default Textures

| Texture | Slot | RGBA Value | Purpose |
|---------|------|------------|---------|
| Fallback | 0 | (0, 0, 0, 255) | Black, prevents sampling destroyed resources |
| Default Base | 1 | (255, 255, 255, 255) | White, for untextured draws |
| Default Normal | 2 | (128, 128, 255, 255) | Flat tangent-space normal (0, 0, 1) |
| Default Specular | 3 | (10, 10, 10, 0) | Dielectric F0 (~0.04 reflectance) |

### 7.3 Fallback Descriptor

**Function**: `VulkanTextureManager::fallbackDescriptor(const SamplerKey& samplerKey)`

**Returns**: `vk::DescriptorImageInfo` pointing to fallback texture with the requested sampler configuration.

**Usage**: Used in bindless descriptor updates when a texture is unavailable or not yet resident.

---

## 8. Render Targets

### 8.1 Overview

Render targets are GPU textures created explicitly for rendering (cockpit displays, environment maps, dynamic textures). Unlike regular textures, they are not uploaded from bmpman data but created directly on the GPU.

### 8.2 Render Target Record

```cpp
struct RenderTargetRecord {
    vk::Extent2D extent{};           // Width and height
    vk::Format format;               // Pixel format (e.g., B8G8R8A8_SRGB)
    uint32_t mipLevels = 1;          // Number of mip levels
    uint32_t layers = 1;             // Array layers (6 for cubemaps)
    bool isCubemap = false;          // True for environment maps
    std::array<vk::UniqueImageView, 6> faceViews{};  // Attachment views
};
```

### 8.3 Render Target API

| Function | Purpose |
|----------|---------|
| `createRenderTarget()` | Creates a new GPU-backed render target |
| `hasRenderTarget()` | Checks if a handle has a render target record |
| `renderTargetExtent()` | Returns the render target dimensions |
| `renderTargetFormat()` | Returns the pixel format |
| `renderTargetMipLevels()` | Returns the mip level count |
| `renderTargetAttachmentView()` | Returns the view for a specific face |
| `transitionRenderTargetToAttachment()` | Layout transition for rendering |
| `transitionRenderTargetToShaderRead()` | Layout transition for sampling |
| `generateRenderTargetMipmaps()` | Records mipmap generation commands |

### 8.4 Render Target Eviction

Render targets are excluded from LRU eviction. Their bindless slot mappings are treated as pinned because:
- They are long-lived GPU resources (cockpit displays, monitors, envmaps)
- Evicting them causes visible flicker
- They are explicitly managed by the caller

---

## 9. Texture Format Support

### 9.1 Compressed Formats

| bmpman Flag | Vulkan Format | Block Size |
|-------------|---------------|------------|
| `BMP_TEX_DXT1` | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` | 8 bytes |
| `BMP_TEX_DXT3` | `VK_FORMAT_BC2_UNORM_BLOCK` | 16 bytes |
| `BMP_TEX_DXT5` | `VK_FORMAT_BC3_UNORM_BLOCK` | 16 bytes |
| `BMP_TEX_BC7` | `VK_FORMAT_BC7_UNORM_BLOCK` | 16 bytes |

**Block-Compressed Size Calculation**:
```cpp
size_t calculateCompressedSize(uint32_t w, uint32_t h, vk::Format format) {
    const size_t blockSize = (format == vk::Format::eBc1RgbaUnormBlock) ? 8 : 16;
    const size_t blocksWide = (w + 3) / 4;
    const size_t blocksHigh = (h + 3) / 4;
    return blocksWide * blocksHigh * blockSize;
}
```

### 9.2 Uncompressed Formats

| Source Format | Vulkan Format | Notes |
|---------------|---------------|-------|
| 8-bit (AABITMAP/grayscale) | `VK_FORMAT_R8_UNORM` | Single channel, 1 byte/pixel |
| 16-bit | `VK_FORMAT_B8G8R8A8_UNORM` | Expanded to 4 bytes in upload |
| 24-bit | `VK_FORMAT_B8G8R8A8_UNORM` | Expanded to 4 bytes in upload |
| 32-bit | `VK_FORMAT_B8G8R8A8_UNORM` | Native BGRA layout |

**Upload Size Calculation**:
```cpp
size_t calculateLayerSize(uint32_t w, uint32_t h, vk::Format format) {
    if (isBlockCompressedFormat(format)) {
        return calculateCompressedSize(w, h, format);
    }
    if (format == vk::Format::eR8Unorm) {
        return static_cast<size_t>(w) * h;
    }
    return static_cast<size_t>(w) * h * 4;  // Expanded to RGBA
}
```

---

## 10. LRU Eviction Strategy

### 10.1 When Eviction Occurs

LRU eviction is triggered only when:
1. A new texture needs a bindless slot
2. No free slots are available
3. No non-resident textures have slots to reclaim
4. `allowResidentEvict=true` (only at frame-start upload phase)

### 10.2 Eviction Criteria

A texture is eligible for eviction when:
- `lastUsedSerial <= completedSerial` (GPU has finished all work referencing it)
- It is not a render target (render targets are pinned)
- It is not in the reserved slot range (0-3)

### 10.3 Eviction Selection

The oldest texture by `lastUsedFrame` is selected:
```cpp
for (const auto& [handle, slot] : m_bindlessSlots) {
    if (m_renderTargets.find(handle) != m_renderTargets.end()) {
        continue;  // Skip render targets
    }
    auto residentIt = m_residentTextures.find(handle);
    if (residentIt != m_residentTextures.end()) {
        const auto& other = residentIt->second;
        if (other.lastUsedSerial <= m_completedSerial) {
            if (other.lastUsedFrame < oldestFrame) {
                oldestFrame = other.lastUsedFrame;
                oldestHandle = handle;
            }
        }
    }
}
```

---

## 11. Thread Safety

### 11.1 Single-Threaded Design

The `VulkanTextureManager` is designed for single-threaded access from the render thread. All public methods assume:
- Caller holds exclusive access during the call
- No concurrent modifications to internal containers
- GPU synchronization is handled via serials and deferred release

### 11.2 Bmpman Interaction

Interactions with bmpman (`bm_lock()`, `bm_unlock()`, `bm_get_info()`) occur during upload and must happen on the render thread. Bmpman itself may have internal locking for multi-threaded access.

### 11.3 Deferred Release Safety

The `DeferredReleaseQueue` ensures GPU resources are not destroyed while in use:
- Resources are enqueued with a retire serial
- `collect(completedSerial)` destroys resources where `retireSerial <= completedSerial`
- The completed serial is obtained from timeline semaphore queries

---

## 12. Usage Patterns

### 12.1 Queue Texture for Upload

```cpp
// Queue texture for upload (will be processed at frame start)
textureManager->queueTextureUpload(bitmapHandle, currentFrameIndex, samplerKey);

// Upload happens automatically during flushPendingUploads()
```

### 12.2 Preload Texture Immediately

```cpp
// Preload texture synchronously (blocks until complete)
bool success = textureManager->preloadTexture(bitmapHandle, isAABitmap);
if (!success) {
    // Out of device memory - abort preloading
}
// Note: preloadTexture returns true for most failures (invalid handle, lock failed)
// to allow preloading to continue. Only OOM returns false.
```

### 12.3 Get Bindless Slot

```cpp
// Get bindless slot index (may return fallback slot 0)
uint32_t slot = textureManager->getBindlessSlotIndex(textureHandle);

// Use slot in model push constants
pushConstants.baseMapIndex = slot;
```

### 12.4 Check Bindless Slot Assignment

```cpp
// Check if texture has a bindless slot assigned
bool hasSlot = textureManager->hasBindlessSlot(baseFrameHandle);
// Note: A slot can be assigned before the texture is resident.
// The slot's descriptor points to fallback until the upload completes.
```

### 12.5 Get Texture Descriptor

```cpp
// Get descriptor for resident texture (for push descriptors)
vk::DescriptorImageInfo info = textureManager->getTextureDescriptorInfo(
    textureHandle,
    samplerKey);

if (info.imageView) {
    // Texture is resident, use descriptor
} else {
    // Texture not resident, use fallback
    info = textureManager->fallbackDescriptor(samplerKey);
}
```

### 12.6 Mark Texture Used

```cpp
// Update LRU tracking for a texture (prevents premature eviction)
textureManager->markTextureUsedBaseFrame(baseFrame, currentFrameIndex);
```

---

## 13. Common Issues

### Issue 1: Texture Never Uploads

**Symptoms**: Texture remains in `m_pendingUploads` across frames, appears as fallback (black)

**Causes**:
- Staging buffer consistently exhausted by higher-priority textures
- Texture exceeds staging buffer capacity (12 MB)
- Upload deferred indefinitely due to frame-by-frame budget limits

**Debugging**:
```cpp
// Check staging buffer remaining capacity
vk::DeviceSize remaining = frame.stagingBuffer().remaining();
mprintf(("Staging buffer remaining: %zu bytes\n", static_cast<size_t>(remaining)));

// Check pending upload count
mprintf(("Pending uploads: %zu\n", m_pendingUploads.size()));
```

**Solutions**:
- Increase `STAGING_RING_SIZE` in `VulkanRenderer.h` (currently 12 MB)
- Reduce texture sizes or use higher compression
- Preload critical textures during level load via `preloadTexture()`

### Issue 2: Bindless Slot Exhaustion

**Symptoms**: All textures return slot 0 (fallback), scene appears mostly black

**Causes**:
- More than 1020 unique textures requested simultaneously
- Slots not released when textures deleted
- Slot leak due to missing `releaseBitmap()` calls

**Debugging**:
```cpp
// Check slot usage
mprintf(("Free bindless slots: %zu\n", m_freeBindlessSlots.size()));
mprintf(("Assigned slots: %zu\n", m_bindlessSlots.size()));
mprintf(("Pending slot requests: %zu\n", m_pendingBindlessSlots.size()));
```

**Solutions**:
- Increase `kMaxBindlessTextures` in `VulkanConstants.h` (requires device limit check)
- Ensure `releaseBitmap()` is called when bmpman frees handles
- Use texture atlasing to reduce unique texture count

### Issue 3: Texture Unavailable Forever

**Symptoms**: Texture marked unavailable, never becomes resident

**Causes**:
- Invalid bitmap handle (bmpman doesn't recognize it)
- Array texture has mismatched frame dimensions
- Unsupported pixel format
- `bm_lock()` failure (corrupt or missing file)

**Debugging**:
```cpp
// Check unavailable reason
auto it = m_unavailableTextures.find(baseFrame);
if (it != m_unavailableTextures.end()) {
    const char* reasons[] = {
        "InvalidHandle", "InvalidArray", "BmpLockFailed",
        "TooLargeForStaging", "UnsupportedFormat"
    };
    mprintf(("Texture %d unavailable: %s\n", baseFrame,
             reasons[static_cast<int>(it->second.reason)]));
}
```

**Solutions**:
- Fix bitmap handle registration with bmpman
- Ensure array texture frames have consistent dimensions and format
- Convert unsupported formats to BC1/BC3/BC7 or 32-bit BGRA
- Check for missing or corrupt texture files

### Issue 4: Stale Descriptors

**Symptoms**: Texture deleted but old image still visible, or validation errors about destroyed objects

**Causes**:
- Descriptor not updated after texture deletion
- Deferred release queue not collected (stale serial)
- Missing descriptor sync after upload flush

**Solutions**:
- Ensure `collect(completedSerial)` called at frame start
- Verify `beginModelDescriptorSync()` called after `flushPendingUploads()`
- Check that `setSafeRetireSerial()` uses correct upcoming serial

---

## 14. Performance Considerations

### 14.1 Staging Buffer Size

The 12 MB staging buffer limits how many textures can be uploaded per frame. For texture-heavy scenes:
- Consider increasing `STAGING_RING_SIZE`
- Preload textures during level load
- Use compressed formats to reduce upload size

### 14.2 Bindless vs. Push Descriptors

- **Bindless**: Lower CPU overhead for scenes with many unique textures
- **Push Descriptors**: Better for frequently-changing single textures

The current implementation prefers bindless for model textures (many materials) and push descriptors for 2D/UI rendering.

### 14.3 Sampler Caching

Samplers are cached by `SamplerKey` to avoid redundant Vulkan object creation. The cache is never cleared during runtime (samplers are small and reused).

### 14.4 LRU Overhead

LRU tracking (`lastUsedFrame`, `lastUsedSerial`) adds minimal overhead per texture access. Eviction scanning is O(n) over assigned slots but only occurs under slot pressure.

---

## Appendix: Key Functions Reference

| Function | Purpose | When Called |
|----------|---------|-------------|
| `queueTextureUpload()` | Queue texture for upload | When texture is needed during draw |
| `queueTextureUploadBaseFrame()` | Queue by base frame handle | Internal, avoids redundant base frame lookup |
| `preloadTexture()` | Upload texture immediately | Level load / preload phase |
| `flushPendingUploads()` | Process all pending uploads | Frame start (via VulkanTextureUploader) |
| `getBindlessSlotIndex()` | Get bindless slot for texture | Model rendering (push constants) |
| `getTextureDescriptorInfo()` | Get descriptor for push descriptor | Push descriptor binding |
| `markTextureUsedBaseFrame()` | Update LRU tracking | When texture is referenced |
| `deleteTexture()` | Mark texture for deletion | When texture no longer needed |
| `releaseBitmap()` | Release texture (handle reuse) | When bmpman frees handle |
| `collect()` | Clean up retired textures | Frame start, after GPU completion |
| `createRenderTarget()` | Create GPU render target | Bitmap RTT creation |

---

## References

- `code/graphics/vulkan/VulkanTextureManager.h` - Residency structures and public API
- `code/graphics/vulkan/VulkanTextureManager.cpp` - State transition and upload logic
- `code/graphics/vulkan/VulkanConstants.h` - Bindless slot constants (`kMaxBindlessTextures`)
- `code/graphics/vulkan/VulkanRenderer.h` - Staging buffer size (`STAGING_RING_SIZE`)
- `code/graphics/vulkan/VulkanRingBuffer.h` - Staging ring buffer implementation
- `code/graphics/vulkan/VulkanDeferredRelease.h` - Serial-gated resource destruction
- `docs/DESIGN_PHILOSOPHY.md` - State-as-location pattern explanation

---

## Future Work

A proposed redesign of the texture ownership model is documented in `docs/vulkan/VULKAN_DESIGN_PHILOSOPHY_COMPLIANCE_PLAN.md`. The plan proposes:

- Replacing `UnavailableTexture` struct with a simple `set<int>` for permanent failures
- Removing the retry loop mechanism (`retryPendingBindlessSlots`)
- Making `getBindlessSlotIndex()` a const lookup-only method
- Moving all slot allocation to the upload phase

This document describes the **current** implementation. See the compliance plan for proposed changes.
