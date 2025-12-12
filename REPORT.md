# Vulkan Renderer Bug Investigation

**Status:** Ongoing Investigation
**Started:** 2025-12-12
**Analysis Tool:** GPT-5.2 Pro (40-minute extended reasoning)
**Verification:** Claude Opus 4.5

This document tracks potential bugs identified in the Vulkan renderer. Items marked "Verified" have been confirmed against the source code. Items marked "Pending" require manual verification before fixing.

---

## Summary

| Severity | Total | Verified | Pending |
|----------|-------|----------|---------|
| Critical | 5 | 0 | 5 |
| High | 4 | 1 | 3 |
| Medium | 3 | 0 | 3 |

---

## Critical Issues

These bugs cause crashes, data corruption, or resource exhaustion.

### C1. Ring Buffer Wraps Mid-Frame (Data Corruption)

**Status:** Pending
**File:** `VulkanRingBuffer.cpp:28-60`

**Problem:**
When out of space, the ring buffer wraps to offset 0 instead of failing:

```cpp
if (alignedOffset + requestSize > m_size) {
    alignedOffset = 0;
    ...
}
```

**Impact:**
Within a single frame, earlier allocations may still be in use by the GPU when later allocations overwrite them. Causes silent data corruption, flickering, garbage rendering.

**Suggested Fix:**
Do not wrap within a frame. Assert/throw when out of space, or implement a growable allocator with per-frame tracking.

---

### C2. Model Descriptor Sets Allocated Per Draw (Pool Explosion)

**Status:** Pending
**Files:** `VulkanGraphics.cpp:590-640`, `VulkanDescriptorLayouts.cpp:110-165`

**Problem:**
Model rendering path allocates a new descriptor set every draw call and never frees or resets the pool. Each model set includes a bindless array of 1024 samplers.

**Impact:**
Pool sizes become astronomical: `kModelSetsPerPool (4096*3=12288) * kMaxBindlessTextures (1024)` = ~12.5 million descriptors. Most drivers will fail pool creation or exhaust descriptor heap memory.

**Suggested Fix:**
Allocate one model descriptor set per frame-in-flight (already exists as `frame.modelDescriptorSet`). Use dynamic offsets and push constants for per-draw variation.

---

### C3. GBuffer Format Array Mismatch (Pipeline Incompatibility)

**Status:** Pending
**Files:** `VulkanRenderTargets.cpp`, `VulkanRenderTargets.h`, `VulkanPipelineManager.cpp`

**Problem:**
GBuffer attachments use different formats:
```cpp
{ R16G16B16A16Sfloat, R16G16B16A16Sfloat, R8G8B8A8Unorm }
```

But `VulkanRenderTargets` exposes only a single `m_gbufferFormat` (first format), and `VulkanPipelineManager` replicates that format for all attachments.

**Impact:**
Dynamic rendering requires pipeline's `pColorAttachmentFormats[]` to match actual formats. Mismatch causes pipeline creation failure, validation errors, or undefined rendering.

**Suggested Fix:**
Store and expose all gbuffer formats as an array. Extend `PipelineKey` to include all attachment formats. Pass correct format array to `vk::PipelineRenderingCreateInfo`.

---

### C4. Immediate Buffer Deletion (Use-After-Free)

**Status:** Pending
**File:** `VulkanBufferManager.cpp:55-95`

**Problem:**
`deleteBuffer()` resets `UniqueBuffer`/`UniqueDeviceMemory` immediately, unlike resize/update which defer to `m_retiredBuffers`.

**Impact:**
If GPU is still using the buffer (common with multiple frames in flight), immediate deletion causes use-after-free on GPU, validation errors, and driver crashes.

**Suggested Fix:**
Move buffers to `m_retiredBuffers` with `retiredAtFrame = m_currentFrame` and destroy after safe delay (same pattern as resize/update).

---

### C5. Swapchain Acquire Crashes on Resize

**Status:** Pending
**File:** `VulkanRenderer.cpp:274-326`

**Problem:**
`acquireImage()` returns sentinel value when swapchain needs recreation, but `flip()` asserts this never happens:

```cpp
uint32_t imageIndex = acquireImage(frame.imageAvailableSemaphore());
Assertion(imageIndex != std::numeric_limits<uint32_t>::max(), ...);
```

**Impact:**
Hard crash when window is resized or minimized (VK_ERROR_OUT_OF_DATE_KHR / VK_SUBOPTIMAL_KHR).

**Suggested Fix:**
Handle sentinel return in `flip()`: skip frame and re-acquire after swapchain recreation.

---

## High Severity Issues

These bugs cause validation errors, incorrect rendering, or resource leaks.

### H1. Frame Count Constant Mismatch

**Status:** Verified
**Files:** `VulkanRenderer.h:139`, `VulkanConstants.h:8`

**Problem:**
Two constants define frames-in-flight with conflicting values:

| Constant | Value | Location |
|----------|-------|----------|
| `MAX_FRAMES_IN_FLIGHT` | 2 | VulkanRenderer.h:139 |
| `kFramesInFlight` | 3 | VulkanConstants.h:8 |

Renderer uses `MAX_FRAMES_IN_FLIGHT = 2` for frame cycling, but texture manager uses `kFramesInFlight = 3`.

**Impact:**
Wasted memory (resources held one frame longer), unused array slots, inconsistent codebase inviting future bugs.

**Root Cause:**
Value `3` likely confused with `kGBufferCount = 3` (VulkanRenderTargets.h:13) which defines G-buffer attachments, unrelated to frame synchronization.

**Suggested Fix:**
Consolidate to single constant `MAX_FRAMES_IN_FLIGHT = 2` in VulkanConstants.h. Remove from VulkanRenderer.h. Update all references.

---

### H2. Depth Layout Tracking Incorrect

**Status:** Pending
**File:** `VulkanRenderingSession.cpp:42-170`

**Problem:**
Depth transitions assume incorrect `oldLayout`:
- `transitionDepthToAttachment` assumes `DEPTH_ATTACHMENT_OPTIMAL`
- But `endDeferredGeometry()` transitions depth to `SHADER_READ_ONLY_OPTIMAL`
- Next frame's transition uses wrong source layout

**Impact:**
Validation layer errors, undefined behavior, potential rendering corruption.

**Suggested Fix:**
Track current layout state for depth image. Update after each barrier. Use tracked state as `oldLayout`.

---

### H3. Deferred Depth Sampling/Attachment Conflict

**Status:** Pending
**Files:** `VulkanRenderingSession.cpp`, `VulkanRenderer.cpp:350-400`

**Problem:**
After deferred geometry:
1. Depth transitioned to `SHADER_READ_ONLY_OPTIMAL`
2. Swapchain rendering binds same depth as attachment with `DEPTH_ATTACHMENT_OPTIMAL`
3. Global descriptors reference it as shader-read

An image subresource cannot be in two layouts simultaneously.

**Impact:**
Validation errors, undefined behavior.

**Suggested Fix:**
Options:
1. Don't bind depth attachment during deferred lighting pass
2. Use `DEPTH_READ_ONLY_OPTIMAL` for both sampling and attachment (with depth writes disabled)
3. Use separate depth copy for sampling

---

### H4. Texture Destruction Never Called (Memory Leak)

**Status:** Pending
**Files:** `VulkanTextureManager.h`, `VulkanTextureManager.cpp`, `VulkanRenderer.cpp`

**Problem:**
Texture manager has `processPendingDestructions(currentFrame)` and `setCurrentFrame()`, but renderer never calls them. Only calls:
```cpp
m_textureManager->markUploadsCompleted(m_currentFrame);
```

**Impact:**
`retireTexture()` queues VkImage destruction but nothing processes it. VkImage memory leaks indefinitely.

**Suggested Fix:**
In `flip()` or frame begin/end, call:
```cpp
m_textureManager->setCurrentFrame(frameCounter);
m_textureManager->processPendingDestructions(frameCounter);
```

---

## Medium Severity Issues

These bugs cause suboptimal behavior or potential edge-case failures.

### M1. Depth Barrier Stage Masks Incomplete

**Status:** Pending
**File:** `VulkanRenderingSession.cpp:136-165`

**Problem:**
Depth transition uses only early fragment tests:
```cpp
bd.srcStageMask = eEarlyFragmentTests;
bd.srcAccessMask = eDepthStencilAttachmentWrite;
```

Depth writes may occur in late fragment tests depending on pipeline.

**Suggested Fix:**
Use `srcStageMask = eEarlyFragmentTests | eLateFragmentTests`.

---

### M2. Descriptor Limit Validation Incorrect

**Status:** Pending
**File:** `VulkanDescriptorLayouts.cpp:10-30`

**Problem:**
Compares `maxDescriptorSetStorageBuffers` (per-set limit, typically 16-32) against `kModelSetsPerPool` (~12k):
```cpp
Assertion(limits.maxDescriptorSetStorageBuffers >= kModelSetsPerPool, ...)
```

This assertion is conceptually wrong and fails on most GPUs.

**Suggested Fix:**
Check correct limits:
- `maxDescriptorSetStorageBuffers >= 1`
- `maxDescriptorSetSampledImages >= kMaxBindlessTextures`
- Consider `maxPerStageDescriptorSampledImages`

---

### M3. Shader Reflection File is Stub

**Status:** Pending
**File:** `VulkanShaderReflection.cpp`

**Problem:**
File contains only duplicated struct declarations and forward declarations, no function definitions. Contains `#pragma once` in a `.cpp` file.

**Impact:**
Linker errors if any translation unit expects these functions. Indicates incomplete refactor.

**Suggested Fix:**
Either implement reflection functions or remove/disable compilation of this file.

---

## Additional Issues (Lower Priority)

### A1. Global Descriptor Set Updated While In Use

**File:** `VulkanRenderer.cpp` (`bindDeferredGlobalDescriptors()`)

Only one global descriptor set exists, but multiple frames can be in flight. Updating while in use violates spec unless update-after-bind is enabled.

**Fix:** Allocate global set per frame, or enable update-after-bind with proper flags.

---

### A2. Depth Format Selection Doesn't Ensure Sampling Support

**File:** `VulkanRenderTargets.cpp` (`findDepthFormat()`)

Chooses format only by `DEPTH_STENCIL_ATTACHMENT_BIT`, but image is also sampled.

**Fix:** Also require sampled image feature bits.

---

## Files Affected

| File | Issues |
|------|--------|
| VulkanRenderer.cpp | C5, H1, H4, A1 |
| VulkanRenderer.h | H1 |
| VulkanConstants.h | H1 |
| VulkanRingBuffer.cpp | C1 |
| VulkanBufferManager.cpp | C4 |
| VulkanDescriptorLayouts.cpp | C2, M2 |
| VulkanGraphics.cpp | C2 |
| VulkanRenderTargets.cpp | C3, A2 |
| VulkanRenderTargets.h | C3 |
| VulkanPipelineManager.cpp | C3 |
| VulkanRenderingSession.cpp | H2, H3, M1 |
| VulkanTextureManager.cpp | H1, H4 |
| VulkanTextureManager.h | H1, H4 |
| VulkanShaderReflection.cpp | M3 |

---

## Next Steps

1. Verify pending issues against source code
2. Prioritize fixes by impact (Critical first)
3. Fix frame count mismatch (H1) - already verified
4. Address ring buffer wrap (C1) and immediate deletion (C4) - data corruption risks
5. Resolve descriptor pool explosion (C2) - resource exhaustion

---

## Changelog

- **2025-12-12:** Initial report created from GPT-5.2 Pro analysis. Frame count mismatch (H1) verified.
