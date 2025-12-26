# Vulkan Architecture Refactoring Plan

Tracks remaining architectural work for correctness, phase enforcement, and type-driven invariants.

## Goals

- Remove correctness hazards in frame/descriptor/swapchain handling.
- Enforce phase validity via capability tokens per `docs/DESIGN_PHILOSOPHY.md`.
- Eliminate "failure as state" patterns (UnavailableTexture, retry loops).
- Keep changes localized and reviewable.

## Guiding Invariants

- **Phase separation**: upload-only operations are uncallable from render-only paths (capability tokens).
- **No fake optionals**: internal APIs do not return empty descriptors; boundaries may return optionals.
- **State as location**: queued/resident/retired tracked by container membership, not state flags.
- **Failure is absence**: failed objects do not exist as structs; absence from container indicates failure.
- **Boundary checks only**: `VulkanGraphics.cpp` may validate backend/recording; deep logic consumes valid tokens.

---

## Descriptor and Sync Hazards

### 1. Per-frame global descriptor sets

- **Current**: single `m_globalDescriptorSet` updated every frame (race: update while in-flight).
- **Fix**: allocate `kFramesInFlight` global sets, update/bind only current frame.
- **Touchpoints**: `VulkanRenderer.h`, `VulkanRenderer.cpp`, `VulkanDescriptorLayouts.h`, `VulkanDescriptorLayouts.cpp`.

### 2. Model UBO descriptor refresh on buffer recreation

- **Current**: `DynamicUniformBinding` tracks only handle + offset; descriptor updates skipped if handle unchanged.
- **Fix**: track buffer generation or resolved `vk::Buffer` and refresh descriptor on change.
- **Touchpoints**: `VulkanFrame.h`, `VulkanBufferManager.h`, `VulkanBufferManager.cpp`, `VulkanRenderer.cpp`.

### 3. Swapchain layout tracking reset on recreation

- **Current**: `m_swapchainLayouts` only resets when image count changes.
- **Fix**: track swapchain generation in `VulkanDevice` and reset layouts when generation changes.
- **Touchpoints**: `VulkanDevice.h`, `VulkanDevice.cpp`, `VulkanRenderingSession.h`, `VulkanRenderingSession.cpp`.

### 4. Layout-aware swapchain barriers

- **Current**: `transitionSwapchainToAttachment` and `transitionSwapchainToPresent` use fixed src stage/access masks.
- **Fix**: use `stageAccessForLayout(oldLayout)` for src stage/access.
- **Touchpoints**: `VulkanRenderingSession.cpp`.

### 5. Remove sentinel acquireImage

- **Current**: `acquireImage()` returns `UINT32_MAX` on failure.
- **Fix**: remove or make private; use `acquireImageOrThrow` or a typestate token at the boundary.
- **Touchpoints**: `VulkanRenderer.h`, `VulkanRenderer.cpp`, `VulkanFrameFlow.h`.

### 6. Eliminate empty descriptor returns in draw path

- **Current**: `getTextureDescriptorInfo()` returns empty `vk::DescriptorImageInfo` on miss; callers branch on `imageView`.
- **Fix**: split APIs:
  - `residentDescriptor(TextureId, SamplerKey)` asserts residency.
  - `drawDescriptor(TextureId, frameIndex, SamplerKey)` always returns valid descriptor (resident or fallback).
- **Search gate**: no deep `if (info.imageView)` checks in draw paths.

### 7. Remove session-side dynamic_cast in deferred transitions

- **Current**: `VulkanRenderingSession::endDeferredGeometry()` uses runtime type checks.
- **Fix**: thread deferred tokens through session or make transitions uncallable outside deferred geometry.
- **Touchpoints**: `VulkanRenderingSession.cpp`, `VulkanRenderer.cpp`.

---

## Capability Token Gaps

### 8. Finish RenderCtx threading

- **Current**: several draw helpers call `ensureRenderingStarted()` internally instead of requiring a `RenderCtx`.
- **Fix**: move `ensureRenderingStarted()` to boundary call sites and pass `RenderCtx` through draw helpers.
- **Touchpoints**: `VulkanRenderer.cpp`, `VulkanGraphics.cpp`.

### 9. Buffer operations without token

- **Current**: `updateBufferData()`, `mapBuffer()`, `flushMappedBuffer()` lack phase tokens.
- **Fix**: add `const FrameCtx&` parameter (or determine phase-independent).
- **Touchpoints**: `VulkanRenderer.h`, `VulkanRenderer.cpp`, `VulkanGraphics.cpp`.

### 10. Uniform binding uses VulkanFrame& instead of FrameCtx

- **Current**: `setModelUniformBinding(VulkanFrame&, ...)`, `setSceneUniformBinding(VulkanFrame&, ...)`.
- **Fix**: change to `const FrameCtx&`.
- **Touchpoints**: `VulkanRenderer.h`, `VulkanRenderer.cpp`.

### 11. Seal FrameCtx provenance (optional)

- Make `FrameCtx` constructor private and friend minimal producers.
- **Touchpoints**: `VulkanFrameCaps.h`, `VulkanGraphics.cpp`.

### 12. Concentrate boundary checks in VulkanGraphics.cpp

- Replace repeated backend/recording checks with helpers like `requireBackend()` / `tryFrameCtx()`.
- Avoid new deep checks in renderer/managers.

---

## Texture Manager Refactor

See `docs/PLAN_REFACTOR_TEXTURE_MANAGER.md` for the detailed 8-phase plan covering:

- **Phase 1**: `TextureId` adoption (eliminate raw int handles internally)
- **Phase 2**: Unique pending upload queue
- **Phase 3**: Remove `UnavailableReason` enum (failure is absence)
- **Phase 4**: Phase-strict bindless slot allocation (draw path becomes const/lookup-only)
- **Phase 5**: Render target ownership unification
- **Phase 6**: Submission cleanup (replace `waitIdle` with fence waits)
- **Phase 7**: Descriptor API cleanup (no fake empty descriptors)
- **Phase 8**: Documentation and tests

---

## Optional / Multi-PR

### 16. VMA migration

- Follow `docs/PLAN_VMA.md` phases for allocator lifetime and backend allocations.
- **Search gate**: no backend-owned allocations using `allocateMemory`, `bindBufferMemory`, `bindImageMemory`.

---

## Validation

- Build: Vulkan-enabled configuration.
- Unit tests: `test_vulkan_shader_layout_contracts` plus any new tests.
- Grep gates: no deep `if (info.imageView)` checks, no retry loops in texture manager.
- Validation layers: swapchain resize, deferred passes, RTT rendering.
