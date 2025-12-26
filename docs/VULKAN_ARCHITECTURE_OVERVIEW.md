# Vulkan Architecture Overview

This document is the entry point for the Vulkan renderer documentation in `docs/`. It provides a high-level map of the Vulkan backend, its core invariants, and where to look next depending on what you are changing or debugging.

---

## Table of Contents

1. [Overview](#overview)
2. [Design Constraints](#design-constraints-read-before-editing)
3. [Vulkan Requirements](#vulkan-requirements)
4. [Terminology and Core Types](#terminology-and-core-types)
5. [Renderer Architecture](#renderer-architecture)
6. [Frame Flow](#frame-flow)
7. [Render Targets](#render-targets)
8. [Getting Started](#getting-started)
9. [Quick Reference](#quick-reference)
10. [Testing and Behavioral Invariants](#testing-and-behavioral-invariants)
11. [Document Index](#document-index)

---

## Overview

The Vulkan renderer is a modern graphics backend for FreeSpace Open (FSO), implementing the FreeSpace 2 rendering pipeline using Vulkan 1.4. The renderer leverages core Vulkan 1.3+ features (dynamic rendering, synchronization2, timeline semaphores, descriptor indexing) without requiring them as extensions. It is designed around type-driven correctness principles, using capability tokens and typestate patterns to make invalid API sequencing a compile-time error rather than a runtime bug.

**Key architectural decisions:**

- **Dynamic rendering** (Vulkan 1.3 core) instead of classic `VkRenderPass` objects for flexibility
- **Push descriptors** (`VK_KHR_push_descriptor`) for per-draw bindings; global descriptor sets for bindless textures
- **Frames-in-flight model** with ring-buffered resources (2 frames by default, see `kFramesInFlight` in `VulkanConstants.h`)
- **Synchronization2** (Vulkan 1.3 core) barriers for all image layout transitions
- **Type-driven phase enforcement** via capability tokens

---

## Design Constraints (Read Before Editing)

The Vulkan renderer is intentionally structured around the principles in `docs/DESIGN_PHILOSOPHY.md`:

- **Correctness by construction**: Encode phase/order invariants in types and APIs where possible.
- **Capability tokens and typestate**: Make invalid sequencing hard/impossible to compile.
- **Boundaries vs internals**: Do conditional checks at boundaries; keep internals assumption-safe.
- **State as location**: Prefer container membership/variants over scattered state enums/flags.

When you add new rendering features (post-processing, upscalers like DLSS, new targets), prefer:

- Explicit target state transitions
- RAII scope guards for pass lifetime
- Centralized ownership for resource lifetimes and "previous frame" state

---

## Vulkan Requirements

### Minimum Version

**Vulkan 1.4** is required. The renderer uses `VK_API_VERSION_1_4` for instance and device creation. The minimum version check is defined in `VulkanDevice.cpp` as `MinVulkanVersion(1, 4, 0, 0)`.

### Vulkan 1.3+ Core Features Used

These features are part of core Vulkan 1.3 (and thus 1.4) and do not require extensions:

| Feature | Purpose |
|---------|---------|
| Dynamic Rendering | Renderpass-less rendering via `vkCmdBeginRendering`/`vkCmdEndRendering` |
| Synchronization2 | Modern barrier API with fine-grained stage/access control (`VkPipelineStageFlags2`, `VkAccessFlags2`) |
| Timeline Semaphores | Serial-based GPU completion tracking for deferred resource cleanup |
| Descriptor Indexing | Bindless texture arrays with `descriptorBindingPartiallyBound` and `runtimeDescriptorArray` |
| Maintenance4 | Buffer/image requirements queries, local workgroup size limits |

### Required Device Extensions

| Extension | Purpose |
|-----------|---------|
| `VK_KHR_swapchain` | Presentation to window surface |
| `VK_KHR_push_descriptor` | Per-draw descriptor updates without pre-allocated sets |
| `VK_KHR_maintenance5` | Enhanced pipeline creation, improved buffer copies |

### Optional Device Extensions

These extensions are enabled when available but not required:

| Extension | Purpose |
|-----------|---------|
| `VK_KHR_maintenance6` | Additional maintenance features |
| `VK_EXT_extended_dynamic_state3` | Dynamic color blend state, polygon mode |
| `VK_EXT_dynamic_rendering_local_read` | Read from attachments during dynamic rendering |
| `VK_EXT_vertex_attribute_divisor` | Instance rate vertex attributes with custom divisor |
| `VK_EXT_debug_utils` | Debug markers and validation layer integration (instance extension) |

---

## Terminology and Core Types

These names are used across the Vulkan docs and correspond to types in the Vulkan renderer:

### Capability Tokens

Tokens that prove a particular phase or capability is active. They are move-only and have private constructors (where noted), making them unforgeable by construction.

| Token / Type | Header | Description |
|--------------|--------|-------------|
| `RecordingFrame` | `VulkanFrameFlow.h` | Move-only token proving "we are recording commands for a frame"; only `VulkanRenderer` can mint this token (private constructor, friend class) |
| `InFlightFrame` | `VulkanFrameFlow.h` | Token representing a frame that has been submitted to the GPU but not yet recycled; holds `SubmitInfo` with serial for deferred cleanup |
| `FrameCtx` | `VulkanFrameCaps.h` | Boundary token proving we have a current recording frame and a renderer instance; provides access to `VulkanFrame` internals |
| `UploadCtx` | `VulkanPhaseContexts.h` | Upload-phase context (private constructor); makes "upload-only" APIs uncallable from draw paths |
| `RenderCtx` | `VulkanPhaseContexts.h` | Rendering-phase context (private constructor) proving dynamic rendering is active for a specific target; contains the command buffer and `RenderTargetInfo` |

### Bound Frame Tokens

Tokens that extend `FrameCtx` with proof that specific uniform buffers are bound. Created via `require*Bound()` helper functions that assert the required bindings exist:

| Token / Type | Header | Description |
|--------------|--------|-------------|
| `ModelBoundFrame` | `VulkanFrameCaps.h` | Proof that ModelData UBO and model descriptor set are bound; contains `modelSet`, `modelUbo`, `transformDynamicOffset`, `transformSize` |
| `NanoVGBoundFrame` | `VulkanFrameCaps.h` | Proof that NanoVGData UBO is bound; contains `nanovgUbo` |
| `DecalBoundFrame` | `VulkanFrameCaps.h` | Proof that DecalGlobals and DecalInfo UBOs are bound; contains `globalsUbo`, `infoUbo` |

### Deferred Lighting Tokens

Typestate tokens encoding the deferred lighting call order (`begin` -> `end` -> `finish`):

| Token / Type | Header | Description |
|--------------|--------|-------------|
| `DeferredGeometryCtx` | `VulkanPhaseContexts.h` | Proof that deferred geometry pass is active (private constructor); consumed when ending geometry pass |
| `DeferredLightingCtx` | `VulkanPhaseContexts.h` | Proof that deferred lighting pass is active (private constructor); consumed when finishing deferred lighting |

### Core Infrastructure Types

| Type | Header | Description |
|------|--------|-------------|
| `VulkanRenderTargets` | `VulkanRenderTargets.h` | Owns images/views/samplers for scene, post, bloom, G-buffer, depth, SMAA targets; tracks layout state per image |
| `VulkanRenderingSession` | `VulkanRenderingSession.h` | Owns render target selection, dynamic rendering begin/end, layout transitions, and clear state |
| `VulkanFrame` | `VulkanFrame.h` | Per-frame resources: command buffer, synchronization primitives, ring buffer allocators, per-frame UBO bindings |
| `VulkanRenderer` | `VulkanRenderer.h` | Top-level renderer; orchestrates frame lifecycle, token minting, and high-level draw calls |
| `RenderTargetInfo` | `VulkanRenderTargetInfo.h` | Pipeline compatibility "contract": color format, color attachment count, depth format |

### Internal RAII Types

| Type | Location | Description |
|------|----------|-------------|
| `ActivePass` | `VulkanRenderingSession.h` (private) | RAII guard for dynamic rendering lifetime; destructor calls `cmd.endRendering()` |
| `InitCtx` | `VulkanRenderer.h` (private) | Initialization-phase capability token; prevents "immediate submit" helpers from being callable during frame recording |

If you are unsure which token you should have, start with `VULKAN_RENDER_PASS_STRUCTURE.md` or `VULKAN_CAPABILITY_TOKENS.md`.

---

## Renderer Architecture

```
+------------------------------------------------------------------+
|                        VulkanRenderer                             |
|  - Owns VulkanDevice, VulkanRenderTargets, VulkanRenderingSession |
|  - Mints capability tokens (RecordingFrame, FrameCtx, RenderCtx)  |
|  - Orchestrates frame lifecycle and submit timeline               |
+------------------------------------------------------------------+
         |                    |                      |
         v                    v                      v
+----------------+  +--------------------+  +-------------------+
|  VulkanDevice  |  | VulkanRenderTargets|  |VulkanRenderingSes.|
|  - Physical/   |  | - Depth buffer     |  | - Target selection|
|    logical dev |  | - G-buffer (5 att.)|  | - Dynamic render  |
|  - Swapchain   |  | - Scene HDR        |  | - Layout tracking |
|  - Queues      |  | - Post targets     |  | - Clear state     |
+----------------+  | - Bloom mips       |  +-------------------+
                    | - SMAA targets     |
                    | - Effect snapshot  |
                    +--------------------+
```

### Key Source Files

| File | Responsibility |
|------|----------------|
| `VulkanRenderer.cpp/h` | Frame lifecycle, token minting, high-level API, deferred lighting orchestration |
| `VulkanDevice.cpp/h` | Device selection, swapchain creation/recreation, queues, pipeline cache |
| `VulkanRenderTargets.cpp/h` | Render target image/view/sampler creation, layout state tracking |
| `VulkanRenderingSession.cpp/h` | Target switching, dynamic rendering begin/end, barriers, clear ops |
| `VulkanFrame.cpp/h` | Per-frame resources: command pool/buffer, fences, semaphores, ring buffers |
| `VulkanFrameFlow.h` | `RecordingFrame`, `InFlightFrame`, `SubmitInfo` tokens |
| `VulkanFrameCaps.h` | `FrameCtx` and bound-frame tokens (`ModelBoundFrame`, `NanoVGBoundFrame`, `DecalBoundFrame`) |
| `VulkanPhaseContexts.h` | `UploadCtx`, `RenderCtx`, deferred lighting tokens |
| `VulkanConstants.h` | Global constants: `kFramesInFlight`, `kMaxBindlessTextures`, bindless slot assignments |
| `VulkanPipelineManager.cpp/h` | Pipeline cache, shader variant compilation |
| `VulkanTextureManager.cpp/h` | Texture upload, bindless slot assignment, residency management |
| `VulkanBufferManager.cpp/h` | Device-local buffer management, staging, deferred release |
| `VulkanDescriptorLayouts.cpp/h` | Descriptor set layouts, pipeline layouts, descriptor pool management |

---

## Frame Flow

A complete frame follows this sequence:

```
 CPU                                          GPU
  |                                            |
  | 1. acquireAvailableFrame()                 |
  |    [blocks on fence if needed, returns     |
  |     VulkanFrame from available pool]       |
  |                                            |
  | 2. acquireNextImage()                      |
  |    [signals imageAvailable semaphore]      |
  |                                            |
  | 3. beginFrame()                            |
  |    - Mint RecordingFrame token             |
  |    - Reset command buffer                  |
  |    - Begin command buffer recording        |
  |                                            |
  | 4. Record rendering commands               |
  |    - Target transitions (barriers)         |
  |    - Dynamic rendering scopes              |
  |    - Draw calls with push descriptors      |
  |                                            |
  | 5. endFrame()                              |
  |    - End command buffer recording          |
  |    - Transition swapchain to present       |
  |                                            |
  | 6. submitRecordedFrame()                   |
  |    - Wait on: imageAvailable               |  [waits]
  |    - Signal: renderFinished, timeline      |-------->  7. Execute
  |    - Consume RecordingFrame -> InFlightFrame           |
  |    - Push InFlightFrame to in-flight queue            |
  |                                                        |
  | 8. present()                                           |
  |    - Wait on: renderFinished               <-----------|
  |    [swapchain presents]                                |
  |                                                        v
  | 9. recycleOneInFlight()                    |
  |    - Wait on fence (CPU blocks)            |  [fence signaled]
  |    - Query timeline semaphore for serial   |
  |    - Trigger deferred releases for serial  |
  |    - Return frame to available pool        |
  |                                            |
  +--------------------------------------------+
```

### Synchronization Primitives

**Per-Frame (in `VulkanFrame`):**

| Primitive | Type | Purpose |
|-----------|------|---------|
| `m_imageAvailable` | Binary semaphore | GPU-GPU: Presentation engine -> Queue submission |
| `m_renderFinished` | Binary semaphore | GPU-GPU: Queue submission -> Present |
| `m_inflightFence` | Fence | CPU-GPU: Block CPU until frame GPU work completes |
| `m_timelineSemaphore` | Timeline semaphore | Per-frame timeline (currently unused; global timeline preferred) |

**Global (in `VulkanRenderer`):**

| Primitive | Type | Purpose |
|-----------|------|---------|
| `m_submitTimeline` | Timeline semaphore | Track GPU completion serial across all frames; drives deferred resource cleanup |
| `m_submitSerial` | `uint64_t` | Monotonically increasing serial assigned to each submit |
| `m_completedSerial` | `uint64_t` | Cached last-known completed serial |

See `VULKAN_SYNCHRONIZATION.md` for complete synchronization details.

---

## Render Targets

The renderer supports multiple render target configurations managed by `VulkanRenderTargets`:

### Scene Targets

| Target | Format | Depth | Purpose |
|--------|--------|-------|---------|
| Swapchain | Surface-dependent (commonly `VK_FORMAT_B8G8R8A8_UNORM` or `_SRGB`) | Main depth | Final presentation, HUD |
| Swapchain (no depth) | Same | None | Post-processing output, overlays |
| Scene HDR | `VK_FORMAT_R16G16B16A16_SFLOAT` | Main depth | HDR scene rendering |
| Scene HDR (no depth) | Same | None | Post-processing intermediate |
| Scene Effect | `VK_FORMAT_R16G16B16A16_SFLOAT` | None | Mid-scene snapshot for distortion/effects |

### Deferred G-Buffer Targets

Five attachments rendered simultaneously (all `VK_FORMAT_R16G16B16A16_SFLOAT`):

| Index | Name | Content |
|-------|------|---------|
| 0 | Color | Diffuse albedo |
| 1 | Normal | World-space normals |
| 2 | Position | World-space position |
| 3 | Specular | Specular parameters |
| 4 | Emissive | Emissive contribution (captures pre-deferred scene) |

The G-buffer attachment count is defined as `kGBufferCount = 5` in `VulkanRenderTargets.h`.

### Post-Processing Targets

All post-processing targets use `VK_FORMAT_B8G8R8A8_UNORM`:

| Target | Purpose |
|--------|---------|
| Post LDR | Tonemapped output from HDR scene |
| Post Luminance | FXAA luminance encoding |
| SMAA Edges | SMAA edge detection output |
| SMAA Blend | SMAA blend weights |
| SMAA Output | SMAA final anti-aliased output |

### Bloom Targets

Bloom uses ping-pong buffers with a mip chain (all `VK_FORMAT_R16G16B16A16_SFLOAT`):

| Target | Description |
|--------|-------------|
| Bloom 0 | First ping-pong buffer with 4 mip levels |
| Bloom 1 | Second ping-pong buffer with 4 mip levels |

Constants: `kBloomPingPongCount = 2`, `kBloomMipLevels = 4` in `VulkanRenderTargets.h`.

### Depth Targets

| Target | Format | Purpose |
|--------|--------|---------|
| Main depth | `VK_FORMAT_D32_SFLOAT_S8_UINT` (preferred), `VK_FORMAT_D24_UNORM_S8_UINT`, or `VK_FORMAT_D32_SFLOAT` | Scene geometry depth; sampled for deferred lighting |
| Cockpit depth | Same as main | Cockpit-only depth (for depth-aware post effects like lightshafts) |

Depth format is selected at runtime by `findDepthFormat()` based on device support, preferring formats with stencil for future expansion.

---

## Getting Started

### Recommended Reading Order

If you are new to this renderer:

1. **`VULKAN_RENDER_PASS_STRUCTURE.md`**
   - Target typestates
   - Dynamic rendering + RAII pass guard
   - Clear ops and frame lifecycle

2. **`VULKAN_SYNCHRONIZATION.md`**
   - Frames-in-flight model
   - Timeline/binary semaphores and fences
   - Layout transitions and sync2 barrier rules

3. **`VULKAN_CAPABILITY_TOKENS.md`**
   - Token types and their purposes
   - Token creation, consumption, and lifetime
   - Common patterns and mistakes

4. **`VULKAN_DESCRIPTOR_SETS.md`** and **`VULKAN_TEXTURE_BINDING.md`**
   - Push descriptors vs bindless model binding
   - Sampler cache and descriptor validity rules

5. **`VULKAN_2D_PIPELINE.md`** and **`VULKAN_HUD_RENDERING.md`**
   - 2D/UI/HUD rendering contracts
   - Clip/scissor behavior and invariants

6. **`VULKAN_FRAME_LIFECYCLE.md`**
   - Frame state machine, timeline semaphores, serial tracking

---

## Quick Reference

### Where Do I Change X?

**Frame lifecycle / recording / submit / present:**
- `VULKAN_SYNCHRONIZATION.md`
- Code: `VulkanRenderer.cpp` (`beginRecording`, `advanceFrame`, `submitRecordedFrame`, present logic)

**Adding a new render target or changing target sizes:**
- `VULKAN_RENDER_PASS_STRUCTURE.md` (target types and transitions)
- `VULKAN_DYNAMIC_BUFFERS.md` (if new transient buffers are needed)
- Code: `VulkanRenderTargets.cpp/h`, `VulkanRenderingSession.cpp/h`

**Adding/adjusting post-processing stages:**
- `VULKAN_POST_PROCESSING.md` (pipeline flow, target transitions, uniforms)
- `VULKAN_PIPELINE_MANAGEMENT.md` (pipeline and shader integration overview)
- `VULKAN_RENDER_PASS_STRUCTURE.md` (where passes occur and how targets switch)
- `VULKAN_DESCRIPTOR_SETS.md` (push descriptor bindings and global sets)

**Investigating binding/descriptor bugs:**
- `VULKAN_DESCRIPTOR_SETS.md`
- `VULKAN_TEXTURE_BINDING.md`
- `VULKAN_TEXTURE_RESIDENCY.md` (texture upload and bindless slot assignment)

**HUD/UI rendering issues (clip/scissor, overlay order):**
- `VULKAN_HUD_RENDERING.md`
- `VULKAN_2D_PIPELINE.md`

**Memory pressure / dynamic allocation / ring buffers:**
- `VULKAN_DYNAMIC_BUFFERS.md`
- Code: `VulkanBufferManager.cpp/h`, `VulkanRingBuffer.cpp/h`

**Pipeline issues / shader variants / cache behavior:**
- `VULKAN_PIPELINE_MANAGEMENT.md` (architecture and lifecycle)
- `VULKAN_PIPELINE_USAGE.md` (construction patterns, common mistakes)

**Uniform buffer alignment / std140 layout:**
- `VULKAN_UNIFORM_ALIGNMENT.md` (alignment rules, adding new structs)
- `VULKAN_UNIFORM_BINDINGS.md` (binding points, struct definitions)

**Deferred lighting / G-buffer rendering:**
- `VULKAN_DEFERRED_LIGHTING_FLOW.md` (complete flow, G-buffer channels)
- Code: `VulkanPhaseContexts.h` (typestate tokens), `VulkanRenderer.cpp` (deferred API)

**DLSS / upscaling:**
- `PLAN_DLSS.md` (implementation plan and required invariants)

### Common Code Patterns

**Beginning frame recording:**
```cpp
// In game loop
RecordingFrame recording = renderer.beginRecording();
FrameCtx ctx(renderer, recording);
// ... record commands ...
// Frame automatically submitted when recording goes out of scope
```

**Requiring a bound uniform buffer (for model rendering):**
```cpp
// Asserts that ModelData UBO is bound, returns proof token
ModelBoundFrame mbf = requireModelBound(frameCtx);
// mbf.modelSet, mbf.modelUbo now guaranteed valid for draw calls
```

**Transitioning render targets:**
```cpp
// Inside VulkanRenderingSession
m_renderingSession->suspendRendering();           // End active dynamic rendering
m_renderingSession->requestSceneHdrTarget();      // Select new target
RenderCtx ctx = renderer.ensureRenderingStarted(frameCtx);  // Begin rendering to new target
```

**Obtaining a render context (inside VulkanRenderer):**
```cpp
RenderTargetInfo info = m_session.ensureRendering(cmd, imageIndex);
RenderCtx ctx(cmd, info);  // Private constructor - only VulkanRenderer creates RenderCtx
```

---

## Testing and Behavioral Invariants

The Vulkan tests are split between:

- **Real unit tests** that exercise components with minimal stubs
- **Behavioral "fake state machine" tests** that encode invariants without requiring a GPU

Entry point: `VULKAN_INTEGRATION_TESTS.md`

When changing renderer state machines (target switching, pass boundaries, post-processing enablement), prefer adding/updating a behavioral invariant test that locks in the expected semantics.

### Key Invariants to Test

- Target transitions must suspend active rendering before switching
- Capability tokens cannot be forged (private constructors on `RecordingFrame`, `UploadCtx`, `RenderCtx`, deferred tokens)
- Layout transitions must track current layout accurately
- Bound-frame tokens require corresponding UBO bindings (assertions in `require*Bound()` functions)
- Submit serial increases monotonically; completed serial never exceeds submit serial

---

## Document Index

### Core Architecture

| Document | Topics |
|----------|--------|
| `VULKAN_DEVICE_INIT.md` | Instance/device creation, feature chains, required extensions, queue selection |
| `VULKAN_SWAPCHAIN.md` | Image acquisition, presentation, vsync, format selection, resize handling |
| `VULKAN_FRAME_LIFECYCLE.md` | Frame state machine, recording/in-flight containers, timeline semaphores |
| `VULKAN_RENDER_PASS_STRUCTURE.md` | Target typestates, dynamic rendering boundaries, clear ops |
| `VULKAN_SYNCHRONIZATION.md` | Fences/semaphores, sync2 barriers, layout transitions |
| `VULKAN_CAPABILITY_TOKENS.md` | Token types (FrameCtx, RenderCtx, etc.), creation, consumption, lifetime |

### Descriptors and Textures

| Document | Topics |
|----------|--------|
| `VULKAN_DESCRIPTOR_SETS.md` | Descriptor layouts, pools, push descriptors, bindless binding, update safety rules |
| `VULKAN_TEXTURE_BINDING.md` | Texture lifecycle, sampler cache, bindless slot rules, descriptor validity |
| `VULKAN_TEXTURE_RESIDENCY.md` | Texture residency state machine, upload batching, fallback handling |

### Pipelines and Shaders

| Document | Topics |
|----------|--------|
| `VULKAN_PIPELINE_MANAGEMENT.md` | Pipeline architecture, cache keys, shader variants, vertex input modes |
| `VULKAN_PIPELINE_USAGE.md` | Pipeline key construction, binding patterns, debugging tips |

### Uniforms

| Document | Topics |
|----------|--------|
| `VULKAN_UNIFORM_BINDINGS.md` | Uniform buffer structs, binding points, std140 layout |
| `VULKAN_UNIFORM_ALIGNMENT.md` | Alignment rules, adding new uniform structs, common mistakes |

### Buffers and Memory

| Document | Topics |
|----------|--------|
| `VULKAN_MEMORY_ALLOCATION.md` | Buffer/texture allocation, ring buffers, staging, deferred release |
| `VULKAN_DYNAMIC_BUFFERS.md` | Dynamic uniform buffers, orphaning semantics |

### Rendering Paths

| Document | Topics |
|----------|--------|
| `VULKAN_2D_PIPELINE.md` | Interface pipeline, coordinate systems, 2D draw path contracts |
| `VULKAN_HUD_RENDERING.md` | HUD draw order and shaders, clip/scissor, RTT cockpit displays |
| `VULKAN_DEFERRED_LIGHTING_FLOW.md` | Complete deferred lighting flow, G-buffer channels, light volumes |
| `VULKAN_POST_PROCESSING.md` | Post-processing pipeline chain, target transitions, uniform data flow |
| `VULKAN_MODEL_RENDERING_PIPELINE.md` | Model rendering path, vertex pulling, bindless textures |

### Plans and History

| Document | Topics |
|----------|--------|
| `PLAN_DLSS.md` | Upscaling plan (render vs display extents, jitter, motion vectors, pass placement) |
| `PLAN_REFACTOR_ARCHITECTURE.md` | Roadmap for design philosophy adoption |
| `PLAN_REFACTOR_TEXTURE_MANAGER.md` | Texture manager refactoring plans |

### Testing and Debugging

| Document | Topics |
|----------|--------|
| `VULKAN_INTEGRATION_TESTS.md` | Test philosophy and key invariant tests |
| `VULKAN_ERROR_HANDLING.md` | Error handling patterns, validation, debugging |
| `VULKAN_PERFORMANCE_OPTIMIZATION.md` | Performance considerations and optimization strategies |
