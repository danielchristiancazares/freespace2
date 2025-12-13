# Vulkan Renderer Bug Investigation

**Status:** Ongoing Investigation
**Started:** 2025-12-12
**Last Updated:** 2025-12-13

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

| Severity | Count | Unverified | Open | Resolved |
|----------|-------|------------|------|----------|
| Critical | 9     | 0          | 6    | 3        |
| High     | 13    | 0          | 11   | 2        |
| Medium   | 8     | 0          | 8    | 0        |
| Total    | 30    | 0          | 25   | 5        |

---

## Critical Issues

These bugs cause crashes, data corruption, or resource exhaustion.

### C1. Ring Buffer Wraps Mid-Frame (Data Corruption)

**Status:** Open
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

**Status:** Open
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

**Status:** Open
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

**Status:** Resolved
**File:** `VulkanBufferManager.cpp:79-102`
**Source:** GPT-5.2 Pro

**Problem:**
`deleteBuffer()` reset `UniqueBuffer`/`UniqueDeviceMemory` immediately, unlike `updateBufferData` which defers to `m_retiredBuffers`:

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

**Fix Applied:**
1. Modified `deleteBuffer()` to move GPU resources to `m_retiredBuffers` instead of immediate destruction
2. Now uses the same deferred deletion pattern as `updateBufferData()` and `resizeBuffer()`
3. Buffer destruction deferred by `FRAMES_BEFORE_DELETE` (3 frames) to ensure GPU has finished using resources
4. Added unit test `test_vulkan_buffer_manager_retirement.cpp` to verify all three destruction paths use deferred deletion

---

### C5. Swapchain Acquire Crashes on Resize

**Status:** Open (Verified 2025-12-13)
**File:** `VulkanRenderer.cpp:113-130, 301-302`
**Source:** GPT-5.2 Pro

**Problem:**
`acquireImage()` returns sentinel value when swapchain needs recreation, but `flip()` asserts this never happens:

```cpp
uint32_t imageIndex = acquireImage(frame);
Assertion(imageIndex != std::numeric_limits<uint32_t>::max(), "flip() failed to acquire swapchain image");
```

**Verification (Claude Opus 4.5):**
Confirmed. The code flow in `acquireImage()` (lines 113-130):
1. Line 114: `acquireNextImage()` called, returns `needsRecreate = true` on `VK_ERROR_OUT_OF_DATE_KHR`
2. Lines 116-121: Swapchain is recreated successfully via `recreateSwapchain()`
3. Line 122: Returns `std::numeric_limits<uint32_t>::max()` **without attempting re-acquire**
4. Line 302 in `flip()`: Assertion fires on sentinel value, crash

The swapchain recreation succeeds but the function doesn't retry acquisition. The assertion is correct (sentinel means failure), but `acquireImage()` shouldn't return sentinel after successful recreation.

**Impact:**
Hard crash when window is resized or minimized (VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR).

**Fix:**
After successful swapchain recreation at line 121, retry the acquire instead of returning sentinel:
```cpp
if (m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
    m_renderTargets->resize(m_vulkanDevice->swapchainExtent());
    result = m_vulkanDevice->acquireNextImage(frame.imageAvailable()); // Retry
    if (result.success) {
        return result.imageIndex;
    }
}
```

---

### C6. Misaligned Shader Pointer on ARM

**Status:** Resolved
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

**Fix Applied:**
1. Changed filesystem shader loading to use `std::vector<uint32_t>` instead of `std::vector<char>`
2. Buffer size calculated as `(fileSize + 3) / 4` to round up to uint32_t boundary
3. File read uses `reinterpret_cast<char*>(buffer.data())` for byte-wise reading
4. `codeSize` preserved as exact file size (not rounded up)
5. Now matches the pattern used by the embedded shader path
6. Added unit test `test_vulkan_shader_alignment.cpp` to verify 4-byte alignment of pCode for various file sizes

---

### C7. Large Texture Infinite Loop

**Status:** Open (Verified 2025-12-13)
**File:** `VulkanTextureManager.cpp:644-646`
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

**Verification (Claude Opus 4.5):**
Confirmed. The code flow:
1. `STAGING_RING_SIZE = 12 * 1024 * 1024` (12 MiB) at `VulkanRenderer.h:142`
2. Line 581: `stagingBudget = frame.stagingBuffer().size()` = 12 MiB
3. Line 568: Textures queued without any size checking
4. Lines 644-646: If `stagingUsed + totalUploadSize > stagingBudget`, defer to next frame
5. For oversized texture: even with `stagingUsed = 0`, condition `0 + 64MB > 12MB` is always true
6. No fallback to `uploadImmediate()` path exists for oversized textures

A 4K RGBA texture (4096x4096x4 = 64 MiB) will be deferred forever. The `uploadImmediate()` function at line 287 creates dedicated staging buffers and could handle any size, but is never invoked as a fallback.

**Impact:**
If single texture exceeds 12MB staging budget, condition is always true. Texture is pushed to `remaining` every frame, never uploaded. Game hangs waiting for texture or renders black.

**Fix:**
If `totalUploadSize > stagingBudget`, bypass the ring buffer and use the `uploadImmediate` path (synchronous upload) or allocate a dedicated temporary staging buffer.

---

### C8. Immediate Texture Deletion (Use-After-Free)

**Status:** Open (Verified 2025-12-13)
**File:** `VulkanTextureManager.cpp:908-912`
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

**Verification (Claude Opus 4.5):**
Confirmed. Line numbers corrected from 754-758 to 908-912.
1. `deleteTexture()` at lines 908-912 immediately erases from map, triggering RAII destruction of VkImage/VkDeviceMemory/VkImageView
2. `retireTexture()` at lines 940-965 properly defers: marks as Retired, queues to `m_pendingDestructions` with `m_currentFrame + kFramesInFlight` delay
3. **Neither function has any callers** - grep shows zero call sites for `VulkanTextureManager::deleteTexture` or `retireTexture`

This is a **latent bug** - the dangerous code path exists but is never executed. If texture deletion is ever wired up using `deleteTexture` instead of `retireTexture`, use-after-free will occur.

**Impact:**
If GPU is currently executing a frame that references this texture, it will access freed memory and crash.

**Fix:**
Use `retireTexture` logic or add handle to `m_pendingDestructions` with delay of `kFramesInFlight` before destroying.

---

### C9. Fallback Texture Handle Never Initialized

**Status:** Resolved
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

**Fix Applied:**
1. Added `createFallbackTexture()` method to `VulkanTextureManager`
2. Creates a 1x1 black RGBA texture during initialization
3. Uses synthetic handle `kFallbackTextureHandle = -1000` to avoid collision with bmpman handles
4. Stores in `m_textures` map and sets `m_fallbackTextureHandle` to the synthetic handle
5. Called from constructor after `createDefaultSampler()`

---

## High Severity Issues

These bugs cause validation errors, incorrect rendering, or resource leaks.

### H1. Frame Count Constant Mismatch

**Status:** Resolved
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

**Fix Applied:**
1. Changed `kFramesInFlight` from 3 to 2 in `VulkanConstants.h`
2. Removed `MAX_FRAMES_IN_FLIGHT` from `VulkanRenderer.h`
3. Added `#include "VulkanConstants.h"` to `VulkanRenderer.h`
4. Updated all references to use `kFramesInFlight`
5. Changed `descriptorWritten` initializer to use value-initialization (`{}`)

---

### H2. Depth Layout Not Tracked After Deferred Pass

**Status:** Open
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

**Status:** Open
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

**Status:** Open (Verified 2025-12-12)
**Files:** `VulkanTextureManager.h`, `VulkanTextureManager.cpp`, `VulkanRenderer.cpp`
**Source:** GPT-5.2 Pro

**Problem:**
Texture manager has `processPendingDestructions(currentFrame)` and `setCurrentFrame()`, but renderer never calls them. Only calls:
```cpp
m_textureManager->markUploadsCompleted(m_currentFrame);
```

**Verification (Claude Opus 4.5):**
Confirmed. The texture retirement system has THREE issues:

1. **`setCurrentFrame()` never called**: `m_currentFrame` in VulkanTextureManager stays at default value 0 forever (VulkanTextureManager.h:155). VulkanRenderer.cpp:294-298 calls `m_bufferManager->onFrameEnd()` and `m_textureManager->markUploadsCompleted()` but never `setCurrentFrame()`.

2. **`processPendingDestructions()` never called**: Even if textures were queued, they would never be destroyed. Grep of entire codebase shows zero call sites.

3. **`retireTexture()` never called**: The entire retirement mechanism is dead code. Grep shows only declaration (VulkanTextureManager.h:98) and implementation (VulkanTextureManager.cpp:935) - no callers. The only texture deletion path is `deleteTexture()` which does immediate destruction (see C8).

The retirement system at VulkanTextureManager.cpp:935-993 is complete and correct (queues destruction with `m_currentFrame + kFramesInFlight` delay), but is entirely unused.

**Impact:**
If `retireTexture()` were ever called (e.g., when texture residency system evicts textures), the deferred destructions would never process and VkImage memory would leak indefinitely. Currently a latent bug since `retireTexture()` has no callers.

**Fix:**
In `flip()` after `frame.wait_for_gpu()`, call:
```cpp
m_textureManager->setCurrentFrame(m_globalFrameCounter++);
m_textureManager->processPendingDestructions(m_globalFrameCounter);
```
Also wire up `retireTexture()` to the texture residency/eviction system when implemented.

---

### H5. Global Descriptor Set Race Condition

**Status:** Open
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

**Status:** Open
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

**Status:** Open (Verified 2025-12-12, line numbers corrected 2025-12-13)
**Files:** `VulkanGraphics.cpp:899-901, 1146-1148`, `VulkanRenderingSession.cpp:247-249`
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

**Verification (Claude Opus 4.5):**
Confirmed bug exists in 3 locations:
1. `VulkanGraphics.cpp:899-901` - `gr_vulkan_render_primitives()`
2. `VulkanGraphics.cpp:1146-1148` - `gr_vulkan_render_primitives_batched()`
3. `VulkanRenderingSession.cpp:247-249` - `applyDynamicState()` default dynamic state setup

All three hardcode `blendEnable = VK_FALSE` while the pipeline key at lines 481, 707, and 1012 correctly captures `material_info->get_blend_mode()`.

**Impact:**
All transparent rendering is broken when EDS3 is supported. Glass, smoke, HUD elements, particles all appear opaque.

**Fix:**
Set `blendEnable` based on `material_info->get_blend_mode() != ALPHA_BLEND_NONE` or similar check.

---

### H8. Scissor Ignores Clip Region (UI Broken)

**Status:** Resolved
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

**Fix Applied (2025-12-13):**
1. Corrected Vulkan clip state semantics to match engine expectations (`clip_left/top` remain 0; Vulkan scissor uses `offset_x/y` + `clip_width/height`).
2. Updated Vulkan draw paths to apply the current clip scissor instead of forcing full-screen scissor.
3. Added a Vulkan unit test covering clip/scissor state derivation.

---

### H9. Buffer Offset Alignment for R8 Textures

**Status:** Open (Verified 2025-12-12)
**File:** `VulkanTextureManager.cpp:391, 464`
**Source:** Gemini 3 Deep Think

**Problem:**
In the `uploadImmediate()` path (synchronous upload with dedicated staging buffer), texture layers are packed tightly without alignment:

```cpp
// Lines 391 and 464 in uploadImmediate()
offset += layerSize;
```

For R8 format (1 byte/pixel) with small dimensions, `layerSize` may not be a multiple of 4.

**Verification (Claude Opus 4.5):**
Line number in original report was incorrect (claimed 242). Actual bug is at lines 391 (data copy loop) and 464 (region generation loop) in `uploadImmediate()`. The ring buffer path (`flushPendingUploads()`) is NOT affected because it uses `optimalBufferCopyOffsetAlignment` from device properties (VulkanRenderer.cpp:92), which ensures each allocation is properly aligned.

**Impact:**
Vulkan spec requires `bufferOffset` in `vkCmdCopyBufferToImage` to be a multiple of 4 bytes. Unaligned offsets in the `uploadImmediate()` path cause validation errors or device loss on strict drivers when uploading R8 texture arrays where `width * height % 4 != 0`.

**Fix:**
Align offset to 4 bytes in `uploadImmediate()`:
```cpp
size_t paddedSize = (layerSize + 3) & ~3;
offset += paddedSize;
```

---

### H10. Depth Format Selection Has Two Critical Flaws

**Status:** Open (Verified 2025-12-13)
**File:** `VulkanRenderTargets.cpp:68-84`
**Source:** Claude Desktop, Claude Opus 4.5

**Problem:**
Two issues in `findDepthFormat()`:

**Flaw 1 - Incomplete capability check (lines 76-80):**
```cpp
for (auto format : candidates) {
    vk::FormatProperties props = m_device.physicalDevice().getFormatProperties(format);
    if (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
        return format;  // Only checks attachment, not sampling!
    }
}
```

But depth buffer is created with BOTH usages (line 34):
```cpp
imageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
```

And a sample view is created for deferred lighting (lines 57-65).

**Flaw 2 - Silent fallback masks failure (line 83):**
```cpp
return vk::Format::eD32Sfloat; // Fallback
```

If NO candidate format supports depth attachment, the code silently returns `eD32Sfloat` without any verification. This masks a fundamental capability failure that should crash immediately rather than cause undefined behavior later.

**Verification (Claude Opus 4.5):**
Confirmed both flaws:
1. Line 34: `usage = eDepthStencilAttachment | eSampled` - depth IS sampled
2. Lines 57-65: `m_depthSampleView` created for deferred lighting
3. Lines 76-80: Loop only checks `eDepthStencilAttachment`, not `eSampledImage`
4. Line 83: Returns unchecked format if loop finds nothing - silent failure

**Impact:**
- Flaw 1: If selected format supports attachment but not sampling, deferred lighting will have undefined behavior
- Flaw 2: If no candidate works, invalid format returned silently; image creation may fail cryptically or produce garbage

**Fix:**
1. Check for BOTH required features in the loop
2. Remove fallback - throw/assert if no suitable format found:
```cpp
for (auto format : candidates) {
    vk::FormatProperties props = m_device.physicalDevice().getFormatProperties(format);
    if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) &&
        (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage)) {
        return format;
    }
}
Assertion(false, "No suitable depth format found with attachment and sampling support");
return vk::Format::eUndefined; // Unreachable
```

---

### H11. G-Buffer Clear Flag Consumed Prematurely

**Status:** Open
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

### H12. Command Pool Reset Overload Selection Breaks in `VULKAN_HPP_NO_EXCEPTIONS`

**Status:** Open (Verified 2025-12-13)
**File:** `VulkanFrame.cpp:152-172`
**Source:** Fresh analysis

**Problem:**
`VulkanFrame::reset()` tries to support both Vulkan-Hpp signatures (returning `void` vs `vk::Result`), but it always calls the `std::true_type` lambda:

```cpp
auto reset_pool = [](auto& dev, VkCommandPool pool, std::true_type) { ... };
auto reset_pool_result = [](auto& dev, VkCommandPool pool, std::false_type) { ... };

vk::Result poolResult = reset_pool(..., std::integral_constant<bool, returnsVoid>{});
```

When `returnsVoid == false` (e.g. `VULKAN_HPP_NO_EXCEPTIONS`), this does not compile because the wrong lambda is selected.

**Verification (Local source inspection):**
Confirmed in `code/graphics/vulkan/VulkanFrame.cpp` that `reset_pool_result` is never called; `reset_pool(...)` is always invoked, and its third parameter is `std::true_type` so the call is ill-formed when `returnsVoid` is false (the argument becomes `std::false_type`).

**Impact:**
Build breaks under common “no exceptions” Vulkan-Hpp configurations.

**Fix:**
Use `if constexpr (returnsVoid)` to call `resetCommandPool()` and synthesize `vk::Result::eSuccess` in the `void` case; otherwise capture and validate the returned `vk::Result`.

---

### H13. Push Descriptor *Properties* Chained Into `getFeatures2()` (Wrong Query)

**Status:** Open (Verified 2025-12-13)
**File:** `VulkanDevice.cpp:526-534`
**Source:** Fresh analysis

**Problem:**
`vk::PhysicalDevicePushDescriptorPropertiesKHR` is a **properties** struct (for `getProperties2()`), but it is currently chained into the **features** chain used with `getFeatures2()`:

```cpp
vals.pushDescriptorProps = vk::PhysicalDevicePushDescriptorPropertiesKHR{};
vals.extDynamicState3.pNext = &vals.pushDescriptorProps;
dev.getFeatures2(&feats);
```

**Verification (Claude Opus 4.5):**
Confirmed. The code structure at lines 510-534:
1. Lines 515-517: `getProperties2(&props)` is called but with NO chain - only gets basic `VkPhysicalDeviceProperties`
2. Line 526: `pushDescriptorProps` initialized as `PhysicalDevicePushDescriptorPropertiesKHR`
3. Line 533: Incorrectly chained into features chain via `extDynamicState3.pNext`
4. Line 534: `getFeatures2(&feats)` called - wrong query type for a properties struct

The `maxPushDescriptors` field is never read anywhere in the codebase, making this a **latent bug**. The struct will contain uninitialized/zero values rather than actual device limits.

**Impact:**
Currently latent since `maxPushDescriptors` is unused. If code is added that relies on this property, it will get garbage/zero values, potentially causing push descriptor updates to exceed device limits.

**Fix:**
Chain into the properties query instead:
```cpp
props.pNext = &vals.pushDescriptorProps;
dev.getProperties2(&props);
```

---

## Medium Severity Issues

These bugs cause suboptimal behavior or potential edge-case failures.

### M1. Depth Barrier Stage Masks Incomplete

**Status:** Open
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

**Status:** Open
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

**Status:** Open
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

**Status:** Open
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

**Status:** Open
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

**Status:** Open
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

### M7. Shader Reflection `.cpp` Is a Stub / Duplicate Header

**Status:** Open (Verified 2025-12-13)
**Files:** `VulkanShaderReflection.cpp:1-43`, `VulkanShaderReflection.h:1-43`
**Source:** Fresh analysis

**Problem:**
`VulkanShaderReflection.cpp` duplicates the header content (including `#pragma once`) and does not implement any declared functions. It is effectively header text in a `.cpp`.

**Verification (GPT-5.2 Pro):**
1. `VulkanShaderReflection.cpp` is a line-for-line duplicate of `VulkanShaderReflection.h` (including `#pragma once`).
2. No definitions exist anywhere in the repo for `reflectShaderDescriptorBindings`, `validateShaderBindings`, or `validateShaderPair`.
3. Grep of the codebase shows zero call sites for these APIs.

**Impact:**
Currently this looks like dead code; if anything starts calling these APIs, it will become a linker error (or mislead future maintenance).

**Fix:**
Either implement the reflection functions and keep a real `.cpp`, or remove/stop compiling the `.cpp` and keep this as declarations only.

---

### M8. Potential Incorrect Unsigned Usage (Wraparound / Narrowing)

**Status:** Open
**Files:** `VulkanGraphics.cpp:480`, `VulkanTextureManager.cpp:143, 176, 215, 225`, `VulkanPipelineManager.cpp:182-183, 217, 225, 312`
**Source:** Fresh analysis

**Problem:**
Several code paths cast signed integers (or platform-sized `size_t`) into 32-bit unsigned types without validating bounds:

- `VulkanGraphics.cpp:480`: `static_cast<uint32_t>(batch.n_verts)` is used as `indexCount` for `drawIndexed()`. If `batch.n_verts <= 0` (or otherwise corrupted), this wraps to a huge value and submits an invalid draw.
- `VulkanTextureManager.cpp:143`: `layers = static_cast<uint32_t>(numFrames)` where `numFrames` is `int`. If `numFrames <= 0`, `layers` becomes a huge value and drives loops/allocations/copies.
- `VulkanTextureManager.cpp:215, 225`: loops use `width * height` in 32-bit math (`uint32_t`), which can overflow for large textures and under-iterate while still writing with `dst[i * 4 + ...]`.
- `VulkanPipelineManager.cpp:182-183, 217, 225`: `size_t` values (buffer index, stride, offsets) are narrowed to `uint32_t` for Vulkan structs without range checks.
- `VulkanPipelineManager.cpp:312`: debug output truncates `size_t layout_hash` via `static_cast<unsigned int>(...)` and `0x%x` formatting, hiding the full key on 64-bit builds.

**Impact:**
Edge-case failures become hard-to-debug GPU hangs/validation errors/crashes, and debug logs may be misleading on 64-bit builds.

**Fix:**
Add assertions (or explicit clamps) before narrowing/casting:
- Verify counts are positive and within `uint32_t` range before casting.
- Compute `width * height` in `size_t` (or `uint64_t`) and validate size fits destination buffers.
- For Vulkan struct fields, assert `<= std::numeric_limits<uint32_t>::max()` before `static_cast<uint32_t>(...)`.
- Update debug formatting for `size_t` (e.g., `%zx`) or cast to `uint64_t` and print with `PRIu64/PRIx64`.

---

## Files Affected

| File | Issues |
|------|--------|
| VulkanRenderer.cpp | C5, H1, H4, H5, C9 |
| VulkanRenderer.h | H1 |
| VulkanConstants.h | H1 |
| VulkanRingBuffer.cpp | C1 |
| VulkanFrame.cpp | H12 |
| VulkanBufferManager.cpp | C4, M4, M5 |
| VulkanDescriptorLayouts.cpp | C2, M2 |
| VulkanGraphics.cpp | C2, H7, H8, M6, M8 |
| VulkanRenderTargets.cpp | C3, H10 |
| VulkanRenderTargets.h | C3 |
| VulkanPipelineManager.cpp | C3, M8 |
| VulkanRenderingSession.cpp | H2, H3, H6, H11, M1 |
| VulkanTextureManager.cpp | C7, C8, H4, H9, M8 |
| VulkanTextureManager.h | C9, H4 |
| VulkanShaderManager.cpp | C6 |
| VulkanDevice.cpp | H13, M3 |
| VulkanShaderReflection.cpp | M7 |
| VulkanShaderReflection.h | M7 |

---

## Status Definitions

| Status | Meaning |
|--------|---------|
| **Unverified** | Bug identified by AI analysis, not yet manually confirmed in source |
| **Open** | Bug confirmed to exist via manual source inspection, fix pending |
| **Resolved** | Fix implemented, builds successfully, tests pass |

All bugs have been manually verified against source code. Bugs marked "Open" are confirmed to exist but fixes have not yet been implemented.

---

## Recommended Fix Priority

### Immediate (Will crash or corrupt)
1. ~~**C9** - Fallback texture crashes on first texture retirement~~ **(RESOLVED)**
2. **C1** - Ring buffer corruption causes random visual glitches
3. ~~**C6** - ARM crash affects Mac M-series users~~ **(RESOLVED)**
4. **C2** - Descriptor pool exhaustion after ~12k draws
5. ~~**C4**~~/C8 - Use-after-free on buffer/texture deletion - **C4 RESOLVED**
6. **H12** - Frame reset may not compile (NO_EXCEPTIONS)

### High Priority (Broken rendering)
7. **H7** - All transparency broken on EDS3 GPUs
8. ~~**H8** - UI clipping completely broken~~ **(RESOLVED)**
9. ~~**H1** - Frame count mismatch causes resource lifetime bugs~~ **(RESOLVED)**
10. **H2/H3** - Depth layout issues cause validation spam
11. **H13** - Push-descriptor properties queried incorrectly

### Medium Priority (Leaks and inefficiency)
12. **H4** - Texture memory leak
13. **M5** - Buffer handle vector growth
14. **M6** - Wasted descriptor sync work
15. **M7** - Shader reflection stub/duplication

---

## Changelog

- **2025-12-12 (Initial):** Report created from GPT-5.2 Pro analysis. Frame count mismatch (H1) verified.
- **2025-12-12 (Update 1):** Added Gemini 3 Deep Think findings (4 batches). Verified 13 additional bugs.
- **2025-12-12 (Update 2):** Added Claude Desktop findings. Verified 3 additional bugs including critical C9.
- **2025-12-12 (Update 3):** Consolidated all findings. Total: 26 verified bugs (9 critical, 11 high, 6 medium).
- **2025-12-12 (Update 4):** Folded in fresh-analysis items (H12, H13, M7). Total: 29 verified bugs (9 critical, 13 high, 7 medium).
- **2025-12-12 (Update 5):** Resolved H1 (frame count constant mismatch). Consolidated to single `kFramesInFlight = 2` constant.
- **2025-12-12 (Update 6):** Resolved C9 (fallback texture handle never initialized). Added `createFallbackTexture()` to create 1x1 black texture during `VulkanTextureManager` initialization.
- **2025-12-12 (Update 7):** Resolved C4 (immediate buffer deletion use-after-free). Modified `deleteBuffer()` to use deferred deletion via `m_retiredBuffers`, matching pattern used by `updateBufferData()` and `resizeBuffer()`. Added unit test `test_vulkan_buffer_manager_retirement.cpp`.
- **2025-12-12 (Update 8):** Resolved C6 (misaligned shader pointer on ARM). Changed filesystem shader loading from `std::vector<char>` to `std::vector<uint32_t>` to guarantee 4-byte alignment required by Vulkan spec for `pCode`. Added unit test `test_vulkan_shader_alignment.cpp`.
- **2025-12-12 (Update 9):** Verified H9 (buffer offset alignment for R8 textures). Corrected line numbers from 242 to 391/464. Confirmed bug exists only in `uploadImmediate()` path; ring buffer path (`flushPendingUploads()`) is safe due to `optimalBufferCopyOffsetAlignment`.
- **2025-12-12 (Update 10):** Verified H4 (texture pending destructions never processed). Confirmed all three methods (`setCurrentFrame`, `processPendingDestructions`, `retireTexture`) have zero callers in the codebase. The entire texture retirement system is dead code - currently a latent bug waiting to become active when texture eviction is implemented.
- **2025-12-13 (Update 11):** Re-verified H7 (blending unconditionally disabled with EDS3). Bug confirmed to exist. Corrected line numbers: `VulkanGraphics.cpp:946-948` to `899-901`, `VulkanGraphics.cpp:1193-1195` to `1146-1148`, `VulkanRenderingSession.cpp:250-252` to `247-249`. The functions are `gr_vulkan_render_primitives()`, `gr_vulkan_render_primitives_batched()`, and `applyDynamicState()`. Pipeline key correctly captures blend_mode at lines 481, 707, 1012 but EDS3 code overrides with `VK_FALSE`.
- **2025-12-13 (Update 12):** Verified H13 (push descriptor properties in wrong query chain). Confirmed `PhysicalDevicePushDescriptorPropertiesKHR` is chained into `getFeatures2()` at line 533 instead of `getProperties2()` at line 516. The `maxPushDescriptors` field is never read, making this a latent bug that will cause incorrect values if the property is ever used.
- **2025-12-13 (Update 13):** Verified C7 (large texture infinite loop). Corrected line numbers from 495-498 to 644-646. Confirmed `STAGING_RING_SIZE` is 12 MiB, textures are queued without size checking, and no fallback to `uploadImmediate()` exists. Any texture exceeding 12 MiB will be deferred forever.
- **2025-12-13 (Update 14):** Verified M7 (shader reflection stub/duplication). Confirmed `VulkanShaderReflection.cpp` duplicates the header and there are no implementations or call sites for the declared APIs.
- **2025-12-13 (Update 15):** Verified C5 (swapchain acquire crashes on resize). Confirmed `acquireImage()` returns sentinel after successful swapchain recreation without retrying acquisition. The assertion in `flip()` correctly fires on sentinel, but the bug is in `acquireImage()` not retrying.
- **2025-12-13 (Update 16):** Verified C8 (immediate texture deletion use-after-free). Corrected line numbers from 754-758 to 908-912. Confirmed `deleteTexture()` does immediate RAII destruction while `retireTexture()` properly defers. Neither function has any callers - latent bug.
- **2025-12-13 (Update 17):** Verified and expanded H10 (depth format selection flaws). Confirmed two issues: (1) loop only checks `eDepthStencilAttachment` but depth is also sampled for deferred lighting, (2) silent fallback at line 83 returns unchecked `eD32Sfloat` if loop finds nothing - masks capability failure. Fix should assert/throw instead of fallback.
