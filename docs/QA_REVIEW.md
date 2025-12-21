# Vulkan QA Review (code/graphics/vulkan)

Date: 2025-12-21

Scope: QA review of Vulkan backend code in `code/graphics/vulkan/`, with emphasis on correctness, lifetime/sync,
validation-safety, and “foot-gun” APIs.

## Critical

### Texture uploads were previously flushed while dynamic rendering is active (invalid), now mitigated

- `VulkanTextureManager::flushPendingUploads()` is documented as **only callable when no rendering is active**
  (`code/graphics/vulkan/VulkanTextureManager.h:130`), and it records barriers + `copyBufferToImage`
  (`code/graphics/vulkan/VulkanTextureManager.cpp:587`, `code/graphics/vulkan/VulkanTextureManager.cpp:790`).
- However, the primitive draw path starts dynamic rendering before resolving texture descriptors:
  - `gr_vulkan_render_primitives()` calls `ensureRenderingStarted()` (`code/graphics/vulkan/VulkanGraphics.cpp:760`).
  - It then calls `getTextureDescriptor()` (`code/graphics/vulkan/VulkanGraphics.cpp:932`).
  - Previously, `VulkanRenderer::getTextureDescriptor()` “lazy loaded” a missing texture by calling
    `queueTextureUpload()` + `flushPendingUploads()` while rendering could already be active.

Current behavior (mitigation applied in local changes):
- `VulkanRenderer::getTextureDescriptor()` queues the upload but **never flushes uploads**; it returns a stable fallback
  descriptor on miss (`code/graphics/vulkan/VulkanRenderer.cpp:583`).
- Upload flush is centralized to `VulkanRenderer::beginFrame()` before any rendering begins
  (`code/graphics/vulkan/VulkanRenderer.cpp:187`).

Impact:
- This can trigger validation errors and/or undefined behavior when a texture is first referenced mid-pass.
- It also creates an implicit “record transfer commands in the middle of a render pass” hazard.

### Texture eviction/deletion previously destroyed GPU resources immediately (descriptor use-after-free risk), now partially mitigated

Previously:
- LRU eviction erased entries in `onTextureResident()` (RAII destroyed `VkImage`/`VkImageView` immediately).
- `deleteTexture()` erased immediately too.

Current behavior (mitigation applied in local changes):
- Eviction path marks textures `Retired` and defers actual destruction via `m_pendingDestructions` +
  `processPendingDestructions()` (`code/graphics/vulkan/VulkanTextureManager.cpp:906`,
  `code/graphics/vulkan/VulkanTextureManager.cpp:1022`).
- `deleteTexture()` now also defers destruction (`code/graphics/vulkan/VulkanTextureManager.cpp:868`).
- `VulkanRenderer` provides a conservative “safe retire serial” (last submitted) via
  `VulkanTextureManager::setSafeRetireSerial()` (`code/graphics/vulkan/VulkanRenderer.cpp:205`).

Impact:
- If any submitted command buffer still references a descriptor pointing at an image/view that gets erased,
  the GPU can read freed resources.

## High

### Frame lifecycle / serial tracking is inconsistent (breaks safe deferred destruction)

Previously:
- `prepareFrameForReuse()` was invoked twice (recycle + beginRecording), and `completedSerial` was always `0`.

Current behavior (mitigation applied in local changes):
- `prepareFrameForReuse()` is executed exactly once per recycled frame (`code/graphics/vulkan/VulkanRenderer.cpp:307`).
- A monotonic `m_completedSerial` is tracked from FIFO fence waits and passed into
  `VulkanTextureManager::processPendingDestructions()` (`code/graphics/vulkan/VulkanRenderer.cpp:319`).

Impact:
- Anything keyed off “GPU completed serial” is unreliable.
- Cleanup can run earlier/later than intended, depending on which path is taken.

### Debug/telemetry file IO in hot paths

Previously: hard-coded “agent log” writes existed in hot paths.

Current behavior (mitigation applied in local changes):
- Removed hard-coded file IO blocks; no `c:\\...\\.cursor\\debug.log` writes remain in `code/graphics/vulkan/`.

Impact:
- Non-portable Windows-only absolute path.
- Potentially catastrophic perf (sync file IO per draw / per texture event).
- Not gated behind `FSO_DEBUG`/`Cmdline_graphics_debug_output`.

### Texture upload state machine was inconsistent (fixed)

Previously, the code mixed a "recorded upload" vs "GPU completed upload" concept:
- `flushPendingUploads()` moved textures to `Resident` immediately, while `markUploadsCompleted()` attempted to
  transition `Uploading -> Resident` even though nothing set `Uploading`.

Current behavior:
- The unused `Uploading` state and `markUploadsCompleted()` path were removed.
- Uploads are recorded during `VulkanRenderer::beginFrame()` into the same primary command buffer (before rendering),
  so a separate "upload completion" state machine is not required for correctness.

## Medium

### Validation layers enabled but callback drops messages

Previously: `debugReportCallback()` discarded all validation output.

Current behavior (mitigation applied in local changes):
- `debugReportCallback()` logs messages via `vkprintf` (`code/graphics/vulkan/VulkanDevice.cpp:35`).

Impact:
- Makes diagnosis harder when Vulkan validation is enabled.

### `VulkanShaderReflection.cpp` is a header duplicate / dead TU

`code/graphics/vulkan/VulkanShaderReflection.cpp` is effectively a copy of the header and provides no definitions.

Impact:
- Confusing maintenance signal; looks like a missing implementation.

### Swapchain layout transition uses BottomOfPipe as destination stage (sync2 smell)

`transitionSwapchainToPresent()` uses `dstStageMask = eBottomOfPipe` (`code/graphics/vulkan/VulkanRenderingSession.cpp:369`).

Impact:
- Often discouraged in sync2; can cause validation/perf issues on some drivers/platforms.

### `VulkanRenderingSession::endActivePass()` is a no-op despite being called at boundaries

`endActivePass()` does nothing and relies on `RenderScope` RAII (`code/graphics/vulkan/VulkanRenderingSession.cpp:129`).

Impact:
- Easy to misuse (callers may assume it actually ends rendering).
- Boundary APIs must ensure scopes are destroyed before switching targets.

## Low

### `VulkanFrame::reset()` overload handling is incomplete

`VulkanFrame::reset()` attempts to handle “void-return vs vk::Result-return” but only defines the `true_type` path
(`code/graphics/vulkan/VulkanFrame.cpp:84`). If `resetCommandPool()` returns `vk::Result`, this will not compile.

### Unfinished / empty branches and TODOs

- Static buffers are host-visible with TODO for staging/device-local (`code/graphics/vulkan/VulkanBufferManager.cpp:53`).
- Deferred shader pipeline caching is TODO (`code/graphics/vulkan/VulkanRenderer.cpp:1194`).
- Empty `else {}` in deferred lights (`code/graphics/vulkan/VulkanDeferredLights.cpp:76`).

## Architecture Notes (Texture Manager Focus)

The texture manager and its call sites currently mix **resource residency**, **descriptor availability**, and
**upload execution** in a way that creates correctness hazards (especially “flush while rendering”).

### What looks risky in the current design

- “Descriptor request” (`getTextureDescriptor`) can perform GPU work (record barriers/copies) at arbitrary times.
- There is no enforced contract that “uploads happen only at frame-start (or other safe points).”
- Destruction/eviction is not tied to a real completed GPU timeline/serial, and the existing serial-based
  destruction logic is not wired up.
- Slot assignment/eviction is coupled to the texture record lifetime; erasing the record destroys the GPU view even
  if descriptors may still reference it.

### Recommended direction

1) Make texture residency requests non-blocking and side-effect free for command recording:
   - If a texture is not resident at draw time, bind a stable fallback/default descriptor and **queue** the upload.
   - Defer `flushPendingUploads()` to a known-safe point (e.g., `VulkanRenderer::beginFrame()`).

2) Decouple GPU lifetime from bmpman lifetime and from slot bookkeeping:
   - A descriptor slot should always point to a valid `VkImageView` (fallback/default until real data arrives).
   - When retiring/evicting, overwrite the slot with fallback first, then destroy the old view only after the GPU is
     known to be past the last submission that could reference it.

3) Use one source of truth for “GPU completed”:
   - Either fences per frame or a timeline semaphore/serial, but it must be consistent and propagated so that
     `processPendingDestructions()` has a meaningful `completedSerial`.

4) Make state machine explicit and tested:
   - e.g., `Missing -> Queued -> Uploading (recorded in frame N) -> Resident (GPU completed frame N)` and keep
     descriptor updates in the same phase (frame start) to avoid mid-pass hazards.

## Suggested Next Steps (Practical)

- Remove or gate all “agent log” file IO behind debug flags, and remove hard-coded absolute paths.
- Decide the policy for texture misses during a frame:
  - Option A: allow drawing with fallback and update next frame (recommended for correctness).
  - Option B: prefetch/ensure-resident before any rendering begins (requires stronger call-site discipline).
- Wire up texture retirement to real completed GPU progress (serial/timeline) before enabling any eviction.
- Fix `VulkanRenderer` frame reuse flow so `completedSerial` is meaningful and `prepareFrameForReuse()` is only
  applied once per frame recycle.
