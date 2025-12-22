# Vulkan Backend (FSO) — `code/graphics/vulkan`

This directory contains the Vulkan renderer backend for FreeSpace Open (FSO). It provides the implementation behind the `gr_*` graphics API using Vulkan-Hpp (`vk::*`) and modern Vulkan features (dynamic rendering, `pipelineBarrier2`).

## File Manifest

| File | Purpose |
|------|---------|
| `VulkanDevice.*` | Instance, surface, physical/logical device, swapchain, pipeline cache |
| `VulkanRenderer.*` | Frame orchestration, submission, per-frame sync |
| `VulkanRenderingSession.*` | Render pass state machine, layout transitions, dynamic state |
| `VulkanFrame.*` | Per-frame resources: command buffer, fences, semaphores, ring buffers |
| `VulkanGraphics.*` | Engine glue: `gr_vulkan_*` functions, `g_currentFrame` injection |
| `VulkanBufferManager.*` | Buffer creation, updates, deferred deletion |
| `VulkanTextureManager.*` | Texture uploads, residency tracking, sampler cache |
| `VulkanShaderManager.*` | Shader module loading/caching by type + variant |
| `VulkanShaderReflection.*` | SPIR-V reflection helpers for pipeline layout/validation |
| `VulkanPipelineManager.*` | Pipeline creation/caching by `PipelineKey` |
| `VulkanDescriptorLayouts.*` | Set layouts, pipeline layouts, descriptor pool allocation |
| `VulkanDeferredLights.*` | Deferred lighting pass orchestration and light volume meshes |
| `VulkanRenderTargets.*` | Depth buffer, G-buffer, resize handling |
| `VulkanRingBuffer.*` | Per-frame transient allocations (uniform, vertex, staging) |
| `VulkanLayoutContracts.*` | Shader ↔ pipeline layout binding contracts |
| `VulkanModelValidation.*` | Model draw validation and safety checks |
| `VulkanModelTypes.h` | Shared model/mesh-related types |
| `VulkanVertexTypes.h` | Vertex layout/type definitions |
| `VulkanClip.h` | Clipping and scissor helpers |
| `VulkanDebug.*` | Vulkan debug helpers and instrumentation |
| `VulkanConstants.h` | Shared constants (frames-in-flight, bindless limits) |

## Constants

- `kFramesInFlight` in `VulkanConstants.h` defines the frame-ring size and any per-frame tracking. Avoid duplicating a second frames-in-flight constant elsewhere.

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
- Logical device + queues (graphics/present)
- Swapchain + image views
- Pipeline cache (device lifetime)

Presentation API:
- `acquireNextImage(semaphore)` → `AcquireResult`
- `present(semaphore, imageIndex)` → `PresentResult`
- `recreateSwapchain(width, height)`

Swapchain recreation triggers `VulkanRenderTargets::resize()`.

### `VulkanRenderer` (Orchestration)

Coordinates:
- Initialization order: device → layouts → render targets → session → frames → managers
- Frame ring (`kFramesInFlight` frames via `VulkanFrame`)
- CPU/GPU sync via `VulkanFrame::wait_for_gpu()` fence
- Frame lifecycle tracking: available/in-flight deques (renderer) + `std::optional<RecordingFrame>` (graphics backend)
- Descriptor state: global set (frame-wide), model set (per-frame; bound per draw via dynamic offsets), and per-draw push descriptors

Key methods:
- `initialize()` / `shutdown()`
- `flip()` — frame boundary
- `ensureRenderingStarted(frameCtx)` — lazy render pass start + dynamic state (returns `RenderCtx`)

Model descriptor sync happens in `beginFrame()` via `beginModelDescriptorSync()` before recording starts.

### `VulkanRenderingSession` (Render Pass State Machine)

Responsible for:
- Dynamic-rendering state machine (single “current target” truth)
- Image layout transitions (swapchain, depth, G-buffer) via `pipelineBarrier2`
- Target switching:
  - `requestSwapchainTarget()` — swapchain + depth
  - `beginDeferredPass()` — select G-buffer target
  - `endDeferredGeometry()` — transition G-buffer → shader-read and select swapchain (no depth)
- Lazy render pass begin via `ensureRendering(cmd, imageIndex)`. The session owns the active pass and ends it automatically at frame/target boundaries.
- Dynamic state application (`applyDynamicState()`) is performed when a pass begins after selecting the target.

The render target’s “contract” (attachment formats + count) is exposed as `RenderTargetInfo` and is used to key pipelines.

**Y-flip:** Viewport has negative height; front-face is set to clockwise to preserve winding.

### `VulkanFrame` (Per-Frame Resources)

Packages everything for one frame in the ring:
- Command pool + command buffer
- Fence (CPU wait) + semaphores (image-available, render-finished, timeline)
- Ring buffers: uniform, vertex, staging
- Per-frame descriptor set + dynamic offset tracking

### `VulkanRenderTargets`

Owns:
- Depth image, depth attachment view, depth sample view (for shader reads)
- G-buffer images (`kGBufferCount = 5`)
- Resize logic tied to swapchain recreation

### Resource Managers

| Manager | Responsibility |
|---------|----------------|
| `VulkanDescriptorLayouts` | Set layouts, pipeline layouts, pool allocation |
| `VulkanShaderManager` | Shader module cache (type + variant) |
| `VulkanPipelineManager` | Pipeline cache (`PipelineKey` → pipeline) |
| `VulkanBufferManager` | Buffer CRUD, serial-gated deferred deletion (retireSerial → destroy when completedSerial >= retireSerial) |
| `VulkanTextureManager` | Upload scheduling, residency state machine, sampler cache |

### Pipelines (`VulkanPipelineManager` + `VulkanLayoutContracts`)

Pipelines are “contract-driven” and cached by `PipelineKey`:
- `PipelineKey` includes shader type + variant flags, render target contract (color/depth formats, attachment count, sample count), blend mode, and (for vertex-attribute shaders) `vertex_layout::hash()`.
- Shader layout contracts (`VulkanLayoutContracts`) define, per `shader_type`:
  - which pipeline layout to use (`Standard`, `Model`, `Deferred`)
  - which vertex input mode to use (`VertexAttributes` vs. `VertexPulling`)

Pipeline layout families (descriptor model):
- **Standard:** set 0 = per-draw push descriptors, set 1 = global set.
- **Model:** set 0 = bindless model set + dynamic UBO, push constants (vertex pulling; no vertex attributes).
- **Deferred:** set 0 = deferred push descriptors, set 1 = global G-buffer set.

Caching layers:
- `VulkanShaderManager` caches shader modules by (type, flags).
- `VulkanPipelineManager` caches vertex input state by `vertex_layout::hash()` and pipelines by `PipelineKey`.
- `VulkanDevice` owns a `VkPipelineCache` persisted as `vulkan_pipeline.cache` and passed into pipeline creation.

## Synchronization Primitives

| Primitive | Usage |
|-----------|-------|
| Fence | CPU waits for GPU (frame reuse safety) |
| Binary semaphore | GPU→GPU: image-available, render-finished |
| Timeline semaphore | Authoritative GPU completion counter for serial-gated deferred releases |

## Frame Flow

```
flip()
├── if recording: endFrame() + submitFrame()
├── advance frame index
├── VulkanFrame::wait_for_gpu()           # fence wait
├── managers: collect(completedSerial)
├── acquireNextImage()                    # recreate swapchain if needed
		└── beginFrame()
		    ├── reset command pool
		    ├── begin command buffer
	    ├── flushPendingUploads()             # texture staging
	    ├── beginModelDescriptorSync()        # descriptor writes (after upload flush)
	    └── VulkanRenderingSession::beginFrame()

gr_vulkan_setup_frame()
└── set viewport/scissor

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
- **Y-flip changes winding.** Negative viewport height → clockwise front-face.
- **Alignment matters.** Uniform offsets must respect `minUniformBufferOffsetAlignment`.
- **Swapchain resize cascades.** Anything tied to swapchain extent must handle `recreateSwapchain()`.
- **`PipelineKey.layout_hash` is caller-owned for vertex-attribute shaders.** If a shader uses `VertexAttributes`, ensure `PipelineKey.layout_hash = layout.hash()` (vertex-pulling shaders intentionally ignore the layout hash).
- **Pipeline layout compatibility matters.** “Deferred” pipelines use a different set-0 layout than “Standard”; bind the global set using the matching pipeline layout when mixing pipeline families.
