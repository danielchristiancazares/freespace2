# Vulkan Texture Manager Refactor Plan

This document presents a focused implementation plan for refactoring `VulkanTextureManager` to better adhere to the type-driven design principles in `docs/DESIGN_PHILOSOPHY.md`. The refactor preserves existing engine semantics (bmpman integration, bindless binding, render targets, deferred release safety) while eliminating representable invalid states.

**Status**: Planning complete. Implementation not started.

**Last Updated**: 2025-01

---

## Table of Contents

1. [Overview](#1-overview)
2. [Terminology](#2-terminology)
3. [Related Documentation](#3-related-documentation)
4. [Scope](#4-scope)
5. [Current State Analysis](#5-current-state-analysis)
6. [Target Architecture](#6-target-architecture)
7. [Implementation Phases](#7-implementation-phases)
8. [Phase Dependencies](#8-phase-dependencies)
9. [Risk Assessment](#9-risk-assessment)
10. [Definition of Done](#10-definition-of-done)
11. [Type and Container Sketches](#11-type-and-container-sketches)
12. [API Transition Map](#12-api-transition-map)
13. [Additional Correctness Improvements](#13-additional-correctness-improvements)
14. [Commit Strategy](#14-commit-strategy)

---

## 1. Overview

The `VulkanTextureManager` manages GPU texture resources for the Vulkan renderer, handling:

- Texture upload from bmpman to GPU memory
- Bindless texture slot allocation for model rendering
- Render target creation and lifecycle
- Deferred GPU resource release
- LRU eviction when bindless slots are exhausted

While the current implementation uses "state-as-location" (container membership defines state) for much of its state machine, several design-philosophy violations remain. These violations introduce representable invalid states that the code compensates for at runtime through guards, retries, and sentinel values.

**Goal**: Refactor the texture manager so that invalid states become compile-time or data-structure errors rather than runtime conditions.

---

## 2. Terminology

This section defines key terms from `docs/DESIGN_PHILOSOPHY.md` as they apply to this refactor.

### State as Location

Container membership defines object state. Instead of:

```cpp
struct Texture { enum State { Pending, Resident, Failed } state; };
std::map<Id, Texture> textures;
```

Use separate containers:

```cpp
std::map<Id, PendingTexture> pending;   // presence = pending
std::map<Id, ResidentTexture> resident; // presence = resident
// absence from both = not loaded
```

The data structure *is* the state machine. Iterating a container guarantees valid state.

### Capability Token

A move-only type that proves a specific phase is active. Operations requiring that phase take the token as a parameter. Example: `UploadCtx` proves upload phase is active; `flushPendingUploads(const UploadCtx&)` cannot be called without it.

### Fake Inhabitant / Sentinel Value

A value within a type's domain used to represent "absence" or "invalid". Examples: `-1` for "no handle", `UINT32_MAX` for "no eviction candidate", `nullptr` for "not initialized". These are design flaws because the type can represent states it should not.

### Inhabitant Branching

Branching on whether a value exists (`if (ptr != nullptr)`, `if (opt.has_value())`) deep inside logic rather than at system boundaries. The fix is to make existence proof-by-construction, so internal logic never sees invalid states.

### Boundary Validation

Conditionals at system entry points that reject invalid input before it enters the system. This is acceptable because it prevents invalid states from being represented internally. Example: `TextureId::tryFromBaseFrame(int)` validates at the boundary.

### Typestate Transition

A function that consumes one type and produces another, encoding a valid state change. Move semantics prevent reusing the consumed state.

---

## 3. Related Documentation

| Document | Relevance |
|----------|-----------|
| `docs/DESIGN_PHILOSOPHY.md` | Core principles: make invalid states unrepresentable |
| `docs/VULKAN_CAPABILITY_TOKENS.md` | Phase tokens (`UploadCtx`, `FrameCtx`, etc.) |
| `docs/VULKAN_TEXTURE_BINDING.md` | Bindless binding model, reserved slots, sampler caching |
| `docs/VULKAN_TEXTURE_RESIDENCY.md` | Current residency state machine |
| `docs/VULKAN_SYNCHRONIZATION.md` | Serial/timeline model, safe points, deferred release |

---

## 4. Scope

### In Scope

- `code/graphics/vulkan/VulkanTextureManager.{h,cpp}` structural refactor
- Tightening API contracts between:
  - Draw-path: `VulkanTextureBindings` (`code/graphics/vulkan/VulkanTextureBindings.h`)
  - Upload-path: `VulkanTextureUploader` (`code/graphics/vulkan/VulkanTextureBindings.h`)
  - Renderer orchestration: `VulkanRenderer` (`code/graphics/vulkan/VulkanRenderer.{h,cpp}`)
- Render-target ownership unification (eliminate split RTT state)
- Bindless slot allocation phase enforcement (draw path becomes lookup-only)
- Sentinel value removal or quarantine
- Queue stall removal (`queue.waitIdle()` replaced with fence/timeline waits)
- Test and documentation updates to match new contracts

### Out of Scope

Unless required for correctness, the following are excluded:

- Changing bmpman public semantics (e.g., making `bm_make_render_target()` asynchronous)
- Major shader/binding layout changes (descriptor set layout stays compatible)
- Reworking the upload pipeline beyond texture manager boundaries

---

## 5. Current State Analysis

### 5.1 What Works Well

**State-as-Location Pattern**

The texture manager already uses container membership as the primary state machine:

| Container | State Represented | Location |
|-----------|-------------------|----------|
| `m_residentTextures` | Texture is resident on GPU | `VulkanTextureManager.h:258` |
| `m_pendingUploads` | Upload is queued | `VulkanTextureManager.h:265` |
| `m_unavailableTextures` | Texture failed to load | `VulkanTextureManager.h:259` |
| `m_bindlessSlots` | Bindless slot assigned | `VulkanTextureManager.h:261` |
| `m_freeBindlessSlots` | Slot available for reuse | `VulkanTextureManager.h:280` |

**Capability Token for Upload Phase**

The upload-phase token `UploadCtx` already exists and gates `flushPendingUploads()` / `updateTexture()`. See `code/graphics/vulkan/VulkanPhaseContexts.h:14-32`.

### 5.2 Design Philosophy Violations

The following issues represent places where invalid states remain representable:

#### Violation 1: Sentinel Values in LRU Eviction

LRU eviction uses `UINT32_MAX` and `-1` as sentinels to represent "no candidate found":

```cpp
// VulkanTextureManager.cpp:1309-1310
uint32_t oldestFrame = UINT32_MAX;
int oldestHandle = -1;
```

**Problem**: The selection logic must branch on these sentinels. An eviction candidate should be represented as `std::optional<Candidate>`, making "no candidate" unrepresentable as a valid candidate.

#### Violation 2: Retry Loops / Phase Violations

`retryPendingBindlessSlots()` exists because bindless slots can be allocated from rendering paths:

```cpp
// VulkanTextureManager.cpp:1256-1272
void VulkanTextureManager::retryPendingBindlessSlots() { ... }
```

**Problem**: Retry loops prove architectural problems (see `docs/DESIGN_PHILOSOPHY.md`). The draw path should not allocate slots; allocation should be restricted to the upload phase.

#### Violation 3: Render Target Split Ownership

Render target metadata lives in `m_renderTargets`, while the GPU image lives in `m_residentTextures`. This creates a "split brain" state where either can exist without the other:

```cpp
// VulkanTextureManager.cpp:1442-1446
// Fallback path: "record without resident" can occur
```

**Problem**: The invariant "RTT exists implies both metadata and GPU image exist" is not enforced by data structure.

#### Violation 4: Queue-Wide Stalls

`queue.waitIdle()` is used in several texture-manager operations:

| Location | Operation |
|----------|-----------|
| `VulkanTextureManager.cpp:288-289` | Builtin solid texture creation |
| `VulkanTextureManager.cpp:548-549` | `uploadImmediate` preload path |
| `VulkanTextureManager.cpp:1784-1785` | Render target initialization |

**Problem**: `waitIdle()` stalls the entire queue. Use fence or timeline waits on the specific submitted work instead.

#### Violation 5: Raw Integers as Internal Keys

Many internal maps and methods use `int` base-frame handles. The strong typedef `TextureId` exists (`code/graphics/vulkan/VulkanTextureId.h`) but is not used as the internal key.

**Problem**: Internal code must repeatedly check `baseFrame < 0` because the type permits invalid values.

---

## 6. Target Architecture

### 6.1 Handle Correctness: Make Wrong Handles Unrepresentable

**Goal**: Inside `VulkanTextureManager`, "texture handle" means "bmpman base frame >= 0" by construction.

**Mechanism**:

- Use `TextureId` (`code/graphics/vulkan/VulkanTextureId.h`) as the internal identity type
- Keep raw `int` only at boundaries:
  - Inputs from bmpman/engine (`bitmapHandle`, `baseFrameHandle`)
  - Conversion happens exactly once via `TextureId::tryFromBaseFrame(...)`

**Consequences**:

- Internal containers become `unordered_map<TextureId, ...>`
- Internal helper APIs accept `TextureId` (not `int`), eliminating repeated validity checks

### 6.2 Bindless Slot Allocation: Enforce Phase Validity

**Goal**: The draw path must never allocate or evict bindless slots mid-frame.

**Mechanism**:

- Move slot allocation to upload phase (frame-start safe point) only
- Make bindless index lookup a pure query:
  - `bindlessSlot(TextureId) const -> BindlessSlot` returns assigned slot if present, else fallback
  - Requesting a bindless index can *queue an upload* (boundary action) but cannot mutate slot ownership

**Behavior Change**:

If a texture becomes resident mid-frame, its bindless slot may not activate until the next upload-phase sync. This one-frame delay is acceptable and far simpler than retry loops.

### 6.3 Render Target Integrity: Unify Ownership

**Goal**: "Render target exists" implies "resident GPU image + metadata exists" with no split state.

**Preferred Mechanism** (state-as-location):

Separate containers by semantic kind:

```cpp
std::unordered_map<TextureId, BitmapTexture> m_bitmaps;        // sample-only
std::unordered_map<TextureId, RenderTargetTexture> m_targets;  // RTT images + views + metadata
```

This makes "is render target" a data-structure fact, not a runtime check.

**Alternative** (variant):

```cpp
std::unordered_map<TextureId, std::variant<BitmapTexture, RenderTargetTexture>> m_resident;
```

This uses variant membership as state, but encourages visitor branching.

### 6.4 Submission Ownership: Remove Queue-Wide Stalls

**Goal**: Avoid `queue.waitIdle()` in favor of waiting only for submitted work.

**Mechanism**:

- For one-off submissions, use a fence or timeline wait on the submitted work
- Longer-term: consolidate one-off submissions under renderer-owned capability tokens

---

## 7. Implementation Phases

The plan is incremental. Each phase is a coherent "correctness win" with a clear end state and tests.

### Phase 0: Baseline and Guardrails

**Goal**: Lock in current behavior to detect regressions during refactoring.

**Actions**:

- Verify existing Vulkan texture tests run and are understood:
  - `test/src/graphics/test_vulkan_texture_contract.cpp`
  - `test/src/graphics/test_vulkan_fallback_texture.cpp`
  - `test/src/graphics/test_vulkan_texture_render_target.cpp`
  - `test/src/graphics/test_vulkan_texture_upload_alignment.cpp`
- Optionally add an "invariants checklist" section to `docs/VULKAN_TEXTURE_RESIDENCY.md`

**Exit Criteria**:

- [ ] Tests pass unchanged
- [ ] No functional change yet

---

### Phase 1: Internal Identity Type Adoption (`TextureId`)

**Goal**: Eliminate negative/sentinel handle checks from internals by construction.

**Code Changes**:

- Update `VulkanTextureManager` containers to be keyed by `TextureId`:
  - `m_residentTextures`, `m_unavailableTextures`, `m_bindlessSlots`
  - `m_pendingRetirements`, `m_pendingBindlessSlots`, `m_renderTargets`
  - Pending upload structures
- Add `std::hash<TextureId>` specialization in `VulkanTextureId.h`:

```cpp
namespace std {
template <>
struct hash<graphics::vulkan::TextureId> {
  size_t operator()(const graphics::vulkan::TextureId& id) const noexcept {
    return std::hash<int>{}(id.baseFrame());
  }
};
} // namespace std
```

- Update internal helper signatures:
  - `markTextureUsedBaseFrame` becomes `markTextureUsed(TextureId, ...)`
  - `getBindlessSlotIndex(int)` becomes `getBindlessSlotIndex(TextureId)`
  - `tryAssignBindlessSlot`, `retireTexture`, `hasRenderTarget`, etc.

**Boundary Conversions**:

At public entry points that accept `int`, convert once:

- `queueTextureUpload(int bitmapHandle, ...)` -> resolve base frame -> `TextureId`
- `releaseBitmap(int bitmapHandle)` -> resolve base frame -> `TextureId`
- `createRenderTarget(int baseFrameHandle, ...)` -> `TextureId::tryFromBaseFrame(...)`

**Exit Criteria**:

- [ ] Internals no longer accept "maybe-invalid" handles
- [ ] All conversions occur at boundaries with `std::optional<TextureId>` checked exactly once

---

### Phase 2: Unique Pending Upload Queue

**Goal**: Remove representable-invalid state "same texture queued multiple times" and O(n) `isUploadQueued` search.

**Current Issue**:

```cpp
// VulkanTextureManager.cpp:1221-1224
// m_pendingUploads is std::vector<int>; isUploadQueued does linear search
```

**Design**:

Introduce `PendingUploadQueue` (private helper type):

```cpp
class PendingUploadQueue {
public:
  bool enqueue(TextureId id);      // Idempotent by construction
  bool empty() const;
  std::deque<TextureId> takeAll(); // Preserves order
private:
  std::deque<TextureId> m_fifo;
  std::unordered_set<TextureId> m_membership;
};
```

**Refactor**:

- Replace `m_pendingUploads` and `isUploadQueued`
- Update call sites in `queueTextureUpload*`, `getBindlessSlotIndex`, slow-path descriptor resolution

**Exit Criteria**:

- [ ] Upload queue is unique by construction (no runtime dedup)
- [ ] No linear searches for "already queued"

---

### Phase 3: Remove `UnavailableReason` (Failure as Absence)

**Goal**: Eliminate "failed texture state" structs/enums from the internal state machine.

**Current Issue**:

`m_unavailableTextures` stores `UnavailableReason` (`VulkanTextureManager.h:105-121`), which is "state as data" and encourages inhabitant branching.

**Proposed Model**:

- Remove `UnavailableTexture` and `UnavailableReason`
- Optional: keep a boundary validation cache:

```cpp
std::unordered_set<TextureId> m_permanentlyRejected;
```

This represents "input is outside supported domain" (too large, unsupported format), not "texture exists but failed".

**Failure Classification**:

| Failure Type | Handling |
|--------------|----------|
| Permanent (cacheable) | Texture exceeds staging capacity; unsupported format |
| Transient (do not cache) | `bm_lock` failure; staging transient exhaustion |

**Migration**:

- Replace `markUnavailable(...)` with:
  - `markPermanentlyRejected(TextureId)` for domain errors
  - "Do nothing" (absence + log) for transient errors
- Ensure `releaseBitmap()` erases from `m_permanentlyRejected` to prevent handle-reuse poisoning

**Exit Criteria**:

- [ ] No `UnavailableReason` enum or failed-texture-record structs
- [ ] Draw-path and upload-path treat non-resident textures uniformly: they are absent
- [ ] Permanent domain-invalid inputs are rejected once (optional cache)

---

### Phase 4: Bindless Slot Allocation Redesign

**Goal**: Remove phase violations; make bindless slot allocation a known safe-point operation.

**Current Issue**:

Draw path can attempt slot assignment, relying on `retryPendingBindlessSlots()` at frame start.

**Target Behavior**:

| Path | Behavior |
|------|----------|
| Draw path | Returns fallback slot if none exists; queues upload if not resident; records interest (optional); **does not allocate** |
| Upload phase | After uploads and retirements, assigns slots for resident textures that need them |

**Implementation**:

- Replace `tryAssignBindlessSlot(...)`, `retryPendingBindlessSlots()`, `m_pendingBindlessSlots`
- Introduce:
  - `void assignBindlessSlots();` called from `flushPendingUploads(const UploadCtx&)`
  - `std::optional<BindlessSlot> acquireFreeSlotOrEvict();` (upload-phase only)
  - `std::optional<TextureId> findEvictionCandidate() const;` (returns optional, no sentinels)

**Slot Assignment Policy Options**:

| Option | Description |
|--------|-------------|
| A (simple) | Assign slots to all resident textures until exhausted |
| B (preferred) | Only assign to textures requested via `bindlessSlot()` call; maintain `m_bindlessRequested: unordered_set<TextureId>` |

**LRU Eviction**:

Replace sentinel-based selection with `std::optional<Candidate>`:

```cpp
struct EvictionCandidate {
  TextureId id;
  uint32_t lastUsedFrame;
};

std::optional<EvictionCandidate> findEvictionCandidate() const;
```

Selection filters:
- Must be resident
- Must not be render target (pinned)
- Must have `lastUsedSerial <= m_completedSerial` (GPU-safe eviction)

**API Cleanup**:

- `getBindlessSlotIndex(...)` becomes `const` and cannot mutate slot ownership

**Exit Criteria**:

- [ ] No bindless retry loop containers
- [ ] No bindless slot allocation in draw path
- [ ] Slot eviction is total (no sentinels)

---

### Phase 5: Render Target Ownership Refactor

**Goal**: Remove representable-invalid state where RTT metadata exists without the corresponding resident image.

**Proposed Structure**:

```cpp
struct RenderTargetTexture {
  VulkanTexture gpu;
  RenderTargetRecord rt;
  UsageTracking usage;
};

std::unordered_map<TextureId, BitmapTexture> m_bitmaps;
std::unordered_map<TextureId, RenderTargetTexture> m_targets;
```

**Consequences**:

- RTT-specific APIs become trivial lookups in `m_targets`
- `retireTexture()` no longer needs separate RTT record cleanup
- `releaseBitmap()` loses the "if we somehow have a render-target record without a resident texture" branch (`VulkanTextureManager.cpp:1442-1446`)

**Migration**:

- Extract RTT creation path from `createRenderTarget()` to insert exactly one owned object
- Update all RTT queries and transitions to use new container
- Update eviction logic to treat RTT as pinned based on container membership

**Exit Criteria**:

- [ ] RTT state cannot be split across multiple containers
- [ ] All RTT-related operations are correct-by-construction via container choice

---

### Phase 6: Submission Cleanup

**Goal**: Eliminate queue-wide stalls and reduce hidden GPU submissions.

#### Step 6A: Replace `waitIdle()` with Fence Waits (Low Risk)

For each one-off submit in `VulkanTextureManager.cpp`:

1. Create a `vk::Fence`
2. Submit using that fence
3. Wait for the fence
4. Reset/recycle command pool if applicable

**Targets**:

| Location | Operation |
|----------|-----------|
| `VulkanTextureManager.cpp:174-313` | `createSolidTexture` |
| `VulkanTextureManager.cpp:346-578` | `uploadImmediate` |
| `VulkanTextureManager.cpp:1721-1799` | `createRenderTarget` init clear/transition |

#### Step 6B: Token-Gated Submissions (Larger Refactor, Optional)

**Problem**: `gr_vulkan_bm_make_render_target()` can be called while not recording (`code/graphics/vulkan/VulkanGraphics.cpp:749-755`), so RTT initialization currently "self-submits".

**Options**:

| Option | Description |
|--------|-------------|
| 1 | Keep synchronous RTT init; expose narrow submit helper from `VulkanRenderer`; texture manager records callback, renderer performs submit+wait |
| 2 | Make RTT GPU init lazy and token-gated; `createRenderTarget()` becomes metadata-only; VkImage creation occurs at first safe token point |

**Exit Criteria**:

- [ ] No `queue.waitIdle()` remains in `VulkanTextureManager`
- [ ] Submission ownership is explicit (fence) and ideally centralized (renderer)

---

### Phase 7: Descriptor API Cleanup

**Goal**: Reduce "if imageView != nullptr" inhabitant branching by giving absence a real type.

**Current Issue**:

```cpp
// VulkanTextureManager.cpp:1563-1581
// getTextureDescriptorInfo returns default vk::DescriptorImageInfo with imageView == nullptr when not resident
```

**Preferred Options**:

| Option | API |
|--------|-----|
| 1 (domain-optional) | `std::optional<vk::DescriptorImageInfo> tryGetResidentDescriptor(TextureId, SamplerKey) const;` |
| 2 (typestate token) | `ResidentTextureId` only constructible from container membership; descriptor API requires it |

**Fallback Decision**:

Fallback selection happens at a boundary API (`VulkanTextureBindings`), not deep in the manager:

```cpp
auto info = textures.tryGetResidentDescriptor(id, samplerKey);
if (!info) {
  textures.queueUpload(id, samplerKey);
  return textures.fallbackDescriptor(samplerKey);
}
textures.markUsed(id, frameIndex);
return *info;
```

**Exit Criteria**:

- [ ] No internal logic branches on a fake "empty descriptor" inhabitant
- [ ] Fallback decision happens at boundary API

---

### Phase 8: Documentation and Test Updates

**Goal**: Ensure the refactor is complete only when invariants are documented and tested.

**Documentation Updates**:

| Document | Updates |
|----------|---------|
| `docs/VULKAN_TEXTURE_RESIDENCY.md` | Update state machine containers; remove unavailable reasons; document slot assignment phase |
| `docs/VULKAN_TEXTURE_BINDING.md` | Document one-frame delay semantics for bindless slot activation (if adopted) |
| `docs/VULKAN_SYNCHRONIZATION.md` | Document fence vs timeline pattern changes |

**Test Updates**:

- Update signature contract test if descriptor API changes
- Add invariant test: draw path cannot allocate/evict slots (method becomes `const`)
- Add behavioral test for pending upload queue uniqueness
- Update RTT tests to reflect single-source-of-truth ownership model

**Exit Criteria**:

- [ ] Tests validate key invariants and prevent regressions
- [ ] Docs reflect actual state machine and phase boundaries

---

## 8. Phase Dependencies

```
Phase 0: Baseline
    |
    v
Phase 1: TextureId adoption
    |
    +---> Phase 2: Unique pending upload queue
    |         |
    |         v
    +---> Phase 3: Remove UnavailableReason
    |
    +---> Phase 4: Bindless slot redesign
    |         |
    |         v
    +---> Phase 5: RTT ownership refactor
              |
              v
          Phase 6: Submission cleanup
              |
              v
          Phase 7: Descriptor API cleanup
              |
              v
          Phase 8: Docs and tests
```

**Notes**:

- Phase 1 is a prerequisite for Phases 2-5 (they all depend on `TextureId`)
- Phases 2-5 can be done in any order after Phase 1
- Phase 6 depends on Phase 5 (RTT ownership affects submission paths)
- Phase 8 should be done last to capture final state

**Suggested Order** (for reviewable diffs):

1. Phase 1 + Phase 2 (pure type/container refactor; no behavior changes)
2. Phase 4 (bindless slot redesign; intentional behavior changes)
3. Phase 5 (RTT ownership; touches multiple call sites)
4. Phase 3 (remove UnavailableReason; pushes failure handling to boundaries)
5. Phase 6 (submission cleanup; fence waits first)
6. Phase 7 + Phase 8 (API cleanup + docs/tests)

---

## 9. Risk Assessment

### Risk: bmpman Render Target Semantics

If RTT creation becomes lazy (Phase 6B option 2), callers may assume the GPU image exists immediately.

**Mitigation**:

- Prefer Phase 6A (fence waits) first to remove `waitIdle` without changing semantics
- Adopt laziness only after auditing call sites and adding explicit tests

### Risk: Bindless One-Frame Delay Visibility

Changing slot assignment phase may cause one-frame "fallback sampling" for newly-resident textures.

**Mitigation**:

- Keep existing "slow path flush now" in `VulkanRenderer` for critical cases (animated textures) where the renderer already forces an upload flush mid-frame (`VulkanRenderer.cpp:2744-2748`)
- Ensure descriptor sync ordering: slot assignment occurs before descriptor writes

### Risk: Widespread Signature Churn

Moving from `int` to `TextureId` touches many call sites.

**Mitigation**:

- Make Phase 1 mechanical and compile-driven
- Keep boundary `int` APIs temporarily; add internal overloads; delete old APIs once call sites migrate

### Risk: Stale File/Line References

Code anchors in this document (e.g., `VulkanTextureManager.cpp:1309`) may become stale during implementation.

**Mitigation**:

- Use search patterns (`"UINT32_MAX"`, `"waitIdle"`) in addition to line numbers
- Update document during implementation phases

---

## 10. Definition of Done

This refactor is complete when:

- [ ] Internal texture identity is `TextureId` everywhere; raw `int` exists only at boundaries
- [ ] No sentinel values remain in resource selection paths (LRU/eviction)
- [ ] Draw path is lookup-only for bindless slot ownership; no retries, no slot mutation mid-frame
- [ ] Render target ownership is unified (no split RTT state possible)
- [ ] No `queue.waitIdle()` remains in `VulkanTextureManager`
- [ ] Tests and docs reflect the new invariants and prevent regressions

---

## 11. Type and Container Sketches

These are shape-of-the-solution sketches, not final APIs.

### 11.1 TextureId Hashing

```cpp
// In VulkanTextureId.h
namespace std {
template <>
struct hash<graphics::vulkan::TextureId> {
  size_t operator()(const graphics::vulkan::TextureId& id) const noexcept {
    return std::hash<int>{}(id.baseFrame());
  }
};
} // namespace std
```

**Invariant**: All `TextureId` values are base frames (>= 0) by construction.

### 11.2 Pending Upload Queue

```cpp
class PendingUploadQueue {
public:
  // Returns true if newly enqueued; false if already present (idempotent)
  bool enqueue(TextureId id) {
    if (!m_membership.insert(id).second) {
      return false;
    }
    m_fifo.push_back(id);
    return true;
  }

  bool empty() const { return m_fifo.empty(); }

  // Extract everything; preserves deterministic upload order
  std::deque<TextureId> takeAll() {
    m_membership.clear();
    return std::exchange(m_fifo, {});
  }

private:
  std::deque<TextureId> m_fifo;
  std::unordered_set<TextureId> m_membership;
};
```

**Invariants**:

- A texture is either queued or not queued; duplicates cannot exist
- Upload order is stable and observable (aids debugging)

### 11.3 Bindless Slot Strong Type

```cpp
struct BindlessSlot {
  uint32_t value = kBindlessTextureSlotFallback;
};

inline bool isDynamic(BindlessSlot s) {
  return s.value >= kBindlessFirstDynamicTextureSlot && s.value < kMaxBindlessTextures;
}
```

**Invariant**: Prevents treating arbitrary integers as slot indices (boundary-only conversions).

### 11.4 Resident Texture Containers

```cpp
struct UsageTracking {
  uint32_t lastUsedFrame = 0;
  uint64_t lastUsedSerial = 0;
};

struct BitmapTexture {
  VulkanTexture gpu;      // Always sample-only after upload
  UsageTracking usage;
};

struct RenderTargetTexture {
  VulkanTexture gpu;      // Layout mutable (attachment <-> shader read)
  RenderTargetRecord rt;  // Extent/format/mips/views
  UsageTracking usage;
};

std::unordered_map<TextureId, BitmapTexture> m_bitmaps;
std::unordered_map<TextureId, RenderTargetTexture> m_targets;
```

**Invariants**:

- "Is render target" is container membership, not runtime predicate
- RTT metadata cannot exist without image, and vice versa

### 11.5 Slot Tracking as Location (Optional Enhancement)

If "has slot" should also be state-as-location:

```cpp
std::unordered_map<TextureId, SlottedBitmapTexture> m_slottedBitmaps;
std::unordered_map<TextureId, BitmapTexture>        m_unslottedBitmaps;
```

Higher churn but eliminates "slot map contains id that is not resident".

---

## 12. API Transition Map

### 12.1 Public Draw-Path APIs

**Current**:

```cpp
vk::DescriptorImageInfo getTextureDescriptorInfo(int baseFrame, SamplerKey);
uint32_t getBindlessSlotIndex(int baseFrame);
```

**Proposed**:

```cpp
std::optional<vk::DescriptorImageInfo> tryGetResidentDescriptor(TextureId, SamplerKey) const;
BindlessSlot bindlessSlot(TextureId) const;
```

**Boundary Pattern** (`VulkanTextureBindings`):

```cpp
auto info = textures.tryGetResidentDescriptor(id, samplerKey);
if (!info) {
  textures.queueUpload(id, samplerKey);
  return textures.fallbackDescriptor(samplerKey);
}
textures.markUsed(id, frameIndex);
return *info;
```

### 12.2 Upload-Path APIs

**Current** (already token-gated):

```cpp
flushPendingUploads(const UploadCtx&)
updateTexture(const UploadCtx&, ...)
```

**Proposed**: Keep token gating; refine internal signatures to be `TextureId`-based.

### 12.3 Legacy Boundary APIs

Must remain stable initially:

- `gr_vulkan_bm_make_render_target(...)` may be called outside recording

**Plan**:

- Preserve behavior first (Phase 6A fence waits)
- Token-gate RTT init (Phase 6B option 2) only after call-site audit

---

## 13. Additional Correctness Improvements

These smaller issues can reintroduce invalid states if ignored during refactoring.

### 13.1 Sampler Cache Key Safety

`m_samplerCache` uses a pre-hashed `size_t` key (`VulkanTextureManager.cpp:317-343`). While safe today, it is not type-enforced.

**Refactor To**:

```cpp
std::unordered_map<SamplerKey, vk::UniqueSampler, SamplerKeyHash> m_samplerCache;
```

This makes "different SamplerKey -> different cache entry" unrepresentable by construction.

### 13.2 Layout State for Non-RTT Textures

For non-RTT bitmap textures, layout should be `ShaderReadOnlyOptimal` after upload. Prefer encoding this structurally by:

- Removing mutable layout tracking from bitmap textures, or
- Using a distinct `SampledTexture` type without layout mutation

### 13.3 Serial-Gated Deferred Release

`releaseBitmap()` must drop handle mappings immediately, but GPU lifetimes must be protected via serial-gated deferred release. No "destroy now" paths.

---

## 14. Commit Strategy

To keep diffs reviewable and regression risk low:

| Commit Group | Phases | Risk Level |
|--------------|--------|------------|
| 1 | Phase 1 + Phase 2 | Low (pure type/container refactor) |
| 2 | Phase 4 | Medium (intentional behavior changes) |
| 3 | Phase 5 | Medium (touches multiple call sites) |
| 4 | Phase 3 | Low (pushes failure handling to boundaries) |
| 5 | Phase 6 | Low-Medium (fence waits first) |
| 6 | Phase 7 + Phase 8 | Low (API cleanup + docs/tests) |

**Each Commit Must**:

- Compile successfully
- Pass texture-related tests
- Pass a normal build

---

## References

- `code/graphics/vulkan/VulkanTextureManager.h` - Texture manager class definition
- `code/graphics/vulkan/VulkanTextureManager.cpp` - Texture manager implementation
- `code/graphics/vulkan/VulkanTextureBindings.h` - Draw-path and upload-path APIs
- `code/graphics/vulkan/VulkanTextureId.h` - Strong-typed texture identity
- `code/graphics/vulkan/VulkanPhaseContexts.h` - Capability tokens (`UploadCtx`, etc.)
- `docs/DESIGN_PHILOSOPHY.md` - Type-driven design principles
- `docs/VULKAN_TEXTURE_BINDING.md` - Bindless binding architecture
- `docs/VULKAN_TEXTURE_RESIDENCY.md` - Current residency state machine
- `docs/VULKAN_SYNCHRONIZATION.md` - Synchronization infrastructure
