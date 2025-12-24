# Remediation Plan 3 — Type-Driven Vulkan Pipeline Hardening

This plan implements a **type-driven remediation pass** over the Vulkan rendering pipeline, following `docs/DESIGN_PHILOSOPHY.md` (make invalid states unrepresentable; push conditionals to boundaries).

The intent is **correctness-by-construction**, delivered as a sequence of small, reviewable PRs with explicit acceptance gates.

---

## Guiding invariants (what “done” means)

### Compile-time / structural invariants
- **Recording provenance**: only `VulkanRenderer` can mint a “recording is active” token (`RecordingFrame`). Non-friend code cannot fabricate it.
- **Phase separation**: upload-only operations are uncallable from render-only code paths, and vice versa (capability tokens enforce it).
- **No sentinel identities in internal logic**: internal Vulkan code does not use `-1`, negative “fake handles”, or other sentinel integers to represent texture identity.
- **No fake optionals in internal logic**: internal APIs do not communicate residency/validity via “empty structs”, null `imageView`, or `std::optional` that must be re-checked deep in the call tree. Boundaries may still return optionals.
- **State as location**: “queued/resident/unavailable/retired” is represented via container membership (already mostly true for textures and frames), not via state flags.

### Runtime invariants (allowed, but only at boundaries)
- `VulkanGraphics.cpp` may perform boundary checks (backend exists, recording exists) and either assert or no-op.
- Deep logic (renderer/session/managers) should not contain repeated `if (exists)` routing; it should consume valid tokens/handles by construction.

---

## Current baseline (what’s already good)

The Vulkan backend already implements several key patterns:
- **Move-only recording token**: `RecordingFrame` is move-only (`code/graphics/vulkan/VulkanFrameFlow.h`).
- **Frame state as location**: frames move between `m_availableFrames` and `m_inFlightFrames` (`code/graphics/vulkan/VulkanRenderer.h/.cpp`).
- **Render target state as location + RAII**: render target state is owned by `std::unique_ptr<RenderTargetState>` and active rendering by `std::optional<ActivePass>` with RAII end-rendering (`code/graphics/vulkan/VulkanRenderingSession.*`).
- **Contract-driven pipeline layouts**: `shader_type -> {PipelineLayoutKind, VertexInputMode}` via `VulkanLayoutContracts` with unit tests (`code/graphics/vulkan/VulkanLayoutContracts.*`, `test/src/graphics/test_vulkan_shader_layout_contracts.cpp`).
- **Descriptor correctness rule**: bindless descriptor arrays are fallback-filled and updated in batches (`VulkanRenderer::updateModelDescriptors`).

The remediation work focuses on remaining “invalid state representable” hotspots and on removing sentinel-based identity.

---

## Primary gaps to remediate

### G1 — `RecordingFrame` is forgeable
`RecordingFrame` currently has a public constructor, which allows non-friend code to fabricate a recording token. This weakens the “token proves phase validity” invariant.

### G2 — Synthetic negative texture handles + `-1` “uninitialized”
`VulkanTextureManager` uses sentinel integers (`-1000`… and `-1`) to represent fallback/default textures and initialization state. This violates “sentinel-free resource binding” and leaks into draw logic.

### G3 — “Resident-ness” represented by empty descriptors
`VulkanTextureManager::getTextureDescriptorInfo()` returns an empty `vk::DescriptorImageInfo` on miss, and callers branch on `if (info.imageView)`. This is inhabitant-branching deep in logic.

### G4 — Redundant runtime state checks where typestate exists
Example: `VulkanRenderingSession::endDeferredGeometry()` uses `dynamic_cast` to validate the active target state even though deferred flow already uses typestate tokens at the renderer layer.

### G5 — Debug logging / absolute paths in hot paths
There are absolute-path debug writes (e.g. `.cursor\\debug.log`) in hot paths (including headers). This is both portability- and performance-hostile and obscures deterministic behavior.

---

## Rollout strategy

- **PR cadence**: small, incremental PRs (each should compile and pass unit tests).
- **Churn control**: avoid refactors unless required for invariants. Prefer wrappers/adapters first, then delete old APIs.
- **Testing**: targeted unit tests for the new invariants + existing Vulkan tests. Use grep-based “no gaps” checks for sentinel removal.

---

## Milestone plan (PR-by-PR)

### M0 — Pre-flight gates and tracking (1 PR)
**Goal**: ensure each PR is self-validating and easy to review.

**Changes**
- Add/update a short checklist in this file under “Per-PR acceptance gates”.

**Acceptance gates**
- Builds in your standard Vulkan-enabled configuration.
- Unit tests (at minimum): `test_vulkan_shader_layout_contracts`.

---

### M1 — Seal `RecordingFrame` (P0, high leverage, minimal blast radius) (1 PR)
**Goal**: make it impossible to fabricate the recording token outside the owner.

**Changes**
- `code/graphics/vulkan/VulkanFrameFlow.h`
  - Make `RecordingFrame(VulkanFrame&, uint32_t)` **private**.
  - Keep `friend class VulkanRenderer;` (and add specific friends only if truly needed).

**Testing**
- Add a compile-time test that `RecordingFrame` is not constructible from arbitrary code:
  - New test file example: `test/src/graphics/test_vulkan_recordingframe_sealed.cpp`
  - `static_assert(!std::is_constructible_v<graphics::vulkan::RecordingFrame, graphics::vulkan::VulkanFrame&, uint32_t>);`

**Acceptance gates**
- Vulkan build passes.
- New test compiles/passes.

---

### M2 — (Optional) Seal `FrameCtx` provenance (P0/P1) (1 PR)
**Goal**: ensure a `FrameCtx` cannot be forged either; it should be produced only by the boundary (`VulkanGraphics.cpp`) via `currentFrameCtx()` and/or by `VulkanRenderer`.

**Changes**
- `code/graphics/vulkan/VulkanFrameCaps.h`
  - Make `FrameCtx` constructor private and friend the minimal set of producers.
  - Alternatively: keep `FrameCtx` constructible, but ensure all meaningful operations require deeper tokens (`RecordingFrame`, `RenderCtx`).

**Acceptance gates**
- Vulkan build passes.

---

### M3 — Replace synthetic texture-handle sentinels with explicit builtin ownership (P1) (2–3 PRs)
**Goal**: remove negative “fake handles” (`-1000`, etc.) and `-1` initialization sentinels from internal Vulkan logic.

#### M3.1 — Clarify `TextureId` and boundary conversion (small PR)
- `code/graphics/vulkan/VulkanTextureId.h`
  - Update documentation to reflect current reality: `TextureId` represents only bmpman base frames (`>= 0`) and **cannot** represent builtins today.
  - Add an **internal, non-optional constructor/factory** so internal code can produce a `TextureId` without reintroducing deep `std::optional` checks. Example shape:
    - Keep `TextureId::tryFromBaseFrame(int)` as the **boundary** (returns `std::optional`).
    - Add `TextureId::fromBaseFrameUnchecked(int)` (asserts `baseFrame >= 0`) and friend the minimal internal producers (e.g. `VulkanTextureManager`, `VulkanTextureBindings`).

#### M3.2 — Make builtins owned objects (main PR)
- `code/graphics/vulkan/VulkanTextureManager.{h,cpp}`
  - Remove:
    - `kFallbackTextureHandle`, `kDefaultTextureHandle`, `kDefaultNormalTextureHandle`, `kDefaultSpecTextureHandle`
    - `m_fallbackTextureHandle = -1` (and similar) fields
  - Add:
    - `struct BuiltinTextures { VulkanTexture fallback; VulkanTexture defaultBase; VulkanTexture defaultNormal; VulkanTexture defaultSpec; };`
    - `BuiltinTextures m_builtins;` (preferred) or `std::unique_ptr<BuiltinTextures>` if two-phase init is unavoidable.
  - Add builtin descriptor helpers:
    - `vk::DescriptorImageInfo fallbackDescriptor(const SamplerKey&) const;`
    - `vk::DescriptorImageInfo defaultBaseDescriptor(const SamplerKey&) const;`
    - etc.
  - Preserve the **bindless reserved slots contract** from `code/graphics/vulkan/VulkanConstants.h` without relying on synthetic handles:
    - Ensure descriptor sync explicitly writes:
      - slot `kBindlessTextureSlotFallback` → fallback descriptor
      - slots `kBindlessTextureSlotDefaultBase/Normal/Spec` → default descriptors
    - Then apply dynamic/resident texture slot updates for slots `>= kBindlessFirstDynamicTextureSlot`.
    - This avoids “defaults silently becoming fallback” if the dynamic slot map does not mention reserved slots.

- Update users:
  - `code/graphics/vulkan/VulkanTextureBindings.h`
  - `code/graphics/vulkan/VulkanRenderer.cpp` (bindless update + any default-texture descriptor utilities)

#### M3.3 — Delete remaining synthetic-handle references (cleanup PR)
**Search gate (“no gaps”)**
- In `code/graphics/vulkan/`, no remaining references to:
  - `kFallbackTextureHandle`, `kDefaultTextureHandle`, `kDefaultNormalTextureHandle`, `kDefaultSpecTextureHandle`
  - “uninitialized handle == -1” patterns for builtins
  - branches that exist solely to distinguish builtin textures by negative handle

**Acceptance gates**
- Vulkan build passes.
- Vulkan tests pass.

---

### M4 — Make “resident descriptor” unforgeable (P1) (2 PRs)
**Goal**: stop representing residency via “empty descriptors” and deep `if (imageView)` branching.

#### M4.1 — Split resident-only vs draw-path descriptor APIs
- `code/graphics/vulkan/VulkanTextureManager.{h,cpp}`
  - Replace (or wrap) `getTextureDescriptorInfo(int, SamplerKey)` with:
    - `vk::DescriptorImageInfo residentDescriptor(TextureId, const SamplerKey&) const;` (asserts if not resident)
    - `vk::DescriptorImageInfo drawDescriptor(TextureId, uint32_t frameIndex, const SamplerKey&);`
      - Always returns a valid descriptor (resident or fallback).
      - Queues upload when needed (unless unavailable).

- `code/graphics/vulkan/VulkanTextureBindings.h`
  - Switch `descriptor(TextureId, ...)` to call `drawDescriptor` and remove `if (info.imageView)` branching.

#### M4.2 — Bindless update consumes resident descriptors only
- Ensure `appendResidentBindlessDescriptors(...)` returns a form that guarantees residency:
  - e.g. `vector<pair<slot, TextureId>>` where `TextureId` is known-resident by construction (container membership).
- Update `VulkanRenderer::updateModelDescriptors` to call `residentDescriptor(...)` (no empty descriptors possible).

**Search gate**
- No deep `if (info.imageView)` checks in Vulkan draw path; residency branching is limited to boundary APIs (`drawDescriptor`).

**Acceptance gates**
- Vulkan build passes.
- Vulkan tests pass.

---

### M5 — Typed bindless slot indices + pressure behavior (P1/P2) (1–2 PRs)
**Goal**: encode “fallback is always valid” without using `0` as a magical sentinel in internal logic.

**Changes**
- Introduce `BindlessSlotIndex` type (wrapper over `uint32_t` with bounds checking).
- Update internal APIs to return/accept `BindlessSlotIndex` where practical (manager/renderer), while keeping engine-facing API as `uint32_t` if needed.
- Add unit tests for:
  - pinned slot behavior (render targets should not be evicted)
  - safe eviction constraints (`lastUsedSerial <= completedSerial`)
  - pressure behavior: returns fallback and retries later

**Acceptance gates**
- Vulkan build + unit tests pass.

---

### M6 — Remove session-side `dynamic_cast` by tokenizing deferred transitions (P2) (1–2 PRs)
**Goal**: if deferred call order is enforced, session shouldn’t need runtime type checks.

**Changes**
- Introduce session-owned tokens:
  - `DeferredGeometryPass` (move-only)
  - `DeferredLightingPass` or “ready” token for the next phase
- Thread tokens through `VulkanRenderer` deferred API (or replace the remaining runtime assertions where redundant).
- Delete `dynamic_cast` check in `VulkanRenderingSession::endDeferredGeometry`.

**Acceptance gates**
- Vulkan build passes.
- Vulkan tests pass.

---

### M7 — Concentrate boundary checks in `VulkanGraphics.cpp` (P2) (1 PR)
**Goal**: keep runtime “is backend/recording present?” checks at the boundary only.

**Changes**
- Add `requireBackend()` / `tryFrameCtx()` style helpers and replace repeated boilerplate checks.
- Avoid adding new deep checks in renderer/managers.

**Acceptance gates**
- Vulkan build passes.

---

### M8 — Debug logging and hot-path hygiene (P2) (1 PR)
**Goal**: remove absolute-path file writes and header-based I/O in hot paths.

**Changes**
- Replace file I/O logging with existing engine logging or `tracing` (and gate it under debug or a runtime flag).
- Move any remaining logging out of headers into `.cpp` where possible.
  - Known offenders to cover explicitly:
    - `code/graphics/vulkan/VulkanTextureBindings.h` (header hot path)
    - `code/graphics/util/UniformBufferManager.cpp` (absolute-path debug log writes)

**Search gate**
- No `\\.cursor\\debug.log` absolute-path writes in `code/`.

---

### M9 — VMA migration (no gaps) (P2/P3, multi-PR track)
**Goal**: migrate Vulkan allocations to VMA as described in `docs/VMA_PLAN.md`, without changing higher-level semantics.

**Approach**
- Execute `docs/VMA_PLAN.md` phases:
  - Add VMA TU (`VulkanVma.cpp`) and config/fwd headers.
  - Add allocator lifetime to `VulkanDevice`.
  - Migrate `VulkanRingBuffer`/`VulkanFrame`.
  - Migrate `VulkanBufferManager`.
  - Migrate `VulkanTextureManager` and `VulkanRenderTargets`.

**Search gate (“no gaps”)**
- After completion, `code/graphics/vulkan/` contains no remaining `allocateMemory`, `bindBufferMemory`, `bindImageMemory` sites for backend-owned allocations.

---

## Per-PR acceptance gates (standard)

Each PR must satisfy:
- **Build**: Vulkan-enabled build succeeds (use your standard Windows build configuration).
- **Unit tests**:
  - Minimum: `test_vulkan_shader_layout_contracts`
  - Plus any new tests added in the PR.
- **No-regression grep gates** (when applicable): sentinel removal and “no gaps” checks described in milestones.

---

## Final “Definition of Done”

This remediation is complete when:
- Recording and render/upload phases are token-proven; `RecordingFrame` cannot be fabricated.
- Builtin textures are owned objects, not synthetic ints; no sentinel identity is used internally.
- Internal descriptor APIs never return “empty info”; draw-path always receives a valid descriptor (resident or fallback) without deep inhabitant-branching.
- Deferred transitions are tokenized end-to-end; session no longer uses runtime `dynamic_cast` to validate state.
- Debug logging is portable and not in header hot paths.
- (If pursuing M9) VMA migration is complete with “no gaps” verification.


