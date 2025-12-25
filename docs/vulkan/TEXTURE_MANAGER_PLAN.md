# Vulkan Texture Manager Refactor Plan (Design-Philosophy Compliance)

This document is a focused implementation plan for refactoring `VulkanTextureManager` to better adhere to
`docs/DESIGN_PHILOSOPHY.md` while preserving existing engine semantics (bmpman integration, bindless binding, render
targets, deferred release safety).

Related docs (read for context, not duplicated here):
- `docs/DESIGN_PHILOSOPHY.md` (core rules: invalid states unrepresentable)
- `docs/vulkan/VULKAN_CAPABILITY_TOKENS.md` (phase tokens)
- `docs/vulkan/VULKAN_TEXTURE_BINDING.md` (binding model + reserved slots)
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md` (current residency state machine)
- `docs/vulkan/VULKAN_SYNCHRONIZATION.md` (serial/timeline model and safe points)

---

## 0. Scope

### In Scope
- `code/graphics/vulkan/VulkanTextureManager.{h,cpp}` structural refactor to reduce representable invalid states.
- Tighten API contracts between:
  - draw-path: `VulkanTextureBindings` (`code/graphics/vulkan/VulkanTextureBindings.h`)
  - upload-path: `VulkanTextureUploader` (`code/graphics/vulkan/VulkanTextureBindings.h`)
  - renderer orchestration: `VulkanRenderer` (`code/graphics/vulkan/VulkanRenderer.{h,cpp}`)
- Refactor render-target ownership to eliminate "half-present" RTT state.
- Refactor bindless slot allocation so the draw path becomes lookup-only and cannot mutate slot state mid-frame.
- Remove or quarantine sentinel values / fake inhabitants.
- Remove global queue stalls (`queue.waitIdle`) from texture-manager operations (use fence/timeline waits instead).
- Update relevant tests and Vulkan docs to match the new contracts.

### Out of Scope (unless required for correctness)
- Changing bmpman public semantics (e.g., making `bm_make_render_target()` asynchronous).
- Major shader/binding layout changes (descriptor set layout stays compatible).
- Reworking the entire upload pipeline beyond texture manager boundaries.

---

## 1. Current State (Key Facts + Anchors)

### State-as-location already in use (good)
`VulkanTextureManager` uses container membership as the primary state machine:
- Resident textures: `m_residentTextures` (`code/graphics/vulkan/VulkanTextureManager.h:258`)
- Pending uploads: `m_pendingUploads` (`code/graphics/vulkan/VulkanTextureManager.h:265`)
- Unavailable textures: `m_unavailableTextures` (`code/graphics/vulkan/VulkanTextureManager.h:259`)
- Bindless slot assignments: `m_bindlessSlots` + `m_freeBindlessSlots` (`code/graphics/vulkan/VulkanTextureManager.h:261,280`)

### Capability token already exists (good but incomplete)
- Upload-phase token `UploadCtx` exists and gates `flushPendingUploads()` / `updateTexture()`
  (`code/graphics/vulkan/VulkanPhaseContexts.h:14-32`).

### Known design-philosophy violations (examples)
These are issues where invalid states remain representable and the code compensates at runtime:

1) Sentinel values (fake inhabitants)
- LRU eviction uses `UINT32_MAX` and `-1` as sentinels
  (`code/graphics/vulkan/VulkanTextureManager.cpp:1309-1310`).

2) Retry loops / phase violations
- `retryPendingBindlessSlots()` exists because bindless slots can be allocated from rendering paths
  (`code/graphics/vulkan/VulkanTextureManager.cpp:1256-1272`).

3) Render target "split brain" ownership
- Render target metadata in `m_renderTargets`, GPU image in `m_residentTextures`
  (plus optional "record without resident" fallback path in `releaseBitmap()`,
  `code/graphics/vulkan/VulkanTextureManager.cpp:1442-1446`).

4) Queue-wide stalls
- `queue.waitIdle()` used in texture-manager submissions:
  - builtin solid textures (`createSolidTexture`, `...cpp:288-289`)
  - `uploadImmediate` preload path (`...cpp:548-549`)
  - RTT initialization (`createRenderTarget`, `...cpp:1784-1785`)

5) Raw integers leak across internal APIs
- Many internal maps and methods are keyed by `int` base-frame handles.
  A strong typedef `TextureId` exists (`code/graphics/vulkan/VulkanTextureId.h`) but is not used as the internal key.

---

## 2. Target Architecture (What "Correct-by-Construction" Means Here)

### 2.1 Handle correctness: make wrong handles unrepresentable
Design goal: inside `VulkanTextureManager`, "texture handle" means "bmpman base frame >= 0" by construction.

Mechanism:
- Use `TextureId` (`code/graphics/vulkan/VulkanTextureId.h`) as the internal identity type.
- Keep raw `int` only at boundaries:
  - inputs from bmpman/engine (`bitmapHandle`, `baseFrameHandle`)
  - conversion happens exactly once via `TextureId::tryFromBaseFrame(...)`.

Consequences:
- Internal containers become `unordered_map<TextureId, ...>` or similar.
- Internal helper APIs accept `TextureId` (not `int`), eliminating repeated `baseFrame < 0` checks.

### 2.2 Bindless slot allocation: enforce phase validity
Design goal: the draw path must never allocate/evict bindless slots mid-frame.

Mechanism:
- Move slot allocation to upload phase (frame-start safe point) only.
- Make bindless index lookup a pure query:
  - `bindlessIndex(TextureId) -> BindlessSlot` returns assigned slot if present, else fallback slot.
  - Requesting a bindless index can *queue an upload* (boundary action) but cannot mutate slot ownership.

Expected behavior change:
- If a texture becomes resident mid-frame, its bindless slot may not become active until the next upload-phase sync.
  (One-frame delay is acceptable and far simpler than retry loops; see also `docs/vulkan/VULKAN_TEXTURE_BINDING.md`.)

### 2.3 Render target integrity: unify ownership so mismatches are impossible
Design goal: "render target exists" implies "resident GPU image + metadata exists" with no split state.

Mechanism options (preferred: state-as-location):
1) Separate containers by semantic kind (best for invariants):
   - `m_residentBitmaps: map<TextureId, BitmapTexture>` (sample-only images)
   - `m_residentRenderTargets: map<TextureId, RenderTargetTexture>` (RTT images + views + metadata)
   This makes "is render target" a compile-time/data-structure fact, not a runtime check.

2) Single container with a tagged variant:
   - `m_resident: map<TextureId, std::variant<BitmapTexture, RenderTargetTexture>>`
   This is state-as-location via variant membership (still okay, but can encourage visitor branching).

### 2.4 Submission ownership: remove hidden queue-wide stalls
Design goal: avoid `queue.waitIdle()` in favor of waiting only for work we submitted.

Mechanisms:
- For one-off submissions that must complete before returning, use a fence or timeline wait on the submitted work.
- Longer-term: consolidate one-off submissions under renderer-owned capability tokens so phase validity is enforced
  (similar to upload-phase `UploadCtx`).

---

## 3. Proposed Refactor Phases (Implementation Plan)

The plan is intentionally incremental. Each phase is a coherent "correctness win" with a clear end state and tests.

### Phase 0: Baseline + Guardrails
Goal: lock in current behavior to detect regressions during structural refactors.

Actions:
- Ensure existing Vulkan texture tests run and are understood:
  - `test/src/graphics/test_vulkan_texture_contract.cpp`
  - `test/src/graphics/test_vulkan_fallback_texture.cpp`
  - `test/src/graphics/test_vulkan_texture_render_target.cpp`
  - `test/src/graphics/test_vulkan_texture_upload_alignment.cpp`
- Add a short "invariants checklist" section to `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md` (optional) once the plan
  begins to land, so docs stay synchronized with code.

Exit criteria:
- Tests pass unchanged.
- No functional change yet.

---

### Phase 1: Internal identity type (`TextureId`) adoption
Goal: eliminate negative/sentinel handle checks from internals by construction.

Code changes:
- Update `VulkanTextureManager` containers to be keyed by `TextureId`:
  - `m_residentTextures`, `m_unavailableTextures` (if still present), `m_bindlessSlots`, `m_pendingRetirements`,
    `m_pendingBindlessSlots` (if still present), `m_renderTargets`, and pending upload structures.
- Add `std::hash<TextureId>` specialization or wrapper hash (likely in `VulkanTextureId.h`) so it can be used in
  `unordered_map` and `unordered_set`.
- Update internal helper signatures to accept `TextureId`:
  - `markTextureUsedBaseFrame` becomes `markTextureUsed(TextureId, ...)`
  - `getBindlessSlotIndex(int)` becomes `getBindlessSlotIndex(TextureId)` (even before it becomes const/lookup-only)
  - `tryAssignBindlessSlot`, `retireTexture`, `hasRenderTarget`, etc.

Boundary conversions:
- At public entry points that still accept `int`, convert once:
  - `queueTextureUpload(int bitmapHandle, ...)` -> resolve base frame -> `TextureId`.
  - `releaseBitmap(int bitmapHandle)` -> resolve base frame -> `TextureId`.
  - `createRenderTarget(int baseFrameHandle, ...)` -> `TextureId::tryFromBaseFrame(...)`.

Testing changes:
- Minimal, mostly mechanical. Update tests only if they depended on `int`-typed internal details.

Exit criteria:
- Internals no longer accept "maybe-invalid" handles.
- All conversions occur at boundaries with `std::optional<TextureId>` checked exactly once.

---

### Phase 2: Pending upload queue becomes unique by construction
Goal: remove representable-invalid state "same texture is queued multiple times", and remove O(n) `isUploadQueued`.

Current issue:
- `m_pendingUploads` is `std::vector<int>` and `isUploadQueued` does a linear search
  (`code/graphics/vulkan/VulkanTextureManager.cpp:1221-1224`).

Design:
- Introduce `PendingUploadQueue` (private helper type) with:
  - `std::deque<TextureId> fifo;`
  - `std::unordered_set<TextureId> membership;`
  - `enqueue(TextureId)` is idempotent by construction.
  - `takeAll()` or iteration that preserves order.

Refactor:
- Replace `m_pendingUploads` and `isUploadQueued`.
- Update call sites (`queueTextureUpload*`, `getBindlessSlotIndex`, slow-path descriptor resolution in renderer).

Exit criteria:
- Upload queue is unique by construction (no runtime dedup needed).
- No linear searches for "already queued".

---

### Phase 3: Replace `UnavailableTexture` / `UnavailableReason` with boundary validation (failure as absence)
Goal: eliminate "failed texture state" structs/enums in the internal state machine.

Current issue:
- `m_unavailableTextures` stores `UnavailableReason` (`code/graphics/vulkan/VulkanTextureManager.h:105-121`), which is
  "state as data" and encourages inhabitant branching ("if unavailable ...").

Proposed model:
- Remove `UnavailableTexture` and `UnavailableReason`.
- Optional: keep a *boundary validation cache*:
  - `std::unordered_set<TextureId> m_permanentlyRejected;`
  This is not "a texture exists but is failed"; it's "this input is outside our supported domain under current
  algorithm" (too large for staging, unsupported format).

Failure classification rules (enforced at boundaries):
- Permanent rejection (cacheable):
  - Texture larger than staging ring can ever upload (until algorithm changes).
  - Unsupported/unknown format.
- Transient failure (do not cache):
  - `bm_lock` failure, staging transient exhaustion, etc.

Migration steps:
- Replace `markUnavailable(...)` with either:
  - `markPermanentlyRejected(TextureId)` (set membership), or
  - "do nothing" (absence) plus a log, depending on failure kind.
- Ensure `releaseBitmap()` erases from `m_permanentlyRejected` so handle reuse cannot poison future textures.

Exit criteria:
- No `UnavailableReason` enum and no "failed texture record" structs.
- Draw-path and upload-path treat non-resident textures uniformly: they are absent.
- Permanent domain-invalid inputs are rejected once (optional cache) to prevent infinite retry churn.

---

### Phase 4: Bindless slot allocation redesign (no retry loops; lookup-only draw path)
Goal: remove phase violations and make bindless slot allocation a known safe-point operation.

Current issue:
- Draw path can attempt slot assignment and then relies on `retryPendingBindlessSlots()` at frame start.

Target behavior:
- Draw path:
  - Returns fallback slot if no slot exists.
  - Queues upload if texture not resident.
  - Records "interest" (optional) but does not allocate.
- Upload phase:
  - After uploads and retirements are processed, assign slots for resident textures that need them.

Implementation details:
- Replace `tryAssignBindlessSlot(...)`, `retryPendingBindlessSlots()`, and `m_pendingBindlessSlots`.
- Introduce:
  - `void assignBindlessSlots();` called from `flushPendingUploads(const UploadCtx&)` at the safe point.
  - `std::optional<BindlessSlot> acquireFreeSlotOrEvict();` (upload-phase only)
  - `std::optional<TextureId> findEvictionCandidate() const;` (returns optional, no sentinels)
- Decide policy for "which resident textures need slots":
  Option A (simple): assign slots to all resident textures until slots are exhausted.
  Option B (preferred): only textures requested via bindless usage get slots:
    - Maintain `m_bindlessRequested: unordered_set<TextureId>` updated by draw-path `bindlessIndex()`.
    - Upload-phase allocates slots for requested+resident textures.

LRU eviction:
- Replace sentinel-based selection with `std::optional<Candidate>`:
  - Candidate holds `TextureId id; uint32_t lastUsedFrame;`
  - Selection filters:
    - must be resident
    - must not be render target (pinned)
    - must have `lastUsedSerial <= m_completedSerial` (GPU-safe eviction)

API cleanup:
- `getBindlessSlotIndex(...)` becomes `const` and cannot mutate slot ownership.

Exit criteria:
- No bindless retry loop containers.
- No bindless slot allocation in draw path.
- Slot eviction is total (no sentinels).

---

### Phase 5: Render target ownership refactor (make mismatches impossible)
Goal: remove the representable-invalid state where RTT metadata exists without the corresponding resident image.

Proposed structure (preferred):
- Create a dedicated type:
  - `struct RenderTargetTexture { VulkanTexture gpu; RenderTargetRecord rt; UsageTracking usage; };`
- Replace:
  - `m_renderTargets` + RTT-in-`m_residentTextures` coupling
  with:
  - `m_renderTargets: unordered_map<TextureId, RenderTargetTexture>`
- Keep bitmap textures in a separate container:
  - `m_bitmapTextures: unordered_map<TextureId, BitmapTexture>`

Consequences:
- RTT-specific APIs (`renderTargetExtent`, `renderTargetAttachmentView`, transitions, mip gen) become trivial:
  - lookup in `m_renderTargets` and operate on `RenderTargetTexture.gpu`.
- `retireTexture()` no longer needs to "also tryTakeRenderTargetRecord()" to clean up views separately.
- `releaseBitmap()` does not need the "if we somehow have a render-target record without a resident texture" branch
  (`...cpp:1442-1446`) because that state cannot exist.

Migration steps:
- Extract RTT creation path from `createRenderTarget()` so it inserts exactly one owned object.
- Update all RTT queries and transitions to use the new container.
- Update eviction logic to treat RTT as pinned based on container membership, not map lookups.

Exit criteria:
- RTT state cannot be split across multiple containers.
- All RTT-related operations are correct-by-construction via container choice.

---

### Phase 6: Submission cleanup (remove `queue.waitIdle()`; enforce phase validity where possible)
Goal: eliminate queue-wide stalls and reduce hidden GPU submissions.

Step 6A (minimum, low-risk): replace `waitIdle()` with a fence wait
- For each one-off submit in `VulkanTextureManager.cpp`:
  - Create a `vk::Fence`, submit using that fence, wait for the fence, then reset/recycle command pool if applicable.
  This makes the wait precise: "wait for this submission", not "wait for the entire queue to go idle".

Targets:
- `createSolidTexture` (`...cpp:174-313`)
- `uploadImmediate` (`...cpp:346-578`)
- `createRenderTarget` init clear/transition (`...cpp:1721-1799`)

Step 6B (preferred, larger refactor): move one-off submissions behind renderer capability tokens
Problem: `gr_vulkan_bm_make_render_target()` can be called while not recording (`code/graphics/vulkan/VulkanGraphics.cpp:749-755`),
so RTT initialization currently "self-submits" in the texture manager.

Options:
1) Keep synchronous RTT init but make submission explicit via a renderer-owned submit helper:
   - `VulkanRenderer` exposes a narrow token/entry point for "submit one-time transfer work and wait".
   - Texture manager records a callback that fills a cmd buffer; renderer performs submit+wait.
   - This makes queue/timeline ownership centralized and testable.

2) Make RTT GPU init lazy and token-gated:
   - `createRenderTarget()` becomes metadata-only (callable anytime).
   - Actual VkImage creation/clear/transition occurs at the first safe token point:
     - either during upload phase (`UploadCtx`) or when the RTT is first bound as a target (`FrameCtx`).
   - Requires carefully validating bmpman semantics (ensure no code requires a fully-initialized VkImage at creation).

Builtins (`fallback/default*`) creation:
- Similar choice: keep in ctor with fence waits (6A) or move to an init token path (6B) to avoid hidden submits in ctors.

Exit criteria:
- No `queue.waitIdle()` remains in `VulkanTextureManager`.
- Submission ownership is explicit (fence) and ideally centralized (renderer) for phase clarity.

---

### Phase 7: Descriptor API cleanup (remove "empty descriptor = absence" inhabitant branching)
Goal: reduce downstream "if imageView != nullptr" branching by giving the absence a real type.

Current:
- `getTextureDescriptorInfo(int, SamplerKey)` returns a default `vk::DescriptorImageInfo` with `imageView == nullptr`
  when not resident (`code/graphics/vulkan/VulkanTextureManager.cpp:1563-1581`).

Preferred options:
1) Domain-optional:
   - `std::optional<vk::DescriptorImageInfo> tryGetResidentDescriptor(TextureId, SamplerKey) const;`
   - Callers decide fallback once at the boundary (e.g., in `VulkanTextureBindings::descriptor`).

2) Typestate token:
   - `ResidentTextureId` only constructible from container membership; descriptor API requires it.
   - This eliminates "resident check" from deeper logic entirely.

Notes:
- There is already a contract test asserting the current signature
  (`test/src/graphics/test_vulkan_texture_contract.cpp`). Update it to assert the *new* contract.

Exit criteria:
- No internal logic branches on a fake "empty descriptor" inhabitant.
- Fallback decision happens at a boundary API (`VulkanTextureBindings`), not deep in the manager.

---

### Phase 8: Docs + Tests updates (keep invariants locked)
Goal: ensure the refactor is "done" only when invariants are documented and tested.

Docs to update:
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md`
  - Update state machine containers (pending queue type, removal of unavailable reasons, slot assignment phase).
- `docs/vulkan/VULKAN_TEXTURE_BINDING.md`
  - Document any one-frame delay semantics for bindless slot activation (if adopted).
- `docs/vulkan/VULKAN_SYNCHRONIZATION.md`
  - If submission/wait mechanism changes materially (fence vs timeline), document the pattern.

Tests to update/add:
- Update signature contract test if descriptor API changes.
- Add an invariant test that:
  - Draw path cannot allocate/evict slots (ideally a compile-time/API test: method becomes `const` and has no mutation).
- Add a behavioral test for new "pending upload queue uniqueness" (fake state-machine test acceptable).
- Ensure RTT tests still reflect the now-single-source-of-truth ownership model (fake RTT test may need adjustment if API changes).

Exit criteria:
- Tests validate the key invariants and prevent regressions.
- Docs reflect the actual state machine and phase boundaries.

---

## 4. Risk Assessment + Mitigations

### Risk: bmpman render target semantics
If RTT creation is made lazy (Phase 6B option 2), callers may assume the GPU image exists immediately.

Mitigation:
- Prefer Phase 6A (fence waits) first to remove `waitIdle` without changing semantics.
- Only adopt laziness after auditing call sites and adding explicit tests.

### Risk: bindless one-frame delay visibility
Changing slot assignment phase may cause one-frame "fallback sampling" for newly-resident textures in the model path.

Mitigation:
- Keep the existing "slow path flush now" in `VulkanRenderer` for critical cases (animated textures) where the renderer
  already forces an upload flush mid-frame (`code/graphics/vulkan/VulkanRenderer.cpp:2744-2748`).
- Ensure descriptor sync ordering is correct: slot assignment occurs before descriptor writes.

### Risk: widespread signature churn
Moving from `int` to `TextureId` will touch many call sites.

Mitigation:
- Make Phase 1 mechanical and compile-driven.
- Keep boundary `int` APIs temporarily and add internal overloads to reduce immediate churn, then delete old APIs once
  call sites are migrated.

---

## 5. Definition of Done (Correctness Criteria)

This refactor is complete when:
- Internal texture identity is `TextureId` everywhere; raw `int` exists only at boundaries.
- No sentinel values remain in resource selection paths (LRU/eviction).
- Draw path is lookup-only for bindless slot ownership; no retries, no slot mutation mid-frame.
- Render target ownership is unified (no split RTT state possible).
- No `queue.waitIdle()` remains in `VulkanTextureManager`.
- Tests and docs reflect the new invariants and prevent regressions.

---

## 6. Concrete Type and Container Sketches

These are not final APIs; they are "shape of the solution" sketches that make invariants explicit.

### 6.1 `TextureId` hashing for unordered containers
`TextureId` is currently equality comparable but not hashable. To use it as a key:

```cpp
// In VulkanTextureId.h (or a dedicated header included by users)
namespace std {
template <>
struct hash<graphics::vulkan::TextureId> {
  size_t operator()(const graphics::vulkan::TextureId& id) const noexcept {
    return std::hash<int>{}(id.baseFrame());
  }
};
} // namespace std
```

Invariants:
- All `TextureId` values are base frames (>= 0) by construction.

### 6.2 Pending upload queue (unique by construction)

```cpp
class PendingUploadQueue {
public:
  bool enqueue(TextureId id) {
    if (!m_membership.insert(id).second) {
      return false; // already present
    }
    m_fifo.push_back(id);
    return true;
  }

  bool empty() const { return m_fifo.empty(); }

  // Extract everything (preserves deterministic upload order).
  std::deque<TextureId> takeAll() {
    m_membership.clear();
    return std::exchange(m_fifo, {});
  }

private:
  std::deque<TextureId> m_fifo;
  std::unordered_set<TextureId> m_membership;
};
```

Invariants:
- A texture is either queued or not queued; duplicates cannot exist.
- Upload order is stable and observable (helps with debugging).

### 6.3 Bindless slot strong type (optional but recommended)

```cpp
struct BindlessSlot {
  uint32_t value = kBindlessTextureSlotFallback;
};

inline bool isDynamic(BindlessSlot s) {
  return s.value >= kBindlessFirstDynamicTextureSlot && s.value < kMaxBindlessTextures;
}
```

Invariants:
- Prevents accidentally treating arbitrary integers as slot indices (boundary-only conversions).

### 6.4 Resident texture containers (state as location)

Preferred split (keeps RTT-vs-bitmap logic from leaking everywhere):

```cpp
struct UsageTracking {
  uint32_t lastUsedFrame = 0;
  uint64_t lastUsedSerial = 0;
};

struct BitmapTexture {
  VulkanTexture gpu;      // Always sample-only after upload.
  UsageTracking usage;
};

struct RenderTargetTexture {
  VulkanTexture gpu;      // Layout is mutable (attachment <-> shader read).
  RenderTargetRecord rt;  // Extent/format/mips/views.
  UsageTracking usage;
};

std::unordered_map<TextureId, BitmapTexture> m_bitmaps;       // resident bitmap textures
std::unordered_map<TextureId, RenderTargetTexture> m_targets; // resident render targets
```

Invariants:
- "is render target" is container membership, not a runtime predicate.
- RTT metadata cannot exist without the image, and vice versa.

### 6.5 Slot tracking as location (optional enhancement)
If we want "has slot" to also be state-as-location (not `m_bindlessSlots` map + filtering), split further:

```cpp
std::unordered_map<TextureId, SlottedBitmapTexture> m_slottedBitmaps;
std::unordered_map<TextureId, BitmapTexture>        m_unslottedBitmaps;
```

This is higher churn but removes representable invalid states like "slot map contains id that is not resident".

---

## 7. API Transition Map (Old -> New)

This section exists to keep the refactor mechanically safe and reviewable.

### 7.1 Public draw-path APIs (`VulkanTextureBindings`)

Current:
- `vk::DescriptorImageInfo VulkanTextureManager::getTextureDescriptorInfo(int baseFrame, SamplerKey)`
- `uint32_t VulkanTextureManager::getBindlessSlotIndex(int baseFrame)`

Proposed:
- `std::optional<vk::DescriptorImageInfo> VulkanTextureManager::tryGetResidentDescriptor(TextureId, SamplerKey) const`
- `BindlessSlot VulkanTextureManager::bindlessSlot(TextureId) const`

Draw-path boundary (`VulkanTextureBindings`) becomes the only place that chooses fallback:

```cpp
auto info = textures.tryGetResidentDescriptor(id, samplerKey);
if (!info) {
  textures.queueUpload(id, samplerKey); // boundary action
  return textures.fallbackDescriptor(samplerKey);
}
textures.markUsed(id, frameIndex);
return *info;
```

### 7.2 Upload-path APIs (`VulkanTextureUploader`)

Current (already token-gated):
- `flushPendingUploads(const UploadCtx&)`
- `updateTexture(const UploadCtx&, ...)`

Proposed:
- Keep token gating; refine internal signatures to be `TextureId`-based where possible.

### 7.3 Legacy boundary APIs (bmpman integration)

Must remain stable initially:
- `gr_vulkan_bm_make_render_target(...)` may be called outside recording.

Plan:
- Preserve behavior first (Phase 6A fence waits).
- Only after a call-site audit consider token-gating RTT init (Phase 6B option 2).

---

## 8. Additional Correctness Improvements (Not Optional If Touched)

These are smaller issues that can quietly reintroduce invalid states if ignored during the refactor.

### 8.1 Sampler cache key must be collision-safe
`m_samplerCache` currently uses a pre-hashed `size_t` key (`VulkanTextureManager.cpp:317-343`).
Even if it is "safe today" due to limited enum values, it is not enforced by the type system.

Refactor to:
- `std::unordered_map<SamplerKey, vk::UniqueSampler, SamplerKeyHash> m_samplerCache;`

This makes "different SamplerKey -> different cache entry" unrepresentable by construction.

### 8.2 Avoid "layout state" fields where they cannot vary
For non-RTT bitmap textures, layout should be `ShaderReadOnlyOptimal` after upload.
Prefer encoding that invariant structurally by:
- removing mutable layout tracking from bitmap textures, or
- using a distinct `SampledTexture` type that does not expose layout mutation.

### 8.3 Serial-gated deferred release must remain the only lifetime mechanism
`releaseBitmap()` must still drop handle mappings immediately, but GPU lifetimes must be protected via serial-gated
deferred release (no "destroy now" paths).

---

## 9. Suggested Commit/Review Strategy

To keep diffs reviewable and regression risk low:
1) Phase 1 + Phase 2 (pure type/container refactor; no behavior changes intended)
2) Phase 4 (bindless slot allocation redesign; behavior changes are intentional and testable)
3) Phase 5 (RTT ownership refactor; correctness win, but touches multiple call sites)
4) Phase 3 (remove UnavailableReason; pushes failure handling to boundaries)
5) Phase 6 (submission cleanup; fence waits first, then optional token centralization)
6) Phase 7 + Phase 8 (API cleanup + docs/tests updates)

Each commit should:
- compile, and
- run the most relevant texture-related tests (plus a normal build).
