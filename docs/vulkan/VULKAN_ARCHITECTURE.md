# Vulkan Architecture (Entry Point)

This document is the entry point for the Vulkan renderer documentation in `docs/vulkan/`. It gives a high-level map of the Vulkan backend, its core invariants, and where to look next depending on what you are changing or debugging.

## Design Constraints (Read Before Editing)

The Vulkan renderer is intentionally structured around the principles in `docs/DESIGN_PHILOSOPHY.md`:

- Correctness by construction: encode phase/order invariants in types and APIs where possible.
- Capability tokens and typestate: make invalid sequencing hard/impossible to compile.
- Boundaries vs internals: do conditional checks at boundaries; keep internals assumption-safe.
- State as location: prefer container membership/variants over scattered state enums/flags.

When you add new rendering features (post-processing, upscalers like DLSS, new targets), prefer:

- explicit target state transitions
- RAII scope guards for pass lifetime
- centralized ownership for resource lifetimes and "previous frame" state

## Terminology (Current Core Concepts)

These names are used across the Vulkan docs and (roughly) correspond to types in the Vulkan renderer:

- RecordingFrame: "we are recording commands for a frame" (frame-in-flight token).
- FrameCtx: a boundary token proving we have a current recording frame and a renderer instance.
- RenderCtx: a capability token proving dynamic rendering is active for a specific target.
- VulkanRenderTargets: owns images/views/samplers for scene, post, bloom, gbuffer, depth, etc.
- VulkanRenderingSession: owns render target selection, dynamic rendering begin/end, and layout transitions.
- ActivePassGuard: RAII guard for dynamic rendering lifetime (begin/end).

If you are unsure which token you should have, start with `VULKAN_RENDER_PASS_STRUCTURE.md`.

## How the Renderer is Put Together (Bird's Eye View)

At a high level:

1) Frame flow:
   - acquire swapchain image
   - record commands
   - submit to graphics queue
   - present

2) Within a frame, rendering is organized around target transitions:
   - swapchain targets (with/without depth)
   - scene HDR targets (with/without depth)
   - deferred G-buffer targets
   - post-processing targets (bloom ping-pong, tonemap LDR, SMAA targets, etc.)
   - bitmap render targets (RTT)

3) Dynamic rendering is used (not classic VkRenderPass objects):
   - we begin/end dynamic rendering scopes explicitly
   - we do explicit sync2 barriers and layout transitions

## "Start Here" Reading Order

If you are new to this renderer:

1) `VULKAN_RENDER_PASS_STRUCTURE.md`
   - target typestates
   - dynamic rendering + RAII pass guard
   - clear ops and frame lifecycle

2) `VULKAN_SYNCHRONIZATION.md`
   - frames-in-flight model
   - timeline/binary semaphores and fences
   - layout transitions and sync2 barrier rules

3) `VULKAN_DESCRIPTOR_SETS.md` and `VULKAN_TEXTURE_BINDING.md`
   - push descriptors vs bindless model binding
   - sampler cache and descriptor validity rules

4) `VULKAN_2D_PIPELINE.md` and `VULKAN_HUD_RENDERING.md`
   - 2D/UI/HUD rendering contracts
   - clip/scissor behavior and invariants

5) `VULKAN_RECENT_FIXES.md`
   - context for recent bugfixes and invariants that were tightened

## "Where Do I Change X?"

- Frame lifecycle / recording / submit / present:
  - Start with `VULKAN_SYNCHRONIZATION.md`

- Adding a new render target or changing target sizes:
  - `VULKAN_RENDER_PASS_STRUCTURE.md` (target types and transitions)
  - `VULKAN_DYNAMIC_BUFFERS.md` (if new transient buffers are needed)

- Adding/adjusting post-processing stages:
  - `pipeline_management.md` (pipeline and shader integration overview)
  - `VULKAN_RENDER_PASS_STRUCTURE.md` (where passes occur and how targets switch)
  - `VULKAN_DESCRIPTOR_SETS.md` (push descriptor bindings and global sets)

- Investigating binding/descriptor bugs:
  - `VULKAN_DESCRIPTOR_SETS.md`
  - `VULKAN_TEXTURE_BINDING.md`

- HUD/UI rendering issues (clip/scissor, overlay order):
  - `VULKAN_HUD_RENDERING.md`
  - `VULKAN_2D_PIPELINE.md`

- Memory pressure / dynamic allocation / ring buffers:
  - `VULKAN_DYNAMIC_BUFFERS.md`

- DLSS / upscaling:
  - `VULKAN_DLSS_PLAN.md` (implementation plan and required invariants)

## Testing and Behavioral Invariants

The Vulkan tests are split between:

- "real" unit tests that exercise components with minimal stubs, and
- behavioral "fake state machine" tests that encode invariants without requiring a GPU.

Entry point:

- `VULKAN_INTEGRATION_TESTS.md`

When changing renderer state machines (target switching, pass boundaries, post-processing enablement),
prefer adding/updating a behavioral invariant test that locks in the expected semantics.

## Document Index

- `VULKAN_RENDER_PASS_STRUCTURE.md`: target typestates, dynamic rendering boundaries, clear ops, frame lifecycle
- `VULKAN_SYNCHRONIZATION.md`: frames-in-flight, submissions, fences/semaphores, sync2 + layout transitions
- `VULKAN_DESCRIPTOR_SETS.md`: descriptor layouts, pools, push descriptors, bindless binding, update safety rules
- `VULKAN_TEXTURE_BINDING.md`: texture lifecycle, sampler cache, bindless slot rules, descriptor validity
- `VULKAN_DYNAMIC_BUFFERS.md`: ring buffers, managed buffers, orphaning, deferred releases
- `VULKAN_2D_PIPELINE.md`: interface pipeline, coordinate systems, 2D draw path contracts
- `VULKAN_HUD_RENDERING.md`: HUD draw order and shaders, clip/scissor, RTT cockpit displays
- `pipeline_management.md`: pipeline/shader management, compilation and caching
- `VULKAN_RECENT_FIXES.md`: recent bug history and the invariants that resulted
- `VULKAN_DLSS_PLAN.md`: upscaling plan (render vs display extents, jitter, motion vectors, pass placement)
- `VULKAN_INTEGRATION_TESTS.md`: test philosophy and key invariant tests

