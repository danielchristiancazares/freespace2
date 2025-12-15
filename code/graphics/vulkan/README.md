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
| `VulkanPipelineManager.*` | Pipeline creation/caching by `PipelineKey` |
| `VulkanDescriptorLayouts.*` | Set layouts, pipeline layouts, descriptor pool allocation |
| `VulkanRenderTargets.*` | Depth buffer, G-buffer, resize handling |
| `VulkanRingBuffer.*` | Per-frame transient allocations (uniform, vertex, staging) |
| `FrameLifecycleTracker.*` | State machine for "are we recording?" checks |
| `VulkanLayoutContracts.*` | Shader ↔ pipeline layout binding contracts |
| `VulkanConstants.h` | Shared constants (⚠️ see note below) |

## Constants — Known Issue

**Bug:** Two different frame-count constants exist:

```cpp
// VulkanConstants.h
constexpr uint32_t kFramesInFlight = 3;

// VulkanRenderer.h
static constexpr size_t MAX_FRAMES_IN_FLIGHT = 2;  // actual frame ring size
```

`VulkanTextureManager` uses `kFramesInFlight` for descriptor tracking, but only 2 frames exist. The `clearRetiredSlotsIfAllFramesUpdated` logic waits for 3 frames to cycle before clearing retired slots — this never happens, so retired slot tracking leaks.

**Fix:** Unify on one constant. Delete `MAX_FRAMES_IN_FLIGHT`, use `kFramesInFlight` everywhere.

## Entry Points

The engine's renderer glue is in `VulkanGraphics.cpp`. It holds a process-global `renderer_instance` and exposes `gr_vulkan_*` functions wired into the engine's `gr_*` interface.

Key contract points:

- `VulkanRenderer::flip()` — called once per frame to submit the previous frame, advance the frame ring, wait on fences, acquire a swapchain image, and begin recording the next frame.
- `gr_vulkan_setup_frame()` — called immediately after `flip()`. Injects `VulkanFrame*` into the module-local `g_currentFrame` and sets viewport/scissor. Does *not* start rendering (deferred to first draw/clear).

**If nothing renders**, trace: `flip()` → `beginFrame()` → `ensureRenderingStarted()` → `VulkanRenderingSession::ensureRenderingActive()`.

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
- Frame ring (`MAX_FRAMES_IN_FLIGHT` frames via `VulkanFrame`)
- CPU/GPU sync via `VulkanFrame::wait_for_gpu()` fence
- Frame lifecycle tracking via `FrameLifecycleTracker`
- Descriptor state: global set (frame-wide) and model sets (per-draw)

Key methods:
- `initialize()` / `shutdown()`
- `flip()` — frame boundary
- `ensureRenderingStarted(cmd)` — lazy render pass start + dynamic state

Model descriptor sync happens in `beginFrame()` via `beginModelDescriptorSync()` before recording starts.

### `VulkanRenderingSession` (Render Pass State Machine)

Responsible for:
- Per-frame state reset (`resetFrameState()`)
- Image layout transitions (swapchain, depth, G-buffer)
- Render mode switching:
  - `RenderMode::Swapchain` — forward rendering
  - `RenderMode::DeferredGBuffer` — deferred geometry pass
- Lazy render pass begin (`ensureRenderingActive()`)
- Dynamic state application (`applyDynamicState()`)

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
| `VulkanBufferManager` | Buffer CRUD, deferred deletion (retire → destroy after N frames) |
| `VulkanTextureManager` | Upload scheduling, residency state machine, sampler cache |

## Synchronization Primitives

| Primitive | Usage |
|-----------|-------|
| Fence | CPU waits for GPU (frame reuse safety) |
| Binary semaphore | GPU→GPU: image-available, render-finished |
| Timeline semaphore | Debugging/telemetry (not strictly required for correctness) |

## Frame Flow

```
flip()
├── if recording: endFrame() + submitFrame()
├── advance frame index
├── VulkanFrame::wait_for_gpu()           # fence wait
├── managers: onFrameEnd(), markUploadsCompleted()
├── acquireNextImage()                    # recreate swapchain if needed
└── beginFrame()
    ├── reset command pool
    ├── begin command buffer
    ├── beginModelDescriptorSync()        # descriptor writes
    ├── flushPendingUploads()             # texture staging
    └── VulkanRenderingSession::resetFrameState()

gr_vulkan_setup_frame()
├── inject g_currentFrame
└── set viewport/scissor

[first draw/clear]
└── ensureRenderingStarted()
    ├── VulkanRenderingSession::ensureRenderingActive()
    └── VulkanRenderingSession::applyDynamicState()

[end of frame: next flip() submits and presents]
```

## Common Gotchas

- **`g_currentFrame` is module-local** to `VulkanGraphics.cpp`. Don't try to access it from other translation units.
- **Buffer handles can be valid before `VkBuffer` exists.** Lazy creation happens on first `updateBufferData()`.
- **Descriptor writes happen at frame start.** Writing descriptors mid-recording fights the design.
- **Render pass is lazy.** `setup_frame()` doesn't begin rendering; first draw/clear does.
- **Y-flip changes winding.** Negative viewport height → clockwise front-face.
- **Alignment matters.** Uniform offsets must respect `minUniformBufferOffsetAlignment`.
- **Swapchain resize cascades.** Anything tied to swapchain extent must handle `recreateSwapchain()`.
