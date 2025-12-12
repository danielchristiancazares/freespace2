# Vulkan Renderer Bug Investigation

**Status:** Ongoing Investigation
**Started:** 2025-12-12
**Last Updated:** 2025-12-12

## Analysis Tools Used

| Tool | Duration | Findings |
|------|----------|----------|
| GPT-5.2 Pro | 40 minutes extended reasoning | Initial comprehensive analysis |
| Gemini 3 Deep Think | 4 batches | Secondary validation + new findings |
| Claude Desktop | Single pass | Tertiary validation + new findings |
| Claude Opus 4.5 | Manual verification | Ground truth verification against source |

All findings below have been verified against the actual source code.

---

## Summary

| Severity | Count | Verified | Pending |
|----------|-------|----------|---------|
| Critical | 9 | 1 | 8 |
| High | 11 | 1 | 10 |
| Medium | 6 | 0 | 6 |
| **Total** | **26** | **2** | **24** |

---

## Critical Issues

These bugs cause crashes, data corruption, or resource exhaustion.

### C1. Ring Buffer Wraps Mid-Frame (Data Corruption)

**Status:** Pending
**File:** `VulkanRingBuffer.cpp:42-48`
**Source:** GPT-5.2 Pro

**Problem:**
When out of space, the ring buffer wraps to offset 0 instead of failing:

```cpp
if (alignedOffset + requestSize > m_size) {
    // FIXME: Is this a bug?
    // Potential issue: Do not wrap within a frame; wrapping could overwrite in-flight GPU reads.
    alignedOffset = 0;
    if (requestSize > m_size) {
        throw std::runtime_error("Allocation size exceeds remaining ring buffer capacity");
    }
}
```

The code even acknowledges the issue with a FIXME comment.

**Impact:**
Within a single frame, earlier allocations may still be in use by the GPU when later allocations overwrite them. Causes silent data corruption, flickering, garbage rendering.

**Fix:**
Do not wrap within a frame. Assert/throw when out of space, or implement a growable allocator with per-frame tracking.

---

### C2. Model Descriptor Sets Allocated Per Draw (Pool Explosion)

**Status:** Pending
**Files:** `VulkanGraphics.cpp:605`, `VulkanDescriptorLayouts.cpp:110-165`
**Source:** GPT-5.2 Pro

**Problem:**
Model rendering path allocates a new descriptor set every draw call and never frees or resets the pool:

```cpp
// VulkanGraphics.cpp:604-605
// Allocate fresh descriptor set for this draw to avoid race conditions
vk::DescriptorSet modelSet = renderer_instance->allocateModelDescriptorSet();
```

Each model set includes a bindless array of 1024 samplers.

**Impact:**
Pool sizes become astronomical: `kModelSetsPerPool (4096*3=12288) * kMaxBindlessTextures (1024)` = ~12.5 million descriptors. Most drivers will fail pool creation or exhaust descriptor heap memory quickly.

**Fix:**
Use the already-allocated per-frame `frame.modelDescriptorSet` instead of allocating per-draw. Use dynamic offsets and push constants for per-draw variation. Reset descriptor pool at frame boundaries.

---

### C3. GBuffer Format Array Mismatch (Pipeline Incompatibility)

**Status:** Pending
**Files:** `VulkanRenderTargets.cpp`, `VulkanRenderTargets.h`, `VulkanPipelineManager.cpp`
**Source:** GPT-5.2 Pro

**Problem:**
GBuffer attachments use different formats:
```cpp
{ R16G16B16A16Sfloat, R16G16B16A16Sfloat, R8G8B8A8Unorm }
```

But `VulkanRenderTargets` exposes only a single `m_gbufferFormat` (first format), and `VulkanPipelineManager` replicates that format for all attachments.

**Impact:**
Dynamic rendering requires pipeline's `pColorAttachmentFormats[]` to match actual formats. Mismatch causes pipeline creation failure, validation errors, or undefined rendering.

**Fix:**
Store and expose all gbuffer formats as an array. Extend `PipelineKey` to include all attachment formats. Pass correct format array to `vk::PipelineRenderingCreateInfo`.

---

### C4. Immediate Buffer Deletion (Use-After-Free)

**Status:** Pending
**File:** `VulkanBufferManager.cpp:79-97`
**Source:** GPT-5.2 Pro

**Problem:**
`deleteBuffer()` resets `UniqueBuffer`/`UniqueDeviceMemory` immediately, unlike `updateBufferData` which defers to `m_retiredBuffers`:

```cpp
void VulkanBufferManager::deleteBuffer(gr_buffer_handle handle) {
    // ...
    buffer.buffer.reset();   // Immediate destruction!
    buffer.memory.reset();
    buffer.size = 0;
}
```

**Impact:**
If GPU is still using the buffer (common with multiple frames in flight), immediate deletion causes use-after-free on GPU, validation errors, and driver crashes.

**Fix:**
Move buffers to `m_retiredBuffers` with `retiredAtFrame = m_currentFrame` and destroy after safe delay (same pattern as `updateBufferData`).

---

### C5. Swapchain Acquire Crashes on Resize

**Status:** Pending
**File:** `VulkanRenderer.cpp:301-302`
**Source:** GPT-5.2 Pro

**Problem:**
`acquireImage()` returns sentinel value when swapchain needs recreation, but `flip()` asserts this never happens:

```cpp
uint32_t imageIndex = acquireImage(frame);
Assertion(imageIndex != std::numeric_limits<uint32_t>::max(), "flip() failed to acquire swapchain image");
```

**Impact:**
Hard crash when window is resized or minimized (VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR).

**Fix:**
Handle sentinel return in `flip()`: skip frame and re-acquire after swapchain recreation, or loop until successful acquisition.

---

### C6. Misaligned Shader Pointer on ARM

**Status:** Pending
**File:** `VulkanShaderManager.cpp:107-113`
**Source:** Gemini 3 Deep Think

**Problem:**
Filesystem shader loading uses `std::vector<char>` which has 1-byte alignment:

```cpp
std::vector<char> buffer(fileSize);
file.read(buffer.data(), fileSize);
moduleInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
```

Vulkan requires `pCode` to be 4-byte aligned.

**Impact:**
Crash (SIGBUS) on ARM architectures (Mac M-series, Android) or strict x86 modes. The embedded shader path correctly uses `std::vector<uint32_t>`, but filesystem fallback does not.

**Fix:**
Use `std::vector<uint32_t>` for filesystem loading:
```cpp
std::vector<uint32_t> buffer((fileSize + 3) / 4);
file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
moduleInfo.pCode = buffer.data();
```

---

### C7. Large Texture Infinite Loop

**Status:** Pending
**File:** `VulkanTextureManager.cpp:495-498`
**Source:** Gemini 3 Deep Think

**Problem:**
Textures larger than staging budget are deferred indefinitely:

```cpp
if (stagingUsed + totalUploadSize > stagingBudget) {
    remaining.push_back(baseFrame);
    continue; // defer to next frame
}
```

`stagingBudget` is fixed at 12MB. A 4K RGBA texture is ~64MB.

**Impact:**
If single texture exceeds 12MB staging budget, condition is always true. Texture is pushed to `remaining` every frame, never uploaded. Game hangs waiting for texture or renders black.

**Fix:**
If `totalUploadSize > stagingBudget`, bypass the ring buffer and use the `uploadImmediate` path (synchronous upload) or allocate a dedicated temporary staging buffer.

---

### C8. Immediate Texture Deletion (Use-After-Free)

**Status:** Pending
**File:** `VulkanTextureManager.cpp:754-758`
**Source:** Gemini 3 Deep Think

**Problem:**
`deleteTexture` immediately erases texture from map, destroying GPU resources:

```cpp
void VulkanTextureManager::deleteTexture(int bitmapHandle) {
    int base = bm_get_base_frame(bitmapHandle, nullptr);
    m_textures.erase(base);  // Immediate destruction via RAII!
}
```

Compare with `retireTexture` which properly defers destruction.

**Impact:**
If GPU is currently executing a frame that references this texture, it will access freed memory and crash.

**Fix:**
Use `retireTexture` logic or add handle to `m_pendingDestructions` with delay of `kFramesInFlight` before destroying.

---

### C9. Fallback Texture Handle Never Initialized

**Status:** Pending
**Files:** `VulkanTextureManager.h:145`, `VulkanRenderer.cpp:674-675`
**Source:** Claude Desktop

**Problem:**
Fallback texture handle is initialized to -1 and never set:

```cpp
// VulkanTextureManager.h:145
int m_fallbackTextureHandle = -1;
```

But it's used with an assertion that will always fail:

```cpp
// VulkanRenderer.cpp:674-675
int fallbackHandle = m_textureManager->getFallbackTextureHandle();
Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");
```

**Impact:**
When `writeFallbackDescriptor` is called during texture retirement, assertion fires and crashes. This will happen on the first texture retirement.

**Fix:**
Create a 1x1 black/white texture during `VulkanTextureManager` initialization and set `m_fallbackTextureHandle` to its handle.

---

## High Severity Issues

These bugs cause validation errors, incorrect rendering, or resource leaks.

### H1. Frame Count Constant Mismatch

**Status:** Verified
**Files:** `VulkanRenderer.h:139`, `VulkanConstants.h:8`
**Source:** GPT-5.2 Pro

**Problem:**
Two constants define frames-in-flight with conflicting values:

| Constant | Value | Location |
|----------|-------|----------|
| `MAX_FRAMES_IN_FLIGHT` | 2 | VulkanRenderer.h:139 |
| `kFramesInFlight` | 3 | VulkanConstants.h:8 |

Renderer uses `MAX_FRAMES_IN_FLIGHT = 2` for frame cycling:
```cpp
m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
```

But texture manager uses `kFramesInFlight = 3`:
```cpp
std::array<bool, kFramesInFlight> descriptorWritten = {false, false, false};
```

**Impact:**
Wasted memory (resources held one frame longer), unused array slots, inconsistent codebase inviting future bugs.

**Root Cause:**
Value `3` likely confused with `kGBufferCount = 3` (VulkanRenderTargets.h:13) which defines G-buffer attachments, unrelated to frame synchronization.

**Fix:**
Consolidate to single constant `MAX_FRAMES_IN_FLIGHT = 2` in VulkanConstants.h. Remove from VulkanRenderer.h. Update all references.

---

### H2. Depth Layout Not Tracked After Deferred Pass

**Status:** Pending
**File:** `VulkanRenderingSession.cpp:67, 144`
**Source:** GPT-5.2 Pro, Gemini 3 Deep Think

**Problem:**
Depth transitions assume incorrect `oldLayout`:

```cpp
// Line 67: transitionDepthToAttachment assumes DepthAttachmentOptimal
toDepth.oldLayout = m_targets.isDepthInitialized()
    ? vk::ImageLayout::eDepthAttachmentOptimal  // WRONG after deferred!
    : vk::ImageLayout::eUndefined;

// Line 144: But endDeferredGeometry transitions depth to ShaderReadOnly
bd.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
```

After deferred pass, depth is in `ShaderReadOnlyOptimal` but next frame's `transitionDepthToAttachment` assumes `DepthAttachmentOptimal`.

**Impact:**
Validation layer errors, undefined behavior, potential rendering corruption.

**Fix:**
Track current layout state for depth image. Update after each barrier. Use tracked state as `oldLayout`. Or add explicit transition from `ShaderReadOnly` back to `DepthAttachment` when needed.

---

### H3. Deferred Depth Sampling/Attachment Conflict

**Status:** Pending
**Files:** `VulkanRenderingSession.cpp`, `VulkanRenderer.cpp:350-400`
**Source:** GPT-5.2 Pro

**Problem:**
After deferred geometry:
1. Depth transitioned to `SHADER_READ_ONLY_OPTIMAL`
2. Swapchain rendering binds same depth as attachment with `DEPTH_ATTACHMENT_OPTIMAL`
3. Global descriptors reference it as shader-read

An image subresource cannot be in two layouts simultaneously.

**Impact:**
Validation errors, undefined behavior.

**Fix:**
Options:
1. Don't bind depth attachment during deferred lighting pass
2. Use `DEPTH_READ_ONLY_OPTIMAL` for both sampling and attachment (with depth writes disabled)
3. Use separate depth copy for sampling

---

### H4. Texture Pending Destructions Never Processed (Memory Leak)

**Status:** Pending
**Files:** `VulkanTextureManager.h`, `VulkanTextureManager.cpp`, `VulkanRenderer.cpp`
**Source:** GPT-5.2 Pro

**Problem:**
Texture manager has `processPendingDestructions(currentFrame)` and `setCurrentFrame()`, but renderer never calls them. Only calls:
```cpp
m_textureManager->markUploadsCompleted(m_currentFrame);
```

**Impact:**
`retireTexture()` queues VkImage destruction but nothing processes it. VkImage memory leaks indefinitely.

**Fix:**
In `flip()` or frame begin/end, call:
```cpp
m_textureManager->setCurrentFrame(frameCounter);
m_textureManager->processPendingDestructions(frameCounter);
```

---

### H5. Global Descriptor Set Race Condition

**Status:** Pending
**File:** `VulkanRenderer.cpp:77, 321-358`
**Source:** Gemini 3 Deep Think

**Problem:**
`m_globalDescriptorSet` is allocated once and shared across all frames:

```cpp
// Line 77: Single allocation
m_globalDescriptorSet = m_descriptorLayouts->allocateGlobalSet();

// Line 321-358: Updated during any frame
void VulkanRenderer::bindDeferredGlobalDescriptors() {
    // ... writes to m_globalDescriptorSet
    m_vulkanDevice->device().updateDescriptorSets(writes, {});
}
```

**Impact:**
If frame N is executing on GPU (reading the set) while frame N+1 calls `bindDeferredGlobalDescriptors` to update it, this is a race condition / undefined behavior.

**Fix:**
Make `m_globalDescriptorSet` per-frame: `std::array<vk::DescriptorSet, MAX_FRAMES_IN_FLIGHT> m_globalDescriptorSets`. Or use update-after-bind with proper flags.

---

### H6. LoadOp::Load with Undefined Layout

**Status:** Pending
**File:** `VulkanRenderingSession.cpp:48, 162`
**Source:** Gemini 3 Deep Think

**Problem:**
Swapchain transition always uses `oldLayout = eUndefined`:

```cpp
// Line 48
toRender.oldLayout = vk::ImageLayout::eUndefined;
```

But rendering may use `LoadOp::eLoad` if clear is not requested:

```cpp
// Line 162
colorAttachment.loadOp = m_shouldClearColor ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
```

**Impact:**
Loading from an image transitioned from `Undefined` results in garbage/undefined values. The "load previous contents" operation has no valid previous contents to load.

**Fix:**
If `m_shouldClearColor` is false, the barrier must preserve previous content: use `oldLayout = ePresentSrcKHR` instead of `eUndefined`. Or always clear on first use.

---

### H7. Blending Unconditionally Disabled with EDS3

**Status:** Pending
**Files:** `VulkanGraphics.cpp:946-948`, `VulkanRenderingSession.cpp:250-252`
**Source:** Gemini 3 Deep Think

**Problem:**
When Extended Dynamic State 3 is supported, blending is forced to FALSE regardless of material settings:

```cpp
if (caps.colorBlendEnable) {
    vk::Bool32 blendEnable = VK_FALSE;  // ALWAYS FALSE!
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
}
```

The pipeline key correctly stores `blend_mode` from material, but EDS3 overrides it.

**Impact:**
All transparent rendering is broken when EDS3 is supported. Glass, smoke, HUD elements, particles all appear opaque.

**Fix:**
Set `blendEnable` based on `material_info->get_blend_mode() != ALPHA_BLEND_NONE` or similar check.

---

### H8. Scissor Ignores Clip Region (UI Broken)

**Status:** Pending
**File:** `VulkanGraphics.cpp:968, 1214`
**Source:** Gemini 3 Deep Think

**Problem:**
`createClipScissor()` exists and correctly reads `gr_screen.clip_*` values, but is NEVER CALLED:

```cpp
// Line 50-60: Correct implementation exists
vk::Rect2D createClipScissor() {
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{
        static_cast<int32_t>(gr_screen.clip_left),
        static_cast<int32_t>(gr_screen.clip_top)};
    // ...
}

// Line 968, 1214: But all render functions use full screen
vk::Rect2D scissor = createFullScreenScissor();
cmd.setScissor(0, 1, &scissor);
```

**Impact:**
UI clipping via `gr_set_clip()` updates `gr_screen.clip_*` but scissor ignores it. UI elements (text boxes, lists, scroll areas) will draw outside their intended bounds.

**Fix:**
Replace `createFullScreenScissor()` with `createClipScissor()` in render functions.

---

### H9. Buffer Offset Alignment for R8 Textures

**Status:** Pending
**File:** `VulkanTextureManager.cpp:242`
**Source:** Gemini 3 Deep Think

**Problem:**
Texture layers are packed tightly without alignment:

```cpp
offset += layerSize;
```

For R8 format (1 byte/pixel) with small dimensions, `layerSize` may not be a multiple of 4.

**Impact:**
Vulkan spec requires `bufferOffset` in `vkCmdCopyBufferToImage` to be a multiple of 4 bytes. Unaligned offsets cause validation errors or device loss on strict drivers.

**Fix:**
Align offset to 4 bytes:
```cpp
size_t paddedSize = (layerSize + 3) & ~3;
offset += paddedSize;
```

---

### H10. Depth Format Fallback Doesn't Check Sampling Support

**Status:** Pending
**File:** `VulkanRenderTargets.cpp:76-83`
**Source:** Claude Desktop

**Problem:**
Depth format selection only checks attachment support:

```cpp
for (auto format : candidates) {
    vk::FormatProperties props = m_device.physicalDevice().getFormatProperties(format);
    if (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
        return format;  // Only checks attachment, not sampling!
    }
}
```

But depth buffer is used for sampling in deferred lighting (lines 57-65 create a sampled view).

**Impact:**
If fallback format doesn't support `eSampledImage`, image creation succeeds but sampling is undefined behavior.

**Fix:**
Also require `vk::FormatFeatureFlagBits::eSampledImage`:
```cpp
if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) &&
    (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)) {
```

---

### H11. G-Buffer Clear Flag Consumed Prematurely

**Status:** Pending
**File:** `VulkanRenderingSession.cpp:184-185`
**Source:** Claude Desktop

**Problem:**
Clear flags are reset after beginning rendering to any target:

```cpp
// After beginSwapchainRendering or beginGBufferRendering:
m_shouldClearColor = false;
m_shouldClearDepth = false;
```

**Impact:**
If rendering mode switches mid-frame (swapchain -> G-buffer -> swapchain), the second swapchain pass won't clear even if it should. Can cause ghosting or stale frame content.

**Fix:**
Track clear state per render target type, not globally. Or reset clear flags only at frame boundaries.

---

## Medium Severity Issues

These bugs cause suboptimal behavior or potential edge-case failures.

### M1. Depth Barrier Stage Masks Incomplete

**Status:** Pending
**File:** `VulkanRenderingSession.cpp:139-140`
**Source:** GPT-5.2 Pro

**Problem:**
Depth transition uses only early fragment tests:

```cpp
bd.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
bd.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
```

Depth/stencil writes may occur in late fragment tests depending on pipeline configuration.

**Fix:**
Use `srcStageMask = eEarlyFragmentTests | eLateFragmentTests`.

---

### M2. Descriptor Limit Validation Incorrect

**Status:** Pending
**File:** `VulkanDescriptorLayouts.cpp:18-20`
**Source:** GPT-5.2 Pro

**Problem:**
Compares `maxDescriptorSetStorageBuffers` (per-set limit, typically 16-32) against `kModelSetsPerPool` (~12k):

```cpp
Assertion(limits.maxDescriptorSetStorageBuffers >= kModelSetsPerPool, ...)
```

This assertion is conceptually wrong - comparing apples to oranges.

**Fix:**
Check correct limits:
- `maxDescriptorSetStorageBuffers >= 1` (per-set usage)
- `maxDescriptorSetSampledImages >= kMaxBindlessTextures`
- Consider `maxPerStageDescriptorSampledImages`

---

### M3. Device Scoring Algorithm Broken

**Status:** Pending
**File:** `VulkanDevice.cpp:191-192`
**Source:** Gemini 3 Deep Think

**Problem:**
Device selection score calculation:

```cpp
score += deviceTypeScore(device.properties.deviceType) * 1000;  // max ~2000
score += device.properties.apiVersion * 100;  // ~420,000,000 for Vulkan 1.4
```

`apiVersion` is a packed integer (~4.2 million for Vulkan 1.4). Multiplied by 100 = ~420 million, completely dwarfing the device type score.

**Impact:**
An integrated GPU with Vulkan 1.4 scores ~421M while a discrete GPU with Vulkan 1.3 scores ~419M. Wrong GPU selected on systems with both.

**Note:** Partially mitigated by Vulkan 1.4 requirement filter (unsuitable devices rejected before scoring), but logic is still fundamentally wrong.

**Fix:**
Either remove apiVersion from scoring (all passing devices have 1.4+), or use normalized version comparison:
```cpp
score += deviceTypeScore(...) * 1000000;  // Make type dominant
```

---

### M4. One Memory Allocation Per Buffer

**Status:** Pending
**File:** `VulkanBufferManager.cpp:137, 275`
**Source:** Gemini 3 Deep Think

**Problem:**
Each buffer creation calls `vkAllocateMemory`:

```cpp
buffer.memory = m_device.allocateMemoryUnique(allocInfo);
```

**Impact:**
Vulkan limits active device memory allocations via `maxMemoryAllocationCount` (typically 4096). With many buffers (particles, UI elements, model chunks), this limit can be exceeded.

**Fix:**
Implement a sub-allocator (or use VMA). Allocate large memory blocks and bind buffers to offsets within those blocks.

---

### M5. Unbounded Buffer Vector Growth

**Status:** Pending
**File:** `VulkanBufferManager.cpp:75, 79-97`
**Source:** Gemini 3 Deep Think

**Problem:**
`createBuffer` always pushes to vector:

```cpp
m_buffers.push_back(std::move(buffer));
return gr_buffer_handle(static_cast<int>(m_buffers.size() - 1));
```

`deleteBuffer` clears the slot but doesn't mark it for reuse:

```cpp
buffer.buffer.reset();
buffer.memory.reset();
buffer.size = 0;
// No free list, slot never reused
```

**Impact:**
Vector grows indefinitely as buffers are created/deleted. Memory leak of handle tracking structures over long sessions.

**Fix:**
Implement `std::vector<uint32_t> m_freeIndices` free-list to reuse slots. Or use a different handle allocation strategy.

---

### M6. Per-Frame Model Descriptor Set Never Used

**Status:** Pending
**Files:** `VulkanRenderer.cpp:591-593`, `VulkanGraphics.cpp:605`
**Source:** Gemini 3 Deep Think

**Problem:**
`beginModelDescriptorSync` allocates and updates a per-frame descriptor set:

```cpp
// VulkanRenderer.cpp:591-593
if (!frame.modelDescriptorSet) {
    frame.modelDescriptorSet = m_descriptorLayouts->allocateModelDescriptorSet();
}
// Then writes all resident textures to it...
```

But draw code ignores this and allocates fresh sets per-draw:

```cpp
// VulkanGraphics.cpp:605
vk::DescriptorSet modelSet = renderer_instance->allocateModelDescriptorSet();
```

**Impact:**
- Per-frame descriptor sync work is completely wasted
- Per-draw allocation leaks (pool never reset)
- Both wasteful and buggy

**Fix:**
Refactor `gr_vulkan_render_model` to use `frame.modelDescriptorSet` instead of allocating new sets. Use push constants or dynamic offsets for per-draw variation.

---

## Files Affected

| File | Issues |
|------|--------|
| VulkanRenderer.cpp | C5, H1, H4, H5, C9 |
| VulkanRenderer.h | H1 |
| VulkanConstants.h | H1 |
| VulkanRingBuffer.cpp | C1 |
| VulkanBufferManager.cpp | C4, M4, M5 |
| VulkanDescriptorLayouts.cpp | C2, M2 |
| VulkanGraphics.cpp | C2, H7, H8, M6 |
| VulkanRenderTargets.cpp | C3, H10 |
| VulkanRenderTargets.h | C3 |
| VulkanPipelineManager.cpp | C3 |
| VulkanRenderingSession.cpp | H2, H3, H6, H11, M1 |
| VulkanTextureManager.cpp | C7, C8, H4, H9 |
| VulkanTextureManager.h | C9, H4 |
| VulkanShaderManager.cpp | C6 |
| VulkanDevice.cpp | M3 |

---

## Verification Status

| Status | Meaning |
|--------|---------|
| **Verified** | Bug confirmed in code AND fix verified working |
| **Pending** | Bug confirmed in code, fix not yet implemented or tested |

All bugs currently marked "Pending" have been verified to exist in the source code through manual inspection. They remain "Pending" because fixes have not been implemented and tested.

---

## Recommended Fix Priority

### Immediate (Will crash or corrupt)
1. **C9** - Fallback texture crashes on first texture retirement
2. **C1** - Ring buffer corruption causes random visual glitches
3. **C6** - ARM crash affects Mac M-series users
4. **C2** - Descriptor pool exhaustion after ~12k draws
5. **C4/C8** - Use-after-free on buffer/texture deletion

### High Priority (Broken rendering)
6. **H7** - All transparency broken on EDS3 GPUs
7. **H8** - UI clipping completely broken
8. **H1** - Frame count mismatch causes resource lifetime bugs
9. **H2/H3** - Depth layout issues cause validation spam

### Medium Priority (Leaks and inefficiency)
10. **H4** - Texture memory leak
11. **M5** - Buffer handle vector growth
12. **M6** - Wasted descriptor sync work

---

## Changelog

- **2025-12-12 (Initial):** Report created from GPT-5.2 Pro analysis. Frame count mismatch (H1) verified.
- **2025-12-12 (Update 1):** Added Gemini 3 Deep Think findings (4 batches). Verified 13 additional bugs.
- **2025-12-12 (Update 2):** Added Claude Desktop findings. Verified 3 additional bugs including critical C9.
- **2025-12-12 (Update 3):** Consolidated all findings. Total: 26 verified bugs (9 critical, 11 high, 6 medium).
