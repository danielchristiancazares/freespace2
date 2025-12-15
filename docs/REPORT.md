# Vulkan Renderer Bug Investigation - Open Issues

**Status:** Ongoing Investigation
**Started:** 2025-12-12
**Last Updated:** 2025-12-13

**Related Documents:**
- [REPORT_FIXED.md](REPORT_FIXED.md) - Resolved issues
- [REPORT_CHANGELOG.md](REPORT_CHANGELOG.md) - Full change history

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

| Severity | Open |
|----------|------|
| Critical | 6    |
| High     | 8    |
| Medium   | 7    |
| Total    | 21   |

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

### C5. Swapchain Acquire Crashes on Resize

**Status:** Open (Addressed, not fixed)
**File:** `VulkanRenderer.cpp:113-138, 301-302`
**Source:** GPT-5.2 Pro

**Problem:**
`flip()` asserts when `acquireImage()` returns a sentinel value:

```cpp
uint32_t imageIndex = acquireImage(frame);
Assertion(imageIndex != std::numeric_limits<uint32_t>::max(), "flip() failed to acquire swapchain image");
```

**Root Cause:**
`flip()` cannot handle acquisition failure. It requires a valid image index and crashes otherwise. This is the actual bug - the function has a precondition that cannot always be satisfied.

**What was addressed (not fixed):**
Retry logic was added to `acquireImage()` so that after successful swapchain recreation, it retries acquisition instead of immediately returning sentinel. This reduces crash frequency but does not fix the bug:
- If recreation fails, sentinel still returned → crash
- If retry fails, sentinel still returned → crash
- The assertion in `flip()` is still a guard against invalid state
- Invalid state can still occur, just less frequently

**Why this is not a fix:**
A fix would eliminate the invalid state. The assertion would become dead code. Instead:
- Retries imply invalid data can still enter
- Early return on failure implies invalid state can still occur
- The guard (assertion) is still necessary and can still fire

**Impact:**
Hard crash when window is resized/minimized and either recreation or retry fails.

**Actual fix required:**
Restructure `flip()` to handle acquisition failure gracefully (skip the frame, return early without asserting) so that invalid image index is not an error condition but an expected possibility that the function can handle.

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

## High Severity Issues

These bugs cause validation errors, incorrect rendering, or resource leaks.

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
Build breaks under common "no exceptions" Vulkan-Hpp configurations.

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
| VulkanRenderer.cpp | C5, H4, H5 |
| VulkanRingBuffer.cpp | C1 |
| VulkanFrame.cpp | H12 |
| VulkanDescriptorLayouts.cpp | C2, M2 |
| VulkanGraphics.cpp | C2, M6, M8 |
| VulkanRenderTargets.cpp | C3 |
| VulkanRenderTargets.h | C3 |
| VulkanPipelineManager.cpp | C3, M8 |
| VulkanRenderingSession.cpp | H2, H3, H6, H11, M1 |
| VulkanTextureManager.cpp | C7, C8, H4, M8 |
| VulkanTextureManager.h | H4 |
| VulkanDevice.cpp | H13 |
| VulkanShaderReflection.cpp | M7 |
| VulkanShaderReflection.h | M7 |
| VulkanBufferManager.cpp | M4, M5 |

---

## Status Definitions

| Status | Meaning |
|--------|---------|
| **Open** | Bug confirmed to exist via manual source inspection, fix pending |
| **Resolved** | Fix implemented, builds successfully, tests pass (see [REPORT_FIXED.md](REPORT_FIXED.md)) |

All bugs have been manually verified against source code.

---

## Recommended Fix Priority

### Immediate (Will crash or corrupt)
1. **C1** - Ring buffer corruption causes random visual glitches
2. **C2** - Descriptor pool exhaustion after ~12k draws
3. **C5** - Crash on window resize (addressed, not fixed)
4. **C7** - Large textures never upload (game hangs)
5. **C8** - Use-after-free on texture deletion (latent)
6. **H12** - Frame reset may not compile (NO_EXCEPTIONS)

### High Priority (Broken rendering)
7. **H2/H3** - Depth layout issues cause validation spam
8. **H5** - Global descriptor race condition
9. **H6** - LoadOp::Load with undefined layout
10. **H11** - Clear flag consumed prematurely
11. **H13** - Push-descriptor properties queried incorrectly

### Medium Priority (Leaks and inefficiency)
12. **H4** - Texture memory leak (latent)
13. **M4** - Per-buffer allocation hits device limits
14. **M5** - Buffer handle vector growth
15. **M6** - Wasted descriptor sync work
16. **M7** - Shader reflection stub/duplication
