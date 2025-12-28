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
9. [Dynamic Texture Updates](#9-dynamic-texture-updates)
10. [Texture Format Support](#10-texture-format-support)
11. [LRU Eviction Strategy](#11-lru-eviction-strategy)
12. [Thread Safety](#12-thread-safety)
13. [Draw-Path and Upload-Path APIs](#13-draw-path-and-upload-path-apis)
14. [Debug Logging](#14-debug-logging)
15. [Usage Patterns](#15-usage-patterns)
16. [Common Issues](#16-common-issues)
17. [Performance Considerations](#17-performance-considerations)

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
| `code/graphics/vulkan/VulkanTextureId.h` | `TextureId` strong typedef for texture identity |
| `code/graphics/vulkan/VulkanTextureBindings.h` | Draw-path (`VulkanTextureBindings`) and upload-path (`VulkanTextureUploader`) wrapper APIs |
| `code/graphics/vulkan/VulkanConstants.h` | Bindless slot constants |
| `code/graphics/vulkan/VulkanRenderer.h` | Staging buffer size constant (`STAGING_RING_SIZE`) |

---

## 2. Core Data Structures

### 2.1 TextureId

`TextureId` is a strong typedef wrapping a bmpman "base frame" handle. It provides type safety and makes texture identity explicit throughout the Vulkan backend.

```cpp
class TextureId {
public:
  // Boundary constructor: validates and converts a base-frame handle (>= 0).
  // Returns std::nullopt for invalid handles (< 0).
  [[nodiscard]] static std::optional<TextureId> tryFromBaseFrame(int baseFrame);

  int baseFrame() const;

  bool operator==(const TextureId &other) const;
  bool operator!=(const TextureId &other) const;
};
```

**Key Points**:
- Only valid bmpman base frame handles (`>= 0`) can become `TextureId` values
- Invalid handles are represented as `std::nullopt`, not sentinel values
- Builtin textures (fallback, default base/normal/spec) are not represented as `TextureId`; they have dedicated descriptor APIs
- The internal `fromBaseFrameUnchecked()` method is used only where handle validity is proven by construction (container membership)

**Usage**:
```cpp
// At API boundaries: validate the handle
auto idOpt = TextureId::tryFromBaseFrame(bitmapHandle);
if (!idOpt.has_value()) {
    return; // Invalid handle - treat as absence
}
TextureId id = *idOpt;

// Within container iteration: validity is proven by membership
for (const auto& [id, texture] : m_bitmaps) {
    // id is valid by construction (it was inserted with a valid handle)
}
```

### 2.2 VulkanTexture

The `VulkanTexture` structure holds all GPU resources for a single texture:

```cpp
struct VulkanTexture {
    vk::UniqueImage image;           // GPU image resource
    vk::UniqueDeviceMemory memory;   // Allocated device memory
    vk::UniqueImageView imageView;   // View for shader sampling
    vk::Sampler sampler;             // Borrowed from sampler cache (not owned)
    vk::ImageLayout currentLayout;   // Current image layout (initially eUndefined, typically eShaderReadOnlyOptimal after upload)
    uint32_t width;                  // Texture width in pixels
    uint32_t height;                 // Texture height in pixels
    uint32_t layers;                 // Array layer count (1 for non-arrays)
    uint32_t mipLevels;              // Mipmap level count
    vk::Format format;               // Vulkan pixel format
};
```

**Ownership**: The `image`, `memory`, and `imageView` are owned via `vk::Unique*` wrappers. The `sampler` is borrowed from the manager's sampler cache and must not be destroyed independently.

### 2.3 UsageTracking

Wraps usage metadata used for safe LRU eviction:

```cpp
struct UsageTracking {
    uint32_t lastUsedFrame = 0;  // Frame index when last referenced
    uint64_t lastUsedSerial = 0; // GPU submission serial of last use
};
```

**LRU Tracking**: `lastUsedFrame` and `lastUsedSerial` enable safe eviction under bindless slot pressure. A texture is
safe to evict when `lastUsedSerial <= completedSerial` (GPU has finished all work referencing it).

### 2.4 Permanently Rejected Inputs

Some failures are **domain-invalid** under the current upload algorithm (e.g. a texture too large for the per-frame
staging buffer). These are recorded in `m_permanentlyRejected` as a `std::unordered_set<TextureId, TextureIdHasher>` so they are not
automatically retried.

**Rejection Causes**:
- Texture exceeds staging buffer capacity (12 MB)
- Array texture has mismatched frame dimensions or compression settings across frames

**Key Behavior**:
- Entries are cleared on `releaseBitmap()` to prevent handle-reuse poisoning
- Entries are cleared on `deleteTexture()` to allow re-upload attempts
- The `queueTextureUpload()` function skips textures in this set

### 2.5 SamplerKey

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

### 2.6 PendingUploadQueue

The `PendingUploadQueue` is a unique FIFO queue that prevents duplicate upload requests:

```cpp
class PendingUploadQueue {
public:
    // Returns true if newly enqueued; false if already present (idempotent).
    bool enqueue(TextureId id);

    // Returns true if the id was present and removed.
    bool erase(TextureId id);

    bool empty() const;
    std::deque<TextureId> takeAll();

private:
    std::deque<TextureId> m_fifo;                           // Insertion order
    std::unordered_set<TextureId, TextureIdHasher> m_membership;  // O(1) duplicate check
};
```

**Key Properties**:
- Enqueueing the same `TextureId` multiple times is idempotent (only the first enqueue succeeds)
- The `m_membership` set provides O(1) duplicate detection
- The `m_fifo` deque maintains insertion order for fair upload scheduling
- `takeAll()` atomically drains both containers and returns the FIFO contents

---

## 3. Residency States

### 3.1 Resident Texture

**Containers**:
- `std::unordered_map<TextureId, BitmapTexture, TextureIdHasher> m_bitmaps` - Sampled bitmap textures
- `std::unordered_map<TextureId, RenderTargetTexture, TextureIdHasher> m_targets` - Bmpman render targets

**Properties**:
- GPU resources (`VkImage`, `VkDeviceMemory`, `VkImageView`) are allocated and valid
- Image layout is typically `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` after upload completes
  - Render targets may transition through `COLOR_ATTACHMENT_OPTIMAL` and `TRANSFER_DST_OPTIMAL` during use
  - The `currentLayout` field tracks the actual layout for barrier construction
- Can be bound to descriptors immediately when in shader-read layout
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
                    |  Pending Upload  |<---------+
                    |  (m_pendingUploads)         |
                    +--------+---------+          |
                             |                    |
                    flushPendingUploads()         |
                             |                    |
                +------------+--------------------+--------+
                |            |                             |
         [Success]    [Staging Budget]           [Domain Invalid]
                |       (defer to                          |
                |        next frame)                       v
                v                              +----------------------+
    +------------------+                       | Permanently Rejected |
    |   Resident       |                       | (m_permanentlyRejected)
    | (m_bitmaps/m_targets)                    +----------------------+
    +--------+---------+
             |
    deleteTexture() -> m_pendingRetirements
    releaseBitmap() -> direct retire
    LRU eviction -> direct retire
             |
             v
    +------------------+
    |  retireTexture() |
    | (slot released)  |
    +--------+---------+
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

**Notes**:
- Staging budget exhaustion defers upload to next frame (remains in `m_pendingUploads`)
- Transient failures (e.g., `bm_lock` fails) drop from queue without re-queue; caller may re-request
- `deleteTexture()` uses `m_pendingRetirements` for safe slot reuse timing
- `releaseBitmap()` and LRU eviction call `retireTexture()` directly

### 4.2 Queue Upload

**Function Overloads**:

```cpp
// Primary: takes raw bmpman handle, resolves base frame internally
void queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex, const SamplerKey& samplerKey);

// For callers that already have a base-frame handle
void queueTextureUploadBaseFrame(int baseFrame, uint32_t currentFrameIndex, const SamplerKey& samplerKey);

// For callers that already have a validated TextureId
void queueTextureUpload(TextureId id, uint32_t currentFrameIndex, const SamplerKey& samplerKey);
```

**Behavior** (all overloads converge to the `TextureId` variant):
1. Resolve base frame handle via `bm_get_base_frame()` (if starting from raw handle)
2. Validate handle via `TextureId::tryFromBaseFrame()` - exit early if invalid
3. Check if already resident - skip if so
4. Check if permanently rejected (`m_permanentlyRejected`) - skip if so
5. Warm sampler cache for later descriptor requests via `getOrCreateSampler()`
6. Enqueue into `m_pendingUploads` (unique FIFO; duplicates are rejected by construction)

**Key Point**: Queuing is idempotent. Multiple calls for the same handle are safe and coalesce to a single upload.

### 4.3 Flush Pending Uploads

**Function**: `VulkanTextureManager::flushPendingUploads(const UploadCtx& ctx)`

**Called At**: Frame start, after fence wait, before descriptor sync (via `VulkanTextureUploader::flushPendingUploads()`)

**Process**:

1. **Process Retirements**: `processPendingRetirements()` handles textures marked for deletion in the previous frame
2. **Drain Pending Queue**: `m_pendingUploads.takeAll()` atomically extracts all pending uploads for processing
3. **Iterate Pending Uploads**:
   - Skip if already resident
   - Skip if permanently rejected (`m_permanentlyRejected`)
   - Validate bmpman handle via `bm_get_base_frame()` (skip if released)
   - Lock first frame via `bm_lock()` to determine format/dimensions
   - For array textures, validate all frames have matching dimensions/compression
   - **Two-Phase Budget Check**:
     1. Estimate total upload size before allocation
     2. If size exceeds total staging capacity: permanently reject (domain invalid)
     3. If size exceeds remaining budget for this frame: defer to next frame
     4. Attempt actual staging allocation via `try_allocate()`
   - Copy pixel data to staging buffer (with format conversion for 16/24bpp)
   - Create GPU image, memory, and view
   - Record pipeline barriers and copy commands
   - Transition layout: `UNDEFINED` -> `TRANSFER_DST` -> `SHADER_READ_ONLY`
   - Insert into `m_bitmaps` with usage tracking
4. **Re-queue Deferred**: Uploads that were deferred (budget/alloc failure) are placed in a new `PendingUploadQueue`
5. **Assign Bindless Slots**: `assignBindlessSlots(const UploadCtx&)` resolves pending slot requests at this upload-phase
   safe point (after uploads/retirements, before descriptor sync)

**Failure Handling**:
- **Staging buffer exhausted (this frame)**: defer to next frame (re-queued in remaining queue)
- **Domain invalid** (exceeds staging capacity, invalid array): insert into `m_permanentlyRejected`
- **Transient errors** (e.g. `bm_lock` failure): not re-queued (callers may request later)

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

**Frame-Start Uploads** (via `flushPendingUploads()`):
- Recorded into main command buffer before rendering begins (UploadCtx)
- Draw paths queue uploads only; they do not flush mid-frame
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

## 9. Dynamic Texture Updates

### 9.1 Overview

The `updateTexture()` function allows updating the contents of an existing resident texture without recreating GPU resources. This is used for streaming animations, NanoVG rendering, and other dynamic content.

### 9.2 API

**Function**: `VulkanTextureManager::updateTexture(const UploadCtx& ctx, int bitmapHandle, int bpp, const ubyte* data, int width, int height)`

**Called Via**: `VulkanTextureUploader::updateTexture()` (upload-phase only)

**Parameters**:
- `ctx`: Upload context (provides staging buffer and command buffer)
- `bitmapHandle`: Bmpman handle to update
- `bpp`: Source bits-per-pixel (8, 16, 24, or 32)
- `data`: Pointer to source pixel data
- `width`, `height`: Dimensions of the update region

**Returns**: `true` if update succeeded, `false` otherwise

### 9.3 Constraints

- **Must be resident or creatable**: If the texture is not already resident, a new GPU image is created
- **Single-layer only**: Multi-layer texture arrays cannot be updated (the API doesn't provide a layer index)
- **No compressed formats**: Block-compressed textures cannot be dynamically updated
- **Render targets excluded**: Bmpman render targets (`m_targets`) cannot be overwritten via this API
- **Staging budget**: Update must fit in the current frame's staging buffer

### 9.4 Format Conversion

The update path handles format conversion similar to the initial upload:

| Source bpp | Texture Format | Conversion |
|------------|----------------|------------|
| 8 | `R8_UNORM` | Direct copy or luminance extraction |
| 8 | `B8G8R8A8_UNORM` | Mask placed in red channel |
| 16 | `B8G8R8A8_UNORM` | A1R5G5B5 expanded to BGRA8 |
| 24 | `B8G8R8A8_UNORM` | RGB expanded to BGRA8 (alpha=255) |
| 32 | `B8G8R8A8_UNORM` | Direct copy |

### 9.5 Layout Transitions

The update path manages layout transitions:
1. Barrier from current layout to `TRANSFER_DST_OPTIMAL`
2. Copy staging buffer to image
3. Barrier back to `SHADER_READ_ONLY_OPTIMAL`

The `currentLayout` field is updated to reflect the final state.

---

## 10. Texture Format Support

### 10.1 Compressed Formats

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

### 10.2 Uncompressed Formats

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

## 11. LRU Eviction Strategy

### 11.1 When Eviction Occurs

LRU eviction is triggered only during upload-phase safe points when:
1. A resident texture needs a bindless slot (`m_bindlessRequested`)
2. No free dynamic slots are available (`m_freeBindlessSlots` is empty)

### 11.2 Eviction Criteria

A texture is eligible for eviction when:
- `lastUsedSerial <= completedSerial` (GPU has finished all work referencing it)
- It is not a render target (render targets are pinned)
- It is not in the reserved slot range (0-3)

### 11.3 Eviction Selection

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

## 12. Thread Safety

### 12.1 Single-Threaded Design

The `VulkanTextureManager` is designed for single-threaded access from the render thread. All public methods assume:
- Caller holds exclusive access during the call
- No concurrent modifications to internal containers
- GPU synchronization is handled via serials and deferred release

### 12.2 Bmpman Interaction

Interactions with bmpman (`bm_lock()`, `bm_unlock()`, `bm_get_info()`) occur during upload and must happen on the render thread. Bmpman itself may have internal locking for multi-threaded access.

### 12.3 Deferred Release Safety

The `DeferredReleaseQueue` ensures GPU resources are not destroyed while in use:
- Resources are enqueued with a retire serial
- `collect(completedSerial)` destroys resources where `retireSerial <= completedSerial`
- The completed serial is obtained from timeline semaphore queries

---

## 13. Draw-Path and Upload-Path APIs

The texture system exposes two wrapper classes that enforce phase-appropriate access patterns.

### 13.1 VulkanTextureBindings (Draw-Path)

`VulkanTextureBindings` provides draw-path safe texture access. It can only query existing state and queue uploads; it cannot record GPU commands or mutate bindless slot assignments.

```cpp
class VulkanTextureBindings {
public:
    explicit VulkanTextureBindings(VulkanTextureManager& textures);

    // Returns a valid descriptor (falls back if not resident) and queues upload if needed.
    vk::DescriptorImageInfo descriptor(TextureId id, uint32_t currentFrameIndex,
                                       const VulkanTextureManager::SamplerKey& samplerKey);

    // Returns a stable bindless slot index for this texture id.
    // If not resident or no slot assigned: returns fallback slot 0.
    // Also queues an upload for missing textures.
    uint32_t bindlessIndex(TextureId id, uint32_t currentFrameIndex);
};
```

**Key Behaviors**:
- `descriptor()`: Returns resident texture's descriptor or fallback; queues upload if not resident
- `bindlessIndex()`: Calls `requestBindlessSlot()` (records intent), returns slot or fallback; queues upload if not resident
- Both methods call `markTextureUsed()` to update LRU tracking when texture is resident

### 13.2 VulkanTextureUploader (Upload-Path)

`VulkanTextureUploader` provides upload-phase access. It can record GPU commands but must only be called when no rendering is active.

```cpp
class VulkanTextureUploader {
public:
    explicit VulkanTextureUploader(VulkanTextureManager& textures);

    // Flush all pending uploads and assign bindless slots.
    void flushPendingUploads(const UploadCtx& ctx);

    // Update an existing texture's contents dynamically.
    bool updateTexture(const UploadCtx& ctx, int bitmapHandle, int bpp,
                       const ubyte* data, int width, int height);
};
```

**Key Behaviors**:
- `flushPendingUploads()`: Processes pending queue, handles retirements, assigns bindless slots
- `updateTexture()`: Records copy commands to update existing resident texture

### 13.3 Phase Separation

| Operation | Draw-Path Safe? | Upload-Path Only? |
|-----------|-----------------|-------------------|
| Query resident descriptor | Yes | - |
| Query bindless slot | Yes | - |
| Queue upload request | Yes | - |
| Mark texture used | Yes | - |
| Flush pending uploads | No | Yes |
| Assign bindless slots | No | Yes |
| Update texture contents | No | Yes |
| Record layout transitions | No | Yes |

---

## 14. Debug Logging

### 14.1 HUD Debug Mode

The `-vk_hud_debug` command-line flag enables detailed logging for HUD texture residency tracking. This helps diagnose textures that are drawn while not yet resident.

### 14.2 Debug Data Structures

```cpp
std::unordered_set<int> m_hudDebugMissing;         // Base frames drawn while non-resident
std::unordered_map<int, uint32_t> m_hudDebugLogFlags;  // One-shot log flags per base frame
```

### 14.3 Log Events

When `-vk_hud_debug` is enabled, the following events are logged (once per texture):

| Event | Flag | Description |
|-------|------|-------------|
| `VK_HUD_DEBUG: queue upload` | `kHudLogQueued` | Texture queued for upload |
| `VK_HUD_DEBUG: upload ok` | `kHudLogUploadOk` | Upload completed successfully |
| `VK_HUD_DEBUG: upload skipped (permanently rejected)` | `kHudLogReject` | Texture in rejection set |
| `VK_HUD_DEBUG: upload skipped (bmpman released)` | `kHudLogReleased` | Handle released by bmpman |
| `VK_HUD_DEBUG: upload deferred (bm_lock failed)` | `kHudLogBmLockFail` | bm_lock returned null |
| `VK_HUD_DEBUG: upload deferred (staging budget)` | `kHudLogDeferBudget` | Budget limit hit |
| `VK_HUD_DEBUG: upload deferred (staging alloc failed)` | `kHudLogDeferAlloc` | Allocation failed |
| `VK_HUD_DEBUG: delete texture requested` | `kHudLogReleased` | Texture deletion requested |
| `VK_HUD_DEBUG: release bitmap` | `kHudLogReleased` | Bmpman handle released |

### 14.4 Tracking Missing Textures

```cpp
// Mark a HUD texture as drawn while non-resident (called from draw path)
void markHudTextureMissing(TextureId id);

// Internal: check if logging is enabled for this base frame
bool shouldLogHudDebug(int baseFrame) const;

// Internal: one-shot logging (returns true only on first call per flag)
bool logHudDebugOnce(int baseFrame, uint32_t flag);
```

---

## 15. Usage Patterns

### 15.1 Queue Texture for Upload

```cpp
// Queue texture for upload (will be processed at frame start)
textureManager->queueTextureUpload(bitmapHandle, currentFrameIndex, samplerKey);

// Upload happens automatically during flushPendingUploads()
```

### 15.2 Preload Texture Immediately

```cpp
// Preload texture synchronously (blocks until complete)
bool success = textureManager->preloadTexture(bitmapHandle, isAABitmap);
if (!success) {
    // Out of device memory - abort preloading
}
// Note: preloadTexture returns true for most failures (invalid handle, lock failed)
// to allow preloading to continue. Only OOM returns false.
```

### 15.3 Get Bindless Slot

```cpp
// Get bindless slot index (may return fallback slot 0)
textureManager->requestBindlessSlot(id);
auto slotOpt = textureManager->tryGetBindlessSlot(id);
uint32_t slot = slotOpt.value_or(kBindlessTextureSlotFallback);

// Use slot in model push constants
pushConstants.baseMapIndex = slot;
```

### 15.4 Check Bindless Slot Assignment

```cpp
// Check if texture has a bindless slot assigned
bool hasSlot = textureManager->hasBindlessSlot(id);
```

### 15.5 Get Texture Descriptor

```cpp
// Get descriptor for resident texture (for push descriptors)
auto info = textureManager->tryGetResidentDescriptor(id, samplerKey);
vk::DescriptorImageInfo resolved = info.value_or(textureManager->fallbackDescriptor(samplerKey));
```

### 15.6 Mark Texture Used

```cpp
// Update LRU tracking for a texture (prevents premature eviction)
textureManager->markTextureUsed(id, currentFrameIndex);
```

---

## 16. Common Issues

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

## 17. Performance Considerations

### 17.1 Staging Buffer Size

The 12 MB staging buffer limits how many textures can be uploaded per frame. For texture-heavy scenes:
- Consider increasing `STAGING_RING_SIZE`
- Preload textures during level load
- Use compressed formats to reduce upload size

### 17.2 Bindless vs. Push Descriptors

- **Bindless**: Lower CPU overhead for scenes with many unique textures
- **Push Descriptors**: Better for frequently-changing single textures

The current implementation prefers bindless for model textures (many materials) and push descriptors for 2D/UI rendering.

### 17.3 Sampler Caching

Samplers are cached by `SamplerKey` to avoid redundant Vulkan object creation. The cache is never cleared during runtime (samplers are small and reused).

### 17.4 LRU Overhead

LRU tracking (`lastUsedFrame`, `lastUsedSerial`) adds minimal overhead per texture access. Eviction scanning is O(n) over assigned slots but only occurs under slot pressure.

---

## Appendix: Key Functions Reference

| Function | Purpose | When Called |
|----------|---------|-------------|
| `queueTextureUpload()` | Queue texture for upload (3 overloads) | When texture is needed during draw |
| `queueTextureUploadBaseFrame()` | Queue by base frame handle | Internal, avoids redundant base frame lookup |
| `preloadTexture()` | Upload texture immediately (sync) | Level load / preload phase |
| `flushPendingUploads()` | Process all pending uploads | Frame start (via VulkanTextureUploader) |
| `updateTexture()` | Update existing texture contents | Streaming animations, NanoVG (via VulkanTextureUploader) |
| `requestBindlessSlot()` | Record bindless slot intent | Draw path (CPU-only, no allocation) |
| `tryGetBindlessSlot()` | Lookup bindless slot mapping | Draw path (returns slot or nullopt) |
| `assignBindlessSlots()` | Allocate/evict bindless slots | Upload phase only (frame start) |
| `tryGetResidentDescriptor()` | Get descriptor for resident texture | Push descriptor binding |
| `markTextureUsed()` | Update LRU tracking | When texture is referenced |
| `markHudTextureMissing()` | Track HUD texture drawn while non-resident | HUD draw path (for debug logging) |
| `deleteTexture()` | Mark texture for deletion | When texture no longer needed |
| `releaseBitmap()` | Release texture (handle reuse boundary) | When bmpman frees handle |
| `retireTexture()` | Retire texture and defer GPU destruction | Internal (called by delete/release/evict) |
| `processPendingRetirements()` | Process deferred retirements | Frame start (before uploads) |
| `collect()` | Clean up retired textures | Frame start, after GPU completion |
| `createRenderTarget()` | Create GPU render target | Bitmap RTT creation |
| `isResident()` | Check if texture is resident | Various paths |
| `fallbackDescriptor()` | Get fallback texture descriptor | When texture unavailable |

---

## References

- `code/graphics/vulkan/VulkanTextureManager.h` - Residency structures and public API
- `code/graphics/vulkan/VulkanTextureManager.cpp` - State transition and upload logic
- `code/graphics/vulkan/VulkanTextureId.h` - `TextureId` strong typedef for texture identity
- `code/graphics/vulkan/VulkanTextureBindings.h` - `VulkanTextureBindings` (draw-path) and `VulkanTextureUploader` (upload-path) wrapper classes
- `code/graphics/vulkan/VulkanConstants.h` - Bindless slot constants (`kMaxBindlessTextures`, `kBindlessFirstDynamicTextureSlot`)
- `code/graphics/vulkan/VulkanRenderer.h` - Staging buffer size (`STAGING_RING_SIZE`)
- `code/graphics/vulkan/VulkanRingBuffer.h` - Staging ring buffer implementation
- `code/graphics/vulkan/VulkanDeferredRelease.h` - Serial-gated resource destruction
- `code/graphics/vulkan/VulkanPhaseContexts.h` - `UploadCtx` and other phase context types
- `docs/DESIGN_PHILOSOPHY.md` - State-as-location pattern explanation
- `docs/VULKAN_TEXTURE_BINDING.md` - Bindless texture slot conventions

---

## Future Work

The texture manager refactor plan in `docs/PLAN_REFACTOR_TEXTURE_MANAGER.md` has been implemented. This document now
describes the refactored, type-driven architecture (TextureId identity, unique upload queue, upload-phase bindless slot
allocation, unified render target ownership, and removal of queue-wide stalls).
