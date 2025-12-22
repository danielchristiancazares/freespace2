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

- Model bindless descriptor array is initialized once per per-frame descriptor set (full fallback fill) and then updated incrementally
  by dirty slot ranges. This avoids relying on partially-bound descriptors.
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

### Validation callback logging is actionable

- `debugReportCallback()` logs include severity/type, message id/name, and object/label context, with duplicate suppression to reduce spam
  (`code/graphics/vulkan/VulkanDevice.cpp`).

### Dead shader reflection TU removed

- Removed `code/graphics/vulkan/VulkanShaderReflection.cpp` (it duplicated the header and provided no definitions).

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

### Model vertex attribute presence uses a mask (no sentinel)

- Removed `MODEL_OFFSET_ABSENT`; `ModelPushConstants` now includes `vertexAttribMask` and shaders only consume offsets when the
  corresponding bit is present.

### Static buffers are device-local (staging upload implemented)

- `BufferUsageHint::Static` allocates device-local memory; updates use a staging upload path when buffers are not mapped
  (`code/graphics/vulkan/VulkanBufferManager.cpp`).
- `updateBufferData(..., nullptr)` is treated as allocation-only (persistent mapping setup).

### Deferred lighting no longer leaves undefined swapchain pixels

- Removed temporary diagnostic output paths in `code/graphics/shaders/deferred.frag` that produced magenta output.
- The ambient deferred-light pass now *initializes background pixels* (no geometry) so `loadOp=LOAD` + fragment discard
  cannot manifest as "trails" / stale swapchain memory.
- Deferred begin snapshots the current swapchain color into a per-swapchain-image scene buffer and copies it into the
  emissive G-buffer attachment (OpenGL parity; requires `TRANSFER_SRC` swapchain usage).
- Deferred geometry clears the non-emissive G-buffer attachments on entry and loads emissive (prevents stale G-buffer
  accumulation while preserving pre-deferred backgrounds).

### Bitmap render targets are implemented (bm_make_render_target / bm_set_render_target)

- Vulkan now implements the bmpman RTT API:
  - `gf_bm_make_render_target` creates a GPU-backed image + image views and registers it with `VulkanTextureManager`.
  - `gf_bm_set_render_target` switches the active `VulkanRenderingSession` target between swapchain and bitmap targets.
- Leaving a bitmap render target transitions it to shader-read and generates its mip chain if requested.
- Render-target attachment views participate in serial-gated deferred release, so they are not destroyed while GPU work is in flight.
- Current parity note: render targets are color-only (no depth/stencil attachment), matching the current OpenGL RTT implementation.

### Dynamic viewport/scissor updates are now honored

- Vulkan implements `gf_set_viewport` and now keeps the dynamic scissor state in sync with `gr_set_clip` / `gr_reset_clip`.
- `VulkanRenderingSession::applyDynamicState()` no longer overwrites viewport at render-pass begin, so engine-driven
  viewport changes (e.g. HTL target monitor) remain effective.

## Medium

- Vulkan shader coverage is still incomplete: `VulkanLayoutContracts` declares shader types that are not mapped in
  `VulkanShaderManager`. If invoked, Vulkan will throw (fail-fast) rather than silently render incorrectly.

## Low

No remaining low items.
