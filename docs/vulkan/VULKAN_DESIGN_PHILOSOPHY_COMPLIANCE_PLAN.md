# Vulkan Pipeline Design Philosophy Compliance Plan

## Summary

Architectural redesign of the Vulkan renderer to eliminate design philosophy violations. The core changes center on:

1. **Texture ownership model**: "A texture either exists and is usable, or it doesn't exist"
2. **Bindless slot allocation**: Move to well-defined phase, eliminate retry loops
3. **Capability token extension**: Add tokens where phase violations are possible
4. **Accepted approximations**: Document Vulkan API constraints that require step-coupling

---

## Violations Identified

| Category | Location | Severity | Resolution |
|----------|----------|----------|------------|
| UnavailableTexture | VulkanTextureManager.h:105-121 | Critical | Eliminate - failure is absence |
| Retry loop | VulkanTextureManager.cpp:1256-1272 | Critical | Phase-based slot allocation |
| Buffer ops without token | VulkanRenderer.cpp:2698-2721 | Critical | Add capability tokens |
| Descriptor updates without token | VulkanRenderer.cpp:2908-2972 | Critical | Add capability tokens |
| Step-coupling | VulkanDevice, VulkanRenderer | Accepted | Document as Vulkan API requirement |
| Boolean state flags | Various locations | Low | Evaluate per-flag |

---

## Part 1: Texture Ownership Redesign

### Current State
```
Containers:
  m_residentTextures: map<int, ResidentTexture>       - GPU-resident
  m_unavailableTextures: map<int, UnavailableTexture> - "failed" textures with reason
  m_pendingUploads: vector<int>                       - queued for upload
  m_pendingBindlessSlots: set<int>                    - waiting for slot retry
  m_bindlessSlots: map<int, uint32_t>                 - handle -> slot
```

### Problems
- `UnavailableTexture` represents "exists but unusable" - violates "failure is absence"
- `UnavailableReason` enum caches failure state - state as data, not location
- `retryPendingBindlessSlots()` exists because slot allocation can fail mid-render

### Target State
```
Containers:
  m_residentTextures: map<int, ResidentTexture>  - GPU-resident (owns resources)
  m_pendingUploads: vector<int>                  - queued for upload
  m_bindlessSlots: map<int, uint32_t>            - handle -> slot
  m_permanentlyFailed: set<int>                  - known failures (no struct, no reason)
  m_freeBindlessSlots: vector<uint32_t>          - available slots

REMOVED:
  - m_unavailableTextures (struct with enum reason)
  - m_pendingBindlessSlots (retry mechanism)
  - UnavailableReason enum
```

### Design Principles

1. **Failure is absence**: A texture either exists in `m_residentTextures` or doesn't exist
2. **State as location**: Container membership defines state, no enum flags
3. **Single phase for slot allocation**: All slots assigned during upload phase
4. **Rendering is lookup-only**: `getBindlessSlotIndex()` becomes `const`, never mutates

### State Transitions
```
                    queueTextureUpload(handle)
    [absent] ────────────────────────────────────────► [queued]
        ▲                                                  │
        │ permanent                                        │ flushPendingUploads(UploadCtx)
        │ failure                                          ▼
        │                                              [resident]
    [failed]◄─────────────────────────────────────────     │
        │                                                  │ assignBindlessSlots()
        │ releaseBitmap()                                  ▼
        └─────────────────────────────────────────────[resident + slotted]
                                                           │
                                                           │ retireTexture()
                                                           ▼
                                                       [absent]
```

### Failure Classification

| Failure | Kind | Action |
|---------|------|--------|
| `bm_get_base_frame() < 0` | Boundary | Reject at queue time |
| `bm_lock() == nullptr` | Transient | Log, don't track (retry next frame if re-queued) |
| Image too large for staging | Permanent | Log, add to `m_permanentlyFailed` |
| Unsupported format | Permanent | Log, add to `m_permanentlyFailed` |
| VkResult OOM | Fatal | Return false, abort preloading |

### Implementation Steps

1. **Remove `UnavailableTexture` struct and `UnavailableReason` enum** from header
2. **Add `std::unordered_set<int> m_permanentlyFailed`** - tracks handles that will never succeed
3. **Update `queueTextureUpload()`** - reject permanently failed handles at boundary
4. **Refactor `flushPendingUploads()`**:
   - Classify failures as permanent vs transient
   - Permanent: add to `m_permanentlyFailed`, log once
   - Transient: just log, don't track
5. **Update `releaseBitmap()`** - also erase from `m_permanentlyFailed`
6. **Remove all `m_unavailableTextures` references** throughout codebase

---

## Part 2: Bindless Slot Allocation Redesign

### Root Cause Analysis

**Current Flow (problematic):**
1. Frame start: `retryPendingBindlessSlots()` with `allowResidentEvict=true`
2. During rendering: `getBindlessSlotIndex()` -> `tryAssignBindlessSlot(allowResidentEvict=false)`
3. If all slots full during rendering: add to `m_pendingBindlessSlots`, return fallback
4. Next frame: retry at step 1

**The disagreement:** Upload phase thinks slots are available for eviction. Rendering phase discovers they're not. Two components disagree about *when* slot assignment should happen.

### Target Design: Phase-Strict Slot Assignment

**Key invariant:** Slot allocation only happens during upload phase. Rendering is lookup-only.

```
┌─────────────────────────────────────────────────────────────────────┐
│                     UPLOAD PHASE ONLY                               │
│  flushPendingUploads():                                             │
│    1. processPendingRetirements()  <- frees slots                   │
│    2. Upload queued textures -> m_residentTextures                  │
│    3. assignBindlessSlots() <- assigns slots (with eviction)        │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                     RENDERING PHASE                                 │
│  getBindlessSlotIndex(handle) const:                                │
│    has_slot(handle) -> return slot                                  │
│    no_slot(handle)  -> return FALLBACK, queue for next frame        │
│                                                                     │
│  (No slot assignment, no eviction, no retry, method is CONST)       │
└─────────────────────────────────────────────────────────────────────┘
```

### New Function Signatures

```cpp
// Private: only called from flushPendingUploads()
void assignBindlessSlots();
std::optional<uint32_t> acquireBindlessSlot(int forHandle);
int findEvictionCandidate() const;

// Public: lookup-only during rendering
uint32_t getBindlessSlotIndex(int textureHandle) const;  // NOW CONST
```

### Implementation Steps

1. **Remove `m_pendingBindlessSlots`** container entirely
2. **Remove `retryPendingBindlessSlots()`** function
3. **Remove `tryAssignBindlessSlot()`** function (replaced by `assignBindlessSlots`)
4. **Add `assignBindlessSlots()`** - called at end of `flushPendingUploads()`:
   - Iterates all resident textures without slots
   - Assigns slots with LRU eviction allowed
   - No retry - if no slot available, texture remains unslotted this frame
5. **Simplify `getBindlessSlotIndex()`**:
   - Remove `tryAssignBindlessSlot()` call
   - Remove `m_pendingBindlessSlots.insert()`
   - Make method `const`
   - Just lookup and return slot or fallback

### One-Frame Delay Behavior

A texture requested mid-frame but not yet resident:
- **Frame N**: Queued for upload, returns fallback (slot 0)
- **Frame N+1**: Uploaded, slot assigned, returns actual slot

This is acceptable - visual pop-in for one frame is better than complex retry logic.

---

## Part 3: Capability Token Extension

### Principle

Tokens aren't a feature to scope - they're a technique applied wherever phase violations are possible. Find sites where code can execute in wrong phase, add tokens.

### Identified Gaps (from exploration)

| Operation | Current | Phase Required | Token Needed |
|-----------|---------|----------------|--------------|
| `updateBufferData()` | No token | Recording | `FrameCtx` |
| `mapBuffer()` | No token | Recording | `FrameCtx` |
| `flushMappedBuffer()` | No token | Recording | `FrameCtx` |
| `setModelUniformBinding()` | `VulkanFrame&` | Recording | `FrameCtx` |
| `setSceneUniformBinding()` | `VulkanFrame&` | Recording | `FrameCtx` |
| `bindDeferredGlobalDescriptors()` | No params | Recording | `RecordingFrame&` |
| `requestSwapchainTarget()` | No token | Recording | `FrameCtx` |
| `requestSceneHdrTarget()` | No token | Recording | `FrameCtx` |
| `requestPostLdrTarget()` | No token | Recording | `FrameCtx` |
| `createBitmapRenderTarget()` | No token | Init | `InitCtx` |

### Implementation Steps

1. **Buffer operations** (`VulkanRenderer.cpp:2698-2721`):
   - Add `const FrameCtx&` parameter to `updateBufferData()`, `mapBuffer()`, `flushMappedBuffer()`
   - Update `VulkanBufferManager` delegation if needed
   - Update all call sites in `VulkanGraphics.cpp`

2. **Uniform binding** (`VulkanRenderer.cpp:2908-2972`):
   - Change `VulkanFrame&` -> `const FrameCtx&`
   - Remove direct frame access, use `ctx.frame()` instead

3. **Descriptor binding** (`VulkanRenderer.cpp:1101-1187`):
   - Add `RecordingFrame&` parameter to `bindDeferredGlobalDescriptors()`
   - Verify it's only called from `deferredLightingFinish()` which has the token

4. **Render target requests** (`VulkanRenderingSession.h:37-51`):
   - Add `const FrameCtx&` to all `request*Target()` methods
   - Validate frame is recording before allowing target switch

5. **Update bridge functions** (`VulkanGraphics.cpp`):
   - Create tokens at boundary using `currentFrameCtx()`
   - Pass to internal methods

### Files to Modify
- `code/graphics/vulkan/VulkanRenderer.h` - add token parameters
- `code/graphics/vulkan/VulkanRenderer.cpp` - update implementations
- `code/graphics/vulkan/VulkanRenderingSession.h` - add token parameters
- `code/graphics/vulkan/VulkanRenderingSession.cpp` - validate tokens
- `code/graphics/vulkan/VulkanGraphics.cpp` - update bridge functions

---

## Part 4: Accepted Approximations (Documentation)

### Vulkan API Constraints
The following step-coupling is **required by Vulkan's architecture**:

1. **VulkanDevice::initialize()/shutdown()**
   - OS window/surface must exist before Vulkan instance creation
   - Physical device selection requires surface capabilities
   - This is fundamental to Vulkan, not a design flaw

2. **VulkanRenderer::initialize()/shutdown()**
   - Managers depend on device handles
   - Dependency chain requires sequential initialization
   - InitCtx token already enforces phase boundary

3. **VulkanMovieManager::m_available flag**
   - Graceful degradation for unsupported hardware
   - Alternative would be to throw, breaking gameplay

### Documentation Updates
- Add comments in VulkanDevice.h explaining why two-phase init exists
- Add comments in VulkanRenderer.h explaining dependency chain
- Update VULKAN_ARCHITECTURE.md with "Accepted Approximations" section

---

## Implementation Order

1. **Phase 1**: Texture ownership redesign (Part 1 + Part 2)
   - Highest impact, breaks dependencies on UnavailableTexture
   - Eliminates retry loop which is deeply embedded

2. **Phase 2**: Capability token extension (Part 3)
   - Can be done incrementally
   - Each token addition is independent

3. **Phase 3**: Documentation (Part 4)
   - Low effort, high clarity value
   - Do alongside implementation

---

## Files to Modify

### Critical (Texture System)
- `code/graphics/vulkan/VulkanTextureManager.h`
- `code/graphics/vulkan/VulkanTextureManager.cpp`

### Critical (Capability Tokens)
- `code/graphics/vulkan/VulkanRenderer.h`
- `code/graphics/vulkan/VulkanRenderer.cpp`
- `code/graphics/vulkan/VulkanRenderingSession.h`
- `code/graphics/vulkan/VulkanRenderingSession.cpp`
- `code/graphics/vulkan/VulkanGraphics.cpp`

### Documentation
- `docs/vulkan/VULKAN_ARCHITECTURE.md`
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md`
- `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md`

---

## Implementation Phases

### Phase 1: Texture Ownership (Parts 1 + 2)
**Scope:** VulkanTextureManager redesign
**Files:**
- `code/graphics/vulkan/VulkanTextureManager.h`
- `code/graphics/vulkan/VulkanTextureManager.cpp`

**Changes:**
1. Remove `UnavailableTexture`, `UnavailableReason`, `m_unavailableTextures`
2. Remove `m_pendingBindlessSlots`, `retryPendingBindlessSlots()`, `tryAssignBindlessSlot()`
3. Add `m_permanentlyFailed: set<int>`
4. Add `assignBindlessSlots()` called at end of `flushPendingUploads()`
5. Make `getBindlessSlotIndex()` const and lookup-only
6. Classify failures as permanent vs transient

**Testing:** Verify texture loading, fallback behavior, slot assignment timing

### Phase 2: Capability Tokens (Part 3)
**Scope:** Add phase tokens to operations that lack them
**Files:**
- `code/graphics/vulkan/VulkanRenderer.h`
- `code/graphics/vulkan/VulkanRenderer.cpp`
- `code/graphics/vulkan/VulkanRenderingSession.h`
- `code/graphics/vulkan/VulkanRenderingSession.cpp`
- `code/graphics/vulkan/VulkanGraphics.cpp`

**Changes:**
1. Add `FrameCtx` to buffer operations
2. Add `FrameCtx` to uniform binding operations
3. Add `RecordingFrame&` to descriptor binding
4. Add `FrameCtx` to render target requests
5. Update all call sites

**Testing:** Compile-time verification; if it compiles, phase violations are prevented

### Phase 3: Documentation (Part 4)
**Scope:** Document accepted approximations
**Files:**
- `docs/vulkan/VULKAN_ARCHITECTURE.md`
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md`
- `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md`

**Changes:**
1. Add "Accepted Approximations" section to architecture doc
2. Update texture residency doc with new state machine
3. Update capability tokens doc with new token requirements

---

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Breaking texture loading | Incremental testing with debug logging |
| Visual regression (pop-in) | One-frame delay is acceptable, document behavior |
| Compile errors at call sites | Systematic update of all callers |
| Performance regression | Slot assignment once per frame is cheaper than retry per texture |

---

## Success Criteria

1. **No retry loops**: `retryPendingBindlessSlots()` removed, `m_pendingBindlessSlots` removed
2. **No failure objects**: `UnavailableTexture` removed, `UnavailableReason` removed
3. **Phase-enforced operations**: All identified gaps have capability token parameters
4. **Const correctness**: `getBindlessSlotIndex()` is const
5. **Documentation**: Accepted approximations clearly documented
