# Vulkan QA Review (code/graphics/vulkan)

Date: 2025-12-21

Scope: QA review of Vulkan backend code in `code/graphics/vulkan/`, with emphasis on correctness, lifetime/sync,
validation-safety, and "foot-gun" APIs.

## Resolved (As Of 2025-12-21)

### Texture uploads no longer flushed mid-rendering

- Upload flush is centralized to `VulkanRenderer::beginFrame()` before any rendering begins.
- Upload recording requires an `UploadCtx` token (only constructible by `VulkanRenderer`), so draw paths cannot record GPU work.
- Draw-path texture requests (`VulkanTextureBindings`) queue uploads and return fallback descriptors.

### Texture eviction/deletion defers GPU resource destruction

- Eviction/deletion defers destruction via a serial-gated deferred release queue (`VulkanDeferredRelease.h`).
- `deleteTexture()` defers retirement to the next upload-phase flush (`m_pendingRetirements`) to prevent mid-frame slot reuse.
- `retireTexture()` drops cache state immediately and moves GPU handles into the deferred queue.
- Eviction only considers textures whose `lastUsedSerial <= completedSerial`.

### Bindless descriptors are always valid

- Model bindless descriptor array is fully written every frame: initialize all `kMaxBindlessTextures` entries to fallback, then
  patch resident textures into their slots. This avoids relying on partially-bound descriptors.
- The model bindless binding does not use `vk::DescriptorBindingFlagBits::ePartiallyBound`, and the model path no longer
  requires `descriptorBindingPartiallyBound` device support.
- `getBindlessSlotIndex()` returns a real slot (or slot 0 fallback) even when a texture is not yet resident.

### Frame lifecycle / serial tracking

- `prepareFrameForReuse()` executes exactly once per recycled frame.
- A monotonic `m_completedSerial` is tracked from FIFO fence waits and passed into manager `collect()` calls.
- Resource retirement during `beginFrame()` uses the upcoming submit serial (prevents premature destruction).

### Debug/telemetry file IO removed

- Hard-coded file IO blocks were removed from hot paths.

### Texture upload state machine simplified

- Unused `Uploading` state and `markUploadsCompleted()` path removed.
- Texture state is container-based ("state as location"): `m_residentTextures`, `m_pendingUploads`, `m_unavailableTextures`.

## Medium

### Validation callback logging is basic

`debugReportCallback()` logs messages via `vkprintf` (`code/graphics/vulkan/VulkanDevice.cpp`).

### `VulkanShaderReflection.cpp` is a header duplicate / dead TU

`code/graphics/vulkan/VulkanShaderReflection.cpp` is effectively a copy of the header and provides no definitions.

### Swapchain layout transition uses BottomOfPipe as destination stage (sync2 smell)

`transitionSwapchainToPresent()` uses `dstStageMask = eBottomOfPipe` (`code/graphics/vulkan/VulkanRenderingSession.cpp`).

### `VulkanRenderingSession::endActivePass()` is a no-op despite being called at boundaries

`endActivePass()` does nothing and relies on `RenderScope` RAII (`code/graphics/vulkan/VulkanRenderingSession.cpp`).

### Stale documentation

- `code/graphics/vulkan/VulkanTextureBindings.h` comment for `bindlessIndex()` is stale: bindless indices now always return a
  valid slot (fallback if not resident).

## Low

### `VulkanFrame::reset()` overload handling is incomplete

`VulkanFrame::reset()` attempts to handle "void-return vs vk::Result-return" but only defines the `true_type` path
(`code/graphics/vulkan/VulkanFrame.cpp`). If `resetCommandPool()` returns `vk::Result`, this will not compile.

### Unfinished / empty branches and TODOs

- Static buffers are host-visible with TODO for staging/device-local (`code/graphics/vulkan/VulkanBufferManager.cpp`).
- Deferred shader pipeline caching is TODO (`code/graphics/vulkan/VulkanRenderer.cpp`).
- Empty `else {}` in deferred lights (`code/graphics/vulkan/VulkanDeferredLights.cpp`).

## Tech Debt (Design Philosophy)

### `DeferredBoundaryState` enum

`VulkanRenderer` uses `enum class DeferredBoundaryState { Idle, InGeometry, AwaitFinish }`.

Per `docs/DESIGN_PHILOSOPHY.md`, state enums should be replaced with typestate types or container membership.

### `m_renderingActive` boolean flag

`VulkanRenderingSession` tracks rendering state via a boolean flag.

The `ActivePass` RAII guard's existence already proves rendering is active; the flag is redundant.

### `MODEL_OFFSET_ABSENT` sentinel

`VulkanModelTypes.h` defines `MODEL_OFFSET_ABSENT = 0xFFFFFFFFu`.

Used at domain boundaries (asset genuinely has no texture map). Acceptable per philosophy (boundary-only sentinel), but could be
replaced with `std::optional<uint32_t>` at API boundaries.
