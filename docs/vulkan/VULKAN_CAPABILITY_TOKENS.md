# Vulkan Capability Tokens Practical Guide

This document provides a practical guide to using capability tokens (`FrameCtx`, `RenderCtx`, `RecordingFrame`, etc.) in the Vulkan renderer. It covers token creation, consumption, lifetime management, and common patterns.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Token Types](#2-token-types)
3. [Token Creation](#3-token-creation)
4. [Token Consumption](#4-token-consumption)
5. [Token Lifetime](#5-token-lifetime)
6. [Common Patterns](#6-common-patterns)
7. [Common Mistakes](#7-common-mistakes)
8. [Integration with Legacy Code](#8-integration-with-legacy-code)

---

## 1. Overview

Capability tokens are move-only types that prove a specific phase or state is active. They encode invariants in the type system, making invalid operations compile-time errors rather than runtime bugs.

**Design Philosophy**: See `docs/DESIGN_PHILOSOPHY.md` for the underlying principles.

**Key Principle**: If you hold a token, you have proof that the corresponding phase is active. No token, no operation.

**Files**:
- `code/graphics/vulkan/VulkanFrameCaps.h` - Frame and render tokens
- `code/graphics/vulkan/VulkanPhaseContexts.h` - Upload and deferred lighting tokens
- `code/graphics/vulkan/VulkanFrameFlow.h` - Recording frame token

---

## 2. Token Types

### 2.1 RecordingFrame

**Purpose**: Proof that a frame is currently recording commands

**Structure** (`VulkanFrameFlow.h:18-37`):
```cpp
struct RecordingFrame {
    RecordingFrame(const RecordingFrame&) = delete;  // Move-only
    RecordingFrame& operator=(const RecordingFrame&) = delete;
    RecordingFrame(RecordingFrame&&) = default;
    RecordingFrame& operator=(RecordingFrame&&) = default;

    VulkanFrame& ref() const { return frame.get(); }

private:
    // Only VulkanRenderer may mint the recording token.
    RecordingFrame(VulkanFrame& f, uint32_t img) : frame(f), imageIndex(img) {}

    std::reference_wrapper<VulkanFrame> frame;
    uint32_t imageIndex;  // Private: swapchain image index

    vk::CommandBuffer cmd() const { return frame.get().commandBuffer(); }

    friend class VulkanRenderer;
};
```

**Properties**:
- Move-only (cannot be copied)
- Holds reference to `VulkanFrame` via `std::reference_wrapper`
- `imageIndex` is private (only accessible by `VulkanRenderer`)
- Created by `VulkanRenderer::beginRecording()`
- Consumed by `VulkanRenderer::advanceFrame()`

**Usage**: Internal to renderer; not exposed directly to engine code. Bridge functions in `VulkanGraphics.cpp` access it via `currentRecording()`.

**Related Type: InFlightFrame** (`VulkanFrameFlow.h:39-51`):
```cpp
struct InFlightFrame {
    std::reference_wrapper<VulkanFrame> frame;
    SubmitInfo submit;

    InFlightFrame(VulkanFrame& f, SubmitInfo s) : frame(f), submit(s) {}

    InFlightFrame(const InFlightFrame&) = delete;  // Move-only
    InFlightFrame& operator=(const InFlightFrame&) = delete;
    InFlightFrame(InFlightFrame&&) = default;
    InFlightFrame& operator=(InFlightFrame&&) = default;

    VulkanFrame& ref() const { return frame.get(); }
};
```
Represents a frame that has been submitted to the GPU and is awaiting completion. Created after `submitRecordedFrame()`.

### 2.2 FrameCtx

**Purpose**: Proof of active recording frame + renderer instance

**Structure** (`VulkanFrameCaps.h:13-27`):
```cpp
struct FrameCtx {
    VulkanRenderer& renderer;

    FrameCtx(VulkanRenderer& inRenderer, graphics::vulkan::RecordingFrame& inRecording)
        : renderer(inRenderer), m_recording(inRecording)
    {
    }

    VulkanFrame& frame() const { return m_recording.ref(); }

private:
    graphics::vulkan::RecordingFrame& m_recording;

    friend class VulkanRenderer;
};
```

**Properties**:
- Copyable (holds references, not ownership)
- Provides access to `VulkanFrame` and `VulkanRenderer`
- Required for most rendering operations

**Creation**: `currentFrameCtx()` helper in `VulkanGraphics.cpp`

**Usage**: Passed to rendering functions that need frame access.

**Related Types** (`VulkanFrameCaps.h:29-62`):
- `ModelBoundFrame` - Wrapper proving model UBO is bound; created by `requireModelBound(FrameCtx)`
- `NanoVGBoundFrame` - Wrapper proving NanoVG UBO is bound; created by `requireNanoVGBound(FrameCtx)`

These types extend `FrameCtx` with additional proof that specific uniform buffers are bound.

### 2.3 RenderCtx

**Purpose**: Proof that dynamic rendering is active

**Structure** (`VulkanPhaseContexts.h:36-48`):
```cpp
struct RenderCtx {
    vk::CommandBuffer cmd;
    RenderTargetInfo targetInfo;

    RenderCtx(const RenderCtx&) = delete;  // Move-only
    RenderCtx& operator=(const RenderCtx&) = delete;
    RenderCtx(RenderCtx&&) = default;
    RenderCtx& operator=(RenderCtx&&) = default;

private:
    RenderCtx(vk::CommandBuffer inCmd, const RenderTargetInfo& inTargetInfo)
        : cmd(inCmd), targetInfo(inTargetInfo) {}
    friend class VulkanRenderer;
};
```

**Properties**:
- Move-only (cannot be copied)
- Contains active command buffer and render target info
- Created by `ensureRenderingStarted(const FrameCtx&)`
- Valid only while render pass is active

**Usage**: Required for draw operations.

### 2.4 UploadCtx

**Purpose**: Proof that upload phase is active

**Structure** (`VulkanPhaseContexts.h:15-32`):
```cpp
struct UploadCtx {
    VulkanFrame& frame;
    vk::CommandBuffer cmd;
    uint32_t currentFrameIndex = 0;

    UploadCtx(const UploadCtx&) = delete;  // Move-only
    UploadCtx& operator=(const UploadCtx&) = delete;
    UploadCtx(UploadCtx&&) = default;
    UploadCtx& operator=(UploadCtx&&) = default;

private:
    UploadCtx(VulkanFrame& inFrame, vk::CommandBuffer inCmd, uint32_t inCurrentFrameIndex)
        : frame(inFrame), cmd(inCmd), currentFrameIndex(inCurrentFrameIndex) {}
    friend class VulkanRenderer;
};
```

**Properties**:
- Move-only
- Created internally during frame start (in `beginFrame()`)
- Used for texture upload operations via `flushPendingUploads(UploadCtx)`

**Usage**: Internal to texture upload system. Passed to `VulkanTextureManager::flushPendingUploads()`.

### 2.5 Deferred Lighting Tokens

**DeferredGeometryCtx** (`VulkanPhaseContexts.h:51-62`):
```cpp
struct DeferredGeometryCtx {
    uint32_t frameIndex = 0;  // Used to validate token matches current frame

    DeferredGeometryCtx(const DeferredGeometryCtx&) = delete;  // Move-only
    DeferredGeometryCtx& operator=(const DeferredGeometryCtx&) = delete;
    DeferredGeometryCtx(DeferredGeometryCtx&&) = default;
    DeferredGeometryCtx& operator=(DeferredGeometryCtx&&) = default;

private:
    explicit DeferredGeometryCtx(uint32_t inFrameIndex) : frameIndex(inFrameIndex) {}
    friend class VulkanRenderer;
};
```
- Proof that deferred geometry pass is active
- Created by `deferredLightingBegin(RecordingFrame&, bool)`
- Consumed by `deferredLightingEnd(RecordingFrame&, DeferredGeometryCtx&&)`

**DeferredLightingCtx** (`VulkanPhaseContexts.h:64-75`):
```cpp
struct DeferredLightingCtx {
    uint32_t frameIndex = 0;  // Used to validate token matches current frame

    DeferredLightingCtx(const DeferredLightingCtx&) = delete;  // Move-only
    DeferredLightingCtx& operator=(const DeferredLightingCtx&) = delete;
    DeferredLightingCtx(DeferredLightingCtx&&) = default;
    DeferredLightingCtx& operator=(DeferredLightingCtx&&) = default;

private:
    explicit DeferredLightingCtx(uint32_t inFrameIndex) : frameIndex(inFrameIndex) {}
    friend class VulkanRenderer;
};
```
- Proof that deferred lighting pass is active
- Created by `deferredLightingEnd(RecordingFrame&, DeferredGeometryCtx&&)`
- Consumed by `deferredLightingFinish(RecordingFrame&, DeferredLightingCtx&&, const vk::Rect2D&)`

**Typestate Pattern**: Enforces call order: `begin()` -> `end()` -> `finish()`. The `frameIndex` member provides runtime validation that the token was created for the current frame.

**See Also**: `docs/vulkan/VULKAN_DEFERRED_LIGHTING_FLOW.md` for complete deferred lighting pipeline documentation.

---

## 3. Token Creation

### 3.1 RecordingFrame Creation

**Function**: `VulkanRenderer::beginRecording()`

**Called At**: Frame start, after acquiring swapchain image

**Process** (`VulkanRenderer.cpp:471-479`):
```cpp
graphics::vulkan::RecordingFrame VulkanRenderer::beginRecording()
{
    auto af = acquireAvailableFrame();

    const uint32_t imageIndex = acquireImageOrThrow(*af.frame);
    beginFrame(*af.frame, imageIndex);

    return graphics::vulkan::RecordingFrame{ *af.frame, imageIndex };
}
```

**Key Point**: Only one `RecordingFrame` exists at a time (stored in `g_backend->recording` as `std::optional<RecordingFrame>`).

### 3.2 FrameCtx Creation

**Function**: `currentFrameCtx()` helper

**Location**: `VulkanGraphics.cpp:96-101`

**Code**:
```cpp
FrameCtx currentFrameCtx()
{
    Assertion(g_backend != nullptr, "Vulkan backend must be initialized before use");
    Assertion(g_backend->recording.has_value(), "Recording not started - flip() must be called first");
    return FrameCtx{ *g_backend->renderer, currentRecording() };
}
```

**Usage Pattern**:
```cpp
void someRenderingFunction() {
    auto ctx = currentFrameCtx();
    // Use ctx.renderer and ctx.frame()
}
```

**Key Point**: Can be called multiple times; returns new `FrameCtx` each time (holds references). Asserts that `flip()` has been called to start the first recording.

### 3.3 RenderCtx Creation

**Function**: `VulkanRenderer::ensureRenderingStarted(const FrameCtx& ctx)`

**Process** (`VulkanRenderer.cpp:495-500`):
1. Validate that the `FrameCtx` belongs to this renderer
2. Delegate to internal `ensureRenderingStartedRecording()` which:
   - Begins dynamic rendering if not already active
   - Returns `RenderCtx` with command buffer and render target info

**Code**:
```cpp
RenderCtx VulkanRenderer::ensureRenderingStarted(const FrameCtx& ctx)
{
    Assertion(&ctx.renderer == this,
        "ensureRenderingStarted called with FrameCtx from a different VulkanRenderer instance");
    return ensureRenderingStartedRecording(ctx.m_recording);
}

RenderCtx VulkanRenderer::ensureRenderingStartedRecording(graphics::vulkan::RecordingFrame& rec)
{
    auto info = m_renderingSession->ensureRendering(rec.cmd(), rec.imageIndex);
    return RenderCtx{ rec.cmd(), info };
}
```

**Usage Pattern**:
```cpp
void drawSomething(const FrameCtx& frameCtx) {
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    // Now can draw: renderCtx.cmd is valid, renderCtx.targetInfo has format info
    renderCtx.cmd.draw(...);
}
```

**Lazy Initialization**: Render pass begins on first `ensureRenderingStarted()` call, not on target request.

### 3.4 UploadCtx Creation

**Function**: Internal, created during `beginFrame()`

**Usage**: Passed to `flushPendingUploads(UploadCtx)` at frame start

**Not Exposed**: Upload context is internal to texture upload system.

---

## 4. Token Consumption

### 4.1 Token Requirements

**Functions Requiring FrameCtx**:
- `ensureRenderingStarted(FrameCtx)` - Begin render pass
- `setViewport(FrameCtx, ...)` - Set viewport
- `setScissor(FrameCtx, ...)` - Set scissor
- `pushDebugGroup(FrameCtx, ...)` - Debug labels
- `beginSceneTexture(FrameCtx, ...)` - Scene rendering
- `getBindlessTextureIndex(FrameCtx, ...)` - Texture lookup

**Functions Requiring RenderCtx**:
- Draw operations (implicit - `RenderCtx` contains `cmd`)
- Pipeline binding (implicit - uses `RenderCtx.cmd`)
- Descriptor binding (implicit - uses `RenderCtx.cmd`)

**Functions Requiring RecordingFrame**:
- `advanceFrame(RecordingFrame prev)` - Ends current frame, submits, begins next frame (takes by value, consumes)
- `endFrame(RecordingFrame& rec)` - End command buffer recording (takes by reference)
- `submitRecordedFrame(RecordingFrame& rec)` - Submit to GPU (takes by reference)

### 4.2 Token Validation

**FrameCtx Validation** (`VulkanRenderer.cpp:660-663`):
```cpp
void VulkanRenderer::beginSceneTexture(const FrameCtx& ctx, bool enableHdrPipeline)
{
    Assertion(&ctx.renderer == this,
        "beginSceneTexture called with FrameCtx from a different VulkanRenderer instance");
    // ...
}
```

**Key Point**: Tokens are validated to ensure they come from the correct renderer instance. This pattern is used consistently across all `FrameCtx`-accepting methods.

### 4.3 Token Movement

**Move Semantics**: Most tokens are move-only to prevent duplication:

```cpp
// CORRECT: Move token (if function takes by value)
auto renderCtx = ensureRenderingStarted(frameCtx);
consumeRenderCtx(std::move(renderCtx));  // Token moved

// WRONG: Cannot copy move-only types
auto renderCtx = ensureRenderingStarted(frameCtx);
auto copy = renderCtx;  // Compile error: RenderCtx is move-only

// CORRECT: Use cmd directly for multiple draws (no move needed)
auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
renderCtx.cmd.draw(...);  // Use directly
renderCtx.cmd.draw(...);  // Same token, same pass
```

**FrameCtx Exception**: `FrameCtx` is copyable because it holds references, not ownership.

---

## 5. Token Lifetime

### 5.1 RecordingFrame Lifetime

**Created**: `beginRecording()` at frame start
**Destroyed**: `advanceFrame()` consumes it, creates new one

**Scope**: Single frame recording session

**Storage**: `std::optional<RecordingFrame> g_backend->recording`

### 5.2 FrameCtx Lifetime

**Created**: `currentFrameCtx()` - can be called anytime during recording
**Destroyed**: When `RecordingFrame` is consumed

**Scope**: Valid only while `RecordingFrame` exists

**Key Point**: Multiple `FrameCtx` instances can exist simultaneously (they're just references).

### 5.3 RenderCtx Lifetime

**Created**: `ensureRenderingStarted()` - begins render pass if needed
**Destroyed**: When render pass ends (target switch or frame end)

**Scope**: Single render pass

**Key Point**: Render pass ends automatically when:
- Target switches (`requestSwapchainTarget()`, etc.)
- Frame ends (`endFrame()`)
- Rendering suspended (`suspendRendering()`)

### 5.4 Common Lifetime Mistakes

**Mistake 1: Holding RenderCtx Across Target Switch**
```cpp
// WRONG
auto renderCtx = ensureRenderingStarted(frameCtx);
// ... draw to swapchain ...
requestPostLdrTarget();  // Ends render pass!
renderCtx.cmd.draw(...);  // ERROR: Render pass ended, cmd invalid
```

**Fix**: Get new `RenderCtx` after target switch:
```cpp
// CORRECT
auto swapchainCtx = ensureRenderingStarted(frameCtx);
// ... draw to swapchain ...
requestPostLdrTarget();
auto ldrCtx = ensureRenderingStarted(frameCtx);  // New token
ldrCtx.cmd.draw(...);  // OK
```

**Mistake 2: Holding FrameCtx After Frame End**
```cpp
// WRONG
auto ctx = currentFrameCtx();
flip();  // Advances frame, destroys RecordingFrame
ctx.frame();  // ERROR: RecordingFrame destroyed
```

**Fix**: Get new `FrameCtx` after frame advance:
```cpp
// CORRECT
flip();  // Advances frame
auto ctx = currentFrameCtx();  // New token for new frame
ctx.frame();  // OK
```

---

## 6. Common Patterns

### 6.1 Pattern: Draw Operation

```cpp
void drawModel(const FrameCtx& frameCtx, ModelData& model) {
    // Get render context (begins pass if needed)
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    
    // Get pipeline
    PipelineKey key = buildPipelineKey(renderCtx.targetInfo);
    vk::Pipeline pipeline = getPipeline(key);
    
    // Bind pipeline
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    
    // Draw
    renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
}
```

### 6.2 Pattern: Multiple Draws Same Pass

```cpp
void drawMultipleModels(const FrameCtx& frameCtx, const std::vector<ModelData>& models) {
    // Get render context once
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    
    // Bind pipeline once (if all models use same pipeline)
    vk::Pipeline pipeline = getPipeline(...);
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    
    // Draw all models (same pass)
    for (const auto& model : models) {
        // ... set uniforms, bind descriptors ...
        renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
    }
}
```

### 6.3 Pattern: Target Switch

```cpp
void renderSceneThenPostProcess(const FrameCtx& frameCtx) {
    // Scene rendering
    frameCtx.renderer.beginSceneTexture(frameCtx, true);
    auto sceneCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    // ... draw scene ...
    
    // Post-processing (target switch)
    frameCtx.renderer.endSceneTexture(frameCtx, true);
    // Scene pass ended automatically
    
    // Post-processing uses new target
    auto postCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    // ... draw post-processing ...
}
```

### 6.4 Pattern: Deferred Lighting

The deferred lighting API operates on `RecordingFrame&` directly, not `FrameCtx`. This is typically called from bridge functions in `VulkanGraphics.cpp`:

```cpp
// Bridge functions in VulkanGraphics.cpp
void gr_vulkan_deferred_lighting_begin(bool clearNonColorBufs)
{
    Assertion(g_backend->recording.has_value(), "...");
    Assertion(std::holds_alternative<std::monostate>(g_backend->deferred), "...");

    auto& renderer = currentRenderer();
    g_backend->deferred = renderer.deferredLightingBegin(*g_backend->recording, clearNonColorBufs);
}

void gr_vulkan_deferred_lighting_end()
{
    auto* geometry = std::get_if<DeferredGeometryCtx>(&g_backend->deferred);
    Assertion(geometry != nullptr, "...");

    auto& renderer = currentRenderer();
    g_backend->deferred = renderer.deferredLightingEnd(*g_backend->recording, std::move(*geometry));
}

void gr_vulkan_deferred_lighting_finish()
{
    auto* lighting = std::get_if<DeferredLightingCtx>(&g_backend->deferred);
    Assertion(lighting != nullptr, "...");

    auto& renderer = currentRenderer();
    vk::Rect2D scissor = createClipScissor();
    renderer.deferredLightingFinish(*g_backend->recording, std::move(*lighting), scissor);
    g_backend->deferred = std::monostate{};
}
```

**Key Points**:
- Typestate tokens enforce correct call order: `begin()` -> `end()` -> `finish()`
- Cannot call `deferredLightingEnd()` without `deferredLightingBegin()`
- The deferred state is stored in `g_backend->deferred` as a `std::variant`
- Each token is consumed (moved) when transitioning to the next phase

---

## 7. Common Mistakes

### Mistake 1: Using Token After Move

```cpp
// WRONG
auto renderCtx = ensureRenderingStarted(frameCtx);
drawSomething(std::move(renderCtx));
drawSomethingElse(renderCtx);  // ERROR: renderCtx moved
```

**Fix**: Use `renderCtx.cmd` directly for multiple draws within the same pass:
```cpp
// CORRECT: Use the token's command buffer directly
auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
renderCtx.cmd.bindPipeline(...);
renderCtx.cmd.draw(...);  // First draw
renderCtx.cmd.draw(...);  // Second draw - same pass, same cmd
```

### Mistake 2: Holding Token Too Long

```cpp
// WRONG
auto renderCtx = ensureRenderingStarted(frameCtx);
// ... many operations ...
requestSwapchainTarget();  // Ends pass
// ... more operations ...
renderCtx.cmd.draw(...);  // ERROR: Pass ended
```

**Fix**: Get token close to use:
```cpp
// CORRECT
// ... setup ...
auto renderCtx = ensureRenderingStarted(frameCtx);
renderCtx.cmd.draw(...);  // Use immediately
// Token destroyed, pass can end
```

### Mistake 3: Copying Move-Only Token

```cpp
// WRONG
auto renderCtx = ensureRenderingStarted(frameCtx);
auto ctx2 = renderCtx;  // ERROR: RenderCtx is move-only
```

**Fix**: Move instead:
```cpp
// CORRECT
auto renderCtx = ensureRenderingStarted(frameCtx);
auto ctx2 = std::move(renderCtx);  // OK: Move semantics
```

### Mistake 4: Using Wrong Token Type

```cpp
// WRONG
void drawSomething(const RenderCtx& ctx) {
    ctx.frame();  // ERROR: RenderCtx doesn't have frame()
}

// CORRECT
void drawSomething(const FrameCtx& frameCtx) {
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    frameCtx.frame();  // OK: FrameCtx has frame()
    renderCtx.cmd.draw(...);  // OK: RenderCtx has cmd
}
```

---

## 8. Integration with Legacy Code

### 8.1 Bridge Pattern

**File**: `code/graphics/vulkan/VulkanGraphics.cpp`

**Pattern**: Legacy engine code doesn't use tokens directly. Bridge functions create tokens:

```cpp
void gr_vulkan_draw_something() {
    // Bridge: Create token from global state
    auto ctx = currentFrameCtx();
    
    // Call token-based API
    drawSomething(ctx);
}
```

### 8.2 Global State Access

**Functions**: `currentFrameCtx()`, `currentFrame()`, `currentRecording()`, `currentRenderer()`

**Location**: `VulkanGraphics.cpp:76-101`

**Code**:
```cpp
VulkanRenderer& currentRenderer()
{
    Assertion(g_backend != nullptr, "Vulkan backend must be initialized before use");
    return *g_backend->renderer;
}

VulkanFrame& currentFrame()
{
    Assertion(g_backend != nullptr, "Vulkan backend must be initialized before use");
    Assertion(g_backend->recording.has_value(), "Recording not started - flip() must be called first");
    return g_backend->recording->ref();
}

RecordingFrame& currentRecording()
{
    Assertion(g_backend != nullptr, "Vulkan backend must be initialized before use");
    Assertion(g_backend->recording.has_value(), "Recording not started - flip() must be called first");
    return *g_backend->recording;
}
```

**Usage**: Called by legacy code paths that need frame access

**Key Point**: These functions assert that recording is active. They bridge legacy implicit state to explicit tokens.

### 8.3 Token Creation in Legacy Paths

**Pattern**: Legacy code calls `gr_vulkan_*` functions, which create tokens internally:

```cpp
// Legacy code
gr_vulkan_set_viewport(x, y, w, h);

// Bridge function
void gr_vulkan_set_viewport(int x, int y, int w, int h) {
    auto ctx = currentFrameCtx();  // Create token
    ctx.renderer.setViewport(ctx, viewport);  // Use token
}
```

---

## Appendix: Token Hierarchy

```
Frame Lifecycle Tokens:
    RecordingFrame (internal, move-only)
        └── FrameCtx (copyable reference wrapper)
                ├── ModelBoundFrame (proof of model UBO binding)
                ├── NanoVGBoundFrame (proof of NanoVG UBO binding)
                └── RenderCtx (move-only, created on demand)
                        └── Used for draw operations
        └── InFlightFrame (internal, move-only)
                └── Created after submit, tracks GPU completion

Phase-Specific Tokens:
    UploadCtx (internal, frame start)
        └── Used for texture uploads

    DeferredGeometryCtx (move-only, typestate)
        └── DeferredLightingCtx (move-only, typestate)
                └── Consumed by deferredLightingFinish()
```

---

## References

- `code/graphics/vulkan/VulkanFrameCaps.h` - FrameCtx, ModelBoundFrame, NanoVGBoundFrame
- `code/graphics/vulkan/VulkanPhaseContexts.h` - UploadCtx, RenderCtx, DeferredGeometryCtx, DeferredLightingCtx
- `code/graphics/vulkan/VulkanFrameFlow.h` - RecordingFrame, InFlightFrame, SubmitInfo
- `code/graphics/vulkan/VulkanRenderer.h` - Token-consuming API declarations
- `code/graphics/vulkan/VulkanRenderer.cpp` - Token-consuming API implementations
- `code/graphics/vulkan/VulkanGraphics.cpp` - Token creation helpers and bridge functions
- `docs/DESIGN_PHILOSOPHY.md` - Capability token principles

