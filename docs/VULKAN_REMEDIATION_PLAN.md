# Type-Driven Vulkan Architecture

This document describes the target architecture concepts for the Vulkan renderer, applying type-driven design principles to GPU resource management, frame lifecycle, and rendering state. It serves as a conceptual reference for architectural decisions during implementation.

For abstract principles, see `DESIGN_PHILOSOPHY.md`. For concrete implementation tasks, see `REMEDIATION_PLAN.md`.

---

## Core Principle

**Scope is the state machine.** The absence of a token IS the state. Functions that require a state take that state's type as a parameter. Invalid states should not compile.

---

## 1. Frame Recording as Typestate

### The Problem

Frame recording has two phases: recording (accepting draw commands) and not-recording (between frames). Code that assumes recording is active when it isn't causes undefined behavior or crashes.

Traditional approach: store a boolean or nullable pointer, check it at runtime.

```cpp
// Runtime check approach (problematic)
class Renderer {
    bool m_isRecording;
    VulkanFrame* m_currentFrame;  // null when not recording

    void draw() {
        if (!m_isRecording) throw std::runtime_error("Not recording");
        // ...
    }
};
```

This makes "draw when not recording" a runtime error instead of a compile-time error.

### The Pattern: Recording Token as Typestate

The recording state should be a value that exists only during recording. Functions that require recording take this value as a parameter.

```cpp
// Typestate approach
struct RecordingFrame {
    VulkanFrame& frame;
    uint32_t imageIndex;

    // Cannot be default-constructed
    RecordingFrame(VulkanFrame& f, uint32_t idx) : frame(f), imageIndex(idx) {}
};

class Renderer {
    // NO m_isRecording, NO m_currentFrame

    RecordingFrame beginRecording();  // Produces token
    void endRecording(RecordingFrame&& rec);  // Consumes token
};

// Draw functions require the token
void draw(RecordingFrame& rec, const Mesh& mesh);
```

**Why this works:** You cannot call `draw()` without a `RecordingFrame`. You cannot construct a `RecordingFrame` except through `beginRecording()`. The type system enforces the phase.

### RAII Extension

For automatic cleanup, wrap the token in an RAII guard:

```cpp
class ActiveRecording {
    RecordingFrame m_recording;
public:
    explicit ActiveRecording(RecordingFrame rec) : m_recording(rec) {}
    ~ActiveRecording() { /* submit frame */ }

    // Move-only
    ActiveRecording(const ActiveRecording&) = delete;
    ActiveRecording(ActiveRecording&&) = default;

    RecordingFrame& get() { return m_recording; }
};

// Usage: recording ends when guard goes out of scope
void renderFrame(Renderer& r) {
    ActiveRecording rec = r.flip();
    draw(rec.get(), mesh);
}  // rec destructor submits
```

---

## 2. Frame Pools as Container State

### The Problem

Frames transition through states: available (ready for recording), recording (accepting commands), in-flight (submitted to GPU), completed (GPU done). Traditional approach uses state enums or polymorphic state objects.

```cpp
// Enum approach (problematic)
struct Frame {
    enum State { Available, Recording, InFlight, Completed } state;
};

// Polymorphic approach (also problematic)
class FramePoolState {
    virtual Frame& acquireFrame() = 0;
};
class WarmupPool : public FramePoolState { ... };
class SteadyPool : public FramePoolState { ... };
```

Both require runtime checks to determine frame state.

### The Pattern: Containers as State Machine

A frame's state is determined by which container holds it. No enum needed.

```cpp
class FramePool {
    std::deque<VulkanFrame*> m_available;   // Available state
    std::deque<InFlightFrame> m_inFlight;   // In-flight state
    // Recording state = returned to caller, not in any container

    // Transitions are container operations
    VulkanFrame* acquireAvailable() {
        auto* f = m_available.front();
        m_available.pop_front();
        return f;  // Now in "recording" state (caller owns it)
    }

    void submitToGpu(VulkanFrame* f, SubmitInfo info) {
        m_inFlight.push_back({f, info});  // Transitions to in-flight
    }

    VulkanFrame* reclaimCompleted() {
        auto inf = m_inFlight.front();
        m_inFlight.pop_front();
        waitForCompletion(inf);
        return inf.frame;  // Returns to available (or caller)
    }
};
```

**Why this works:** A frame cannot be in two states simultaneously because it can only be in one container. State transitions are explicit container operations. Querying state is container membership, not field inspection.

---

## 3. Texture Lifecycle as Location-Based State

### The Problem

Textures transition through states: missing (not loaded), queued (upload requested), uploading (transfer in progress), resident (ready for use), retired (pending destruction). Traditional approach uses a state enum.

```cpp
// Enum approach (problematic)
struct TextureRecord {
    enum State { Missing, Queued, Uploading, Resident, Failed, Retired } state;
    std::optional<GpuTexture> gpu;  // Valid only if Resident (trust me)
    std::optional<UploadJob> job;   // Valid only if Uploading (trust me)
};
```

The `std::optional` fields can lie about the state. Nothing prevents `state == Resident` with `gpu == nullopt`.

### The Pattern: Separate Containers Per State

Each state is a separate container with appropriate data.

```cpp
class TextureManager {
    // Containers ARE the state machine
    std::set<TextureId> m_queued;                          // Queued
    std::map<TextureId, UploadJob> m_uploading;            // Uploading
    std::map<TextureId, GpuTexture> m_resident;            // Resident
    std::map<TextureId, RetiredTexture> m_retired;         // Retired
    // Missing = not in any container

    // State queries are container membership
    bool isResident(TextureId id) const {
        return m_resident.contains(id);
    }

    // Transitions move between containers
    void completeUpload(TextureId id) {
        auto job = std::move(m_uploading.at(id));
        m_uploading.erase(id);
        m_resident.emplace(id, job.finalize());  // Transition
    }
};
```

**Why this works:** A resident texture has a `GpuTexture` by construction (it's in `m_resident` which maps to `GpuTexture`). An uploading texture has an `UploadJob` by construction. The data structure enforces the invariant.

---

## 4. Phase Separation via Capability Tokens

### The Problem

Certain operations are only valid during specific phases. Texture uploads must happen before rendering starts. Recording barriers during an active render pass is invalid.

Traditional approach: runtime assertions.

```cpp
void uploadTexture(TextureId id) {
    assert(!m_renderingActive);  // Runtime check
    // ... record upload commands
}
```

### The Pattern: Phase Tokens as Capability Proof

Create tokens that can only exist during the valid phase. Functions require the token.

```cpp
// Token constructible only by the phase owner
class UploadPhase {
    friend class Renderer;
    UploadPhase() = default;  // Private
public:
    UploadPhase(const UploadPhase&) = delete;
    vk::CommandBuffer cmd;
};

class RenderPhase {
    friend class Renderer;
    RenderPhase() = default;  // Private
public:
    RenderPhase(const RenderPhase&) = delete;
    vk::CommandBuffer cmd;
};

// Functions declare their phase requirement
void uploadTexture(UploadPhase& phase, TextureId id);  // Requires upload phase
void drawMesh(RenderPhase& phase, const Mesh& mesh);   // Requires render phase

// Renderer controls phase transitions
class Renderer {
    UploadPhase beginUploadPhase();
    RenderPhase beginRenderPhase(UploadPhase&& upload);  // Consumes upload phase
    void endFrame(RenderPhase&& render);                  // Consumes render phase
};
```

**Why this works:** You cannot call `uploadTexture()` without an `UploadPhase`. You cannot have both `UploadPhase` and `RenderPhase` simultaneously because `beginRenderPhase` consumes the upload phase. Phase violations become compile-time errors.

---

## 5. Render Pass State: Bake vs Cache

### The Problem

Dynamic render state (depth test, cull mode, blend mode) must be set before drawing. Traditional approach caches current state and only updates when changed.

```cpp
// Caching approach (problematic)
class RenderSession {
    bool m_depthTest = true;
    bool m_depthWrite = true;
    vk::CullMode m_cullMode = vk::CullMode::eBack;

    void setDepthTest(bool enable) {
        if (m_depthTest != enable) {
            m_depthTest = enable;
            cmd.setDepthTestEnable(enable);
        }
    }
};
```

Problems: cache can get out of sync with GPU state; conditional updates are error-prone; state depends on call order.

### The Pattern: Bake State at Pass Entry

Pass all render state as a parameter when beginning the pass. Apply unconditionally.

```cpp
struct RasterState {
    bool depthTest;
    bool depthWrite;
    vk::CullMode cullMode;
    // ... all render state
};

class RenderSession {
    // NO cached state members

    void beginPass(vk::CommandBuffer cmd, const RasterState& state) {
        // Apply unconditionally - no caching, no conditionals
        cmd.setDepthTestEnable(state.depthTest);
        cmd.setDepthWriteEnable(state.depthWrite);
        cmd.setCullMode(state.cullMode);
    }
};
```

**Why this works:** State is explicit at the call site. No hidden dependencies on previous calls. GPU state matches the parameter because it's set unconditionally. The pass owns its complete state.

### When Caching Is Acceptable

Caching is acceptable for state that:
1. Truly persists across passes (global settings)
2. Is expensive to query from external sources
3. Is validated at cache update time

Caching is not acceptable for state that:
1. Varies per-pass or per-draw
2. Can be computed cheaply
3. Is "current GPU state" (the GPU is the source of truth, not a shadow copy)

---

## 6. Sentinel-Free Resource Binding

### The Problem

Resource bindings (uniform buffers, textures) may or may not be present. Traditional approach uses sentinel values.

```cpp
// Sentinel approach (problematic)
constexpr uint32_t OFFSET_ABSENT = 0xFFFFFFFF;

struct VertexLayout {
    uint32_t positionOffset;   // OFFSET_ABSENT if not present
    uint32_t normalOffset;     // OFFSET_ABSENT if not present
    uint32_t texcoordOffset;   // OFFSET_ABSENT if not present
};

void draw(const VertexLayout& layout) {
    if (layout.positionOffset != OFFSET_ABSENT) {
        // use position
    }
}
```

Problems: sentinel value is a valid offset on some platforms; easy to forget the check; nothing prevents using absent offset.

### The Pattern: Type-Safe Wrappers

Wrap optional values in types that enforce checking.

```cpp
class VertexOffset {
    uint32_t m_value;
    bool m_present;

    VertexOffset(uint32_t v) : m_value(v), m_present(true) {}
    VertexOffset() : m_value(0), m_present(false) {}

public:
    static VertexOffset present(uint32_t v) { return VertexOffset(v); }
    static VertexOffset absent() { return VertexOffset(); }

    bool isPresent() const { return m_present; }
    uint32_t get() const {
        Assertion(m_present, "Accessing absent offset");
        return m_value;
    }

    // For GPU interface where sentinel is required
    uint32_t toGpuFormat() const {
        return m_present ? m_value : 0xFFFFFFFF;
    }
};
```

**Why this works:** You cannot access the value without checking presence (or the assertion fires). The sentinel exists only at the GPU boundary (`toGpuFormat()`), not in application logic.

### For Bindings: Existence vs Non-Existence

For resource bindings where "unbound" is common:

```cpp
// Frame tracks bound state internally
class Frame {
    std::optional<UniformBinding> m_modelBinding;

public:
    void bindModelUniform(gr_buffer_handle h, uint32_t offset) {
        m_modelBinding = UniformBinding{h, offset};
    }

    bool hasModelBinding() const { return m_modelBinding.has_value(); }

    const UniformBinding& getModelBinding() const {
        Assertion(m_modelBinding, "No model binding");
        return *m_modelBinding;
    }

    void resetBindings() { m_modelBinding.reset(); }
};

// Draw functions check or require binding
void drawModel(Frame& frame, const Model& model) {
    if (!frame.hasModelBinding()) {
        bindFallbackUniform(frame);
    }
    // proceed with drawing
}
```

---

## Summary: Patterns at a Glance

| Concept | Anti-Pattern | Type-Driven Pattern |
|---------|--------------|---------------------|
| Frame recording | `bool isRecording` + runtime check | `RecordingFrame` token required by functions |
| Frame pool state | `enum State` or polymorphic hierarchy | Separate containers per state |
| Texture lifecycle | `TextureState` enum + optional fields | Container membership determines state |
| Phase separation | `assert(!renderingActive)` | Phase tokens consumed by transitions |
| Render state | Cached members + conditional updates | `RasterState` struct baked at pass entry |
| Optional bindings | Sentinel values (`0xFFFFFFFF`) | Type-safe wrappers with explicit checks |

---

## Guiding Questions

When reviewing or designing Vulkan code, ask:

1. **Can invalid state be represented?** If yes, restructure so it cannot.
2. **Is state queried or proven?** Prefer proving via type (token exists) over querying (check boolean).
3. **Where is state stored?** Prefer container membership over fields.
4. **What phase is this operation valid in?** Require a capability token for that phase.
5. **Is this conditional protecting invalid state?** If so, eliminate the invalid state instead of guarding it.

The goal: **if it compiles, the state is valid.**
