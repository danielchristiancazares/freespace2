# NVIDIA DLSS (Super Resolution) Integration Plan (Vulkan)

This document describes a concrete, phased implementation plan to integrate NVIDIA DLSS Super Resolution into the FreeSpace Open Vulkan renderer.

This plan is intentionally written to align with the project's "correctness by construction" principles in `docs/DESIGN_PHILOSOPHY.md` and the existing Vulkan renderer architecture docs in `docs/vulkan/`.

## References (Read These First)

Design constraints and vocabulary used below:

- `docs/DESIGN_PHILOSOPHY.md` (capability tokens, typestate, state-as-location, boundary-only conditionals)
- `docs/vulkan/VULKAN_RENDER_PASS_STRUCTURE.md` (dynamic rendering, `ActivePassGuard`, target typestates)
- `docs/vulkan/VULKAN_SYNCHRONIZATION.md` (sync2 barriers, layout transitions, frames-in-flight)
- `docs/vulkan/VULKAN_DESCRIPTOR_SETS.md` and `docs/vulkan/VULKAN_TEXTURE_BINDING.md` (push descriptors, bindless textures, sampler cache)
- `docs/vulkan/VULKAN_HUD_RENDERING.md` (HUD/UI ordering and clip/scissor invariants)

## 0. Scope and Non-Goals

### 0.1 In-Scope

- Vulkan-only DLSS **Super Resolution** (temporal upscaler + TAA) on supported NVIDIA GPUs.
- Decouple **render resolution** (3D scene) from **display resolution** (swapchain/UI).
- Required inputs: motion vectors + projection jitter (+ depth, HDR color).
- Integrate DLSS evaluation as a distinct "compute-like" pass between scene rendering and post-processing.

### 0.2 Non-Goals (Separate Work)

- DLSS Frame Generation (DLSS 3 FG) and Reflex (different integration surface + pacing model).
- OpenGL backend support.
- DLAA-only mode (can be built on top of SR once SR is working).

## 1. Terminology and Invariants

### 1.1 Extents / Resolutions

- DisplayExtent: swapchain size (what the OS presents). HUD/UI coordinates live here.
- RenderExtent: internal 3D size (G-buffer, depth, scene HDR pre-upscale).

Invariant A: HUD/UI must continue rendering at DisplayExtent, never at RenderExtent.

Invariant B: When DLSS is disabled, RenderExtent == DisplayExtent (no behavior change).

### 1.2 Buffers (Conceptual)

DLSS SR consumes:

- ColorInHDR: low-res HDR scene color at RenderExtent
- Depth: low-res depth at RenderExtent
- MotionVectors: low-res motion/velocity at RenderExtent
- JitterOffset: per-frame jitter offset (in pixels or normalized units, per SDK conventions)
- ResetHistory: per-frame flag for camera cuts / discontinuities

DLSS SR produces:

- ColorOutHDR: upscaled HDR scene color at DisplayExtent

Post-processing then consumes ColorOutHDR (bloom, tonemap, post effects), and finally HUD/UI draws on top.

### 1.3 Implementation Constraints from `docs/DESIGN_PHILOSOPHY.md`

1) Boundary-only conditionals:
   - "Is DLSS available?" is decided once at initialization and when settings change.
   - Draw paths should not be littered with `if (dlss)` checks.

2) Capability tokens for phase validity:
   - DLSS evaluate should be callable only with an explicit token proving "resources exist and are ready".

3) State as location:
   - DLSS enabled/disabled is modeled as a state object (variant) owned by the renderer, not scattered bools.

## 2. Current Vulkan Frame Structure (Anchor Points)

The Vulkan renderer uses:

- Dynamic rendering (`vkCmdBeginRendering`/`vkCmdEndRendering`) with RAII (`ActivePassGuard`)
- A rendering session (`VulkanRenderingSession`) that owns target selection and layout transitions
- Capability tokens in code (e.g., `RecordingFrame`, `FrameCtx`, `RenderCtx`) to enforce "valid-in-phase" APIs

DLSS must respect this architecture:

- DLSS evaluation should run outside an active dynamic rendering scope.
- Resource layout transitions should follow the same explicit sync2 discipline as other transitions.

## 3. Proposed High-Level Frame Flow (With DLSS Enabled)

1) Record frame (existing).
2) Opaque geometry at RenderExtent:
   - Write G-buffer attachments (existing deferred path)
   - Write MotionVectors attachment (new; see Section 6)
3) Deferred lighting resolves to ColorInHDR at RenderExtent (existing scene HDR target).
4) Transparency/effects policy (see Section 8):
   - Phase 1: render most "scene-affecting" transparencies into ColorInHDR before upscaling.
5) DLSS evaluation pass (new):
   - End/suspend active dynamic rendering
   - Transition ColorInHDR/Depth/MotionVectors to required layouts
   - Evaluate DLSS into ColorOutHDR at DisplayExtent
6) Post-processing at DisplayExtent:
   - Bloom + tonemap + post effects on ColorOutHDR
7) HUD/UI at DisplayExtent (existing ordering).
8) Present (existing).

When DLSS is disabled:

- RenderExtent == DisplayExtent
- ColorOutHDR is either unused or aliases ColorInHDR (implementation detail)
- Post-processing pipeline remains as it is today

## 4. New Modules and Types (Correctness by Construction)

### 4.1 `DlssManager` as a boundary

Create a Vulkan-only module (suggested location): `code/graphics/vulkan/dlss/`.

Goals:

- Encapsulate all NGX/DLSS SDK calls and vendor-specific requirements.
- Expose a small "engine-friendly" interface to the Vulkan renderer.
- Concentrate all availability checks at the boundary.

### 4.2 DLSS typestate: unavailable vs ready

Model DLSS availability as typestate (variant), not nullable pointers:

```cpp
struct DlssUnavailable {
  SCP_string reason; // one-time log message
};

struct DlssReady {
  // Owns NGX context + feature handle for a specific feature key
};

using DlssState = std::variant<DlssUnavailable, DlssReady>;
```

Invariant: once you hold a `DlssReady&`, DLSS calls are valid; no further "is available?" checks are needed.

### 4.3 `DlssFeatureKey` (defines recreation boundaries)

DLSS features must be recreated when any of these changes:

- RenderExtent
- DisplayExtent
- DLSS quality mode (Quality/Balanced/Performance/UltraPerformance)
- HDR pipeline enablement (if DLSS configuration differs)
- Any SDK preset selection we decide to expose (optional in Phase E)

Represent this as a single struct:

```cpp
struct DlssFeatureKey {
  vk::Extent2D renderExtent;
  vk::Extent2D displayExtent;
  DlssQualityMode mode;
  bool hdr = false;
};
```

### 4.4 DLSS evaluation capability token

Create a token that proves all prerequisites for evaluation:

- A `DlssReady` exists for the current `DlssFeatureKey`
- Motion vectors are available for this frame
- Jitter offset computed for this frame
- Required layout transitions have been applied for the inputs and output

This should be constructed only by the renderer boundary immediately before the evaluate call. All internal DLSS evaluate functions require this token.

## 5. Resolution Decoupling (Render vs Display)

This is required even before the DLSS SDK integration is useful.

### 5.1 Resolution policy

Introduce a single "resolution policy" that computes RenderExtent from DisplayExtent + mode scale.

Rules:

- Clamp to at least 1x1.
- Preserve aspect ratio.
- Respect any alignment constraints discovered from the DLSS SDK (treat those as boundary data, not magic constants).
- Recompute when display resolution changes (swapchain recreate) or DLSS mode changes.

### 5.2 Render target ownership changes

`VulkanRenderTargets` must become explicitly dual-extent:

- RenderExtent targets:
  - G-buffer attachments
  - Depth attachment (main + cockpit if needed)
  - Scene HDR input (ColorInHDR)
  - MotionVectors (new)
- DisplayExtent targets:
  - DLSS output HDR (ColorOutHDR)
  - Post-processing intermediates that assume display resolution
  - Swapchain (existing)

### 5.3 Render session/target typestates

Extend `VulkanRenderingSession` target selection so "render to low-res scene HDR" and "render to display-res post-processing" are distinct, explicit target states (like existing SwapchainWithDepth vs SceneHdrWithDepth patterns).

Invariant: The dynamic rendering renderArea must match the active target extent.

## 6. Motion Vectors (Velocity Buffer)

### 6.1 Buffer format and meaning

Default format: `R16G16_SFLOAT`.

If SDK requires higher precision, upgrade to `R32G32_SFLOAT` (make this a feature flag decided at boundary after querying SDK requirements).

Define one motion-vector convention and stick to it:

- Recommended internal representation: NDC delta (current - previous) using *unjittered* projections.
- Convert to DLSS-required units (if different) only at the DLSS boundary.

### 6.2 Where motion vectors are generated

Primary producer: deferred G-buffer pass (opaque geometry).

This avoids "special case motion vectors everywhere"; most pixels come from opaque geometry.

### 6.3 Previous-transform cache (renderer-owned)

We need per-object previous-frame transforms for correct object motion (ships, weapons, debris, rotating submodels).

Design goal (state as location + boundary ownership):

- The renderer owns a cache mapping a stable RenderKey -> previous model matrix.
- Cache is updated once per frame, centrally, without scattering "prev" fields across unrelated systems.

Concrete plan:

1) Define a RenderKey that uniquely identifies a draw's transform history:
   - object instance id
   - submodel id (if drawn independently)
   - additional disambiguator if a single object issues multiple draws with different transforms

2) At draw submission:
   - compute current model matrix (existing)
   - look up prev model matrix (if missing: use current, yielding zero motion for first appearance)
   - send both current and previous transform to the shader path that writes motion vectors

3) After the draw:
   - update cache[RenderKey] = current model matrix

4) Cache lifecycle:
   - on mission start / scene load / large camera discontinuity: clear cache (or mark resetHistory)
   - on object destruction: allow entry to expire (optional: compact periodically)

### 6.4 Camera cuts / resets

DLSS must be told when history is invalid.

Boundary signal sources:

- Mission start/load
- Cutscenes / viewpoint mode changes
- RenderExtent/DisplayExtent changes (swapchain recreate or mode change)

Implementation plan:

- Produce a per-frame ResetHistory flag at the renderer boundary.
- Thread it into DLSS evaluation parameters.

## 7. Projection Jitter

### 7.1 Jitter sequence

Implement a deterministic jitter generator (e.g., Halton base 2/3) keyed by frame index modulo a fixed period.

Output:

- jitterPx: jitter in pixel units relative to RenderExtent, typically within (-0.5 .. +0.5)

### 7.2 Jittered vs unjittered matrices

Do not replace the global projection with jittered projection without preserving the unjittered version.

Maintain:

- unjittered projection (for culling + stable motion-vector math)
- jittered projection (for rasterization)

This reduces invalid state propagation: systems that should not "know about DLSS" can keep using the unjittered projection.

## 8. Transparency, Particles, and HUD Policy

### 8.1 HUD/UI

HUD/UI must not feed into DLSS input; it stays at DisplayExtent (consistent with `docs/vulkan/VULKAN_HUD_RENDERING.md`).

### 8.2 Transparency/effects (Phase decisions)

Phase 1 (simple and consistent):

- Render most scene-affecting transparencies into low-res ColorInHDR before upscaling, so they are temporally stable with the scene.

Phase 2 (quality improvement):

- Add an optional reactive/transparency mask if the SDK supports it and we see ghosting around particles.

## 9. DLSS Evaluation Pass (Vulkan Integration Details)

### 9.1 Placement

Place DLSS evaluation:

- After low-res ColorInHDR is complete
- Before post-processing that assumes display-res input

### 9.2 Dynamic rendering boundaries

DLSS evaluate should not run inside an active dynamic rendering scope.

Plan:

- Call `VulkanRenderingSession::suspendRendering()` (or equivalent) before DLSS evaluate.
- Do not rely on "it happens to work"; make the boundary explicit.

### 9.3 Layout transitions (sync2)

Add explicit transitions for DLSS inputs/outputs, consistent with `docs/vulkan/VULKAN_SYNCHRONIZATION.md`.

Inputs:

- ColorInHDR: color attachment -> shader read
- Depth: depth attachment -> shader read
- MotionVectors: color attachment -> shader read

Output:

- ColorOutHDR: ensure layout suitable for DLSS writes (likely general/storage; exact requirement is SDK-defined)

After evaluate:

- Transition ColorOutHDR to shader read for post-processing, or directly to color attachment if the next step renders into it.

Important correctness rule:

- The DLSS module should not "guess" current image layouts. The renderer boundary transitions resources and then hands them to DLSS evaluate with the capability token.

## 10. Post-Processing and AA Interactions

### 10.1 Anti-aliasing mode interactions

DLSS SR includes temporal AA; running SMAA/FXAA on top is usually redundant and may harm quality.

Plan:

- When DLSS is enabled, force AA mode to "None" (or expose "DLSS handles AA") and disable FXAA/SMAA post passes.
- Optional later: allow a sharpening-only post step, if desired.

### 10.2 Post-processing resolution

Once DLSS is enabled:

- Bloom/tonemap/post effects should operate on ColorOutHDR at DisplayExtent.

## 11. Texture LOD Bias

Lower render resolution changes mip selection; without LOD bias, textures can look overly blurry.

Plan:

- Add a global LOD bias derived from renderScale at the renderer boundary.
- Extend Vulkan sampler caching to include a quantized mipLodBias component in the sampler key (avoid raw float keys).
- Ensure descriptor validity rules remain satisfied (all descriptor slots always valid, per `docs/vulkan/VULKAN_TEXTURE_BINDING.md`).

## 12. Build, Packaging, and Runtime Availability

These are boundary concerns; do not leak them into core draw code.

### 12.1 Build-time

- Add a CMake option (e.g., `FSO_ENABLE_DLSS`) default OFF.
- Do not commit proprietary SDK binaries/headers into the repo.
- When enabled, require developer-supplied SDK path(s).

### 12.2 Runtime

DLSS is "Ready" only if:

- GPU is NVIDIA + DLSS-capable
- required runtime components are present
- feature creation succeeds for current `DlssFeatureKey`

Otherwise:

- Log one clear reason (once).
- Continue without DLSS (RenderExtent == DisplayExtent).

## 13. Testing and Validation

### 13.1 Debug visualizations

- Motion vector view:
  - camera pan over static geometry produces coherent vectors
  - moving ships produce vectors consistent with their motion
- Jitter view:
  - with DLSS disabled but jitter enabled, scene should wobble subpixel (< 1 px)

### 13.2 Automated tests (Vulkan-focused)

- Unit test: jitter sequence determinism and bounds.
- Integration test: render-target resize logic maintains valid extents (no 0-sized images) and correct allocation split (render vs display).
- Optional: GPU test that the velocity attachment is writable and sampled (format/usage).

## 14. Phased Implementation Checklist (Concrete Tasks)

### Phase A: Resolution decoupling without DLSS

Goal: prove plumbing without introducing NGX complexity.

1) Add a `ResolutionState` (DisplayExtent, RenderExtent) owned by the Vulkan renderer boundary.
2) Teach `VulkanRenderTargets` to allocate dual-extent resources.
3) Add a simple non-DLSS upsample/copy step (bilinear) to map RenderExtent scene HDR to DisplayExtent.
4) Ensure HUD/UI and post-processing still behave correctly.

Exit criteria:

- RenderExtent can be set to a smaller size and the game remains visually correct (minus expected quality loss).

### Phase B: Motion vectors + jitter (still no DLSS)

1) Add MotionVectors render target and wiring into the deferred geometry pass.
2) Implement previous-transform cache keyed by RenderKey.
3) Implement jitter generator + jittered/unjittered projection split.
4) Add debug visualizations.

Exit criteria:

- Velocity buffer looks correct in debug view.
- Jitter wobble behaves as expected.

### Phase C: DLSS module integration (boundary only)

1) Add `DlssManager` module and `FSO_ENABLE_DLSS` build option.
2) Initialize SDK context at startup; create `DlssState` (Unavailable/Ready).
3) Implement feature (re)creation keyed by `DlssFeatureKey`.

Exit criteria:

- On supported hardware, feature initializes and logs success.
- On unsupported hardware, logs a clear reason once and continues.

### Phase D: DLSS evaluation pass integration

1) Insert DLSS evaluate pass after low-res scene HDR completion and before post-processing.
2) Implement explicit layout transitions.
3) Wire inputs (ColorInHDR/Depth/MotionVectors/Jitter/ResetHistory) and output (ColorOutHDR).
4) Route post-processing to use ColorOutHDR.

Exit criteria:

- DLSS mode toggles on/off correctly.
- No validation errors; stable output in motion.

### Phase E: Quality options, LOD bias, and polish

1) Add UI/config options:
   - enable/disable DLSS
   - quality mode selection
   - optional sharpening control (if desired)
2) Implement sampler LOD bias keyed by render scale.
3) Optional reactive mask integration (only if artifacts warrant it).

## 15. Code Areas Expected to Change (Non-Exhaustive)

- `code/graphics/vulkan/VulkanRenderTargets.*` (dual extents; motion vectors; DLSS output HDR)
- `code/graphics/vulkan/VulkanRenderingSession.*` (target selection + transitions)
- `code/graphics/vulkan/VulkanRenderer.*` (frame orchestration; jitter; history reset)
- `code/graphics/vulkan/VulkanTextureManager.*` (sampler LOD bias)
- `code/graphics/shaders/*` (motion vector output; potentially new variants)
- New: `code/graphics/vulkan/dlss/*` (isolated SDK integration)

## 16. Open Questions (Explicitly Tracked)

These must be answered during implementation by consulting the specific NGX SDK version we integrate:

1) DLSS input conventions (motion vector units, depth range conventions, jitter representation).
2) Required Vulkan extensions/features for NGX on our Vulkan baseline.
3) Whether exposure is required for stable brightness; if yes, where exposure is computed (engine currently lacks full auto-exposure).
4) How to handle cockpit render-to-texture displays when RenderExtent != DisplayExtent (may need per-RT policy).
