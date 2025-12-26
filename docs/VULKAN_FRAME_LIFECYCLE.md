# Vulkan Frame Lifecycle

This document describes the frame management system in the Vulkan renderer, covering frame states, synchronization primitives, capability tokens, and per-frame resource ownership.

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

| Method | Location | Purpose |
|--------|----------|---------|
| `beginRecording()` | `VulkanRenderer.cpp` | Acquires available frame, returns `RecordingFrame` token |
| `advanceFrame(prev)` | `VulkanRenderer.cpp` | Submits current recording, pushes to in-flight, starts new recording |
| `recycleOneInFlight()` | `VulkanRenderer.cpp` | Waits on oldest in-flight frame, resets it, pushes to available |
| `acquireAvailableFrame()` | `VulkanRenderer.cpp` | Pulls from available queue, blocking on recycle if empty |
| `prepareFrameForReuse()` | `VulkanRenderer.cpp` | Collects deferred resources, resets ring buffers |

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
// VulkanRenderer.h, 347-348
vk::UniqueSemaphore m_submitTimeline;
uint64_t m_submitSerial = 0;     // Incremented on each queue submit
uint64_t m_completedSerial = 0;  // Highest known completed serial
```

### Serial Flow

1. **Submit:** `submitRecordedFrame()` increments `m_submitSerial`, signals timeline at new value
2. **Query:** `queryCompletedSerial()` reads current timeline counter value (`VulkanRenderer.cpp`)
3. **Wait:** `recycleOneInFlight()` waits on the in-flight fence, then queries completion
4. **Update:** `m_completedSerial = max(m_completedSerial, queryCompletedSerial())`

### SubmitInfo Token

Each submission is tagged with metadata:

```cpp
// VulkanFrameFlow.h
struct SubmitInfo {
    uint32_t imageIndex;    // Swapchain image acquired for this frame
    uint32_t frameIndex;    // Index in m_frames[] array
    uint64_t serial;        // m_submitSerial value at submission
    uint64_t timeline;      // Timeline semaphore value signaled
};
```

---

## Capability Tokens

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
    uint32_t imageIndex;

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
| Encapsulated | Private `cmd()` | Command buffer access restricted |

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

| Token | Purpose | Private Constructor |
|-------|---------|---------------------|
| `UploadCtx` | Proves upload phase is active; enables staging operations | `friend class VulkanRenderer` |
| `RenderCtx` | Proves dynamic rendering is active; enables draw calls | `friend class VulkanRenderer` |
| `DeferredGeometryCtx` | Proves G-buffer pass is active | `friend class VulkanRenderer` |
| `DeferredLightingCtx` | Proves lighting pass is active | `friend class VulkanRenderer` |

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
static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;      // 512 KB
static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;      // 1 MB
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

Resources (buffers, textures) are not destroyed immediately; they are retired with a serial and collected when that serial is known to be complete.

### Collection Points

1. **Frame recycle:** `prepareFrameForReuse()` calls managers' `collect(completedSerial)`
2. **Frame begin:** `beginFrame()` opportunistically collects at `m_completedSerial`

```cpp
// VulkanRenderer.cpp
void VulkanRenderer::prepareFrameForReuse(VulkanFrame& frame, uint64_t completedSerial)
{
    m_bufferManager->collect(completedSerial);
    m_textureManager->collect(completedSerial);
    frame.reset();
}
```

### Safe Retire Serial

When a resource is deleted during recording, it is tagged with `m_submitSerial + 1` (the serial of the submission being recorded):

```cpp
// VulkanRenderer.cpp, 320, 324
m_textureManager->setSafeRetireSerial(m_submitSerial + 1);
m_movieManager->setSafeRetireSerial(m_submitSerial + 1);
m_bufferManager->setSafeRetireSerial(m_submitSerial + 1);
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

- **Signal:** Queue submission signals `m_inflightFence`
- **Wait:** `recycleOneInFlight()` calls `frame.wait_for_gpu()` which waits on the fence
- **Reset:** Fence is reset in `VulkanFrame::reset()` before next use

---

## Container Summary

| Container | Type | Purpose |
|-----------|------|---------|
| `m_frames` | `std::array<unique_ptr<VulkanFrame>, 2>` | Owns all frame objects |
| `m_availableFrames` | `std::deque<AvailableFrame>` | Frames ready for reuse |
| `m_inFlightFrames` | `std::deque<InFlightFrame>` | Frames submitted, awaiting GPU |

```cpp
// VulkanRenderer.h
struct AvailableFrame {
    VulkanFrame* frame;
    uint64_t completedSerial;  // Serial at which this frame became available
};
```

---

## Typical Frame Sequence

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

---

## Related Documentation

| Document | Topic |
|----------|-------|
| `VULKAN_ARCHITECTURE_OVERVIEW.md` | High-level renderer overview |
| `VULKAN_CAPABILITY_TOKENS.md` | Token design philosophy |
| `VULKAN_SYNCHRONIZATION.md` | Semaphores, barriers, and fences |
| `VULKAN_DYNAMIC_BUFFERS.md` | Buffer orphaning and update semantics |
