# Vulkan Frame Lifecycle

This document describes the frame management system in the Vulkan renderer, covering frame states, synchronization primitives, capability tokens, and per-frame resource ownership.

---

## Table of Contents

1. [Frame State Machine](#frame-state-machine)
2. [Two-Frame-In-Flight Design](#two-frame-in-flight-design)
3. [Timeline Semaphores and Serial Tracking](#timeline-semaphores-and-serial-tracking)
4. [Capability Tokens](#capability-tokens)
5. [Frame Begin Phase](#frame-begin-phase)
6. [Per-Frame Ring Buffers](#per-frame-ring-buffers)
7. [Deferred Resource Collection](#deferred-resource-collection)
8. [VulkanFrame Synchronization Primitives](#vulkanframe-synchronization-primitives)
9. [Rendering Session Integration](#rendering-session-integration)
10. [Container Summary](#container-summary)
11. [Typical Frame Sequence](#typical-frame-sequence)
12. [Related Documentation](#related-documentation)

---

## Frame State Machine

Each `VulkanFrame` transitions through three states managed by the renderer's container-based system:

```
Available ──[beginRecording]──> Recording ──[advanceFrame]──> InFlight
    ^                                                             │
    │                                                             │
    └──────────────[recycleOneInFlight (wait + reset)]────────────┘
```

| State | Container | Description |
|-------|-----------|-------------|
| **Available** | `m_availableFrames` | Frame is idle; GPU work complete, buffers reset, ready for reuse |
| **Recording** | Held via `RecordingFrame` token | CPU is recording commands; frame not yet submitted |
| **InFlight** | `m_inFlightFrames` | Submitted to GPU; awaiting fence signal |

### Key Methods

| Method | Purpose |
|--------|---------|
| `beginRecording()` | Acquires available frame, acquires swapchain image, returns `RecordingFrame` token |
| `advanceFrame(prev)` | Ends and submits current recording, pushes to in-flight, starts new recording |
| `recycleOneInFlight()` | Waits on oldest in-flight fence, queries timeline, resets frame, pushes to available |
| `acquireAvailableFrame()` | Pulls from available deque, blocking on recycle if empty |
| `prepareFrameForReuse()` | Collects deferred resources from all managers, resets ring buffers |
| `beginFrame()` | Internal: begins command buffer, flushes texture uploads, syncs descriptors |
| `endFrame()` | Internal: updates saved screen copy, ends rendering session, ends command buffer |

---

## Two-Frame-In-Flight Design

The renderer maintains exactly `kFramesInFlight = 2` frame objects (`VulkanConstants.h`):

```cpp
constexpr uint32_t kFramesInFlight = 2;
```

```cpp
// VulkanRenderer.h
std::array<std::unique_ptr<VulkanFrame>, kFramesInFlight> m_frames;
```

### Rationale

- **Pipelining:** While the GPU executes frame N, the CPU records frame N+1
- **Bounded latency:** At most one frame of GPU work queued ahead
- **Predictable memory:** Fixed number of per-frame ring buffers

### Container Invariants

At any instant:

```
|m_availableFrames| + |m_inFlightFrames| + (recording ? 1 : 0) == kFramesInFlight
```

---

## Timeline Semaphores and Serial Tracking

The renderer uses a global timeline semaphore for GPU completion tracking:

```cpp
// VulkanRenderer.h
vk::UniqueSemaphore m_submitTimeline;  // Global GPU completion timeline
uint64_t m_submitSerial = 0;           // Incremented on each queue submit
uint64_t m_completedSerial = 0;        // Highest known completed serial (cached)
```

### Serial Flow

1. **Pre-Submit:** During recording, `setSafeRetireSerial(m_submitSerial + 1)` is called on all resource managers so resources deleted during recording are tagged with the upcoming submission serial
2. **Submit:** `submitRecordedFrame()` increments `m_submitSerial` and signals the timeline semaphore at the new value
3. **Post-Submit:** `setSafeRetireSerial(m_submitSerial)` updates managers to the actual submitted serial
4. **Query:** `queryCompletedSerial()` reads the current timeline counter value via `getSemaphoreCounterValue()`
5. **Wait:** `recycleOneInFlight()` waits on the per-frame fence, then queries the timeline for the completed serial
6. **Update:** `m_completedSerial = max(m_completedSerial, queryCompletedSerial())`
7. **Collect:** `prepareFrameForReuse()` collects resources with `retireSerial <= completedSerial`

### SubmitInfo Token

Each submission is tagged with metadata for tracking:

```cpp
// VulkanFrameFlow.h
struct SubmitInfo {
    uint32_t imageIndex;    // Swapchain image acquired for this frame
    uint32_t frameIndex;    // Index in m_frames[] array (0 to kFramesInFlight-1)
    uint64_t serial;        // m_submitSerial value at submission
    uint64_t timeline;      // Timeline semaphore value signaled (equals serial)
};
```

**Note:** The `serial` and `timeline` fields are currently identical. The distinction exists to support potential future use of per-frame timeline semaphores for more granular tracking.

---

## Capability Tokens

Capability tokens encode invariants in the type system, making invalid operations compile-time errors rather than runtime bugs. See `docs/VULKAN_CAPABILITY_TOKENS.md` for the full practical guide.

### RecordingFrame

`RecordingFrame` proves that command recording is active. It is move-only with a private constructor, making it unforgeable:

```cpp
// VulkanFrameFlow.h
struct RecordingFrame {
    RecordingFrame(const RecordingFrame&) = delete;
    RecordingFrame& operator=(const RecordingFrame&) = delete;
    RecordingFrame(RecordingFrame&&) = default;
    RecordingFrame& operator=(RecordingFrame&&) = default;

    VulkanFrame& ref() const { return frame.get(); }

  private:
    // Only VulkanRenderer may mint the recording token
    RecordingFrame(VulkanFrame& f, uint32_t img) : frame(f), imageIndex(img) {}

    std::reference_wrapper<VulkanFrame> frame;
    uint32_t imageIndex;  // Swapchain image index for this frame

    vk::CommandBuffer cmd() const { return frame.get().commandBuffer(); }

    friend class VulkanRenderer;
};
```

**Properties:**

| Property | Mechanism | Enforcement |
|----------|-----------|-------------|
| Unforgeable | Private constructor | Only `VulkanRenderer::beginRecording()` can create |
| Non-copyable | Deleted copy ops | Cannot duplicate token |
| Move-only | Default move ops | Ownership transfers through frame pipeline |
| Encapsulated | Private `cmd()` | Command buffer access restricted to renderer internals |

### FrameCtx

`FrameCtx` is a copyable wrapper that provides access to the current frame and renderer. It is created from `RecordingFrame` and passed to most rendering operations:

```cpp
// VulkanFrameCaps.h
struct FrameCtx {
    VulkanRenderer& renderer;

    FrameCtx(VulkanRenderer& inRenderer, RecordingFrame& inRecording)
        : renderer(inRenderer), m_recording(inRecording) {}

    VulkanFrame& frame() const { return m_recording.ref(); }

  private:
    RecordingFrame& m_recording;  // Reference, not ownership
    friend class VulkanRenderer;
};
```

**Properties:**
- **Copyable**: Holds references, not ownership; multiple instances can exist simultaneously
- **Created by**: `currentFrameCtx()` helper in `VulkanGraphics.cpp`
- **Consumed by**: Most rendering APIs that need frame access (`ensureRenderingStarted()`, `setViewport()`, etc.)

### InFlightFrame

`InFlightFrame` wraps a submitted frame with its `SubmitInfo`:

```cpp
// VulkanFrameFlow.h
struct InFlightFrame {
    std::reference_wrapper<VulkanFrame> frame;
    SubmitInfo submit;

    InFlightFrame(VulkanFrame& f, SubmitInfo s) : frame(f), submit(s) {}

    // Non-copyable, move-only
    InFlightFrame(const InFlightFrame&) = delete;
    InFlightFrame& operator=(const InFlightFrame&) = delete;
    InFlightFrame(InFlightFrame&&) = default;
    InFlightFrame& operator=(InFlightFrame&&) = default;

    VulkanFrame& ref() const { return frame.get(); }
};
```

### Phase Context Tokens

Additional tokens enforce phase-specific API access (`VulkanPhaseContexts.h`):

| Token | Purpose | Created By |
|-------|---------|------------|
| `UploadCtx` | Proves upload phase is active; enables staging operations | Internal (frame start, `beginFrame()`) |
| `RenderCtx` | Proves dynamic rendering is active; enables draw calls | `ensureRenderingStarted(FrameCtx)` |
| `DeferredGeometryCtx` | Proves G-buffer pass is active | `deferredLightingBegin()` |
| `DeferredLightingCtx` | Proves lighting pass is active | `deferredLightingEnd()` |

### Token Hierarchy

```
Frame Lifecycle:
    RecordingFrame (internal, move-only)
        ├── FrameCtx (copyable reference wrapper)
        │       └── RenderCtx (move-only, created on demand)
        │               └── Used for draw operations
        └── InFlightFrame (internal, move-only, after submit)
                └── Tracks GPU completion

Phase Tokens:
    UploadCtx (internal, frame start)
    DeferredGeometryCtx ──> DeferredLightingCtx (typestate sequence)
```

---

## Frame Begin Phase

The `beginFrame()` method performs critical per-frame initialization after acquiring a swapchain image but before any rendering commands are recorded:

### Operations Sequence

1. **Reset per-frame bindings:** Clear stale uniform buffer bindings from previous frame
2. **Reset scene texture state:** Clear any leaked scene framebuffer state
3. **Begin command buffer:** Start recording with `vk::CommandBufferUsageFlagBits::eOneTimeSubmit`
4. **Collect deferred resources:** Query timeline semaphore and collect resources from all managers
5. **Set safe retire serial:** Tag resource managers with `m_submitSerial + 1` for deletion tracking
6. **Flush texture uploads:** Execute pending texture uploads via `flushPendingUploads(UploadCtx)`
7. **Sync model descriptors:** Update bindless texture array and model vertex heap binding
8. **Begin rendering session:** Initialize swapchain/depth barriers and reset render state

### Upload Phase

Texture uploads are batched and executed at frame start, before any rendering begins:

```cpp
// VulkanRenderer.cpp - beginFrame()
const UploadCtx uploadCtx{frame, cmd, m_frameCounter};
m_textureUploader->flushPendingUploads(uploadCtx);
```

The `UploadCtx` token proves the upload phase is active and provides access to the frame's staging ring buffer. This design ensures:
- Textures requested before rendering starts are available for the current frame
- No render pass is active (uploads require transfer operations)
- Staging buffer allocations are isolated to the current frame

### Descriptor Synchronization

After upload flush, model descriptors are synchronized:

```cpp
// VulkanRenderer.cpp - beginFrame()
vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);
```

This updates the per-frame model descriptor set with:
- Current frame's vertex heap buffer binding
- Newly-resident bindless texture slots (after upload flush)

---

## Per-Frame Ring Buffers

Each `VulkanFrame` owns three ring buffers for transient allocations (`VulkanFrame.h`):

```cpp
VulkanRingBuffer m_uniformRing;   // Uniform buffer data
VulkanRingBuffer m_vertexRing;    // Dynamic vertex data
VulkanRingBuffer m_stagingRing;   // Texture upload staging
```

### Ring Buffer Sizes

```cpp
// VulkanRenderer.h
static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;       // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;       // 1 MB
static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024; // 12 MB
```

### Allocation Interface

```cpp
// VulkanRingBuffer.h, 32-34
struct Allocation {
    vk::DeviceSize offset{0};
    void* mapped{nullptr};
};

Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
std::optional<Allocation> try_allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
void reset();  // Called when frame is recycled
```

### Lifetime

- **Allocated:** During frame recording (CPU writes data)
- **In-use:** While frame is in-flight (GPU reads data)
- **Reset:** When frame is recycled via `VulkanFrame::reset()` after GPU wait

---

## Deferred Resource Collection

Resources (buffers, textures, movie textures) are not destroyed immediately; they are retired with a serial and collected when that serial is known to be complete. This prevents destroying GPU resources that may still be referenced by in-flight command buffers.

### Collection Points

1. **Frame begin:** `beginFrame()` opportunistically collects at `m_completedSerial` (queries timeline)
2. **Frame recycle:** `prepareFrameForReuse()` collects after fence wait confirms GPU completion

```cpp
// VulkanRenderer.cpp - prepareFrameForReuse()
void VulkanRenderer::prepareFrameForReuse(VulkanFrame& frame, uint64_t completedSerial)
{
    m_bufferManager->collect(completedSerial);
    m_textureManager->collect(completedSerial);
    frame.reset();  // Resets command pool and ring buffers
}
```

**Note:** Movie textures are collected in `beginFrame()` rather than `prepareFrameForReuse()`:

```cpp
// VulkanRenderer.cpp - beginFrame()
if (m_movieManager) {
    m_movieManager->collect(m_completedSerial);
}
```

### Safe Retire Serial

The safe retire serial determines what serial value is assigned to resources deleted during the current recording phase. The key insight is:

- **During recording:** Resources deleted may still be referenced by the command buffer being recorded, so they're tagged with `m_submitSerial + 1` (the upcoming submission)
- **After submit:** The serial is updated to the actual submitted value

```cpp
// VulkanRenderer.cpp - beginFrame() (before recording starts)
m_textureManager->setSafeRetireSerial(m_submitSerial + 1);
m_movieManager->setSafeRetireSerial(m_submitSerial + 1);
m_bufferManager->setSafeRetireSerial(m_submitSerial + 1);

// VulkanRenderer.cpp - submitRecordedFrame() (after submit)
m_textureManager->setSafeRetireSerial(m_submitSerial);  // Now equals the actual submitted serial
m_bufferManager->setSafeRetireSerial(m_submitSerial);
```

Resources are only collected when `completedSerial >= retireSerial`.

---

## VulkanFrame Synchronization Primitives

Each frame owns its own sync objects (`VulkanFrame.h`):

```cpp
vk::UniqueFence m_inflightFence;          // CPU waits on this before reuse
vk::UniqueSemaphore m_imageAvailable;     // Signals when swapchain image acquired
vk::UniqueSemaphore m_renderFinished;     // Signals when rendering complete
vk::UniqueSemaphore m_timelineSemaphore;  // Per-frame timeline (unused in current design)
uint64_t m_timelineValue = 0;
```

### Fence Usage

- **Creation:** Fence is created in signaled state to allow first frame without blocking
- **Reset:** `submitRecordedFrame()` resets the fence before queue submission
- **Signal:** Queue submission signals `m_inflightFence`
- **Wait:** `recycleOneInFlight()` calls `frame.wait_for_gpu()` which blocks until fence is signaled

**Important:** The fence must be unsignaled at submission time. It is reset in `submitRecordedFrame()`, not in `VulkanFrame::reset()`.

---

## Rendering Session Integration

`VulkanRenderingSession` manages the active render target and dynamic rendering state within a frame. It is owned by `VulkanRenderer` and operates on the current frame's command buffer.

### Session Lifecycle

1. **`beginFrame(cmd, imageIndex)`**: Called at frame start
   - Resets swapchain layout tracking if generation changed
   - Ends any active render pass (should be none at frame start)
   - Selects default target (swapchain + depth)
   - Resets clear operations to `ClearAll`
   - Transitions swapchain and depth to attachment layouts

2. **`ensureRendering(cmd, imageIndex)`**: Called before draw operations
   - Returns immediately if render pass already active
   - Begins dynamic rendering for current target
   - Applies dynamic state (cull mode, depth test/write)
   - Returns `RenderTargetInfo` for pipeline selection

3. **`endFrame(cmd, imageIndex)`**: Called at frame end
   - Ends any active render pass
   - Transitions swapchain to present layout

### Target Switching

Target changes automatically end the active render pass. After switching targets, callers must obtain a new `RenderCtx` via `ensureRenderingStarted()`:

```cpp
// Target switch pattern
auto ctx1 = ensureRenderingStarted(frameCtx);  // Swapchain rendering
// ... draw to swapchain ...

m_renderingSession->requestPostLdrTarget();     // Ends pass, selects new target
auto ctx2 = ensureRenderingStarted(frameCtx);  // New pass on LDR target
// ... post-processing draws ...
```

### Clear Operations

Clear operations are one-shot and consumed when rendering begins:

```cpp
m_renderingSession->requestClear();           // Request clear on next beginRendering
auto ctx = ensureRenderingStarted(frameCtx);  // Clear executes here
// After this, load ops are LOAD, not CLEAR
```

This design supports suspend/resume patterns where rendering may be interrupted for non-rendering operations (texture updates, copies).

---

## Container Summary

| Container | Type | Purpose |
|-----------|------|---------|
| `m_frames` | `std::array<unique_ptr<VulkanFrame>, 2>` | Owns all frame objects |
| `m_availableFrames` | `std::deque<AvailableFrame>` | Frames ready for reuse (FIFO) |
| `m_inFlightFrames` | `std::deque<InFlightFrame>` | Frames submitted, awaiting GPU (FIFO) |

```cpp
// VulkanRenderer.h
struct AvailableFrame {
    VulkanFrame* frame;
    uint64_t completedSerial;  // Serial at which this frame became available
};
```

### Container Invariant

At any instant:
```
|m_availableFrames| + |m_inFlightFrames| + (recording ? 1 : 0) == kFramesInFlight
```

---

## Typical Frame Sequence

### High-Level Flow

```
Frame N-1 in-flight  ─┐
                      │
CPU: beginRecording() │  ← Acquires available frame, returns RecordingFrame
     │                │
     ├─ record cmds ──┤
     │                │
CPU: advanceFrame()   │  ← Submits frame N, pushes to in-flight
     │                │
     └─ Frame N now in-flight
                      │
GPU: executes N-1 ────┘
GPU: signals fence N-1

CPU: recycleOneInFlight() ← Waits fence N-1, collects resources, pushes to available
```

### Detailed Internal Sequence

```
beginRecording()
├── acquireAvailableFrame()
│   └── [if empty] recycleOneInFlight()  ← Blocks on fence, collects, resets
├── acquireImageOrThrow()                ← Acquire swapchain image (may recreate swapchain)
└── beginFrame()
    ├── resetPerFrameBindings()
    ├── cmd.begin()
    ├── collect(m_completedSerial)       ← Opportunistic deferred release
    ├── setSafeRetireSerial(serial+1)    ← Tag deletions for this recording
    ├── flushPendingUploads(UploadCtx)   ← Execute queued texture uploads
    ├── beginModelDescriptorSync()       ← Update bindless textures + vertex heap
    └── renderingSession.beginFrame()    ← Swapchain/depth barriers

[Recording Phase - Engine records rendering commands]
├── ensureRenderingStarted() → RenderCtx
├── draw commands...
├── target switches (auto-end/begin passes)
└── ...

advanceFrame(prev)
├── endFrame()
│   ├── updateSavedScreenCopy()
│   ├── renderingSession.endFrame()      ← Swapchain to present layout
│   └── cmd.end()
├── submitRecordedFrame()
│   ├── resetFences()
│   ├── submit2() with semaphores        ← imageAvailable wait, renderFinished signal
│   ├── ++m_submitSerial                 ← Timeline signal value
│   ├── setSafeRetireSerial(serial)      ← Update to actual submitted serial
│   └── present()
├── m_inFlightFrames.emplace()           ← Track for GPU completion
├── logFrameCounters()
└── beginRecording()                     ← Start next frame
```

### Semaphore and Fence Flow

```
vkAcquireNextImageKHR
         │
         │ signals imageAvailable
         v
+------------------+
│ Queue Submit     │
│   wait: imageAvailable @ COLOR_ATTACHMENT_OUTPUT
│   signal: renderFinished @ COLOR_ATTACHMENT_OUTPUT
│   signal: m_submitTimeline @ ALL_COMMANDS (value = serial)
│   signal: m_inflightFence
+------------------+
         │
         │ signals renderFinished
         v
vkQueuePresentKHR
   wait: renderFinished
         │
         │ (next frame needs this slot)
         v
recycleOneInFlight()
   wait: fence (CPU blocks)
   query: timeline counter → m_completedSerial
```

---

## Related Documentation

| Document | Topic |
|----------|-------|
| `VULKAN_ARCHITECTURE_OVERVIEW.md` | High-level renderer overview |
| `VULKAN_CAPABILITY_TOKENS.md` | Token design philosophy and practical usage |
| `VULKAN_SYNCHRONIZATION.md` | Semaphores, barriers, fences, and layout transitions |
| `VULKAN_DYNAMIC_BUFFERS.md` | Buffer orphaning and update semantics |
| `VULKAN_RENDER_PASS_STRUCTURE.md` | Render targets and dynamic rendering |
| `VULKAN_TEXTURE_RESIDENCY.md` | Texture upload and bindless residency |
| `VULKAN_DEFERRED_LIGHTING_FLOW.md` | Deferred rendering phase tokens |
| `DESIGN_PHILOSOPHY.md` | Type-driven design and capability token principles |
