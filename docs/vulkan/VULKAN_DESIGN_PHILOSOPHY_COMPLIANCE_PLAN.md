# Vulkan Pipeline Design Philosophy Compliance Plan

**Status**: Draft (Planning Document)
**Last Updated**: 2025-12
**Last Verified Against Code**: 2025-12

**Related Documents**:
- `docs/DESIGN_PHILOSOPHY.md` - Core principles
- `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md` - Capability token implementation guide
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md` - Current texture state machine
- `docs/vulkan/VULKAN_ARCHITECTURE.md` - Renderer architecture overview

---

## Document Purpose

This document is a **planning document** that specifies proposed architectural changes to eliminate design philosophy violations in the Vulkan renderer. It does **not** describe the current implementation; for that, see the texture residency and capability token documents listed above.

The changes described here are **not yet implemented**. This plan serves as a roadmap for future refactoring work.

---

## Table of Contents

1. [Introduction](#introduction)
2. [Background and Motivation](#background-and-motivation)
3. [Current Violations Summary](#current-violations-summary)
4. [Compliance Status](#compliance-status)
5. [Part 1: Texture Ownership Redesign](#part-1-texture-ownership-redesign)
6. [Part 2: Bindless Slot Allocation Redesign](#part-2-bindless-slot-allocation-redesign)
7. [Part 3: Capability Token Extension](#part-3-capability-token-extension)
8. [Part 4: Accepted Approximations](#part-4-accepted-approximations)
9. [Implementation Plan](#implementation-plan)
10. [Risk Assessment and Mitigation](#risk-assessment-and-mitigation)
11. [Success Criteria](#success-criteria)
12. [Glossary](#glossary)

---

## Introduction

This document specifies an architectural redesign of the Vulkan renderer to eliminate violations of the project's type-driven design philosophy. The redesign targets four areas where the current implementation relies on patterns that the design philosophy explicitly prohibits:

1. **Texture ownership model**: Replace `UnavailableTexture` with "failure is absence" semantics
2. **Bindless slot allocation**: Eliminate retry loops via phase-strict slot assignment
3. **Capability token coverage**: Extend tokens to operations currently lacking phase enforcement
4. **Accepted approximations**: Document Vulkan API constraints that require deviations

The goal is a texture and rendering system where invalid states are unrepresentable and invalid call sequences are compile-time errors.

---

## Background and Motivation

### Design Philosophy Principles

The project follows type-driven design principles documented in `docs/DESIGN_PHILOSOPHY.md`. Key principles relevant to this plan:

| Principle | Description | Current Violation |
|-----------|-------------|-------------------|
| **Failure is absence** | Failed objects should not exist; use absence instead of `Failed` enum variants | `UnavailableTexture` struct exists to represent "failed" textures |
| **State as location** | Container membership defines state, not enum flags | `UnavailableReason` enum caches failure state |
| **No retry loops** | Retry loops indicate ownership disagreement between components | `retryPendingBindlessSlots()` exists because slot allocation can fail mid-render |
| **Capability tokens** | Operations valid only in specific phases require proof tokens | Several buffer and descriptor operations lack phase tokens |

### Why This Matters

The current violations create runtime failure modes that could be compile-time errors:

- A texture can exist in `m_unavailableTextures` with a `reason` enum, violating "failure is absence"
- Slot allocation can fail during rendering, requiring retry logic that masks an ownership disagreement
- Buffer operations can be called outside their valid phase with no compile-time enforcement

---

## Current Violations Summary

This table lists design philosophy violations identified in the current codebase. Line numbers are approximate and may drift as the code evolves.

| Category | Location | Severity | Resolution | Status |
|----------|----------|----------|------------|--------|
| `UnavailableReason` enum | `VulkanTextureManager.h:105-111` | Critical | Eliminate - failure is absence | Pending |
| `UnavailableTexture` struct | `VulkanTextureManager.h:119-121` | Critical | Eliminate - failure is absence | Pending |
| `m_unavailableTextures` map | `VulkanTextureManager.h:261` | Critical | Replace with `set<int>` | Pending |
| Retry loop `retryPendingBindlessSlots()` | `VulkanTextureManager.cpp:1256-1272` | Critical | Phase-based slot allocation | Pending |
| `m_pendingBindlessSlots` set | `VulkanTextureManager.h:264` | Critical | Remove entirely | Pending |
| `getBindlessSlotIndex()` mutates state | `VulkanTextureManager.cpp:1482-1515` | High | Make method `const` | Pending |
| Buffer ops without token | `VulkanRenderer.cpp:2754-2777` | Medium | Add capability tokens | Pending |
| `setModelUniformBinding` uses `VulkanFrame&` | `VulkanRenderer.h:96-99` | Low | Upgrade to `FrameCtx` | Pending |
| Step-coupling in device/renderer init | `VulkanDevice.cpp`, `VulkanRenderer.cpp` | Accepted | Document as Vulkan API requirement | Documented |
| `m_available` flag in VulkanMovieManager | `VulkanMovieManager.h:109` | Accepted | Document as hardware capability | Documented |

---

## Compliance Status

### Capability Token Coverage

The renderer already has substantial capability token coverage. This section tracks what exists versus what is proposed.

**Already Implemented**:

| Token | Location | Purpose |
|-------|----------|---------|
| `FrameCtx` | `VulkanFrameCaps.h:13-27` | Active recording + renderer ref |
| `RenderCtx` | `VulkanPhaseContexts.h:36-48` | Dynamic rendering is active |
| `UploadCtx` | `VulkanPhaseContexts.h:15-32` | Upload phase is active |
| `DeferredGeometryCtx` | `VulkanPhaseContexts.h:51-62` | Deferred geometry pass active |
| `DeferredLightingCtx` | `VulkanPhaseContexts.h:64-75` | Deferred lighting pass active |
| `RecordingFrame` | `VulkanFrameFlow.h:18-37` | Frame is recording commands |
| `ModelBoundFrame` | `VulkanFrameCaps.h:29-48` | Model UBO is bound |
| `NanoVGBoundFrame` | `VulkanFrameCaps.h:50-62` | NanoVG UBO is bound |
| `DecalBoundFrame` | `VulkanFrameCaps.h:64-80` | Decal UBOs are bound |

**Proposed Extensions** (not yet implemented):

| Operation | Current Signature | Proposed Token |
|-----------|-------------------|----------------|
| `updateBufferData()` | No token | `const FrameCtx&` |
| `mapBuffer()` | No token | `const FrameCtx&` |
| `flushMappedBuffer()` | No token | `const FrameCtx&` |
| `setModelUniformBinding()` | `VulkanFrame&` | `const FrameCtx&` |
| `setSceneUniformBinding()` | `VulkanFrame&` | `const FrameCtx&` |

### Texture Ownership Redesign Status

The current texture manager uses `UnavailableTexture` struct with reason enum. The proposed redesign has **not been implemented**.

| Component | Current State | Target State | Status |
|-----------|---------------|--------------|--------|
| Failure tracking | `map<int, UnavailableTexture>` | `set<int>` (permanent failures only) | Pending |
| Retry mechanism | `retryPendingBindlessSlots()` | Remove entirely | Pending |
| Slot allocation timing | During rendering via `getBindlessSlotIndex()` | Upload phase only | Pending |
| `getBindlessSlotIndex()` | Non-const (mutates state) | Const (lookup only) | Pending |

---

## Part 1: Texture Ownership Redesign

### Problem Statement

The current texture manager uses an `UnavailableTexture` struct with an `UnavailableReason` enum to represent textures that failed to upload. This violates two design principles:

1. **"Failure is absence"**: A failed texture should not exist as an object; it should simply be absent from the resident container.
2. **"State as location"**: The `UnavailableReason` enum caches failure state as data rather than using container membership to express state.

### Current State

```
Containers:
    m_residentTextures      : map<int, ResidentTexture>       -- GPU-resident textures
    m_unavailableTextures   : map<int, UnavailableTexture>    -- Failed textures with reason enum
    m_pendingUploads        : vector<int>                     -- Queued for upload
    m_pendingBindlessSlots  : set<int>                        -- Waiting for slot retry
    m_bindlessSlots         : map<int, uint32_t>              -- handle -> slot mapping
```

**Problems**:
- `UnavailableTexture` represents "exists but unusable" - a state that should not exist
- `UnavailableReason` enum caches the specific failure - state as data, not location
- `retryPendingBindlessSlots()` exists because slot allocation can fail mid-render

### Target State

```
Containers:
    m_residentTextures      : map<int, ResidentTexture>       -- GPU-resident (owns resources)
    m_pendingUploads        : vector<int>                     -- Queued for upload
    m_bindlessSlots         : map<int, uint32_t>              -- handle -> slot mapping
    m_permanentlyFailed     : set<int>                        -- Known failures (handle only, no struct)
    m_freeBindlessSlots     : vector<uint32_t>                -- Available slots for assignment

REMOVED:
    m_unavailableTextures   -- Struct-based failure tracking
    m_pendingBindlessSlots  -- Retry mechanism container
    UnavailableReason       -- Failure reason enum
```

### Design Principles Applied

| Principle | Application |
|-----------|-------------|
| **Failure is absence** | A texture either exists in `m_residentTextures` or does not exist |
| **State as location** | Container membership defines state; no enum flags |
| **Single phase for slot allocation** | All slots assigned during upload phase, never during rendering |
| **Const rendering** | `getBindlessSlotIndex()` becomes `const`, never mutates state |

### State Machine

```
                        queueTextureUpload(handle)
    [Absent] ------------------------------------------------> [Queued]
        ^                                                          |
        |                                                          |
        | permanent failure                                        | flushPendingUploads(UploadCtx)
        | (logged once)                                            |
        |                                                          v
    [Failed] <------ upload failure ----------------------- [Uploading]
        ^                                                          |
        |                                                          | success
        | releaseBitmap()                                          v
        |                                                     [Resident]
        |                                                          |
        +---- releaseBitmap() ---------------------------------+   | assignBindlessSlots()
                                                               |   v
                                                               | [Resident + Slotted]
                                                               |   |
                                                               |   | retireTexture()
                                                               v   v
                                                             [Absent]
```

**State Definitions**:
- **Absent**: Handle not in any container; no resources allocated
- **Queued**: Handle in `m_pendingUploads`; awaiting upload
- **Resident**: Handle in `m_residentTextures`; GPU resources allocated
- **Resident + Slotted**: Resident and has bindless slot in `m_bindlessSlots`
- **Failed**: Handle in `m_permanentlyFailed`; will never succeed

### Failure Classification

Not all failures are permanent. The redesign classifies failures to determine the appropriate response:

| Failure Condition | Classification | Action |
|-------------------|----------------|--------|
| `bm_get_base_frame() < 0` | Boundary rejection | Reject at queue time; do not add to any container |
| `bm_lock() == nullptr` | Transient | Log warning; do not track (retry if re-queued next frame) |
| Image dimensions exceed staging buffer | Permanent | Add to `m_permanentlyFailed`; log once |
| Unsupported pixel format | Permanent | Add to `m_permanentlyFailed`; log once |
| VkResult out-of-memory | Fatal | Return false from upload; abort preloading |

**Key Insight**: Permanent failures are tracked by handle only (a `set<int>`), not by a struct with a reason. The reason is logged once at failure time; storing it serves no purpose.

### Implementation Steps

1. **Remove `UnavailableTexture` struct and `UnavailableReason` enum**
   - Delete struct definition from `VulkanTextureManager.h`
   - Delete enum definition from `VulkanTextureManager.h`

2. **Add `std::unordered_set<int> m_permanentlyFailed`**
   - Tracks handles that will never succeed
   - No reason stored; reason logged at failure time

3. **Update `queueTextureUpload()`**
   - Reject permanently failed handles at boundary
   - Return early with no side effects for failed handles

4. **Refactor `flushPendingUploads()`**
   - On failure: classify as permanent vs transient
   - Permanent: add to `m_permanentlyFailed`, log once with reason
   - Transient: log warning only, do not track

5. **Update `releaseBitmap()`**
   - Erase from `m_permanentlyFailed` in addition to other containers
   - Allows re-attempting upload after handle is recycled

6. **Remove all `m_unavailableTextures` references**
   - Search entire codebase for usages
   - Update callers to use `m_permanentlyFailed.count()` for failure checks

---

## Part 2: Bindless Slot Allocation Redesign

### Problem Statement

The current design allows bindless slot allocation during the rendering phase. When all slots are exhausted, the texture is added to `m_pendingBindlessSlots` for retry at the next frame start. This retry loop indicates an ownership disagreement: the upload phase believes slots are available, but the rendering phase discovers they are not.

### Root Cause Analysis

**Current Flow (problematic)**:

1. Frame start: `retryPendingBindlessSlots()` with `allowResidentEvict=true`
2. During rendering: `getBindlessSlotIndex()` calls `tryAssignBindlessSlot(allowResidentEvict=false)`
3. If all slots full: add to `m_pendingBindlessSlots`, return fallback slot
4. Next frame: retry at step 1

**The Disagreement**: The upload phase and rendering phase have different policies about slot eviction. This disagreement manifests as a retry loop - a pattern the design philosophy explicitly prohibits.

### Target Design: Phase-Strict Slot Assignment

**Key Invariant**: Slot allocation occurs only during the upload phase. Rendering is lookup-only.

```
+-----------------------------------------------------------------------+
|                         UPLOAD PHASE ONLY                             |
|                                                                       |
|  flushPendingUploads(UploadCtx):                                      |
|    1. processPendingRetirements()      -- Free slots from deletions   |
|    2. Upload queued textures           -- Add to m_residentTextures   |
|    3. assignBindlessSlots()            -- Assign slots (with eviction)|
|                                                                       |
+-----------------------------------------------------------------------+

+-----------------------------------------------------------------------+
|                         RENDERING PHASE                               |
|                                                                       |
|  getBindlessSlotIndex(handle) const:                                  |
|    - has_slot(handle) -> return slot                                  |
|    - no_slot(handle)  -> return FALLBACK_SLOT, queue for next frame   |
|                                                                       |
|  Properties:                                                          |
|    - Method is CONST                                                  |
|    - No slot assignment                                               |
|    - No eviction                                                      |
|    - No retry container                                               |
|                                                                       |
+-----------------------------------------------------------------------+
```

### New Function Signatures

```cpp
// Private: called only from flushPendingUploads()
void assignBindlessSlots();
std::optional<uint32_t> acquireBindlessSlot(int forHandle);
int findEvictionCandidate() const;

// Public: lookup-only during rendering
uint32_t getBindlessSlotIndex(int textureHandle) const;  // NOW CONST
```

**Critical Change**: `getBindlessSlotIndex()` becomes `const`. This compile-time constraint prevents accidental mutation during rendering.

### One-Frame Delay Behavior

A texture requested mid-frame but not yet resident experiences a one-frame delay:

| Frame | State | Return Value |
|-------|-------|--------------|
| N | Requested; queued for upload | Fallback slot (0) |
| N+1 | Uploaded; slot assigned | Actual slot |

**Rationale**: One frame of fallback texture (visual pop-in) is acceptable. The alternative - complex retry logic with eviction during rendering - violates design principles and creates subtle bugs.

### Implementation Steps

1. **Remove `m_pendingBindlessSlots` container**
   - Delete container declaration from header
   - Delete all insert/erase operations

2. **Remove `retryPendingBindlessSlots()` function**
   - Delete function declaration and definition
   - Remove call from `flushPendingUploads()`

3. **Remove `tryAssignBindlessSlot()` function**
   - Replaced by `assignBindlessSlots()` called at end of upload phase

4. **Add `assignBindlessSlots()` function**
   - Called at end of `flushPendingUploads()`
   - Iterates all resident textures without slots
   - Assigns slots with LRU eviction allowed
   - If no slot available: texture remains unslotted this frame (uses fallback)

5. **Simplify `getBindlessSlotIndex()`**
   - Remove `tryAssignBindlessSlot()` call
   - Remove `m_pendingBindlessSlots.insert()`
   - Mark method `const`
   - Pure lookup: return slot if assigned, fallback otherwise

---

## Part 3: Capability Token Extension

### Principle

Capability tokens are not a feature to scope narrowly - they are a technique applied wherever phase violations are possible. The goal is compile-time enforcement: if code lacks the required token, it cannot compile.

### Current Coverage

The Vulkan renderer already has extensive capability token coverage (see `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md` for the full guide). The tokens listed in the Compliance Status section above are already implemented and working.

### Identified Gaps

These operations can currently execute in the wrong phase because they lack token parameters:

| Operation | Current Signature | Required Phase | Proposed Token |
|-----------|-------------------|----------------|----------------|
| `updateBufferData()` | `gr_buffer_handle, size_t, const void*` | Recording | `const FrameCtx&` |
| `mapBuffer()` | `gr_buffer_handle` | Recording | `const FrameCtx&` |
| `flushMappedBuffer()` | `gr_buffer_handle, size_t, size_t` | Recording | `const FrameCtx&` |
| `setModelUniformBinding()` | `VulkanFrame&, ...` | Recording | `const FrameCtx&` |
| `setSceneUniformBinding()` | `VulkanFrame&, ...` | Recording | `const FrameCtx&` |

**Note**: Many operations that originally lacked tokens have been migrated. The render target request methods in `VulkanRenderingSession` are called internally by methods that already require `FrameCtx`, so they have implicit phase enforcement through the call chain.

### Implementation by Category

#### Buffer Operations (`VulkanRenderer.cpp:2754-2777`)

**Current** (from `VulkanRenderer.h:169-173`):
```cpp
void updateBufferData(gr_buffer_handle handle, size_t size, const void* data);
void updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data);
void resizeBuffer(gr_buffer_handle handle, size_t size);
void* mapBuffer(gr_buffer_handle handle);
void flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size);
```

**Proposed**:
```cpp
void updateBufferData(const FrameCtx& ctx, gr_buffer_handle handle, size_t size, const void* data);
void updateBufferDataOffset(const FrameCtx& ctx, gr_buffer_handle handle, size_t offset, size_t size, const void* data);
void resizeBuffer(const FrameCtx& ctx, gr_buffer_handle handle, size_t size);
void* mapBuffer(const FrameCtx& ctx, gr_buffer_handle handle);
void flushMappedBuffer(const FrameCtx& ctx, gr_buffer_handle handle, size_t offset, size_t size);
```

**Rationale**: Buffer operations may be used during command recording. The `FrameCtx` token proves recording is active.

**Consideration**: Some buffer operations (like initial buffer creation data) may legitimately occur outside frame recording. The implementation must evaluate whether these operations truly require phase enforcement or are phase-independent.

#### Uniform Binding (`VulkanRenderer.h:96-103`)

**Current**:
```cpp
void setModelUniformBinding(VulkanFrame& frame,
    gr_buffer_handle handle,
    size_t offset,
    size_t size);
void setSceneUniformBinding(VulkanFrame& frame,
    gr_buffer_handle handle,
    size_t offset,
    size_t size);
```

**Proposed**:
```cpp
void setModelUniformBinding(const FrameCtx& ctx,
    gr_buffer_handle handle,
    size_t offset,
    size_t size);
void setSceneUniformBinding(const FrameCtx& ctx,
    gr_buffer_handle handle,
    size_t offset,
    size_t size);
```

**Rationale**: Replace raw `VulkanFrame&` with `FrameCtx` which provides phase proof. Access frame via `ctx.frame()`. This is a minor upgrade since `VulkanFrame&` already implies a frame exists, but `FrameCtx` is the canonical proof token.

### Files to Modify

| File | Changes |
|------|---------|
| `code/graphics/vulkan/VulkanRenderer.h` | Add token parameters to function declarations |
| `code/graphics/vulkan/VulkanRenderer.cpp` | Update implementations to use token |
| `code/graphics/vulkan/VulkanGraphics.cpp` | Update bridge functions to create and pass tokens |

### Migration Pattern for Bridge Functions

Bridge functions in `VulkanGraphics.cpp` create tokens from global state. This pattern is already established for most rendering operations:

```cpp
// Existing pattern (already used throughout VulkanGraphics.cpp)
void gr_vulkan_some_operation(...)
{
    auto ctx = currentFrameCtx();  // Create token at boundary
    ctx.renderer.someOperation(ctx, ...);  // Pass to internal API
}
```

The `currentFrameCtx()` helper validates that recording is active and returns a `FrameCtx` token. This is documented in `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md` Section 4.2.

---

## Part 4: Accepted Approximations

### Principle

Some step-coupling is required by Vulkan's architecture and cannot be eliminated without abandoning Vulkan itself. These cases are documented as **accepted approximations** rather than violations.

### Vulkan API Constraints

#### VulkanDevice Initialization

**Pattern**: Two-phase init (`initialize()` / `shutdown()`)

**Why Required**:
- OS window/surface must exist before Vulkan instance creation
- Physical device selection requires surface capabilities query
- Logical device creation depends on physical device features

**This is fundamental to Vulkan, not a design flaw.** The WSI (Window System Integration) extension model requires surface existence before device creation.

**Documentation Update**: Add comment in `VulkanDevice.h`:
```cpp
// Two-phase initialization is required by Vulkan's WSI model:
// 1. Instance creation requires window surface for extension queries
// 2. Physical device selection requires surface for capability queries
// 3. Logical device creation requires physical device features
// This is not step-coupling in the design philosophy sense - it is
// fundamental to Vulkan's architecture.
```

#### VulkanRenderer Initialization

**Pattern**: Sequential manager initialization with dependency chain

**Why Required**:
- Managers depend on device handles
- Some managers depend on other managers (e.g., texture manager needs buffer manager)
- InitCtx token already enforces phase boundary

**Documentation Update**: Add comment in `VulkanRenderer.h`:
```cpp
// Manager initialization follows a dependency chain:
// Device -> BufferManager -> TextureManager -> PipelineManager -> ...
// This sequential init is encapsulated within initialize() and
// enforced by InitCtx token. Externally, the renderer is either
// initialized or not - there is no observable intermediate state.
```

#### VulkanMovieManager Availability Flag

**Pattern**: `m_available` boolean flag

**Why Required**:
- Movie playback requires specific hardware video decode support
- Not all GPUs support required video extensions
- Graceful degradation preserves gameplay on unsupported hardware

**Alternative Considered**: Throw exception if unsupported. Rejected because:
- Breaks gameplay on hardware that otherwise works
- Movies are optional enhancement, not core functionality

**Documentation Update**: Add comment in `VulkanMovieManager.h`:
```cpp
// m_available represents hardware capability, not internal state.
// This is boundary information (does this hardware support video decode?)
// not internal state routing. The flag is set once at init and never
// changes. This is acceptable under the "conditionals at boundaries" rule.
```

### Documentation Updates Required

1. **`docs/vulkan/VULKAN_ARCHITECTURE.md`**: Add "Accepted Approximations" section listing these cases
2. **Source files**: Add comments explaining why each approximation is acceptable
3. **`docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md`**: Update state machine diagram after Part 1 changes

---

## Implementation Plan

### Phase 1: Texture Ownership (Parts 1 + 2)

**Scope**: VulkanTextureManager redesign
**Estimated Effort**: High
**Risk Level**: High (affects all texture rendering)

**Primary Files**:
- `code/graphics/vulkan/VulkanTextureManager.h` - Container and struct definitions
- `code/graphics/vulkan/VulkanTextureManager.cpp` - State machine implementation

**Related Files** (may need updates):
- `code/graphics/vulkan/VulkanTextureBindings.h` - Uses `getBindlessSlotIndex()`
- `code/graphics/vulkan/VulkanRenderer.cpp` - Calls texture manager methods

**Changes**:
1. Remove `UnavailableTexture` (lines 119-121), `UnavailableReason` (lines 105-111), `m_unavailableTextures` (line 261)
2. Remove `m_pendingBindlessSlots` (line 264), `retryPendingBindlessSlots()` (lines 1256-1272), `tryAssignBindlessSlot()` (lines 1274+)
3. Add `std::unordered_set<int> m_permanentlyFailed` for tracking handles that will never succeed
4. Add `assignBindlessSlots()` called at end of `flushPendingUploads()`
5. Make `getBindlessSlotIndex()` const and lookup-only (currently mutates state at lines 1489-1495)
6. Classify failures as permanent vs transient (see Failure Classification table in Part 1)

**Testing Strategy**:
- Unit tests for state transitions
- Integration test: load mission with many textures
- Visual verification: textures appear correctly, no corruption
- Stress test: rapid texture load/unload cycles
- Verify one-frame fallback behavior is visually acceptable

**Rollback Plan**: Revert commits; no schema changes or persistent state affected

**Documentation Updates Required**:
- Update `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md` state machine diagram
- Remove references to `UnavailableTexture` from documentation

### Phase 2: Capability Tokens (Part 3)

**Scope**: Add phase tokens to operations lacking them
**Estimated Effort**: Medium
**Risk Level**: Low (compile-time verification)

**Primary Files**:
- `code/graphics/vulkan/VulkanRenderer.h` - Function declarations (lines 96-103, 169-173)
- `code/graphics/vulkan/VulkanRenderer.cpp` - Function implementations (lines 2754-2777)
- `code/graphics/vulkan/VulkanGraphics.cpp` - Bridge functions that create tokens

**Note**: Render target requests (`requestSwapchainTarget()`, etc.) in `VulkanRenderingSession` are called through `FrameCtx`-requiring methods, giving them implicit phase enforcement. These may not need explicit token parameters.

**Changes**:
1. Evaluate buffer operations: determine which truly require phase enforcement vs which are phase-independent
2. Add `const FrameCtx&` to buffer operations that require it
3. Change `VulkanFrame&` to `const FrameCtx&` in `setModelUniformBinding()` and `setSceneUniformBinding()`
4. Update all call sites in `VulkanGraphics.cpp` to use `currentFrameCtx()` pattern

**Testing Strategy**: If it compiles, phase violations are prevented. Run existing test suite.

**Rollback Plan**: Revert commits; signature changes are isolated

### Phase 3: Documentation (Part 4)

**Scope**: Document accepted approximations
**Estimated Effort**: Low
**Risk Level**: None

**Files**:
- `docs/vulkan/VULKAN_ARCHITECTURE.md`
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md`
- `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md`
- Source file comments in `VulkanDevice.h`, `VulkanRenderer.h`, `VulkanMovieManager.h`

**Changes**:
1. Add "Accepted Approximations" section to architecture doc
2. Update texture residency doc with new state machine after Phase 1
3. Update capability tokens doc with new token requirements after Phase 2
4. Add source code comments explaining approximations

**Testing Strategy**: Documentation review

### Recommended Order

1. **Phase 1** first: Highest impact, most complex, breaks existing dependencies
2. **Phase 2** second: Incremental, each token addition is independent
3. **Phase 3** concurrent: Update docs as implementation proceeds

---

## Risk Assessment and Mitigation

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Texture loading breaks** | Medium | High | Incremental testing; extensive debug logging; feature branch with frequent commits |
| **Visual regression (pop-in)** | High | Low | One-frame delay is acceptable; document expected behavior |
| **Compile errors cascade through codebase** | High | Medium | Systematic update of all callers; batch commits per file |
| **Performance regression** | Low | Medium | Slot assignment once per frame is cheaper than per-texture retry |
| **Subtle rendering bugs** | Medium | High | Run full test suite; visual comparison against master branch |
| **Merge conflicts with concurrent work** | Medium | Low | Coordinate with team; rebase frequently |

### Rollback Strategy

All changes are source-only with no persistent state or schema changes:

1. **Phase 1**: Single revert of texture manager commits restores previous behavior
2. **Phase 2**: Individual function signature changes can be reverted independently
3. **Phase 3**: Documentation changes have no runtime effect

---

## Success Criteria

### Quantitative

| Criterion | Metric | Target |
|-----------|--------|--------|
| Retry loops eliminated | Count of retry functions | 0 |
| Failure objects eliminated | Count of failure struct types | 0 |
| Const correctness | `getBindlessSlotIndex()` signature | `const` method |
| Token coverage | Operations lacking phase tokens | 0 (from identified list) |

### Qualitative

| Criterion | Verification Method |
|-----------|---------------------|
| No texture loading regressions | Visual comparison, automated tests |
| Fallback behavior works correctly | Load textures exceeding slot limit |
| Phase violations caught at compile time | Attempt invalid call sequence, verify compile error |
| Documentation accurately reflects implementation | Code review |

### Checklist

**Phase 1: Texture Ownership (Parts 1 + 2)**

- [ ] `UnavailableTexture` struct removed from `VulkanTextureManager.h`
- [ ] `UnavailableReason` enum removed from `VulkanTextureManager.h`
- [ ] `m_unavailableTextures` replaced with `std::unordered_set<int> m_permanentlyFailed`
- [ ] `retryPendingBindlessSlots()` function removed
- [ ] `m_pendingBindlessSlots` container removed
- [ ] `tryAssignBindlessSlot()` function removed or made private upload-only
- [ ] `assignBindlessSlots()` function added, called at end of `flushPendingUploads()`
- [ ] `getBindlessSlotIndex()` method signature is `const`
- [ ] `getBindlessSlotIndex()` implementation is pure lookup (no mutation)

**Phase 2: Capability Tokens (Part 3)**

- [ ] `updateBufferData()` requires `const FrameCtx&` (or determined phase-independent)
- [ ] `mapBuffer()` requires `const FrameCtx&` (or determined phase-independent)
- [ ] `flushMappedBuffer()` requires `const FrameCtx&` (or determined phase-independent)
- [ ] `setModelUniformBinding()` uses `const FrameCtx&` instead of `VulkanFrame&`
- [ ] `setSceneUniformBinding()` uses `const FrameCtx&` instead of `VulkanFrame&`

**Phase 3: Documentation (Part 4)**

- [x] Accepted approximations documented in this plan
- [ ] `VULKAN_ARCHITECTURE.md` updated with "Accepted Approximations" section
- [ ] `VULKAN_TEXTURE_RESIDENCY.md` updated after Phase 1 changes
- [ ] Source file comments added for VulkanDevice two-phase init
- [ ] Source file comments added for VulkanMovieManager availability flag

---

## Glossary

| Term | Definition |
|------|------------|
| **Capability token** | A move-only or restricted type that proves a specific phase or state is active. See `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md`. |
| **Failure is absence** | Design principle: failed objects should not exist as structs/records; absence from a container indicates failure. |
| **State as location** | Design principle: an object's state is determined by which container holds it, not by enum flags or status fields. |
| **Phase violation** | Calling an operation outside its valid phase (e.g., recording GPU commands before frame recording starts). |
| **Step-coupling** | Anti-pattern where operations must be called in a specific order but the type system does not enforce it. |
| **Typestate** | Pattern where type transitions encode valid state sequences (e.g., `Deck` -> `ShuffledDeck`). |
| **Bindless slot** | Index into descriptor array for GPU texture access without per-draw descriptor binding. |
| **Fallback texture** | 1x1 black texture (slot 0) returned when requested texture is unavailable. |
| **FrameCtx** | Capability token proving recording is active and providing renderer access. |
| **RecordingFrame** | Move-only token proving a frame is currently recording commands. |
| **UploadCtx** | Capability token proving upload phase is active. |
| **Resident texture** | Texture with GPU resources allocated, ready for sampling. |
| **Permanent failure** | Texture that will never successfully upload (e.g., unsupported format). |
| **Transient failure** | Temporary failure that may succeed on retry (e.g., staging buffer exhausted). |
