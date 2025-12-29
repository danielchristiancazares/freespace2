# Vulkan Backend (FSO) - `code/graphics/vulkan`

This directory contains the Vulkan renderer backend for FreeSpace Open (FSO). It provides the implementation behind the `gr_*` graphics API using Vulkan-Hpp (`vk::*`) and modern Vulkan features (dynamic rendering, `pipelineBarrier2`).

## File Manifest

| File | Purpose |
|------|---------|
| `VulkanDevice.*` | Instance, surface, physical/logical device, swapchain, pipeline cache |
| `VulkanRenderer.*` | Frame orchestration, submission, per-frame sync, global resource creation |
| `VulkanRenderingSession.*` | Render pass state machine, layout transitions, dynamic state |
| `VulkanFrame.*` | Per-frame resources: command buffer, fence, semaphores, ring buffers |
| `VulkanFrameCaps.h` | Frame capability tokens (`FrameCtx`, `ModelBoundFrame`, `NanoVGBoundFrame`, `DecalBoundFrame`) |
| `VulkanPhaseContexts.h` | Phase tokens (`UploadCtx`, `RenderCtx`, `DeferredGeometryCtx`, `DeferredLightingCtx`) |
| `VulkanFrameFlow.h` | Frame recording tokens (`RecordingFrame`, `InFlightFrame`) |
| `VulkanSync2Helpers.h` | Sync2 image barrier helpers (`makeImageLayoutBarrier`, `transitionImageLayout`) |
| `VulkanDeferredRelease.h` | Serial-gated deferred destruction queue |
| `VulkanTextureId.h` | Type-safe texture identifier |
| `FrameLifecycleTracker.*` | Debug validation for recording state |
| `VulkanGraphics.*` | Engine glue: `gr_vulkan_*` functions, `g_currentFrame` injection |
| `VulkanBufferManager.*` | Buffer creation, updates, deferred deletion |
| `VulkanTextureManager.*` | Texture uploads, residency tracking, sampler cache, bitmap render targets |
| `VulkanMovieManager.*` | YCbCr movie textures: feature checks, pipeline setup, upload/draw, deferred release |
| `VulkanTextureBindings.h` | Draw-path texture API (`VulkanTextureBindings`) + upload-phase uploader (`VulkanTextureUploader`) |
| `VulkanShaderManager.*` | Shader module loading/caching by type + variant |
| `VulkanShaderReflection.h` | SPIR-V reflection helpers for pipeline layout/validation |
| `VulkanPipelineManager.*` | Pipeline creation/caching by `PipelineKey` |
| `VulkanDescriptorLayouts.*` | Set layouts, pipeline layouts, descriptor pool allocation |
| `VulkanDeferredLights.*` | Deferred lighting pass orchestration and light volume meshes |
| `VulkanRenderTargets.*` | Depth buffer, G-buffer, post-processing targets, resize handling |
| `VulkanRenderTargetInfo.h` | Render target contract definition (formats, counts) |
| `VulkanRingBuffer.*` | Per-frame transient allocations (uniform, vertex, staging) |
| `VulkanLayoutContracts.*` | Shader to pipeline layout binding contracts |
| `VulkanModelValidation.*` | Model draw validation and safety checks |
| `VulkanModelTypes.h` | Shared model/mesh-related types |
| `VulkanVertexTypes.h` | Vertex layout/type definitions |
| `VulkanClip.h` | Clipping and scissor helpers |
| `VulkanDebug.*` | Vulkan debug helpers and instrumentation (lightweight logging) |
| `VulkanConstants.h` | Shared constants (frames-in-flight, bindless limits) |

## Constants

Key constants in `VulkanConstants.h`:
- `kFramesInFlight` - frame-ring size for per-frame tracking. Do not duplicate elsewhere.
- `kMaxBindlessTextures` - bindless descriptor array size.
- `kBindlessTextureSlotFallback` (0), `kBindlessTextureSlotDefaultBase` (1), `kBindlessTextureSlotDefaultNormal` (2), `kBindlessTextureSlotDefaultSpec` (3) - reserved bindless slots.
- `kBindlessFirstDynamicTextureSlot` (4) - first slot available for dynamic texture allocation.

## Design Patterns in Practice

This backend adheres to the [Type-Driven Design Philosophy](../../../docs/DESIGN_PHILOSOPHY.md).

### Capability Tokens & Typestates

**Recording Phase** (`VulkanFrameFlow.h`):
- **`RecordingFrame`**: Proof of active frame recording (unforgeable by construction).
- **`InFlightFrame`**: Proof of submitted frame awaiting GPU completion.

**Frame Context** (`VulkanFrameCaps.h`, `VulkanPhaseContexts.h`):
- **`FrameCtx`**: Proof of recording state (derived from `RecordingFrame`).
- **`UploadCtx`**: Proof of upload phase (no rendering active).
- **`RenderCtx`**: Proof of active render pass.

**Bound Resources** (`VulkanFrameCaps.h`):
- **`ModelBoundFrame`**: Proof of allocated model descriptors.
- **`NanoVGBoundFrame`**: Proof of bound NanoVG UBO.
- **`DecalBoundFrame`**: Proof of bound decal UBOs.

**Deferred Lighting** (`VulkanPhaseContexts.h`):
- **`DeferredGeometryCtx`** / **`DeferredLightingCtx`**: Enforces `begin` → `end` → `finish` sequence.

### State as Location
- **Frames**: Managed by moving between `m_availableFrames` and `m_inFlightFrames` deques in `VulkanRenderer`.
- **Render Targets**: Represented by the active `RenderTargetState` subclass instance (`m_target`) in `VulkanRenderingSession`.
- **Texture Residency**: Resident bitmap textures live in `m_bitmaps`; render targets in `m_targets`; pending uploads in `m_pendingUploads`; bindless slot assignments in `m_bindlessSlots`; domain-invalid inputs in `m_permanentlyRejected`.

### Boundary / Adapter
`VulkanGraphics.cpp` bridges the legacy engine's implicit global state to the backend's explicit token requirements.
*Caution:* This is not a place for "inhabitant branching" (e.g., `if (exists)` logic). While it currently validates state to produce tokens, the architectural goal is to push these ownership requirements upstream into the engine.

## Entry Points

The engine's renderer glue is in `VulkanGraphics.cpp`. It holds a process-global `renderer_instance` and exposes `gr_vulkan_*` functions wired into the engine's `gr_*` interface.

Key contract points:

- `VulkanRenderer::flip()` — called once per frame to submit the previous frame, advance the frame ring, wait on fences, acquire a swapchain image, and begin recording the next frame.
- `gr_vulkan_setup_frame()` — called immediately after `flip()`. Configures viewport/scissor for the newly recording frame. Does *not* start rendering (deferred to first draw/clear).

**If nothing renders**, trace: `flip()` → `beginFrame()` → `ensureRenderingStarted()` → `VulkanRenderingSession::ensureRendering()`.

## Architecture

### `VulkanDevice` (Device + Swapchain)

Owns and manages:
- Vulkan instance + debug messenger
- SDL surface
- Physical device selection + feature probing
- Logical device + queues (graphics, present)
- Swapchain + image views + per-image render-finished semaphores
- Pipeline cache (device lifetime)

Presentation API:
- `acquireNextImage(semaphore)` returns `AcquireResult`
- `present(semaphore, imageIndex)` returns `PresentResult`
- `recreateSwapchain(width, height)`
- `swapchainRenderFinishedSemaphore(imageIndex)` returns the semaphore for presenting a specific image

Swapchain recreation triggers `VulkanRenderTargets::resize()`.

### `VulkanRenderer` (Orchestration)

Coordinates:
- Initialization order: device → layouts → render targets → frames → managers → session
- Frame ring (`kFramesInFlight` frames via `VulkanFrame`)
- CPU/GPU sync via `VulkanFrame::wait_for_gpu()` fence
- Frame lifecycle tracking: available/in-flight deques (renderer) + `std::optional<RecordingFrame>` (graphics backend)
- Descriptor state: global set (frame-wide), model set (per-frame; bound per draw via dynamic offsets), and per-draw push descriptors

Key methods:
- `initialize()` / `shutdown()`
- `flip()` — frame boundary
- `ensureRenderingStarted(frameCtx)` — lazy render pass start + dynamic state (returns `RenderCtx`)

**Token-based API:** Rendering methods require proof of state via capability tokens (e.g., `ensureRenderingStarted(FrameCtx)`).

### `VulkanRenderingSession` (Render Pass State Machine)

Responsible for:
- Dynamic-rendering state machine (single "current target" truth)
- Image layout transitions (swapchain, depth, G-buffer, post-processing) via `pipelineBarrier2`
- Target switching (automatically ends previous pass):
  - `requestSwapchainTarget()` / `requestSwapchainNoDepthTarget()` - swapchain with/without depth
  - `requestSceneHdrTarget()` / `requestSceneHdrNoDepthTarget()` - HDR scene texture
  - `requestBitmapTarget(bitmapHandle, face)` - bitmap render targets (bmpman RTT; cubemap faces supported)
  - `requestGBufferEmissiveTarget()` - G-buffer emissive-only (pre-deferred scene copy)
  - `requestPostLdrTarget()` / `requestPostLuminanceTarget()` - post-processing LDR/luminance
  - `requestSmaaEdgesTarget()` / `requestSmaaBlendTarget()` / `requestSmaaOutputTarget()` - SMAA passes
  - `requestBloomMipTarget(pingPongIndex, mipLevel)` - bloom blur mip targets
  - `beginDeferredPass(clearNonColorBufs, preserveEmissive)` - select G-buffer target + clear policy
  - `endDeferredGeometry()` - transition G-buffer to shader-read, select swapchain (no depth)
- Depth attachment selection:
  - `useMainDepthAttachment()` - scene depth
  - `useCockpitDepthAttachment()` - cockpit-only depth (for post-processing parity with OpenGL)
- Lazy render pass begin via `ensureRendering(cmd, imageIndex)`. The session owns the active pass and ends it automatically at frame/target boundaries.
- Dynamic state application (`applyDynamicState()`) is performed when a pass begins.

**Polymorphic State:** The active render target is a `std::unique_ptr<RenderTargetState>` (state as location). There is no target mode enum; the object is the state. Concrete target classes include `SwapchainWithDepthTarget`, `SceneHdrWithDepthTarget`, `DeferredGBufferTarget`, `BitmapTarget`, etc.

**RAII Guard:** `ActivePass` manages `vkCmdBeginRendering`/`vkCmdEndRendering`.

### `VulkanBufferManager` (Buffers)

Handles buffer creation and updates.
- **Lazy Existence:** `createBuffer` returns a handle but does *not* create a `VkBuffer` immediately.
- **ensureBuffer(handle, size):** Called by `updateBufferData` or `setModelUniformBinding`. Creates the `VkBuffer` if it doesn't exist or resizes it if too small. This ensures buffers always exist when descriptors reference them.
- **Deferred Deletion:** `deleteBuffer` queues the buffer for destruction only after the current frame's completion serial is passed.

### `VulkanTextureManager` (Textures)

Split responsibilities:
- **Residency (state as location):** Resident bitmap textures live in `m_bitmaps`; resident render targets live in `m_targets`. Non-resident textures are absent.
- **Uploads:** `queueTextureUpload` enqueues CPU-side requests. `flushPendingUploads(const UploadCtx&)` performs transfers and assigns bindless slots at upload-phase safe points.
- **Bindings (boundary):** `VulkanTextureBindings` owns fallback policy and uses lookup-only manager APIs (`tryGetResidentDescriptor`, `tryGetBindlessSlot`) plus explicit requests (`queueTextureUpload`, `requestBindlessSlot`).
- **Render Targets:** `createRenderTarget` / render-target transitions operate on `m_targets` (unified ownership of image + metadata).

### `VulkanMovieManager` (YCbCr Movie Path)

Owns the Vulkan-native movie path based on `VkSamplerYcbcrConversion`:
- **Feature/format checks:** gated on `samplerYcbcrConversion` and multi-planar format support, including
  `combinedImageSamplerDescriptorCount` for pool sizing.
- **Pipelines per config:** immutable sampler + descriptor set layout/pipeline per colorspace/range combination.
- **Upload:** uses the per-frame staging ring and records `vkCmdCopyBufferToImage` on the active command buffer.
  This upload can occur **mid-frame** (after `beginFrame()`), so `VulkanRenderer::uploadMovieTexture()` suspends
  dynamic rendering before issuing transfer commands.
- **Lifetime:** serial-gated deferred release, matching other Vulkan managers.

### `VulkanFrame` (Per-Frame Resources)

Packages everything for one frame in the ring:
- Command pool + command buffer
- Fence (`inflightFence`) for CPU wait on GPU completion
- Semaphores: `imageAvailable` (acquire), `timelineSemaphore` (serial tracking)
- Ring buffers: `uniformBuffer()`, `vertexBuffer()`, `stagingBuffer()`
- Per-frame descriptor sets (global, model) + dynamic uniform bindings

### Pipelines (`VulkanPipelineManager` + `VulkanLayoutContracts`)

Pipelines are contract-driven and cached by `PipelineKey`:
- `PipelineKey` includes:
  - Shader type + variant flags
  - Render target contract (color/depth formats, attachment count, sample count)
  - Blend mode, color write mask
  - Stencil test state (compare op, masks, reference, pass/fail ops)
  - Vertex layout hash (for vertex-attribute shaders; ignored for vertex-pulling shaders)
- Shader layout contracts (`VulkanLayoutContracts`) define per `shader_type`:
  - `PipelineLayoutKind`: `Standard`, `Model`, or `Deferred`
  - `VertexInputMode`: `VertexAttributes` or `VertexPulling`

**Documentation**: See `docs/VULKAN_PIPELINE_MANAGEMENT.md` for architecture details and `docs/VULKAN_PIPELINE_USAGE.md` for practical usage patterns.

## Synchronization Primitives

| Primitive | Owner | Usage |
|-----------|-------|-------|
| `inflightFence` | VulkanFrame | CPU waits for GPU (frame reuse safety) |
| `imageAvailable` | VulkanFrame | GPU signal on swapchain image acquisition |
| `renderFinishedSemaphore` | VulkanDevice (per swapchain image) | GPU signal for present queue |
| `timelineSemaphore` | VulkanFrame | Per-frame serial for deferred releases |
| `submitTimeline` | VulkanRenderer | Global GPU completion counter for serial-gated destruction |

## Frame Flow

```
flip()
├── if recording: endFrame() + submitFrame()
├── advance frame index
├── VulkanFrame::wait_for_gpu()           # fence wait
├── managers: collect(completedSerial)    # recycle buffers/textures
├── acquireNextImage()                    # recreate swapchain if needed
└── beginFrame()
    ├── reset command pool
    ├── begin command buffer
    ├── flushPendingUploads()             # texture staging
    ├── beginModelDescriptorSync()        # descriptor writes
    └── VulkanRenderingSession::beginFrame()

gr_vulkan_setup_frame()
└── set viewport/scissor (stored for next pass)

[first draw/clear]
└── ensureRenderingStarted()
    └── VulkanRenderingSession::ensureRendering() (session-owned pass)

[end of frame: next flip() submits and presents]
```

## Common Gotchas

- **Buffer handles can be valid before `VkBuffer` exists.** Lazy creation happens on first `updateBufferData()`.
- **Descriptor writes happen at frame start.** Writing descriptors mid-recording fights the design.
- **Render pass is lazy and session-owned.** `setup_frame()` doesn't begin rendering; first draw/clear does. The render pass remains active until a frame/target boundary ends it.
- **Target switches end the active pass automatically.** Call session boundary methods like `requestSwapchainTarget()` directly; they will end any active pass first.
- **Movie uploads can be mid-recording.** The movie path records transfer commands after `beginFrame()`, so always suspend dynamic rendering before uploading.
- **Y-flip changes winding.** Negative viewport height → clockwise front-face.
- **Alignment matters.** Uniform offsets must respect `minUniformBufferOffsetAlignment`.
- **Pipeline layout compatibility matters.** "Deferred" pipelines use a different set-0 layout than "Standard"; bind the global set using the matching pipeline layout when mixing pipeline families.

## Extended Documentation

Detailed docs in `docs/`:
- `VULKAN_ARCHITECTURE_OVERVIEW.md` — high-level system design
- `VULKAN_FRAME_LIFECYCLE.md` — frame recording/submission flow
- `VULKAN_CAPABILITY_TOKENS.md` — typestate token design
- `VULKAN_PIPELINE_MANAGEMENT.md` — pipeline architecture
- `VULKAN_PIPELINE_USAGE.md` — practical usage patterns
- `VULKAN_TEXTURE_BINDING.md` — texture/bindless system
- `VULKAN_DESCRIPTOR_SETS.md` — descriptor layout strategy
- `VULKAN_SYNCHRONIZATION.md` — barriers, semaphores, fences
- `VULKAN_MEMORY_ALLOCATION.md` — VMA integration
- `VULKAN_DEFERRED_LIGHTING_FLOW.md` — G-buffer / lighting passes
