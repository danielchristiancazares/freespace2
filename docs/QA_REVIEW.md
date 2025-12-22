# Vulkan QA Review (code/graphics/vulkan)

Date: 2025-12-22

Scope: QA review of Vulkan backend code in `code/graphics/vulkan/`, with emphasis on correctness, lifetime/sync,
validation-safety, and "foot-gun" APIs.

## Resolved (As Of 2025-12-22)

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
- Swapchain present transition uses a sync2-compatible "NONE" destination stage rather than `BottomOfPipe`.

### Debug/telemetry file IO removed

- Hard-coded file IO blocks were removed from hot paths.

### Texture upload state machine simplified

- Unused `Uploading` state and `markUploadsCompleted()` path removed.
- Texture state is container-based ("state as location"): `m_residentTextures`, `m_pendingUploads`, `m_unavailableTextures`.

### Rendering pass lifetime is boundary-owned (not scope-owned)

- Dynamic rendering is started via an idempotent `VulkanRenderingSession::ensureRendering()` (session owns the active pass).
- Frame/target boundaries always end any active pass internally (`endActivePass()` is no longer a no-op).
- Draw paths consume a `RenderCtx` capability token from `VulkanRenderer::ensureRenderingStarted(frameCtx)` as proof that rendering is active.
- Internal VulkanRenderer draw code (deferred lighting recording) also consumes `RenderCtx` rather than pulling a raw command buffer
  from `RecordingFrame`.
- Draw code can no longer grab the raw command buffer from `VulkanFrame`/`FrameCtx` as an escape hatch:
  `VulkanFrame::commandBuffer()` is private, `RecordingFrame::cmd()` is private, and `FrameCtx` no longer exposes `RecordingFrame` or `cmd()`.
- Recording-only operations that still need a command buffer (frame setup dynamic state, debug labels) go through `VulkanRenderer` methods
  that require a `FrameCtx` token.

### Deferred lighting call order uses typestate tokens (no enum)

- Deferred lighting boundaries use move-only tokens (`DeferredGeometryCtx` -> `DeferredLightingCtx`) rather than a state enum.
- The boundary API (`gr_vulkan_deferred_lighting_begin/end/finish`) stores those tokens and enforces correct call order.

## Medium

### Validation callback logging is basic

`debugReportCallback()` logs messages via `vkprintf` (`code/graphics/vulkan/VulkanDevice.cpp`).

### `VulkanShaderReflection.cpp` is a header duplicate / dead TU

`code/graphics/vulkan/VulkanShaderReflection.cpp` is effectively a copy of the header and provides no definitions.

## Low

### Unfinished / empty branches and TODOs

- Static buffers are host-visible with TODO for staging/device-local (`code/graphics/vulkan/VulkanBufferManager.cpp`).
- Deferred shader pipeline caching is TODO (`code/graphics/vulkan/VulkanRenderer.cpp`).
- Empty `else {}` in deferred lights (`code/graphics/vulkan/VulkanDeferredLights.cpp`).

## Tech Debt (Design Philosophy)

### `MODEL_OFFSET_ABSENT` sentinel

`VulkanModelTypes.h` defines `MODEL_OFFSET_ABSENT = 0xFFFFFFFFu`.

Used only for absent vertex attribute offsets. Model material texture indices are now always valid: the engine reserves
well-known bindless slots for default textures (base=white, normal=flat, spec=dielectric), so shaders no longer need an
\"absent texture\" sentinel for model materials.
