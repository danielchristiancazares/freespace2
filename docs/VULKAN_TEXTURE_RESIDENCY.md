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

The texture residency system manages GPU texture resources using a **state-as-location** pattern. Texture identity is a
validated `TextureId` (bmpman base frame handle `>= 0`). Textures exist in one of these containers, and their state is
determined entirely by container membership:

| Container | State | Description |
|-----------|-------|-------------|
| `m_bitmaps` | Resident (bitmap) | GPU-resident sampled bitmap textures ready for use |
| `m_targets` | Resident (RTT) | GPU-resident bmpman render targets (image + views + metadata) |
| `m_pendingUploads` | Pending | Textures queued for upload to GPU (unique FIFO) |
| `m_permanentlyRejected` | Rejected | Inputs outside supported upload domain (do not auto-retry) |

**Key Principle**: Texture state is determined by container membership, not by flags or enums. A texture is resident if
and only if it exists in `m_bitmaps` or `m_targets`. This eliminates state-flag mismatches by construction.

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

### 2.2 UsageTracking

Wraps usage metadata used for safe LRU eviction:

```cpp
struct UsageTracking {
    uint32_t lastUsedFrame = 0;  // Frame index when last referenced
    uint64_t lastUsedSerial = 0; // GPU submission serial of last use
};
```

**LRU Tracking**: `lastUsedFrame` and `lastUsedSerial` enable safe eviction under bindless slot pressure. A texture is
safe to evict when `lastUsedSerial <= completedSerial` (GPU has finished all work referencing it).

### 2.3 Permanently Rejected Inputs

Some failures are **domain-invalid** under the current upload algorithm (e.g. a texture too large for the per-frame
staging buffer). These are recorded in `m_permanentlyRejected` as a `std::unordered_set<TextureId>` so they are not
automatically retried.

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

**Containers**:
- `std::unordered_map<TextureId, BitmapTexture, TextureIdHasher> m_bitmaps`
- `std::unordered_map<TextureId, RenderTargetTexture, TextureIdHasher> m_targets`

**Properties**:
- GPU resources (`VkImage`, `VkDeviceMemory`, `VkImageView`) are allocated
- Image layout is `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
- Can be bound to descriptors immediately
- May have a bindless slot assignment (dynamic slots only; assignment happens at upload-phase safe points)

**Lifetime**: Texture remains resident until explicitly deleted (`deleteTexture()`), retired due to LRU eviction, or
released when bmpman frees the handle (`releaseBitmap()`).

### 3.2 Pending Upload

**Container**: `PendingUploadQueue m_pendingUploads` (unique FIFO)

**Properties**:
- Texture id is queued for upload (duplicates are unrepresentable by construction)
- No GPU resources allocated yet
- Upload occurs during `flushPendingUploads()` at frame start
- May be deferred to next frame if staging buffer is exhausted

**Transition Triggers**:
- `queueTextureUpload()` - Explicitly queue a texture for upload
- `queueTextureUploadBaseFrame()` - Queue by base frame handle directly
- `VulkanTextureBindings::descriptor()` / `VulkanTextureBindings::bindlessIndex()` - May queue upload when not resident

### 3.3 Permanently Rejected (Domain Invalid)

**Container**: `std::unordered_set<TextureId, TextureIdHasher> m_permanentlyRejected`

These are inputs outside the supported upload domain for the current algorithm. Examples:

- Texture exceeds staging buffer capacity
- Invalid texture-array shape (mismatched dimensions/compression across frames)

**Properties**:
- Not automatically retried
- Cleared on `releaseBitmap()` to prevent handle-reuse poisoning

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
                    (explicit or via VulkanTextureBindings)
                             |
                             v
                    +------------------+
                    |  Pending Upload  |
                    |  (m_pendingUploads)
                    +--------+---------+
                             |
                    flushPendingUploads()
                             |
                +------------+------------------------+
                |                                     |
         [Upload Success]                  [Domain Invalid]
                |                                     |
                v                                     v
    +------------------+                    +----------------------+
    |   Resident       |                    | Permanently Rejected  |
    | (m_bitmaps/m_targets)                | (m_permanentlyRejected)|
    +--------+---------+                    +----------------------+
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
3. Check if permanently rejected - skip if so
4. Warm sampler cache for later descriptor requests
5. Enqueue into `m_pendingUploads` (unique FIFO; duplicates are rejected by construction)

**Key Point**: Queuing is idempotent. Multiple calls for the same handle are safe and coalesce to a single upload.

### 4.3 Flush Pending Uploads

**Function**: `VulkanTextureManager::flushPendingUploads(const UploadCtx& ctx)`

**Called At**: Frame start, after fence wait, before descriptor sync (via `VulkanTextureUploader::flushPendingUploads()`)

**Process**:

1. **Process Retirements**: `processPendingRetirements()` handles textures marked for deletion in the previous frame
2. **Iterate Pending Uploads**:
   - Skip if already resident
   - Skip if permanently rejected (`m_permanentlyRejected`)
   - Validate bmpman handle via `bm_get_base_frame()`
   - For array textures, validate all frames have matching dimensions/compression
   - Check staging buffer capacity and per-frame budget
   - Lock bitmap data via `bm_lock()`
   - Create GPU image, memory, and view
   - Record copy commands to transfer staging data to GPU
   - Transition layout: `UNDEFINED` -> `TRANSFER_DST` -> `SHADER_READ_ONLY`
   - Move into `m_bitmaps`
3. **Assign Bindless Slots**: `assignBindlessSlots(const UploadCtx&)` resolves pending slot requests at this upload-phase
   safe point (after uploads/retirements, before descriptor sync).

**Failure Handling**:
- Staging buffer exhausted: defer to next frame (remains queued in `m_pendingUploads`)
- Domain invalid (e.g. too large for staging, invalid array): insert into `m_permanentlyRejected`
- Transient errors (e.g. `bm_lock` failure): treat as absence (not retried automatically; callers may request later)

### 4.4 Permanent Rejection

**Mechanism**: `m_permanentlyRejected` (`std::unordered_set<TextureId, TextureIdHasher>`)

**Key Point**: Rejection is for domain-invalid inputs under the current algorithm, not for transient failures.
Entries are cleared on `releaseBitmap()` to prevent handle reuse poisoning.

### 4.5 Delete Texture

**Function**: `VulkanTextureManager::deleteTexture(int bitmapHandle)`

**Behavior**:
1. Resolve base frame handle
2. Remove from `m_permanentlyRejected`, `m_bindlessRequested`, `m_pendingUploads`
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

**Function**: `VulkanTextureManager::retireTexture(TextureId id, uint64_t retireSerial)`

**Behavior**:
1. Release bindless slot (return to free list if dynamic)
2. Remove from `m_bitmaps` or `m_targets` immediately
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
- Synchronous fence wait after submission (no queue-wide `waitIdle()`)
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

### 6.2 Ownership Model

- **Dynamic slot ownership**: `m_bindlessSlots` maps `TextureId -> uint32_t` for *dynamic* slots only (slot `>= 4`).
  Reserved slots (0-3) are constants and never appear in the map.
- **Requests**: `m_bindlessRequested` is a `std::unordered_set<TextureId>` representing "this texture should get a slot
  when possible". This is draw-path safe (CPU-only; no allocation/eviction).

### 6.3 Draw Path (Lookup Only)

Draw paths do not allocate or evict slots. They:

1. Call `requestBindlessSlot(TextureId)` (records intent only)
2. Read `tryGetBindlessSlot(TextureId)` (or use `VulkanTextureBindings::bindlessIndex`)
3. Choose fallback slot 0 if no slot exists yet

This may introduce a one-frame delay for slot activation for already-resident textures. Upload-phase safe points assign
slots.

### 6.4 Upload-Phase Safe Point (Allocation/Eviction)

`VulkanTextureManager::flushPendingUploads(const UploadCtx&)` calls `assignBindlessSlots(const UploadCtx&)` after
processing retirements/uploads. This is the only place that mutates `m_bindlessSlots`.

Assignment uses:
- A free-slot pool (`m_freeBindlessSlots`), or
- LRU eviction of a **bitmap** texture that is GPU-safe to retire (`lastUsedSerial <= completedSerial`)

Render targets are excluded from eviction (pinned by container membership in `m_targets`).

### 6.5 Slot Release

Slots are released when a texture is retired or released:
- The `TextureId -> slot` mapping is erased
- The slot is returned to `m_freeBindlessSlots`

`releaseBitmap()` drops CPU-side mappings immediately to avoid bmpman handle reuse poisoning; GPU destruction remains
serial-gated via deferred release.

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

LRU eviction is triggered only during upload-phase safe points when:
1. A resident texture needs a bindless slot (`m_bindlessRequested`)
2. No free dynamic slots are available (`m_freeBindlessSlots` is empty)

### 10.2 Eviction Criteria

A texture is eligible for eviction when:
- `lastUsedSerial <= completedSerial` (GPU has finished all work referencing it)
- It is not a render target (render targets are pinned)
- It is not in the reserved slot range (0-3)

### 10.3 Eviction Selection

The oldest texture by `lastUsedFrame` is selected:
```cpp
for (const auto& [id, slot] : m_bindlessSlots) {
    (void)slot;
    if (m_targets.find(id) != m_targets.end()) {
        continue; // Skip render targets (pinned)
    }
    auto bmpIt = m_bitmaps.find(id);
    if (bmpIt == m_bitmaps.end()) {
        continue;
    }
    const auto& usage = bmpIt->second.usage;
    if (usage.lastUsedSerial <= m_completedSerial) {
        if (usage.lastUsedFrame < oldestFrame) {
            oldestFrame = usage.lastUsedFrame;
            oldestId = id;
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
textureManager->requestBindlessSlot(id);
auto slotOpt = textureManager->tryGetBindlessSlot(id);
uint32_t slot = slotOpt.value_or(kBindlessTextureSlotFallback);

// Use slot in model push constants
pushConstants.baseMapIndex = slot;
```

### 12.4 Check Bindless Slot Assignment

```cpp
// Check if texture has a bindless slot assigned
bool hasSlot = textureManager->hasBindlessSlot(id);
```

### 12.5 Get Texture Descriptor

```cpp
// Get descriptor for resident texture (for push descriptors)
auto info = textureManager->tryGetResidentDescriptor(id, samplerKey);
vk::DescriptorImageInfo resolved = info.value_or(textureManager->fallbackDescriptor(samplerKey));
```

### 12.6 Mark Texture Used

```cpp
// Update LRU tracking for a texture (prevents premature eviction)
textureManager->markTextureUsed(id, currentFrameIndex);
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
mprintf(("Pending slot requests: %zu\n", m_bindlessRequested.size()));
```

**Solutions**:
- Increase `kMaxBindlessTextures` in `VulkanConstants.h` (requires device limit check)
- Ensure `releaseBitmap()` is called when bmpman frees handles
- Use texture atlasing to reduce unique texture count

### Issue 3: Texture Permanently Rejected

**Symptoms**: Texture never becomes resident; repeated sampling uses fallback

**Causes**:
- Domain invalid under current upload algorithm:
  - Array texture has mismatched frame dimensions/compression
  - Texture exceeds staging buffer capacity

**Debugging**:
```cpp
auto idOpt = TextureId::tryFromBaseFrame(baseFrame);
if (idOpt && m_permanentlyRejected.find(*idOpt) != m_permanentlyRejected.end()) {
    mprintf(("Texture %d permanently rejected by upload algorithm\n", baseFrame));
}
```

**Solutions**:
- Ensure array texture frames have consistent dimensions and format
- Reduce texture size or adjust staging strategy if needed

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
| `requestBindlessSlot()` / `tryGetBindlessSlot()` | Bindless slot ownership (lookup-only on draw path) | Model rendering (push constants) |
| `tryGetResidentDescriptor()` | Get descriptor for push descriptor | Push descriptor binding |
| `markTextureUsed()` | Update LRU tracking | When texture is referenced |
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

The texture manager refactor plan in `docs/PLAN_REFACTOR_TEXTURE_MANAGER.md` has been implemented. This document now
describes the refactored, type-driven architecture (TextureId identity, unique upload queue, upload-phase bindless slot
allocation, unified render target ownership, and removal of queue-wide stalls).
