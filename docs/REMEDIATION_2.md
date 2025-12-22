# Vulkan Remediation Plan (Type-Driven)

## Goals
- Eliminate undefined behavior and frame-to-frame state hazards in the Vulkan renderer.
- Encode rendering phase and resource lifetime invariants into types to make invalid usage uncompilable.
- Remove sentinel values and implicit state where the type system can carry the proof instead.
- Align implementation with the design principles in `docs/DESIGN_PHILOSOPHY.md` (typestate, state-as-location, capability tokens, ownership).

## Scope
- Vulkan backend in `code/graphics/vulkan` (renderer, device, frame flow, render session, texture/buffer managers, descriptor layouts).
- Frame lifecycle and descriptor binding, swapchain layout tracking, deferred lighting passes.

## Non-Goals
- Rewriting the entire renderer or changing shader behavior.
- Broad performance rework unless required to preserve correctness.

## Current Risks (Summary)
- Per-frame descriptor state is stored in single global descriptors, leading to updates while previous frames may still use them.
- Model UBO descriptor updates are skipped when the handle is unchanged, even if the underlying `VkBuffer` was recreated (e.g., resize during a frame).
- Swapchain image layout tracking can persist across swapchain recreation when image count is unchanged.
- Swapchain attachment/present transitions use a fixed `srcStage` that ignores the actual prior layout, risking missing hazards when loading.
- Legacy sentinel return values (e.g., `UINT32_MAX`) still exist in `acquireImage()`, even though the active frame flow uses throwing APIs.

### Concrete touchpoints (files)
- `code/graphics/vulkan/VulkanRenderer.h` + `code/graphics/vulkan/VulkanRenderer.cpp`: `m_globalDescriptorSet`, `bindDeferredGlobalDescriptors()`, `recordDeferredLighting()`, `acquireImage()`.
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`: global descriptor pool `maxSets = 1`, `descriptorCount = 6`.
- `code/graphics/vulkan/VulkanFrame.h`: `DynamicUniformBinding` only tracks handle + offset.
- `code/graphics/vulkan/VulkanRenderingSession.h` + `code/graphics/vulkan/VulkanRenderingSession.cpp`: `m_swapchainLayouts` is not keyed to swapchain generation.

## Status Notes (Current Code)
- Recording is already gated by `RecordingFrame` with a private `cmd()` accessor; `FrameCtx` is the rendering capability token.
- Deferred lighting ordering is already enforced by `DeferredGeometryCtx` → `DeferredLightingCtx`, and call sites use a variant state machine.
- Upload flushing is already gated by `UploadCtx` (private constructor) and required by `flushPendingUploads`.
- Shader-read barriers and descriptor stage flags are fragment-only today; only broaden them if vertex/compute sampling is introduced.

## Principles Applied
- **Typestate**: represent frame state transitions (`Acquired` -> `Recording` -> `Submitted`) as distinct types.
- **Capability tokens**: only allow upload or rendering operations while holding explicit phase tokens.
- **State as location**: track swapchain image layout state in a container keyed by generation, not via ad-hoc flags.
- **Ownership**: avoid retry loops or implicit global state; phase state is owned by the renderer and passed explicitly.

## Remediation Phases

### Phase 1 - Correctness Hotfixes (Low Risk, High Impact)
**1) Per-frame global descriptor sets**
- Replace single `m_globalDescriptorSet` with an array sized by `kFramesInFlight`.
- Allocate one global set per frame in `VulkanDescriptorLayouts`.
- Update and bind only the current frame's global set during deferred lighting.
- Resize the global descriptor pool to `kFramesInFlight` sets and `6 * kFramesInFlight` sampled-image descriptors.

Concrete changes (files + methods):
- `code/graphics/vulkan/VulkanDescriptorLayouts.h`: add `allocateGlobalSets()` (returns `std::array<vk::DescriptorSet, kFramesInFlight>` or take a count).
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`: grow pool sizes (`maxSets = kFramesInFlight`, `descriptorCount = 6 * kFramesInFlight`) and allocate one set per frame.
- `code/graphics/vulkan/VulkanRenderer.h`: replace `vk::DescriptorSet m_globalDescriptorSet{}` with `std::array<vk::DescriptorSet, kFramesInFlight> m_globalDescriptorSets{}`.
- `code/graphics/vulkan/VulkanRenderer.cpp`: allocate in `createDescriptorResources()`, update `bindDeferredGlobalDescriptors(uint32_t frameIndex)` to use `m_globalDescriptorSets[frameIndex]`, and pass `rec.ref().frameIndex()` or `frame.frameIndex()` from `recordDeferredLighting()`.
- `code/graphics/vulkan/VulkanRenderer.cpp`: all `vk::WriteDescriptorSet` writes for global bindings should target the current frame's set.

**2) Model UBO descriptor refresh on buffer recreation**
- Track a buffer generation or the resolved `VkBuffer` pointer in `DynamicUniformBinding`.
- On `setModelUniformBinding`, update the descriptor if the generation or `VkBuffer` changes.

Concrete changes (files + methods):
- `code/graphics/vulkan/VulkanBufferManager.h`: add per-buffer `uint64_t generation` (or a parallel metadata vector) and an accessor `getBufferGeneration(gr_buffer_handle)`.
- `code/graphics/vulkan/VulkanBufferManager.cpp`: increment generation whenever the underlying `vk::Buffer` is recreated (`resizeBuffer`, `ensureBuffer` path that allocates).
- `code/graphics/vulkan/VulkanFrame.h`: extend `DynamicUniformBinding` to include `uint64_t bufferGeneration` (or `vk::Buffer resolvedBuffer`) and reset it in `resetPerFrameBindings()`.
- `code/graphics/vulkan/VulkanRenderer.cpp`: in `setModelUniformBinding`, compare both `bufferHandle` and `bufferGeneration` (or `resolvedBuffer`) before deciding to skip a descriptor update.

**3) Swapchain layout reset on recreation**
- Track swapchain generation in `VulkanDevice`; increment on swapchain create/recreate.
- In `VulkanRenderingSession::beginFrame`, reset `m_swapchainLayouts` if the generation changed.

Concrete changes (files + methods):
- `code/graphics/vulkan/VulkanDevice.h/.cpp`: add `uint64_t m_swapchainGeneration` + `uint64_t swapchainGeneration() const` and increment after swapchain (re)creation completes.
- `code/graphics/vulkan/VulkanRenderingSession.h/.cpp`: store `uint64_t m_seenSwapchainGeneration = 0`.
- `code/graphics/vulkan/VulkanRenderingSession.cpp`: in `beginFrame()`, if `m_seenSwapchainGeneration != device.swapchainGeneration()` or `m_swapchainLayouts.size() != device.swapchainImageCount()`, reset with `m_swapchainLayouts.assign(count, vk::ImageLayout::eUndefined)` and update `m_seenSwapchainGeneration`.

**4) Layout-aware swapchain barriers**
- Use `stageAccessForLayout(oldLayout)` for `srcStage/srcAccess` in `transitionSwapchainToAttachment()` and `transitionSwapchainToPresent()`.
- Note: `captureSwapchainColorToSceneCopy()` already uses layout-aware barriers.

Concrete changes (files + methods):
- `code/graphics/vulkan/VulkanRenderingSession.cpp`: read `oldLayout = m_swapchainLayouts[imageIndex]`, compute `src = stageAccessForLayout(oldLayout)`, and apply `src.stage`/`src.access` in the barrier.
- `code/graphics/vulkan/VulkanRenderingSession.cpp`: after each transition, write `m_swapchainLayouts[imageIndex] = newLayout` to keep the tracking authoritative.

**5) Shader-read barrier stage coverage**
- Only broaden shader-read stage masks if vertex/compute shaders actually sample textures/depth; keep fragment-only if not needed.

Concrete changes (files + methods):
- Confirm current usage: if G-buffer/depth is only sampled in fragment shaders, keep `vk::ShaderStageFlagBits::eFragment`.
- If vertex/compute sampling is introduced, update:
  - `code/graphics/vulkan/VulkanRenderingSession.cpp` + `code/graphics/vulkan/VulkanTextureManager.cpp`: `stageAccessForLayout(vk::ImageLayout::eShaderReadOnlyOptimal)` should include `eVertexShader`/`eComputeShader` as needed.
  - `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`: any relevant `DescriptorSetLayoutBinding::stageFlags` for sampled resources (global bindings and bindless textures).

### Phase 2 - Typestate Frame Flow
**Objective:** Make invalid frame sequencing uncompilable.

**1) Replace sentinel returns with typestate tokens**
- Remove or stop using the legacy `acquireImage()` path that returns `UINT32_MAX` (already private).
- Optionally introduce `AcquiredSwapchainImage` and return `std::optional` from the boundary if you prefer non-throwing flow.

Concrete changes (files + methods):
- Define a token type in `code/graphics/vulkan/VulkanFrameFlow.h`:
  ```cpp
  struct AcquiredSwapchainImage {
    VulkanFrame& frame;
    uint32_t imageIndex;
  };
  ```
- `code/graphics/vulkan/VulkanRenderer.h/.cpp`: make `acquireImage()` private-only or remove it; expose `AcquiredSwapchainImage acquireImageOrThrow(VulkanFrame&)` (or `std::optional<AcquiredSwapchainImage>` if you want a non-throwing boundary).
- `code/graphics/vulkan/VulkanRenderer.cpp`: `beginRecording()` should take/produce the token, e.g. `RecordingFrame beginRecording(AcquiredSwapchainImage acquired)`.
- `code/graphics/vulkan/VulkanGraphics.cpp`: thread the token through the boundary state (`std::optional<RecordingFrame> recording`) and early-return if acquisition fails (optional flow) or let exceptions propagate (throwing flow).

**2) Make recording state explicit**
- Status: already enforced by `RecordingFrame` and `FrameCtx`. No change needed unless you want a narrower RecordingScope wrapper.

### Phase 3 - Capability Tokens for Phases
**Objective:** Encode phase validity in types.

**1) Upload phase token**
- Status: already enforced via `UploadCtx` with a private constructor and `flushPendingUploads(const UploadCtx&)`.
- Only add a separate `UploadPhase` token if you want a distinct type from the context payload.

Concrete changes (files + methods):
- Audit that all upload-entry functions accept an `UploadCtx` or derived token:
  - `code/graphics/vulkan/VulkanTextureManager.h` + `code/graphics/vulkan/VulkanTextureManager.cpp`
  - `code/graphics/vulkan/VulkanTextureBindings.h` + `code/graphics/vulkan/VulkanTextureBindings.cpp`
- If a new token is introduced, keep it zero-sized and non-copyable to make it a pure capability.

**2) Deferred pass tokens**
- Status: already enforced by `DeferredGeometryCtx`/`DeferredLightingCtx` and by the VulkanGraphics variant state machine.

### Phase 4 - State-as-Location Cleanup
**Objective:** Remove implicit state that can drift.

- If Phase 1 #3 is applied, swapchain layout reset is already keyed by generation; no further work needed here.
- Move any flags that represent state into the container where membership is proof (e.g., `pending` vs `resident`).

Concrete changes:
- `code/graphics/vulkan/VulkanRenderingSession.cpp`: only if you want a generation-indexed container beyond the Phase 1 reset.
- Audit other subsystems for state enums; texture manager already uses state-as-location (resident/unavailable/pending containers).
- Quick audit commands:
  - `rg -n "enum State|Pending|isInitialized|UINT32_MAX|-1" code/graphics/vulkan`
  - `rg -n "std::optional<.*>.*state" code/graphics/vulkan`

### Phase 5 - Tests and Validation
**1) Validation-layer runs**
- Run Vulkan validation layers with a scenario that exercises:
  - swapchain resize
  - RTT target switches
  - deferred lighting

**2) Integration test scenario**
- Add a minimal integration case in `Testing/` to execute:
  - swapchain recreation
  - deferred pass begin/end
  - RTT creation and mip generation

**3) Debug assertions at boundaries**
- Add one-time boundary asserts where a token is created to detect illegal transitions early during development.

**4) Targeted unit tests**
- Extend `test/src/graphics/test_vulkan_shader_layout_contracts.cpp` (or add a new file under `test/src/graphics/`) to validate:
  - `stageAccessForLayout(vk::ImageLayout::ePresentSrcKHR -> vk::ImageLayout::eColorAttachmentOptimal -> vk::ImageLayout::ePresentSrcKHR)` uses the expected stage/access pairs.
  - `stageAccessForLayout(vk::ImageLayout::eShaderReadOnlyOptimal)` matches the chosen shader stage mask.
- Add a unit test that simulates a buffer resize and asserts the model UBO descriptor refresh triggers when generation changes.

## Implementation Checklist
- Update descriptor layout allocation for per-frame global descriptors.
- Update global descriptor pool sizing for per-frame allocations.
- Update renderer to use frame-indexed descriptors.
- Add buffer generation tracking and bind refresh on change.
- Add swapchain generation tracking and reset layout tracking.
- Fix swapchain barrier source stage/access.
- Update shader-read stage mask + descriptor stageFlags only if vertex/compute sampling is introduced.
- Remove or fence off legacy sentinel-return path (`acquireImage()`).
- Add documentation snippets in `code/graphics/vulkan/README.md` describing new typestate tokens.
- Add/extend unit tests for layout contracts and buffer generation refresh.

## Risk Mitigation
- Phase 1 changes are localized and should be implemented first to remove undefined behavior.
- Typestate changes are API-facing; introduce incrementally with adapter shims to avoid large rewrites.
- Keep runtime asserts only at boundaries; internal code should not need state checks after typestate is in place.

## Rollback Strategy
- Each phase is independently reversible.
- Phase 1 can be reverted file-by-file if issues arise (descriptor array, generation tracking, barrier logic).
- Typestate refactors should be staged behind compile-time toggles to allow fallback if integration issues appear.

## Deliverables
- Correctness hotfix PR (Phase 1)
- Typestate/capability tokens PR (Phase 2-3)
- State-as-location cleanup PR (Phase 4)
- Testing additions PR (Phase 5)

## Success Criteria
- Validation layers show no descriptor set update hazards or layout mismatches during resize/deferred passes.
- No use of sentinel values for frame state.
- Rendering phases are enforced by type signatures, not runtime checks.
- No remaining internal `if (state)` checks that represent invalid states post-typestate refactor.
