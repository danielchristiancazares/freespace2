# Deferred Refactor Plan (Type-Driven, Session-Free)

## Goal

Remove `VulkanRenderingSession` from the **deferred** path by making deferred passes self-contained, type-driven state transitions that are compatible with the engine’s existing immediate-mode call pattern:

- `gr_vulkan_deferred_lighting_begin(clearNonColorBufs)`
- (arbitrary draw calls occur here)
- `gr_vulkan_deferred_lighting_end()`
- `gr_vulkan_deferred_lighting_finish()`

“Invalid sequences” become difficult/impossible by construction: deferred lighting requires a “G-buffer readable” proof token that only the deferred geometry pass can produce.

Non-goal (initially): rewrite forward rendering. Forward can keep `VulkanRenderingSession` until deferred is stable.

## Key Correctness Constraints (must satisfy)

1. **No lazy deferred begin.** Deferred must begin dynamic rendering immediately at `*_begin()` so G-buffer clears execute even when no geometry is drawn (fixes the “uncleared G-buffer sampled” bug).
2. **Swapchain present transition is frame-owned and single-shot.** Only `endFrame()` transitions swapchain → `ePresentSrcKHR`. Per-pass “finalize-to-present” is incorrect for mixed/overlay pipelines.
3. **No `oldLayout = eUndefined` when contents are needed.** If a pass uses `loadOp = eLoad`, the barrier must use the actual prior layout (requires state tracking via tokens).
4. **Queue family indices are explicit.** For barriers that are *not* ownership transfers, set `srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED` and `dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED` (only set real indices when you are intentionally doing a queue family ownership transfer).
5. **G-buffer “no geometry” convention is preserved.** The deferred shader discards when the **Position** buffer stays at its clear value, so Position must be cleared to `{0,0,0,0}` every deferred begin (see `../shaders/deferred.frag`).
6. **Deferred lighting must define the background.** The deferred shader discards for “no geometry” pixels, so swapchain rendering must start from defined contents (`loadOp = eClear` or `loadOp = eLoad` with a real prior producer). `loadOp = eDontCare` is not valid for this pipeline as written.
7. **Anything read later must be stored.** Any attachment that is later sampled (`G-buffer`, `depthSampledView`) or later loaded (`swapchain` overlays) must use `storeOp = eStore` in the pass that writes it.
8. **Baseline dynamic state must not be implicit.** If deferred passes begin dynamic rendering outside the legacy session, they must apply the same baseline dynamic state (viewport/scissor, blend defaults, depth flags, etc.) that `ensureRenderingStarted()` used to apply, otherwise the first draw may inherit stale state.

## High-Level Design

### Replace “session-owned state machine” with “pass objects + state tokens”

Introduce move-only tokens that encode layout/content invariants, stored on `VulkanRenderer` across the `*_begin/end/finish` triplet:

- `SwapchainImageState` (per swapchain image index; persistent across frames)
  - Owns the current swapchain image layout for the acquired image.
- `GBufferLayoutState` (persistent across frames)
  - Owns current layouts for `kGBufferCount` images + depth image.

Introduce RAII pass objects that consume/produce those tokens:

- `DeferredGBufferPass` (writable attachments, dynamic rendering active)
  - Constructible only with `vk::CommandBuffer` and references to layout state.
  - Begins dynamic rendering immediately in `*_begin()`.
  - On completion produces `GBufferReadable` proof token.
- `DeferredLightingPass` (swapchain color attachment, samples from G-buffer)
  - Constructible only with `GBufferReadable` proof + the acquired `SwapchainImageState`.
  - Begins swapchain rendering without depth.
  - Does **not** transition to present.

Finally, replace “am I in deferred?” flags with a closed render-state sum type. The renderer holds exactly one state object at a time (e.g., `Forward`, `DeferredGBufferActive`, `DeferredReady`), and transitions happen by *moving* tokens between states. The active object is the proof.

### Polymorphism remains, but ownership moves

Draw paths currently call `VulkanRenderer::ensureRenderingStarted(cmd)` to lazily begin rendering and obtain a `RenderTargetInfo` contract for pipeline selection.

Refactor `ensureRenderingStarted(cmd)` to dispatch to:

- the active deferred pass (if any), otherwise
- the existing `VulkanRenderingSession` (forward path) as a transitional implementation.

This keeps the immediate-mode draw code mostly unchanged while deferred becomes session-free.

## Concrete Types (sketch)

### Render state (typestate, no flags)

Model “what can draw right now?” as a closed set of states. Only some states implement the “give me a RenderTargetInfo” capability:

```cpp
struct Forward {
  VulkanRenderingSession& session;
};

struct DeferredGBufferActive {
  DeferredGBufferPass pass; // dynamic rendering already begun
};

struct DeferredReady {
  GBufferReadable gbufferReadable; // can start lighting, but normal draws are invalid here
};

using RenderState = std::variant<Forward, DeferredGBufferActive, DeferredReady>;
```

Transitions are moves:
- `Forward` → `DeferredGBufferActive` at `*_begin()`
- `DeferredGBufferActive` → `DeferredReady` at `*_end()`
- `DeferredReady` → `Forward` at `*_finish()`

### Layout helpers (reuse existing logic)

Keep/reuse the existing `stageAccessForLayout(vk::ImageLayout)` mapping (or equivalent) to compute stage/access masks based on the *tracked* old layout.

### Tokens

```cpp
class SwapchainImageState {
public:
  void onAcquire(uint32_t imageIndex) { m_imageIndex = imageIndex; }
  uint32_t imageIndex() const { return m_imageIndex; }
  vk::ImageLayout layout() const { return m_layout; }

  // Performs a barrier + updates internal layout. Non-transfer barriers always use IGNORED queue family indices.
  void transition(vk::CommandBuffer cmd, const VulkanDevice& device, vk::ImageLayout newLayout);

private:
  uint32_t m_imageIndex = UINT32_MAX;
  vk::ImageLayout m_layout = vk::ImageLayout::eUndefined; // carried across frames
};

class GBufferLayoutState {
public:
  vk::ImageLayout color(uint32_t i) const { return m_gbuffer[i]; }
  vk::ImageLayout depth() const { return m_depth; }

  void transitionColor(vk::CommandBuffer cmd, const VulkanRenderTargets& targets, uint32_t i, vk::ImageLayout newLayout);
  void transitionDepth(vk::CommandBuffer cmd, const VulkanRenderTargets& targets, vk::ImageLayout newLayout);

private:
  std::array<vk::ImageLayout, VulkanRenderTargets::kGBufferCount> m_gbuffer{};
  vk::ImageLayout m_depth = vk::ImageLayout::eUndefined;
};

class GBufferReadable {
  friend class DeferredGBufferPass;
  // Contains only what lighting needs; construction is private.
public:
  vk::DescriptorImageInfo color(uint32_t i) const;
  vk::DescriptorImageInfo depth() const;
private:
  // views/samplers + invariant: layouts are shader-read.
};
```

These are “type-driven” without pretending the type system can fully prove Vulkan layouts; the proof is backed by the only code path that can construct `GBufferReadable` (the pass finalize).

### Deferred geometry pass (session-free)

```cpp
struct ClearAllAttachments {};
struct ClearSentinelOnly {};

// Map the engine bool to a type at the API boundary, then never branch on a bool again.
using DeferredClearPolicy = std::variant<ClearAllAttachments, ClearSentinelOnly>;

class DeferredGBufferPass {
public:
  DeferredGBufferPass(vk::CommandBuffer cmd,
                      VulkanRenderTargets& targets,
                      GBufferLayoutState& layouts,
                      DeferredClearPolicy clearPolicy);
  ~DeferredGBufferPass(); // ends rendering if active

  const VulkanRenderingSession::RenderTargetInfo& info() const;

  // End rendering + transition to shader read; produces proof token
  [[nodiscard]] GBufferReadable finish();

private:
  void transitionToAttachment();
  void beginRendering(); // loadOp clears executed here
  void transitionToShaderRead();
};
```

Correctness notes:

- `*_begin()` constructs this pass and calls `beginRendering()` immediately (no lazy start).
- `beginRendering()` must also apply baseline dynamic state (or call the shared helper that forward uses), since `ensureRenderingStarted()` no longer “accidentally” does it for deferred.
- Clears:
  - Always clear the Position buffer to `{0,0,0,0}` every deferred begin.
  - The clear policy may control clearing of other attachments, but must not disable the sentinel clear.
- Barriers use tracked `layouts.color(i)` / `layouts.depth()` as `oldLayout`, and the layout state object updates itself as part of the transition.

### Deferred lighting pass (session-free)

```cpp
class DeferredLightingPass {
public:
  DeferredLightingPass(vk::CommandBuffer cmd,
                       VulkanDevice& device,
                       SwapchainImageState& swapchain,
                       GBufferReadable gbufferReadable,
                       /* no clear flag */);
  ~DeferredLightingPass(); // ends rendering if active

  const VulkanRenderingSession::RenderTargetInfo& info() const;
  const GBufferReadable& sampling() const;

private:
  void transitionSwapchainToColorAttachment();
  void beginRendering();
};
```

Correctness notes:

- Does not present; it leaves the swapchain image in `eColorAttachmentOptimal` so overlay/forward can continue.
- Swapchain transition uses tracked `swapchain.layout()` as `oldLayout` (not `eUndefined`) and updates internal state via `swapchain.transition(...)`.
- `beginRendering()` must also apply baseline dynamic state (viewport/scissor, blend defaults, etc.).
- `loadOp` policy (given the current shader behavior in `../shaders/deferred.frag`):
  - **Always** start swapchain rendering with `loadOp = eClear` (or with `loadOp = eLoad` only if you have an explicit “swapchain already fully defined” proof from an earlier pass in the same frame).
  - Do **not** use `eDontCare`: the shader discards for “no geometry” pixels, so undefined background would leak through.

## Engine Hook Integration (minimal disruption)

### `gr_vulkan_deferred_lighting_begin(clearNonColorBufs)`

- Require a recording frame (like `_end/_finish` already do); get `cmd`.
- End any active forward rendering (add a public “end rendering now” hook if needed) to avoid nested dynamic rendering.
- Map the API bool to a clear-policy *type* once, then transition render state:
  - `clearNonColorBufs == true` → `DeferredGBufferPass(..., ClearAllAttachments{})`
  - `clearNonColorBufs == false` → `DeferredGBufferPass(..., ClearSentinelOnly{})`
- Mark renderer “current render target = deferred gbuffer” for `ensureRenderingStarted(cmd)`.

### Draw calls between begin/end

Update `VulkanRenderer::ensureRenderingStarted(cmd)`:

- Replace “is a deferred pass active?” booleans with a closed render-state sum type (typestate):
  - `Forward` → delegates to `VulkanRenderingSession::ensureRenderingActive()`
  - `DeferredGBufferActive` → returns the deferred pass `info()` (rendering already begun)
- Dispatch via `std::visit`/virtual polymorphism on that state object (no flags; the current state *is* the proof).

This ensures models/primitives “just draw” into whichever pass is active.

### `gr_vulkan_deferred_lighting_end()`

- Transition render state by consuming the geometry pass and producing proof:
  - `DeferredGBufferActive` → `DeferredReady{ pass.finish() }`

### `gr_vulkan_deferred_lighting_finish()`

- Construct lighting pass requiring proof:
  - `DeferredLightingPass pass(cmd, *m_vulkanDevice, m_swapchainStates[imageIndex], std::move(state.gbufferReadable));`
- Bind descriptors using `pass.sampling()` (not raw target access).
- Record deferred lighting draws.
- Destroy pass (ends rendering).
- Transition render state back to `Forward` for subsequent draws.

### Frame boundaries

- `beginFrame(imageIndex)`:
  - Call `m_swapchainStates[imageIndex].onAcquire(imageIndex)`.
  - Do **not** assume `layout = eUndefined`; carry it from prior frame. On first use it starts as `eUndefined`.
- `endFrame(imageIndex)`:
  - End any active pass (deferred or forward).
  - Transition swapchain to present using the tracked layout: `m_swapchainStates[imageIndex].transition(cmd, *m_vulkanDevice, vk::ImageLayout::ePresentSrcKHR);`
- Swapchain / target recreation:
  - When swapchain images are recreated (count/handles change), resize/reset `m_swapchainStates` so all layouts restart at `eUndefined`.
  - When `VulkanRenderTargets` recreates the G-buffer/depth images, reset `GBufferLayoutState` layouts to `eUndefined` (old layouts do not apply to new images).

## What happens to `VulkanRenderTargets`

Keep `VulkanRenderTargets` as a pure resource container (no layout tracking) **but do not pretend that means “no tracking exists”**. Tracking moves to the renderer-level token state (`SwapchainImageState`, `GBufferLayoutState`), which is still type-driven and explicit.

This avoids the correctness pitfall of hardcoding `oldLayout = eUndefined` everywhere.

## Migration Steps (deferred-first)

1. Add new deferred pass/token types (new files or nested in renderer to start).
2. Wire `gr_vulkan_deferred_lighting_begin/end/finish` to the new deferred passes.
3. Update `VulkanRenderer::ensureRenderingStarted(cmd)` to respect an active deferred pass.
4. Keep forward rendering on the existing `VulkanRenderingSession` unchanged.
5. Once stable, optionally refactor forward into similar token/pass types.

## Validation Checklist (targeted)

- Deferred begin/end/finish with **zero deferred geometry draws** still produces correct “no geometry” lighting (no ghosting).
- Deferred + overlay: the post-lighting forward/overlay pass uses `loadOp = eLoad` (not clear) and does not overwrite deferred output.
- Deferred + forward-after-lighting (depth reattachment) performs correct depth layout transitions and does not assume `eUndefined`.
- Swapchain present transition occurs exactly once per frame at `endFrame()`.
