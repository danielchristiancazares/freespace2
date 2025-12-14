# FSO Vulkan Renderer

## Architecture Overview

### Layering

```
Engine (gr_screen function pointers)
  ↓
VulkanGraphics (gf_* implementations; g_currentFrame injection)
  ↓
VulkanRenderer (frame loop + coordination)
  ├─ VulkanDevice (instance/device/queues/swapchain; acquire/present)
  ├─ VulkanDescriptorLayouts (set layouts, pipeline layouts, descriptor pools/sets)
  ├─ VulkanRenderTargets (depth + G-buffer resources)
  ├─ VulkanRenderingSession (mode switching; dynamic rendering; transitions; clears; dynamic state)
  ├─ VulkanFrame[kFramesInFlight]
  │    ├─ VulkanRingBuffer (uniform/vertex/staging suballocations)
  │    └─ DedicatedStagingBuffer[] (oversized texture uploads)
  ├─ VulkanShaderManager (shader module cache)
  ├─ VulkanPipelineManager (pipeline cache/creation)
  ├─ VulkanBufferManager (engine buffers → VkBuffer)
  └─ VulkanTextureManager (texture state machine; StagingPool; descriptor queries)
       └─ StagingPool (ring-vs-dedicated decision; operates on VulkanFrame)
```

### Subsystem Responsibilities

| Subsystem | Responsibility |
|-----------|----------------|
| **VulkanGraphics** | Engine-facing API bridge. Installs `gr_vulkan_*` function pointers into `gr_screen`, maintains module-global `renderer_instance` and per-frame `g_currentFrame`. |
| **VulkanRenderer** | Central coordinator. Initializes all subsystems, drives frame loop (begin/end/submit/present), manages descriptor sync. |
| **VulkanDevice** | Vulkan instance, surface, physical/logical device, queues, swapchain, pipeline cache. Owns device-lifetime resources. |
| **VulkanRenderingSession** | Render pass state machine. Manages render mode (forward vs deferred), layout transitions, dynamic rendering begin/end, dynamic state. |
| **VulkanRenderTargets** | Depth image + views, G-buffer images + views + sampler. Handles resize on swapchain recreation. |
| **VulkanFrame** | Per-frame package: command pool/buffer, fence, semaphores (binary + timeline), ring buffers (uniform/vertex/staging), dedicated staging buffers for oversized texture uploads. |
| **VulkanRingBuffer** | Generic host-visible sub-allocator for per-frame transient memory. |
| **VulkanShaderManager** | Loads and caches shader modules by `shader_type` + variant flags. |
| **VulkanPipelineManager** | Creates and caches pipelines keyed by shader type, variant, formats, blend mode, sample count, vertex layout. |
| **VulkanBufferManager** | Engine buffer handle → VkBuffer. Supports create/update/resize/map/flush and deferred destruction. |
| **VulkanTextureManager** | Texture state machine (`std::variant<Missing,Pending,Staged,Uploading,Resident,Retired>`), upload scheduling via `StagingPool`, sampler cache, descriptor queries. No `Failed` state; oversized textures use dedicated staging. |
| **StagingPool** | Owns ring-vs-dedicated staging decision. Textures request staging without seeing thresholds; pool tries ring buffer first, falls back to per-frame dedicated allocation. |
| **VulkanDescriptorLayouts** | Descriptor set layouts, pipeline layouts, descriptor pool allocation (standard + model/bindless paths). |
| **VulkanLayoutContracts** | Explicit mapping: `shader_type` → pipeline layout kind + vertex input mode. |
| **FrameLifecycleTracker** | Tracks whether renderer is currently recording and which frame index is active. |

---

## File Manifest

| File | Purpose |
|------|---------|
| `VulkanGraphics.*` | Engine glue: `gr_vulkan_*` functions, `g_currentFrame` injection |
| `VulkanRenderer.*` | Frame orchestration, submission, per-frame sync |
| `VulkanDevice.*` | Instance, surface, physical/logical device, swapchain, pipeline cache |
| `VulkanRenderingSession.*` | Render pass state machine, layout transitions, dynamic state |
| `VulkanRenderTargets.*` | Depth buffer, G-buffer, resize handling |
| `VulkanFrame.*` | Per-frame resources: command buffer, fences, semaphores, ring buffers |
| `VulkanRingBuffer.*` | Per-frame transient allocations (uniform, vertex, staging) |
| `VulkanShaderManager.*` | Shader module loading/caching by type + variant |
| `VulkanShaderReflection.*` | SPIR-V reflection and descriptor layout validation |
| `VulkanPipelineManager.*` | Pipeline creation/caching by `PipelineKey` |
| `VulkanBufferManager.*` | Buffer creation, updates, deferred deletion |
| `VulkanTextureManager.*` | Texture uploads, residency tracking, sampler cache |
| `VulkanDescriptorLayouts.*` | Set layouts, pipeline layouts, descriptor pool allocation |
| `VulkanLayoutContracts.*` | Shader ↔ pipeline layout binding contracts |
| `VulkanModelTypes.h` | Model push constants and shared model rendering types |
| `VulkanModelValidation.*` | Vulkan model-path feature/limit validation helpers |
| `VulkanVertexTypes.h` | Shared vertex layout structs (e.g., `DefaultMaterialVertex`) |
| `VulkanConstants.h` | Shared constants (`kFramesInFlight`, `kMaxBindlessTextures`) |
| `VulkanDebug.*` | Logging helpers (`vkprintf`) and capability flags |
| `VulkanClip.h` | Clip/scissor helpers mirroring engine clip semantics |
| `FrameLifecycleTracker.*` | State machine for "are we recording?" checks |

---

## Frame Lifecycle

### Frame Flow

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

### Key Entry Points

- **`VulkanRenderer::flip()`** — Called once per frame. Submits the previous frame, advances the frame ring, waits on fences, acquires a swapchain image, begins recording the next frame.
- **`gr_vulkan_setup_frame()`** — Called immediately after `flip()`. Injects `VulkanFrame*` into `g_currentFrame` and sets viewport/scissor. Does *not* start rendering (deferred to first draw/clear).
- **`VulkanRenderer::ensureRenderingStarted()`** — Lazily begins the render pass and applies dynamic state.

**Debug trace if nothing renders:** `flip()` → `beginFrame()` → `ensureRenderingStarted()` → `VulkanRenderingSession::ensureRenderingActive()`

---

## Synchronization

| Primitive | Usage |
|-----------|-------|
| Fence | CPU waits for GPU (frame reuse safety) |
| Binary semaphore | GPU→GPU: image-available, render-finished |
| Timeline semaphore | Debugging/telemetry (not strictly required for correctness) |

---

## Texture Upload Architecture

Textures are a **state machine** driven by frame events. There is no `Failed` state; invalid states are unrepresentable.

### State Transitions

```
Missing → Pending → Staged → Uploading → Resident
                                            ↓
                                         Retired
```

| State | Meaning |
|-------|---------|
| **Missing** | No record exists. `queryDescriptor()` transitions to Pending. |
| **Pending** | Upload requested but not yet staged. Active in `m_activeUploads`. |
| **Staged** | Staging memory acquired, copy regions prepared. Ready to record. |
| **Uploading** | Copy commands recorded, waiting for frame completion. |
| **Resident** | GPU resources valid. Descriptor is bindable. |
| **Retired** | Marked for deletion after GPU completes in-flight references. |

### Event Dispatch

Upload coordination uses `std::visit` dispatch—no caller routing via `if size > threshold` or `switch(outcome)`:

- **`queryDescriptor()`**: Dispatches `onQuery` event. Missing textures transition to Pending.
- **`flushPendingUploads()`**: Dispatches `onPreRecord` (acquire staging) then `onBeginRecord` (record copies).
- **`markUploadsCompleted()`**: Dispatches `onFrameComplete`. Uploading textures transition to Resident.

### Staging Pool

`StagingPool` encapsulates the ring-vs-dedicated decision:

1. Try `frame.stagingBuffer().try_allocate(bytes)` (ring buffer)
2. If that fails, allocate a `DedicatedStagingBuffer` owned by the frame
3. Dedicated buffers are cleared in `VulkanFrame::reset()` after fence wait

Textures never compare against `kStagingRingSize`. The pool makes the decision internally.
