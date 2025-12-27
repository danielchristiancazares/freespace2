# Vulkan Texture Manager Refactor Plan

This document presents a focused implementation plan for refactoring `VulkanTextureManager` to better adhere to the type-driven design principles in `docs/DESIGN_PHILOSOPHY.md`. The refactor preserves existing engine semantics (bmpman integration, bindless binding, render targets, deferred release safety) while eliminating representable invalid states.

**Status**: Implemented (TextureId identity, unique upload queue, upload-phase bindless slot allocation, unified RTT ownership, descriptor API cleanup, no queue-wide `waitIdle()`).

**Note**: This plan document is retained as a rationale + review map. Sections describing prior issues and phased work are historical; the authoritative current behavior is documented in `docs/VULKAN_TEXTURE_RESIDENCY.md` and `docs/VULKAN_TEXTURE_BINDING.md`.

**Last Updated**: 2025-12-26

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

Prior to this refactor, the implementation used "state-as-location" (container membership defines state) for much of its state machine, but several design-philosophy violations remained. These violations introduced representable invalid states that the code compensated for at runtime through guards, retries, and sentinel values.

**Outcome**: The texture manager has been refactored so that invalid states become compile-time or data-structure errors rather than runtime conditions.

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

### Special Case Pattern

An intentional "always-valid" default object that eliminates null/absent branching. Example: the fallback texture slot (slot 0) is a real texture, not a sentinel. The binding layer may *choose* to use slot 0 when a texture has no assigned dynamic slot; that fallback choice is policy at the boundary, not a hidden decision inside the texture manager. This differs from sentinels because the fallback is valid, usable data - not an in-band encoding of absence.

### Handle Reuse Protection

When an external system (like bmpman) reuses handles after release, internal references can become stale. Solutions:

1. **Generational handles**: Include a generation counter that increments on each reuse (slotmap pattern)
2. **Lifetime coupling**: Ensure internal cleanup is called exactly when the external handle is released
3. **Transient-only references**: Never store references longer than a single operation

This plan uses approach (2) via `releaseBitmap()` - the texture manager MUST be notified of every bmpman release to prevent stale mappings.

**Required invariant**: `releaseBitmap()` must erase the corresponding `TextureId` from every CPU-side container keyed by `TextureId` (resident maps, slot maps, pending upload membership, bindless request sets, render-target records, rejection caches, etc.) *before* deferring GPU destruction. This prevents handle-reuse cache poisoning (a released bmpman handle being immediately reused while stale manager state still exists).

---

## 3. Related Documentation

| Document | Relevance |
|----------|-----------|
| `docs/DESIGN_PHILOSOPHY.md` | Core principles: make invalid states unrepresentable |
| `docs/VULKAN_CAPABILITY_TOKENS.md` | Phase tokens (`UploadCtx`, `FrameCtx`, etc.) |
| `docs/VULKAN_TEXTURE_BINDING.md` | Bindless binding model, reserved slots, sampler caching |
| `docs/VULKAN_TEXTURE_RESIDENCY.md` | Current residency state machine |
| `docs/VULKAN_SYNCHRONIZATION.md` | Serial/timeline model, safe points, deferred release |

### 3.1 External Pattern References (Rationale Only)

These are **non-normative** references used to sharpen the plan's reasoning. The authoritative sources for Vulkan behavior
remain the Vulkan specification and the Vulkan documentation in this repo.

| Reference | Why it matters for this refactor |
|----------|-----------------------------------|
| [Parse, don’t validate (Alexis King)](https://lexi-lambda.github.io/blog/2019/11/05/parse-don-t-validate/) | Reinforces the plan’s boundary rule: do checks once at entry, then operate on validated types internally (eliminates deep “maybe” branching). |
| [Designing with types: Making illegal states unrepresentable](https://fsharpforfunandprofit.com/posts/designing-with-types-making-illegal-states-unrepresentable/) | Mirrors `docs/DESIGN_PHILOSOPHY.md`: replace sentinels/enums/flags with types + container membership. |
| [Typestate analysis](https://en.wikipedia.org/wiki/Typestate_analysis) | Supports capability tokens (`UploadCtx`, `RenderCtx`) as a concrete typestate/protocol-enforcement technique. |
| [Special Case (Martin Fowler)](https://martinfowler.com/eaaCatalog/specialCase.html) | Frames builtin fallback/default textures as an intentional “always-valid” object to eliminate null/empty descriptor inhabitants. |
| [`slotmap` README (persistent unique keys)](https://raw.githubusercontent.com/orlp/slotmap/master/README.md) | Demonstrates a “keys stay invalid after deletion even if storage is reused” model; useful when reasoning about bmpman handle reuse and cache poisoning. |
| Vulkan wait primitives: [`vkQueueWaitIdle`](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkQueueWaitIdle.html), [`vkWaitForFences`](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkWaitForFences.html), [`vkWaitSemaphores`](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/vkWaitSemaphores.html) | Anchors the plan’s “remove queue-wide stalls” work: `waitIdle()` is a queue-wide hammer; prefer waiting on the specific submitted work (fence/timeline). |

---

## 4. Scope

### In Scope

- `code/graphics/vulkan/VulkanTextureManager.{h,cpp}` structural refactor
- Tightening API contracts between:
  - Draw-path: `VulkanTextureBindings` (`code/graphics/vulkan/VulkanTextureBindings.h`)
  - Upload-path: `VulkanTextureUploader` (`code/graphics/vulkan/VulkanTextureBindings.h`)
  - Renderer orchestration: `VulkanRenderer` (`code/graphics/vulkan/VulkanRenderer.{h,cpp}`)
- Render-target ownership unification (eliminate split RTT state)
- Bindless slot allocation phase enforcement (draw path is lookup-only for slot ownership; allocation/eviction requires `UploadCtx`)
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
| `m_bitmaps` | Resident sampled bitmap texture (GPU image + view) | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_bitmaps`) |
| `m_targets` | Resident bmpman render target (GPU image + views + metadata) | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_targets`) |
| `m_pendingUploads` | Upload is queued (unique FIFO) | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_pendingUploads`) |
| `m_permanentlyRejected` | Domain-invalid input (cached; do not retry automatically) | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_permanentlyRejected`) |
| `m_bindlessSlots` | Bindless slot assigned (dynamic slots only) | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_bindlessSlots`) |
| `m_bindlessRequested` | Bindless slot requested (draw-path safe signal) | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_bindlessRequested`) |
| `m_freeBindlessSlots` | Slot available for reuse | `code/graphics/vulkan/VulkanTextureManager.h` (search: `m_freeBindlessSlots`) |

**Capability Token for Upload Phase**

The upload-phase token `UploadCtx` already exists and gates `flushPendingUploads()` / `updateTexture()`. See `code/graphics/vulkan/VulkanPhaseContexts.h` (search: `struct UploadCtx`).

### 5.2 Design Philosophy Violations (Pre-Refactor)

The following issues were present prior to this refactor and are now addressed by the changes in this plan:

#### Violation 1: Sentinel Values in LRU Eviction

LRU eviction uses `UINT32_MAX` and `-1` as sentinels to represent "no candidate found":

```cpp
// VulkanTextureManager.cpp (search: `oldestFrame = UINT32_MAX`)
uint32_t oldestFrame = UINT32_MAX;
int oldestHandle = -1;
```

**Problem**: The selection logic must branch on these sentinels. An eviction candidate should be represented as `std::optional<Candidate>`, making "no candidate" unrepresentable as a valid candidate.

#### Violation 2: Retry Loops / Phase Violations

`retryPendingBindlessSlots()` exists because bindless slots can be allocated from rendering paths:

```cpp
// VulkanTextureManager.cpp (search: `assignBindlessSlots` / `requestBindlessSlot`)
void VulkanTextureManager::retryPendingBindlessSlots() { ... }
```

**Problem**: Retry loops prove architectural problems (see `docs/DESIGN_PHILOSOPHY.md`). The draw path should not allocate slots; allocation should be restricted to the upload phase.

#### Violation 3: Render Target Split Ownership

Render target metadata lives in `m_renderTargets`, while the GPU image lives in `m_residentTextures`. This creates a "split brain" state where either can exist without the other:

```cpp
// VulkanTextureManager.cpp (search: `render-target record without a resident texture`)
// Fallback path: "record without resident" can occur
```

**Problem**: The invariant "RTT exists implies both metadata and GPU image exist" is not enforced by data structure.

#### Violation 4: Queue-Wide Stalls

`queue.waitIdle()` is used in several texture-manager operations:

| Location | Operation |
|----------|-----------|
| `VulkanTextureManager.cpp` (search: `createSolidTexture`) | Builtin solid texture creation |
| `VulkanTextureManager.cpp` (search: `uploadImmediate`) | `uploadImmediate` preload path |
| `VulkanTextureManager.cpp` (search: `createRenderTarget` + `waitIdle`) | Render target initialization |

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

- Move slot allocation to upload-phase safe points only (i.e., operations that allocate/evict require proof of upload phase via `UploadCtx`)
- Make bindless slot ownership a pure lookup:
  - The texture manager exposes dynamic-slot ownership as availability data (`tryGetBindlessSlot(TextureId) -> std::optional<uint32_t>`, dynamic slots only)
  - The draw path decides what to do when a texture is unslotted (e.g., use the fallback reserved slot)
- Keep draw-path side effects explicit and CPU-only:
  - Requests like `requestBindlessSlot(TextureId)` record interest only (no allocation/eviction)
  - Upload/residency work like `queueTextureUpload(...)` is explicit; slot ownership is only mutated in upload phase (e.g., `assignBindlessSlots(const UploadCtx&)`)

**Behavior Change**:

If a texture becomes resident mid-frame, its bindless slot may not activate until the next upload-phase sync. This one-frame delay is acceptable and far simpler than retry loops.

### 6.3 Render Target Integrity: Unify Ownership

**Goal**: "Render target exists" implies "resident GPU image + metadata exists" with no split state.

**Preferred Mechanism** (state-as-location):

Separate containers by semantic kind:

```cpp
std::unordered_map<TextureId, BitmapTexture, TextureIdHasher> m_bitmaps;        // sample-only
std::unordered_map<TextureId, RenderTargetTexture, TextureIdHasher> m_targets;  // RTT images + views + metadata
```

This makes "is render target" a data-structure fact, not a runtime check.

**Alternative** (variant):

```cpp
std::unordered_map<TextureId, std::variant<BitmapTexture, RenderTargetTexture>, TextureIdHasher> m_resident;
```

This uses variant membership as state, but encourages visitor branching.

### 6.4 Submission Ownership: Remove Queue-Wide Stalls

**Goal**: Avoid `queue.waitIdle()` in favor of waiting only for submitted work.

**Mechanism**:

- Prefer renderer-owned submission helpers gated by capability tokens (avoids “hidden submits” inside the texture manager).
  - The renderer already has an init-only token and submit+wait helper (`InitCtx`, `submitInitCommandsAndWait`); treat that as
    the reference pattern for “one-off, block until complete” without a queue-wide stall.
- If a one-off submit must remain in a manager, wait only on the submitted work (fence/timeline), never `queue.waitIdle()`.

---

## 7. Implementation Phases

The plan is incremental. Each phase is a coherent "correctness win" with a clear end state and tests.

### Phase 0: Baseline and Guardrails

**Goal**: Lock in current behavior to detect regressions during refactoring.

**Actions**:

- Verify existing Vulkan texture tests run and are understood:
  - `test/src/graphics/test_vulkan_texture_contract.cpp`
  - `test/src/graphics/test_vulkan_texture_render_target.cpp`
  - `test/src/graphics/test_vulkan_texture_upload_alignment.cpp`
  - `test/src/graphics/test_vulkan_texture_helpers.cpp` (shared helpers used by the above)
- Optionally add an "invariants checklist" section to `docs/VULKAN_TEXTURE_RESIDENCY.md`

**Exit Criteria**:

- [ ] Tests pass unchanged
- [ ] No functional change yet

---

### Phase 1: Internal Identity Type Adoption (`TextureId`)

**Goal**: Eliminate negative/sentinel handle checks from internals by construction.

**Code Changes**:

- Update `VulkanTextureManager` containers to be keyed by `TextureId`:
  - `m_bitmaps`, `m_targets`, `m_bindlessSlots`
  - `m_pendingRetirements`, `m_bindlessRequested`, `m_permanentlyRejected`
  - Pending upload structures (`m_pendingUploads`)
  - Pending upload structures
- Add an explicit hasher (prefer explicit hasher structs over `namespace std` specializations in this repo):

```cpp
// In VulkanTextureId.h
struct TextureIdHasher {
  size_t operator()(const TextureId& id) const noexcept {
    return std::hash<int>{}(id.baseFrame());
  }
};
```

- Use it for all internal containers:
  - `std::unordered_map<TextureId, T, TextureIdHasher>`
  - `std::unordered_set<TextureId, TextureIdHasher>`

- Update internal helper signatures:
  - `markTextureUsedBaseFrame` becomes `markTextureUsed(TextureId, ...)`
  - `getBindlessSlotIndex(int)` is replaced by:
    - `requestBindlessSlot(TextureId)` (draw-path safe; records intent only)
    - `tryGetBindlessSlot(TextureId)` (lookup-only; caller chooses fallback)
  - `tryAssignBindlessSlot`, `retireTexture`, `hasRenderTarget`, etc.

**Boundary Conversions**:

At public entry points that accept `int`, convert once:

- `queueTextureUpload(int bitmapHandle, ...)` -> canonicalize via `bm_get_base_frame(...)` -> `TextureId`
- `releaseBitmap(int bitmapHandle)` -> canonicalize via `bm_get_base_frame(...)` -> `TextureId`
- `createRenderTarget(int baseFrameHandle, ...)` -> `TextureId::tryFromBaseFrame(...)`

**Exit Criteria**:

- [ ] Internals no longer accept "maybe-invalid" handles
- [ ] All conversions occur at boundaries with `std::optional<TextureId>` checked exactly once

---

### Phase 2: Unique Pending Upload Queue

**Goal**: Remove representable-invalid state "same texture queued multiple times" and O(n) `isUploadQueued` search.

**Current Issue**:

```cpp
// VulkanTextureManager.cpp (search: `std::vector<int> m_pendingUploads` / `isUploadQueued`)
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
  std::unordered_set<TextureId, TextureIdHasher> m_membership;
};
```

**Refactor**:

- Replace `m_pendingUploads` and `isUploadQueued`
- Update call sites in `queueTextureUpload*`, bindless slot lookup/request, slow-path descriptor resolution

**Exit Criteria**:

- [ ] Upload queue is unique by construction (no runtime dedup)
- [ ] No linear searches for "already queued"

---

### Phase 3: Remove `UnavailableReason` (Failure as Absence)

**Goal**: Eliminate "failed texture state" structs/enums from the internal state machine.

**Current Issue**:

`m_unavailableTextures` stores `UnavailableReason` (`code/graphics/vulkan/VulkanTextureManager.h`, search: `enum class UnavailableReason`), which is "state as data" and encourages inhabitant branching.

**Proposed Model**:

- Remove `UnavailableTexture` and `UnavailableReason`
- Optional: keep a boundary validation cache:

```cpp
std::unordered_set<TextureId, TextureIdHasher> m_permanentlyRejected;
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

**Definition: Safe Point**

Any operation that allocates/evicts bindless slots must require the `UploadCtx` capability token (upload-phase typestate).
In practice, “upload phase” may occur at frame start *and* at explicit mid-frame upload boundaries where the renderer
suspends dynamic rendering and mints an `UploadCtx` to record transfer work.

**Current Issue**:

Draw path can attempt slot assignment, relying on `retryPendingBindlessSlots()` at frame start.

**Target Behavior**:

| Path | Behavior |
|------|----------|
| Draw path | Reads slot availability from exposed data; decides locally what to do for missing textures; queues upload if not resident; may record a *request* (CPU-side only); **does not allocate/evict** |
| Upload phase | After uploads and retirements, assigns slots for resident textures that need them |

**Implementation**:

- Replace `tryAssignBindlessSlot(...)`, `retryPendingBindlessSlots()`, `m_pendingBindlessSlots`
- Introduce:
  - `void requestBindlessSlot(TextureId id);` (draw-path safe: records interest only; no allocation)
  - `void assignBindlessSlots(const UploadCtx&);` called from `flushPendingUploads(const UploadCtx&)`
  - `std::optional<BindlessSlot> acquireFreeSlotOrEvict(const UploadCtx&);` (upload-phase only)
  - `std::optional<TextureId> findEvictionCandidate() const;` (returns optional, no sentinels)

**Slot Assignment Policy Options**:

| Option | Description |
|--------|-------------|
| A (simple) | Assign slots to all resident textures until exhausted |
| B (preferred) | Only assign to textures requested via `requestBindlessSlot()`; maintain `m_bindlessRequested: unordered_set<TextureId, TextureIdHasher>` |

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

- Slot availability becomes exposed data (no decision-making by texture manager):
  - `std::optional<uint32_t> tryGetBindlessSlot(TextureId) const;` - lookup-only (dynamic slots only)
  - Draw path decides what to do when unslotted (e.g., use reserved fallback slot)
- Upload and request side effects remain explicit boundary calls:
  - `queueTextureUpload*` remains CPU-side only; `requestBindlessSlot(TextureId)` records interest only

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

std::unordered_map<TextureId, BitmapTexture, TextureIdHasher> m_bitmaps;
std::unordered_map<TextureId, RenderTargetTexture, TextureIdHasher> m_targets;
```

**Consequences**:

- RTT-specific APIs become trivial lookups in `m_targets`
- `retireTexture()` no longer needs separate RTT record cleanup
- `releaseBitmap()` loses the "if we somehow have a render-target record without a resident texture" branch (`code/graphics/vulkan/VulkanTextureManager.cpp`, search: `render-target record without a resident texture`)

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

#### Step 6A: Replace `waitIdle()` with Targeted Waits (Low Risk)

For each one-off submit in `VulkanTextureManager.cpp`:

1. Prefer a renderer-owned submit helper gated by a capability token (best ownership boundary).
2. If a local submit is unavoidable, wait only on the submitted work:
   - fence wait (CPU-GPU), or
   - timeline wait on the specific serial value
3. Avoid `queue.waitIdle()` (queue-wide stall) except at shutdown.

**Targets**:

| Location | Operation |
|----------|-----------|
| `VulkanTextureManager.cpp` (search: `createSolidTexture`) | `createSolidTexture` |
| `VulkanTextureManager.cpp` (search: `uploadImmediate`) | `uploadImmediate` |
| `VulkanTextureManager.cpp` (search: `createRenderTarget`) | `createRenderTarget` init clear/transition |

#### Step 6B: Token-Gated Submissions (Larger Refactor, Optional)

**Problem**: `gr_vulkan_bm_make_render_target()` can be called while not recording (`code/graphics/vulkan/VulkanGraphics.cpp`, search: `gr_vulkan_bm_make_render_target`), so RTT initialization currently "self-submits".

**Options**:

| Option | Description |
|--------|-------------|
| 1 | Keep synchronous RTT init; use a narrow submit helper from `VulkanRenderer` (token-gated); texture manager records callback, renderer performs submit+wait |
| 2 | Make RTT GPU init lazy and token-gated; `createRenderTarget()` becomes metadata-only; VkImage creation occurs at first safe token point |

**Exit Criteria**:

- [ ] No `queue.waitIdle()` remains in `VulkanTextureManager`
- [ ] Submission ownership is explicit (fence) and ideally centralized (renderer)

---

### Phase 7: Descriptor API Cleanup

**Goal**: Reduce "if imageView != nullptr" inhabitant branching by giving absence a real type.

**Current Issue**:

```cpp
// VulkanTextureManager.cpp (search: `tryGetResidentDescriptor`)
// tryGetResidentDescriptor returns std::nullopt when not resident (no fake "empty descriptor" inhabitant)
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
- Add contract coverage: draw-path-facing code cannot allocate/evict slots (allocation/eviction requires `UploadCtx` or an upload-only interface/view)
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

- Keep existing "slow path flush now" in `VulkanRenderer` for critical cases (animated textures) where the renderer already forces an upload flush mid-frame (`code/graphics/vulkan/VulkanRenderer.cpp`, search: `animated textures`)
- Ensure descriptor sync ordering: slot assignment occurs before descriptor writes

### Risk: Widespread Signature Churn

Moving from `int` to `TextureId` touches many call sites.

**Mitigation**:

- Make Phase 1 mechanical and compile-driven
- Keep boundary `int` APIs temporarily; add internal overloads; delete old APIs once call sites migrate

### Risk: Stale File/Line References

Code anchors in this document may become stale during implementation.

**Mitigation**:

- Use search patterns (`"UINT32_MAX"`, `"waitIdle"`) in addition to line numbers
- Update document during implementation phases

---

## 10. Definition of Done

This refactor is complete when:

- [ ] Internal texture identity is `TextureId` everywhere; raw `int` exists only at boundaries
- [ ] No sentinel values remain in resource selection paths (LRU/eviction)
- [ ] Draw path is lookup-only for bindless slot ownership; allocation/eviction entry points require `UploadCtx` (no retries, no slot mutation mid-frame)
- [ ] Render target ownership is unified (no split RTT state possible)
- [ ] No `queue.waitIdle()` remains in `VulkanTextureManager`
- [ ] Tests and docs reflect the new invariants and prevent regressions

---

## 11. Type and Container Sketches

These are shape-of-the-solution sketches, not final APIs.

### 11.1 TextureId Hashing

```cpp
// In VulkanTextureId.h
struct TextureIdHasher {
  size_t operator()(const TextureId& id) const noexcept {
    return std::hash<int>{}(id.baseFrame());
  }
};
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
  std::unordered_set<TextureId, TextureIdHasher> m_membership;
};
```

**Invariants**:

- A texture is either queued or not queued; duplicates cannot exist
- Upload order is stable and observable (aids debugging)

### 11.3 Bindless Slot Availability: Ownership Model

**The Wrong Question**: "How do we represent absence of a slot?"

Wrapping in `std::optional` vs using sentinel values is a syntax distinction. Both require branching on presence. The real issue is **who owns the decision** about what to do when a texture isn't slotted.

**Current Design (texture manager decides for caller):**

```cpp
BindlessSlot bindlessSlot(TextureId) const;  // returns fallback if not found
```

The texture manager is making a decision it doesn't own - it's telling the caller "use fallback" when the caller should decide that based on its own context.

**Better Design (texture manager exposes, draw path decides):**

```cpp
// Texture manager exposes availability (lookup-only; dynamic slots only).
std::optional<uint32_t> tryGetBindlessSlot(TextureId) const;

// Draw path owns the decision:
uint32_t slot = textures.tryGetBindlessSlot(id).value_or(kBindlessTextureSlotFallback);
```

**Why This is Better:**

- **Separation of concerns**: Texture manager manages availability ("herd cats to food"). Draw path manages rendering decisions ("cats decide if they're hungry").
- **Decision ownership**: The code that knows its context (draw path) makes the decision about fallback behavior.
- **Reduced texture manager responsibilities**: It doesn't pretend to know what callers should do when textures aren't available.

**The Branching Still Exists** - and that's fine. C++ can't eliminate all branching. The goal is:

1. Put decisions with their owners
2. Reduce possible invalid states
3. Make the texture manager a data provider, not an oracle

**Invariant**: `tryGetBindlessSlot()` only returns valid dynamic slots. Reserved slots (0-3) are constants the draw path uses directly.

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

std::unordered_map<TextureId, BitmapTexture, TextureIdHasher> m_bitmaps;
std::unordered_map<TextureId, RenderTargetTexture, TextureIdHasher> m_targets;
```

**Invariants**:

- "Is render target" is container membership, not runtime predicate
- RTT metadata cannot exist without image, and vice versa

### 11.5 Slot Tracking as Location (Optional Enhancement)

If "has slot" should also be state-as-location:

```cpp
std::unordered_map<TextureId, SlottedBitmapTexture, TextureIdHasher> m_slottedBitmaps;
std::unordered_map<TextureId, BitmapTexture, TextureIdHasher>        m_unslottedBitmaps;
```

Higher churn but eliminates "slot map contains id that is not resident".

---

## 12. API Transition Map

### 12.1 Public Draw-Path APIs

**Implemented**:

```cpp
// VulkanTextureManager (mechanism: availability + explicit requests)
std::optional<vk::DescriptorImageInfo> tryGetResidentDescriptor(TextureId id, const SamplerKey& samplerKey) const;
std::optional<uint32_t> tryGetBindlessSlot(TextureId id) const;  // dynamic slots only
void requestBindlessSlot(TextureId id);                          // CPU-only signal; no allocation/eviction
bool isResident(TextureId id) const;
void queueTextureUpload(TextureId id, uint32_t currentFrameIndex, const SamplerKey& samplerKey);
void markTextureUsed(TextureId id, uint32_t currentFrameIndex);
```

**Boundary Pattern** (`VulkanTextureBindings` owns policy):

```cpp
auto info = textures.tryGetResidentDescriptor(id, samplerKey);
if (!info) {
  textures.queueTextureUpload(id, currentFrameIndex, samplerKey);
  return textures.fallbackDescriptor(samplerKey);
}

textures.markTextureUsed(id, currentFrameIndex);
return *info;
```

**Bindless Pattern** (lookup-only + fallback choice):

```cpp
textures.requestBindlessSlot(id);
uint32_t slot = textures.tryGetBindlessSlot(id).value_or(kBindlessTextureSlotFallback);
```

**Key Difference**: The texture manager exposes mechanism (what exists); `VulkanTextureBindings` and draw code decide policy (what to do when something does not exist).

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

`m_samplerCache` uses a pre-hashed `size_t` key (`code/graphics/vulkan/VulkanTextureManager.cpp`, search: `m_samplerCache`). While safe today, it is not type-enforced.

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
