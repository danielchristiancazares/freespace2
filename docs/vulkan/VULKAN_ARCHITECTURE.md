# Vulkan Architecture (Entry Point)

This document is the entry point for the Vulkan renderer documentation in `docs/vulkan/`. It provides a high-level map of the Vulkan backend, its core invariants, and where to look next depending on what you are changing or debugging.

## Design Constraints (Read Before Editing)

The Vulkan renderer is intentionally structured around the principles in `docs/DESIGN_PHILOSOPHY.md`:

- **Correctness by construction**: Encode phase/order invariants in types and APIs where possible.
- **Capability tokens and typestate**: Make invalid sequencing hard/impossible to compile.
- **Boundaries vs internals**: Do conditional checks at boundaries; keep internals assumption-safe.
- **State as location**: Prefer container membership/variants over scattered state enums/flags.

When you add new rendering features (post-processing, upscalers like DLSS, new targets), prefer:

- Explicit target state transitions
- RAII scope guards for pass lifetime
- Centralized ownership for resource lifetimes and "previous frame" state

## Terminology (Core Concepts)

These names are used across the Vulkan docs and correspond to types in the Vulkan renderer:

| Token / Type | Description |
|--------------|-------------|
| `RecordingFrame` | Move-only token proving "we are recording commands for a frame" (frame-in-flight) |
| `FrameCtx` | Boundary token proving we have a current recording frame and a renderer instance |
| `RenderCtx` | Capability token proving dynamic rendering is active for a specific target |
| `VulkanRenderTargets` | Owns images/views/samplers for scene, post, bloom, G-buffer, depth, etc. |
| `VulkanRenderingSession` | Owns render target selection, dynamic rendering begin/end, and layout transitions |
| `ActivePassGuard` | RAII guard for dynamic rendering lifetime (begin/end) |

If you are unsure which token you should have, start with `VULKAN_RENDER_PASS_STRUCTURE.md` or `VULKAN_CAPABILITY_TOKENS.md`.

## How the Renderer is Put Together (Bird's Eye View)

At a high level:

1. **Frame flow**:
   - Acquire swapchain image
   - Record commands
   - Submit to graphics queue
   - Present

2. **Target transitions within a frame**:
   - Swapchain targets (with/without depth)
   - Scene HDR targets (with/without depth)
   - Deferred G-buffer targets
   - Post-processing targets (bloom ping-pong, tonemap LDR, SMAA targets, etc.)
   - Bitmap render targets (RTT)

3. **Dynamic rendering** (not classic VkRenderPass objects):
   - Begin/end dynamic rendering scopes explicitly
   - Explicit sync2 barriers and layout transitions

## "Start Here" Reading Order

If you are new to this renderer:

1. `VULKAN_RENDER_PASS_STRUCTURE.md`
   - Target typestates
   - Dynamic rendering + RAII pass guard
   - Clear ops and frame lifecycle

2. `VULKAN_SYNCHRONIZATION.md`
   - Frames-in-flight model
   - Timeline/binary semaphores and fences
   - Layout transitions and sync2 barrier rules

3. `VULKAN_CAPABILITY_TOKENS.md`
   - Token types and their purposes
   - Token creation, consumption, and lifetime
   - Common patterns and mistakes

4. `VULKAN_DESCRIPTOR_SETS.md` and `VULKAN_TEXTURE_BINDING.md`
   - Push descriptors vs bindless model binding
   - Sampler cache and descriptor validity rules

5. `VULKAN_2D_PIPELINE.md` and `VULKAN_HUD_RENDERING.md`
   - 2D/UI/HUD rendering contracts
   - Clip/scissor behavior and invariants

6. `VULKAN_RECENT_FIXES.md`
   - Context for recent bugfixes and invariants that were tightened

## "Where Do I Change X?"

**Frame lifecycle / recording / submit / present**:
- `VULKAN_SYNCHRONIZATION.md`

**Adding a new render target or changing target sizes**:
- `VULKAN_RENDER_PASS_STRUCTURE.md` (target types and transitions)
- `VULKAN_DYNAMIC_BUFFERS.md` (if new transient buffers are needed)

**Adding/adjusting post-processing stages**:
- `VULKAN_POST_PROCESSING.md` (pipeline flow, target transitions, uniforms)
- `VULKAN_PIPELINE_MANAGEMENT.md` (pipeline and shader integration overview)
- `VULKAN_RENDER_PASS_STRUCTURE.md` (where passes occur and how targets switch)
- `VULKAN_DESCRIPTOR_SETS.md` (push descriptor bindings and global sets)

**Investigating binding/descriptor bugs**:
- `VULKAN_DESCRIPTOR_SETS.md`
- `VULKAN_TEXTURE_BINDING.md`
- `VULKAN_TEXTURE_RESIDENCY.md` (texture upload and bindless slot assignment)

**HUD/UI rendering issues (clip/scissor, overlay order)**:
- `VULKAN_HUD_RENDERING.md`
- `VULKAN_2D_PIPELINE.md`

**Memory pressure / dynamic allocation / ring buffers**:
- `VULKAN_DYNAMIC_BUFFERS.md`

**Pipeline issues / shader variants / cache behavior**:
- `VULKAN_PIPELINE_MANAGEMENT.md` (architecture and lifecycle)
- `VULKAN_PIPELINE_USAGE.md` (construction patterns, common mistakes)

**Uniform buffer alignment / std140 layout**:
- `VULKAN_UNIFORM_ALIGNMENT.md` (alignment rules, adding new structs)
- `VULKAN_UNIFORM_BINDINGS.md` (binding points, struct definitions)

**Deferred lighting / G-buffer rendering**:
- `VULKAN_DEFERRED_LIGHTING_FLOW.md` (complete flow, G-buffer channels)

**DLSS / upscaling**:
- `VULKAN_DLSS_PLAN.md` (implementation plan and required invariants)

## Testing and Behavioral Invariants

The Vulkan tests are split between:

- "Real" unit tests that exercise components with minimal stubs
- Behavioral "fake state machine" tests that encode invariants without requiring a GPU

Entry point: `VULKAN_INTEGRATION_TESTS.md`

When changing renderer state machines (target switching, pass boundaries, post-processing enablement), prefer adding/updating a behavioral invariant test that locks in the expected semantics.

## Document Index

**Core Architecture**:
- `VULKAN_RENDER_PASS_STRUCTURE.md`: Target typestates, dynamic rendering boundaries, clear ops, frame lifecycle
- `VULKAN_SYNCHRONIZATION.md`: Frames-in-flight, submissions, fences/semaphores, sync2 + layout transitions
- `VULKAN_CAPABILITY_TOKENS.md`: Token types (FrameCtx, RenderCtx, etc.), creation, consumption, lifetime

**Descriptors and Textures**:
- `VULKAN_DESCRIPTOR_SETS.md`: Descriptor layouts, pools, push descriptors, bindless binding, update safety rules
- `VULKAN_TEXTURE_BINDING.md`: Texture lifecycle, sampler cache, bindless slot rules, descriptor validity
- `VULKAN_TEXTURE_RESIDENCY.md`: Texture residency state machine, upload batching, fallback handling

**Pipelines and Shaders**:
- `VULKAN_PIPELINE_MANAGEMENT.md`: Pipeline architecture, cache keys, shader variants, vertex input modes
- `VULKAN_PIPELINE_USAGE.md`: Pipeline key construction, binding patterns, debugging tips

**Uniforms**:
- `VULKAN_UNIFORM_BINDINGS.md`: Uniform buffer structs, binding points, std140 layout
- `VULKAN_UNIFORM_ALIGNMENT.md`: Alignment rules, adding new uniform structs, common mistakes

**Buffers**:
- `VULKAN_DYNAMIC_BUFFERS.md`: Ring buffers, managed buffers, orphaning, deferred releases

**Rendering Paths**:
- `VULKAN_2D_PIPELINE.md`: Interface pipeline, coordinate systems, 2D draw path contracts
- `VULKAN_HUD_RENDERING.md`: HUD draw order and shaders, clip/scissor, RTT cockpit displays
- `VULKAN_DEFERRED_LIGHTING_FLOW.md`: Complete deferred lighting flow, G-buffer channels, light volumes
- `VULKAN_POST_PROCESSING.md`: Post-processing pipeline chain, target transitions, uniform data flow
- `VULKAN_MODEL_RENDERING_PIPELINE.md`: Model rendering path, vertex pulling, bindless textures

**Plans and History**:
- `VULKAN_DLSS_PLAN.md`: Upscaling plan (render vs display extents, jitter, motion vectors, pass placement)
- `VULKAN_RECENT_FIXES.md`: Recent bug history and the invariants that resulted

**Testing and Debugging**:
- `VULKAN_INTEGRATION_TESTS.md`: Test philosophy and key invariant tests
- `VULKAN_ERROR_HANDLING.md`: Error handling patterns, validation, debugging
- `VULKAN_PERFORMANCE_OPTIMIZATION.md`: Performance considerations and optimization strategies

