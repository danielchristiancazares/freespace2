# Vulkan Renderer Architecture Remediation Plan

## Executive Summary

This document outlines a comprehensive, phased remediation plan to eliminate runtime state checks, polymorphic state machines, sentinel values, and dynamic state caching from the Vulkan renderer. The goal is to make invalid states unrepresentable at compile time through type-driven design.

**Core Principle:** Scope is the state machine. The absence of a token IS the state. Functions that require a state take that state's type as a parameter.

---

## Phase 1: Frame Recording State (CRITICAL - Blocks Other Fixes)

### Problem

**Current Violation:** Runtime polymorphism with `NoRecording`/`ActiveRecording` classes
- `std::unique_ptr<RecordingState> m_recording` allows querying state at runtime
- `dynamic_cast` used to check state (violates principles)
- `NoRecording::frame()` throws exceptions (runtime failure instead of compile-time prevention)
- Functions like `recordingFrame()` can be called when no recording exists

**Location:** `VulkanRenderer.h:191-215`, `VulkanRenderer.cpp:275-405`

### Solution: RAII Recording Scope

**New Design:**

```cpp
// RecordingFrame is a value type that exists only when recording is active
struct RecordingFrame {
    VulkanFrame& frame;
    uint32_t imageIndex;
    
    // Explicit constructor - cannot be default-constructed
    RecordingFrame(VulkanFrame& f, uint32_t idx) : frame(f), imageIndex(idx) {}
};

// ActiveRecording is a move-only RAII guard
class ActiveRecording {
public:
    explicit ActiveRecording(RecordingFrame recording) : m_recording(recording) {}
    ~ActiveRecording() {
        // RAII destructor submits the frame
        if (m_recording.frame) {
            finishAndSubmit();
        }
    }
    
    // Move-only
    ActiveRecording(const ActiveRecording&) = delete;
    ActiveRecording& operator=(const ActiveRecording&) = delete;
    ActiveRecording(ActiveRecording&&) = default;
    ActiveRecording& operator=(ActiveRecording&&) = default;
    
    VulkanFrame& frame() const { return m_recording.frame; }
    uint32_t imageIndex() const { return m_recording.imageIndex; }
    
private:
    RecordingFrame m_recording;
    void finishAndSubmit(); // Called by destructor
};
```

**Refactored Interface:**

```cpp
class VulkanRenderer {
public:
    // flip() returns ActiveRecording - caller owns the lifetime
    ActiveRecording flip();
    
    // Functions that need a recording frame take ActiveRecording&
    void renderSomething(ActiveRecording& recording);
    
private:
    // NO m_recording member - recording exists only in caller's scope
    std::deque<VulkanFrame*> m_availableFrames;
    std::deque<InFlightFrame> m_inFlightFrames;
    
    // Internal helper - consumes ActiveRecording, produces InFlightFrame
    InFlightFrame finishRecording(ActiveRecording&& recording);
};
```

**Migration Strategy:**

1. **Step 1.1:** Add `ActiveRecording` class (move-only RAII guard)
2. **Step 1.2:** Refactor `flip()` to return `ActiveRecording` instead of void
3. **Step 1.3:** Update all callers of `recordingFrame()` to take `ActiveRecording&` parameter
4. **Step 1.4:** Remove `m_recording` member and `RecordingState` hierarchy
5. **Step 1.5:** Update `gr_vulkan_push_debug_group` to take `ActiveRecording&` (or remove debug groups during non-recording phases)

**Files Modified:**
- `code/graphics/vulkan/VulkanRenderer.h` - Remove RecordingState hierarchy, add ActiveRecording
- `code/graphics/vulkan/VulkanRenderer.cpp` - Refactor flip(), remove recordingFrame()
- `code/graphics/vulkan/VulkanGraphics.cpp` - Update all callers to pass ActiveRecording&
- `code/graphics/2d.cpp` - Update gr_flip() to hold ActiveRecording scope
- `code/graphics/util/UniformBufferManager.cpp` - Remove GR_DEBUG_SCOPE from onFrameEnd() (no recording available)

**Dependencies:** None (foundational change)

**Breaking Changes:**
- `recordingFrame()` method removed - replaced with parameter passing
- `flip()` now returns a value that must be held
- Debug scopes cannot be used during uniform buffer retirement (before flip)

---

## Phase 2: Frame Pool State Machine

### Problem

**Current Violation:** Runtime polymorphism with `WarmupFramePool`/`SteadyFramePool`
- `std::unique_ptr<FramePoolState> m_framePool` allows querying state
- `transitionIfNeeded()` uses runtime checks to transition states
- State transition is implicit, not explicit in function signature

**Location:** `VulkanRenderer.h:217-234`, `VulkanRenderer.cpp:336-362`

### Solution: Containers as State Machine

**New Design:**

```cpp
class VulkanRenderer {
private:
    // Containers ARE the state machine - no polymorphic state object
    std::deque<VulkanFrame*> m_availableFrames;    // Recordable state
    std::deque<InFlightFrame> m_inFlightFrames;    // Submitted state
    
    // Explicit state transitions - function signatures enforce state machine
    VulkanFrame& acquireFrameFromAvailable();      // Consumes available, produces recording
    InFlightFrame submitFrame(VulkanFrame& frame, uint32_t imageIndex); // Consumes recording, produces in-flight
    VulkanFrame* waitAndRecycle(InFlightFrame inflight); // Consumes in-flight, produces available
    
public:
    // flip() orchestrates the state transitions
    ActiveRecording flip();
};
```

**Implementation:**

```cpp
VulkanFrame& VulkanRenderer::acquireFrameFromAvailable() {
    Assertion(!m_availableFrames.empty(), "No available frames");
    VulkanFrame* frame = m_availableFrames.front();
    m_availableFrames.pop_front();
    prepareFrameForReuse(*frame, 0);
    return *frame;
}

InFlightFrame VulkanRenderer::submitFrame(VulkanFrame& frame, uint32_t imageIndex) {
    endFrame(frame, imageIndex);
    SubmitInfo info = submitRecordedFrame(frame, imageIndex);
    return InFlightFrame{ &frame, info };
}

VulkanFrame* VulkanRenderer::waitAndRecycle(InFlightFrame inflight) {
    prepareFrameForReuse(*inflight.frame, inflight.info.serial);
    return inflight.frame;
}

ActiveRecording VulkanRenderer::flip() {
    // Warmup: acquire from available pool
    VulkanFrame* frame = nullptr;
    if (!m_availableFrames.empty()) {
        frame = &acquireFrameFromAvailable();
    } else {
        // Steady: wait for in-flight frame
        Assertion(!m_inFlightFrames.empty(), "No frames available");
        InFlightFrame inflight = m_inFlightFrames.front();
        m_inFlightFrames.pop_front();
        frame = waitAndRecycle(inflight);
    }
    
    uint32_t imageIndex = acquireImage(*frame);
    beginFrame(*frame, imageIndex);
    return ActiveRecording(RecordingFrame{ *frame, imageIndex });
}
```

**Migration Strategy:**

1. **Step 2.1:** Remove `FramePoolState` hierarchy
2. **Step 2.2:** Add explicit transition functions
3. **Step 2.3:** Update `flip()` to use explicit transitions
4. **Step 2.4:** Remove `m_framePool` member

**Files Modified:**
- `code/graphics/vulkan/VulkanRenderer.h` - Remove FramePoolState hierarchy
- `code/graphics/vulkan/VulkanRenderer.cpp` - Add transition functions, refactor flip()

**Dependencies:** Phase 1 (requires ActiveRecording)

**Breaking Changes:** None (internal refactoring)

---

## Phase 3: Dynamic State Caching Elimination

### Problem

**Current Violation:** Cached dynamic state with dirty flags
- `m_depthTest`, `m_depthWrite`, `m_cullMode` cached in `VulkanRenderingSession`
- State is set conditionally based on cached values
- Violates "set state unconditionally when entering pass" principle

**Location:** `VulkanRenderingSession.h:154-156`, `VulkanRenderingSession.cpp`

### Solution: Bake State at Pass Entry

**New Design:**

```cpp
struct RasterState {
    vk::CullModeFlagBits cullMode;
    bool depthTest;
    bool depthWrite;
    // ... other raster state
};

class VulkanRenderingSession {
public:
    struct RenderScope {
        RenderTargetInfo info;
        RasterState rasterState;  // State baked at entry
        ActivePass guard;
    };
    
    RenderScope beginRendering(vk::CommandBuffer cmd, uint32_t imageIndex, const RasterState& state);
    
private:
    void applyRasterState(vk::CommandBuffer cmd, const RasterState& state);
    // NO cached state members
};
```

**Implementation:**

```cpp
RenderScope VulkanRenderingSession::beginRendering(
    vk::CommandBuffer cmd,
    uint32_t imageIndex,
    const RasterState& state) {
    
    RenderTargetInfo info = m_target->info(m_device, m_targets);
    
    // Apply state unconditionally - no caching, no checks
    applyRasterState(cmd, state);
    
    // Begin rendering
    m_target->begin(*this, cmd, imageIndex);
    
    return RenderScope{ info, state, ActivePass(cmd) };
}

void VulkanRenderingSession::applyRasterState(vk::CommandBuffer cmd, const RasterState& state) {
    cmd.setCullMode(state.cullMode);
    if (m_device.supportsExtendedDynamicState3()) {
        cmd.setDepthTestEnableEXT(state.depthTest);
        cmd.setDepthWriteEnableEXT(state.depthWrite);
    }
    // ... apply all state unconditionally
}
```

**Migration Strategy:**

1. **Step 3.1:** Create `RasterState` struct
2. **Step 3.2:** Remove cached state members from `VulkanRenderingSession`
3. **Step 3.3:** Update `beginRendering()` to take `RasterState` parameter
4. **Step 3.4:** Update all callers to construct `RasterState` at call site
5. **Step 3.5:** Remove all conditional state setting logic

**Files Modified:**
- `code/graphics/vulkan/VulkanRenderingSession.h` - Add RasterState, remove cached members
- `code/graphics/vulkan/VulkanRenderingSession.cpp` - Refactor beginRendering()
- `code/graphics/vulkan/VulkanGraphics.cpp` - Update callers to pass RasterState
- `code/graphics/vulkan/VulkanPipelineManager.cpp` - May need RasterState for pipeline key

**Dependencies:** None (independent)

**Breaking Changes:**
- `beginRendering()` signature changes to require RasterState
- Callers must construct RasterState explicitly

---

## Phase 4: Sentinel Value Elimination

### Problem

**Current Violation:** Sentinel values for "absent" state
- `MODEL_OFFSET_ABSENT = 0xFFFFFFFF` used throughout
- `gr_buffer_handle::invalid()` used for unbound uniforms
- `arrayIndex = MODEL_OFFSET_ABSENT` for unbound textures

**Location:** 
- `VulkanModelTypes.h:10` - MODEL_OFFSET_ABSENT
- `VulkanFrame.h:57-58` - invalid handles
- `VulkanTextureManager.h:109` - MODEL_OFFSET_ABSENT for arrayIndex

### Solution: Separate Types for Present/Absent

**New Design:**

```cpp
// Vertex attribute offsets - present or absent
struct VertexAttributeOffset {
    uint32_t value;
    
    static VertexAttributeOffset present(uint32_t offset) {
        Assertion(offset != 0xFFFFFFFF, "Invalid offset");
        return VertexAttributeOffset{ offset };
    }
    
    static VertexAttributeOffset absent() {
        return VertexAttributeOffset{ 0xFFFFFFFF };
    }
    
    bool isPresent() const { return value != 0xFFFFFFFF; }
    uint32_t get() const {
        Assertion(isPresent(), "Accessing absent offset");
        return value;
    }
};

// Uniform bindings - present or absent
struct ModelUniformBinding {
    gr_buffer_handle handle;
    uint32_t offset;
    
    // Cannot be default-constructed - must be explicitly created
    ModelUniformBinding(gr_buffer_handle h, uint32_t o) : handle(h), offset(o) {
        Assertion(handle.isValid(), "Invalid handle");
    }
};

// Frame does NOT store unbound state - bindings exist only when bound
class VulkanFrame {
public:
    // NO binding fields - bindings are managed externally
    // When a binding is needed, it's passed as a parameter to rendering functions
    
private:
    // Internal: track what's currently bound for descriptor updates
    // But this is implementation detail, not part of public API
    gr_buffer_handle m_boundModelBuffer{};
    uint32_t m_boundModelOffset = 0;
    
public:
    // Binding is set via explicit function, not stored as nullable member
    void setModelUniformBinding(gr_buffer_handle handle, uint32_t offset);
    bool hasModelUniformBinding() const { return m_boundModelBuffer.isValid(); }
    ModelUniformBinding getModelUniformBinding() const {
        Assertion(hasModelUniformBinding(), "No model uniform binding");
        return ModelUniformBinding(m_boundModelBuffer, m_boundModelOffset);
    }
    
    void resetPerFrameBindings() {
        m_boundModelBuffer = gr_buffer_handle::invalid();
        m_boundModelOffset = 0;
    }
};
```

**Shader Interface:**

The shader already uses `OFFSET_ABSENT = 0xFFFFFFFF`, so we keep that for GPU communication, but the C++ type system enforces correctness:

```cpp
struct ModelPushConstants {
    uint32_t vertexOffset;
    uint32_t stride;
    VertexAttributeOffset posOffset;      // Type-safe wrapper
    VertexAttributeOffset normalOffset;
    // ...
    
    // Conversion to GPU format
    void writeToGPU(uint32_t* dest) const {
        dest[0] = vertexOffset;
        dest[1] = stride;
        dest[2] = posOffset.isPresent() ? posOffset.get() : MODEL_OFFSET_ABSENT;
        // ...
    }
};
```

**Migration Strategy:**

1. **Step 4.1:** Create `VertexAttributeOffset` wrapper type
2. **Step 4.2:** Create `ModelUniformBinding` type (non-default-constructible)
3. **Step 4.3:** Update `VulkanFrame` to remove public binding fields, add explicit setter/getter
4. **Step 4.4:** Update all code that checks for sentinels to use type methods or `hasModelUniformBinding()`
5. **Step 4.5:** Update shader interface code to convert types to GPU format
6. **Step 4.6:** Update rendering code to pass bindings as parameters instead of querying frame state

**Files Modified:**
- `code/graphics/vulkan/VulkanModelTypes.h` - Add VertexAttributeOffset type
- `code/graphics/vulkan/VulkanFrame.h` - Use optional bindings
- `code/graphics/vulkan/VulkanRenderer.cpp` - Update binding code
- `code/graphics/vulkan/VulkanGraphics.cpp` - Update model rendering code
- `code/graphics/vulkan/VulkanTextureManager.h` - Use optional for arrayIndex

**Dependencies:** None (independent)

**Breaking Changes:**
- `modelUniformBinding` field removed - use `setModelUniformBinding()` and `getModelUniformBinding()` instead
- Rendering functions must receive bindings as parameters rather than querying frame
- Shader interface conversion needed

---

## Phase 5: Texture State Machine

### Problem

**Current Violation:** Enum-based state with runtime checks
- `TextureState` enum: Missing, Queued, Uploading, Resident, Failed, Retired
- Runtime checks like `if (record.state == TextureState::Resident)`
- State transitions scattered across codebase

**Location:** `VulkanTextureManager.h:99-106`, `VulkanTextureManager.cpp`

### Solution: Containers as State Machine

**New Design:**

```cpp
class VulkanTextureManager {
private:
    // Containers ARE the state machine
    std::unordered_map<int, VulkanTexture> m_residentTextures;      // Resident state
    std::vector<int> m_queuedUploads;                                // Queued state
    std::unordered_map<int, PendingUpload> m_uploadingTextures;     // Uploading state
    std::vector<RetiredTexture> m_retiredTextures;                   // Retired state
    
    // Explicit state transitions
    void queueUpload(int bitmapHandle);
    PendingUpload startUpload(int bitmapHandle, VulkanFrame& frame);
    void completeUpload(PendingUpload upload, uint32_t frameIndex);
    void retireTexture(int bitmapHandle, uint64_t serial);
    void destroyRetiredTexture(RetiredTexture retired, uint64_t completedSerial);
    
public:
    // Queries use container membership, not enum checks
    bool isResident(int bitmapHandle) const {
        return m_residentTextures.find(bitmapHandle) != m_residentTextures.end();
    }
    
    VulkanTexture& getResidentTexture(int bitmapHandle) {
        auto it = m_residentTextures.find(bitmapHandle);
        Assertion(it != m_residentTextures.end(), "Texture not resident");
        return it->second;
    }
};
```

**Migration Strategy:**

1. **Step 5.1:** Create separate containers for each state
2. **Step 5.2:** Add explicit transition functions
3. **Step 5.3:** Replace all enum checks with container membership checks
4. **Step 5.4:** Remove `TextureState` enum and `state` field from `TextureRecord`

**Files Modified:**
- `code/graphics/vulkan/VulkanTextureManager.h` - Replace enum with containers
- `code/graphics/vulkan/VulkanTextureManager.cpp` - Refactor state transitions

**Dependencies:** None (independent)

**Breaking Changes:**
- `TextureRecord::state` removed - use container membership instead

---

## Phase 6: Uniform Buffer Binding State

### Problem

**Current Violation:** Nullable bindings with sentinel handles
- `DynamicUniformBinding{ gr_buffer_handle::invalid(), 0 }` represents unbound state
- Runtime checks for `handle.isValid()`

**Location:** `VulkanFrame.h:57-58`

### Solution: Optional Bindings (Already Addressed in Phase 4)

This is covered by Phase 4's `std::optional<ModelUniformBinding>` approach. No additional work needed.

---

## Implementation Priority

### Critical Path (Must Fix First)
1. **Phase 1: Frame Recording State** - Blocks debug groups, affects all rendering code
2. **Phase 2: Frame Pool State** - Depends on Phase 1

### High Priority (Affects Correctness)
3. **Phase 3: Dynamic State Caching** - Can cause rendering bugs
4. **Phase 4: Sentinel Values** - Code clarity and type safety

### Medium Priority (Code Quality)
5. **Phase 5: Texture State Machine** - Improves maintainability

### Low Priority (Already Addressed)
6. **Phase 6: Uniform Buffer Binding** - Covered by Phase 4 (no std::optional, use explicit setters/getters)

---

## Testing Strategy

### Unit Tests
- Each phase should include unit tests for state transitions
- Test that invalid states are unrepresentable (compile-time checks)
- Test that valid transitions work correctly

### Integration Tests
- Test full frame lifecycle (flip → render → submit)
- Test texture upload lifecycle
- Test uniform binding lifecycle

### Regression Tests
- Ensure existing rendering still works after each phase
- Performance benchmarks to ensure no regressions

---

## Migration Notes

### Backward Compatibility
- Each phase can be implemented incrementally
- Old code paths can coexist during migration
- Use feature flags if needed for gradual rollout

### Code Review Checklist
For each phase:
- [ ] No `dynamic_cast` for state checking
- [ ] No `std::optional` for state representation (except Phase 4 where appropriate)
- [ ] No `if (state == X)` runtime checks
- [ ] State transitions are explicit function signatures
- [ ] Invalid states are unrepresentable (won't compile)
- [ ] RAII guards used for scoped state

---

## Success Criteria

After all phases:
1. ✅ No runtime polymorphism for state machines
2. ✅ No sentinel values (except GPU communication layer)
3. ✅ No dynamic state caching
4. ✅ No nullable state members
5. ✅ All state transitions are explicit function signatures
6. ✅ Invalid states are compile-time errors, not runtime exceptions
7. ✅ Scope-based RAII for all temporary state

---

## Appendix: Example Migration

### Before (Phase 1)
```cpp
void VulkanRenderer::flip() {
    m_recording->finishAndSubmit(*this);
    VulkanFrame& frame = m_framePool->acquireFrame(*this);
    m_recording = std::make_unique<ActiveRecording>(...);
}

VulkanFrame& VulkanRenderer::recordingFrame() {
    return m_recording->frame(*this);  // Can throw if NoRecording
}
```

### After (Phase 1)
```cpp
ActiveRecording VulkanRenderer::flip() {
    // Finish previous recording if it exists (handled by ActiveRecording destructor)
    VulkanFrame& frame = acquireFrameFromAvailable();
    uint32_t imageIndex = acquireImage(frame);
    beginFrame(frame, imageIndex);
    return ActiveRecording(RecordingFrame{ frame, imageIndex });
}

// Caller code:
void renderFrame() {
    auto recording = renderer.flip();  // Owns the recording
    renderStuff(recording);            // Pass by reference
}  // recording destructor submits frame
```

---

## Questions & Decisions Needed

1. **Debug Groups During Non-Recording:** Should we remove `GR_DEBUG_SCOPE` from `UniformBufferManager::onFrameEnd()` entirely, or create a separate debug scope system that doesn't require a recording frame?

2. **Texture Array Index:** Should `arrayIndex` remain as `uint32_t` with sentinel, or become `std::optional<uint32_t>`? (Note: violates "no std::optional" principle, but may be acceptable for GPU resource indices)

3. **Migration Timeline:** Should phases be implemented sequentially or can some run in parallel? (Recommendation: Sequential for Phase 1-2, parallel for Phase 3-5)

4. **Performance Impact:** Will removing dynamic state caching cause performance regressions? (Likely minimal - state is set once per pass, not per draw)

