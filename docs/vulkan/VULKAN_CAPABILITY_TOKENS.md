# Vulkan Capability Tokens Practical Guide

This document provides a practical guide to using capability tokens (`FrameCtx`, `RenderCtx`, `RecordingFrame`, etc.) in the Vulkan renderer. It covers token creation, consumption, lifetime management, and common patterns.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [Token Types](#3-token-types)
4. [Token Creation](#4-token-creation)
5. [Token Consumption](#5-token-consumption)
6. [Token Lifetime](#6-token-lifetime)
7. [Common Patterns](#7-common-patterns)
8. [Common Mistakes](#8-common-mistakes)
9. [Thread Safety](#9-thread-safety)
10. [Error Handling and Debugging](#10-error-handling-and-debugging)
11. [Integration with Legacy Code](#11-integration-with-legacy-code)

---

## 1. Overview

### 1.1 What Are Capability Tokens?

Capability tokens are move-only types that prove a specific phase or state is active. They encode invariants in the type system, making invalid operations compile-time errors rather than runtime bugs.

### 1.2 Why Use Capability Tokens?

Traditional rendering code often relies on implicit global state and runtime assertions:

```cpp
// Traditional approach: relies on programmer discipline
void drawSomething() {
    assert(g_renderPassActive);  // Runtime check - fails in production
    assert(g_commandBuffer != VK_NULL_HANDLE);  // Hope caller did setup
    vkCmdDraw(g_commandBuffer, ...);
}
```

Capability tokens make these invariants explicit and compiler-enforced:

```cpp
// Type-driven approach: invalid calls don't compile
void drawSomething(const RenderCtx& ctx) {
    // RenderCtx can only exist when render pass is active
    // No assertion needed - the type IS the proof
    ctx.cmd.draw(...);
}
```

**Benefits:**
- **Compile-time safety**: Invalid sequencing becomes a compile error, not a runtime crash
- **Self-documenting**: Function signatures declare their requirements explicitly
- **Refactoring confidence**: The compiler catches broken call sites when APIs change
- **Zero runtime cost**: Tokens are typically zero-size or reference wrappers

### 1.3 Design Philosophy

See `docs/DESIGN_PHILOSOPHY.md` for the underlying principles, particularly the sections on:
- **Typestate**: Tokens encode phase transitions (`Deck` -> `ShuffledDeck` pattern)
- **Capability tokens**: Proof of phase validity without runtime checks
- **Move semantics**: Approximating linear types to prevent use-after-transition

**Key Principle**: If you hold a token, you have proof that the corresponding phase is active. No token, no operation.

### 1.4 Source Files

- `code/graphics/vulkan/VulkanFrameCaps.h` - Frame capability tokens and bound-frame wrappers
- `code/graphics/vulkan/VulkanPhaseContexts.h` - Phase-specific tokens (upload, render, deferred)
- `code/graphics/vulkan/VulkanFrameFlow.h` - Frame lifecycle tokens

---

## 2. Quick Reference

### 2.1 Token Summary Table

| Token | Purpose | Move-Only | Created By | Typical Usage |
|-------|---------|-----------|------------|---------------|
| `RecordingFrame` | Frame is recording commands | Yes | `beginRecording()` | Internal frame management |
| `InFlightFrame` | Frame submitted, awaiting GPU | Yes | `submitRecordedFrame()` | GPU synchronization |
| `FrameCtx` | Active recording + renderer ref | No (copyable) | `currentFrameCtx()` | Most rendering operations |
| `ModelBoundFrame` | Model UBO is bound | No | `requireModelBound()` | Model rendering |
| `NanoVGBoundFrame` | NanoVG UBO is bound | No | `requireNanoVGBound()` | NanoVG rendering |
| `DecalBoundFrame` | Decal UBOs are bound | No | `requireDecalBound()` | Decal rendering |
| `RenderCtx` | Dynamic rendering is active | Yes | `ensureRenderingStarted()` | Draw commands |
| `UploadCtx` | Upload phase is active | Yes | Internal (beginFrame) | Texture uploads |
| `DeferredGeometryCtx` | Deferred geometry pass active | Yes | `deferredLightingBegin()` | Deferred rendering |
| `DeferredLightingCtx` | Deferred lighting pass active | Yes | `deferredLightingEnd()` | Deferred rendering |

### 2.2 Token Hierarchy Diagram

```
Frame Lifecycle Tokens:
    RecordingFrame (internal, move-only)
        |-- FrameCtx (copyable reference wrapper)
        |       |-- ModelBoundFrame (proof of model UBO binding)
        |       |-- NanoVGBoundFrame (proof of NanoVG UBO binding)
        |       |-- DecalBoundFrame (proof of decal UBO bindings)
        |       +-- RenderCtx (move-only, created on demand)
        |               +-- Used for draw operations
        +-- InFlightFrame (internal, move-only)
                +-- Created after submit, tracks GPU completion

Phase-Specific Tokens:
    UploadCtx (internal, frame start)
        +-- Used for texture uploads

    DeferredGeometryCtx (move-only, typestate)
        +-- DeferredLightingCtx (move-only, typestate)
                +-- Consumed by deferredLightingFinish()
```

---

## 3. Token Types

### 3.1 RecordingFrame

**Purpose**: Proof that a frame is currently recording commands.

**Definition** (`VulkanFrameFlow.h:18-37`):
```cpp
struct RecordingFrame {
    RecordingFrame(const RecordingFrame&) = delete;  // Move-only
    RecordingFrame& operator=(const RecordingFrame&) = delete;
    RecordingFrame(RecordingFrame&&) = default;
    RecordingFrame& operator=(RecordingFrame&&) = default;

    VulkanFrame& ref() const { return frame.get(); }

private:
    // Only VulkanRenderer may mint the recording token. This makes "recording is active"
    // unforgeable by construction (DESIGN_PHILOSOPHY: capability tokens).
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

### 3.2 SubmitInfo

**Purpose**: Encapsulates frame submission metadata for tracking in-flight work.

**Definition** (`VulkanFrameFlow.h:11-16`):
```cpp
struct SubmitInfo {
    uint32_t imageIndex;   // Swapchain image index
    uint32_t frameIndex;   // Internal frame slot index (0 to MAX_FRAMES_IN_FLIGHT-1)
    uint64_t serial;       // Monotonic frame counter for this session
    uint64_t timeline;     // Timeline semaphore value for GPU synchronization
};
```

**Properties**:
- Plain data struct (copyable)
- Created by `submitRecordedFrame()`
- Stored in `InFlightFrame` for tracking GPU completion

**Usage**: Internal bookkeeping for frame synchronization. The `timeline` value is used with Vulkan timeline semaphores to determine when the GPU has finished processing a frame.

### 3.3 InFlightFrame

**Purpose**: Represents a frame that has been submitted to the GPU and is awaiting completion.

**Definition** (`VulkanFrameFlow.h:39-51`):
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

**Properties**:
- Move-only
- Created after `submitRecordedFrame()` returns `SubmitInfo`
- Used to track when a frame's GPU work completes before reusing resources

**Usage**: Internal to the frame management system. Tracks submitted frames until the GPU signals completion.

### 3.4 FrameCtx

**Purpose**: Proof of active recording frame plus renderer instance reference.

**Definition** (`VulkanFrameCaps.h:13-27`):
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
- Provides access to `VulkanFrame` via `frame()` and `VulkanRenderer` via `renderer`
- Required for most rendering operations
- Multiple instances can exist simultaneously (all reference the same frame)

**Creation**: `currentFrameCtx()` helper in `VulkanGraphics.cpp`

**Usage**: Passed to rendering functions that need frame access.

### 3.5 Bound Frame Wrappers

These types extend `FrameCtx` with additional proof that specific uniform buffers are bound. They ensure that rendering operations requiring particular UBO bindings cannot be called without those bindings being active.

#### ModelBoundFrame

**Purpose**: Proof that the model uniform buffer (ModelData UBO) is bound.

**Definition** (`VulkanFrameCaps.h:29-48`):
```cpp
struct ModelBoundFrame {
    FrameCtx ctx;
    vk::DescriptorSet modelSet;
    DynamicUniformBinding modelUbo;
    uint32_t transformDynamicOffset = 0;
    size_t transformSize = 0;
};

inline ModelBoundFrame requireModelBound(FrameCtx ctx)
{
    Assertion(ctx.frame().modelUniformBinding.bufferHandle.isValid(),
        "ModelData UBO binding not set; call gr_bind_uniform_buffer(ModelData) before rendering models");
    Assertion(ctx.frame().modelDescriptorSet(), "Model descriptor set must be allocated");

    return ModelBoundFrame{ ctx,
        ctx.frame().modelDescriptorSet(),
        ctx.frame().modelUniformBinding,
        ctx.frame().modelTransformDynamicOffset,
        ctx.frame().modelTransformSize };
}
```

**Usage**: Required for model rendering. The `requireModelBound()` function validates bindings and returns the proof token.

#### NanoVGBoundFrame

**Purpose**: Proof that the NanoVG uniform buffer is bound.

**Definition** (`VulkanFrameCaps.h:50-62`):
```cpp
struct NanoVGBoundFrame {
    FrameCtx ctx;
    BoundUniformBuffer nanovgUbo;
};

inline NanoVGBoundFrame requireNanoVGBound(FrameCtx ctx)
{
    Assertion(ctx.frame().nanovgData.handle.isValid(),
        "NanoVGData UBO binding not set; call gr_bind_uniform_buffer(NanoVGData) before rendering NanoVG");
    Assertion(ctx.frame().nanovgData.size > 0, "NanoVGData UBO binding must have non-zero size");

    return NanoVGBoundFrame{ ctx, ctx.frame().nanovgData };
}
```

**Usage**: Required for NanoVG-based UI rendering.

#### DecalBoundFrame

**Purpose**: Proof that both decal uniform buffers (DecalGlobals and DecalInfo) are bound.

**Definition** (`VulkanFrameCaps.h:64-80`):
```cpp
struct DecalBoundFrame {
    FrameCtx ctx;
    BoundUniformBuffer globalsUbo;
    BoundUniformBuffer infoUbo;
};

inline DecalBoundFrame requireDecalBound(FrameCtx ctx)
{
    Assertion(ctx.frame().decalGlobalsData.handle.isValid(),
        "DecalGlobals UBO binding not set; call gr_bind_uniform_buffer(DecalGlobals) before rendering decals");
    Assertion(ctx.frame().decalGlobalsData.size > 0, "DecalGlobals UBO binding must have non-zero size");
    Assertion(ctx.frame().decalInfoData.handle.isValid(),
        "DecalInfo UBO binding not set; call gr_bind_uniform_buffer(DecalInfo) before rendering decals");
    Assertion(ctx.frame().decalInfoData.size > 0, "DecalInfo UBO binding must have non-zero size");

    return DecalBoundFrame{ ctx, ctx.frame().decalGlobalsData, ctx.frame().decalInfoData };
}
```

**Usage**: Required for rendering decals (shield impacts, weapon marks, etc.). Decals require two UBOs: global parameters and per-decal info.

### 3.6 RenderCtx

**Purpose**: Proof that dynamic rendering is active.

**Definition** (`VulkanPhaseContexts.h:36-48`):
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
- Contains active command buffer (`cmd`) and render target info (`targetInfo`)
- Created by `ensureRenderingStarted(const FrameCtx&)`
- Valid only while render pass is active
- `targetInfo` provides format information needed for pipeline selection

**Usage**: Required for draw operations. The `cmd` member is used directly for Vulkan commands.

### 3.7 UploadCtx

**Purpose**: Proof that upload phase is active.

**Definition** (`VulkanPhaseContexts.h:15-32`):
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
- `currentFrameIndex` identifies the frame slot for per-frame resource management

**Usage**: Internal to texture upload system. Passed to `VulkanTextureManager::flushPendingUploads()`.

### 3.8 Deferred Lighting Tokens

These tokens implement a typestate pattern that enforces the correct call sequence for deferred lighting: `begin()` -> `end()` -> `finish()`.

#### DeferredGeometryCtx

**Purpose**: Proof that deferred geometry pass is active.

**Definition** (`VulkanPhaseContexts.h:51-62`):
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

**Lifecycle**:
- Created by `deferredLightingBegin(RecordingFrame&, bool)`
- Consumed by `deferredLightingEnd(RecordingFrame&, DeferredGeometryCtx&&)`

#### DeferredLightingCtx

**Purpose**: Proof that deferred lighting pass is active.

**Definition** (`VulkanPhaseContexts.h:64-75`):
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

**Lifecycle**:
- Created by `deferredLightingEnd(RecordingFrame&, DeferredGeometryCtx&&)`
- Consumed by `deferredLightingFinish(RecordingFrame&, DeferredLightingCtx&&, const vk::Rect2D&)`

**Typestate Enforcement**: The `frameIndex` member provides runtime validation that the token was created for the current frame, catching bugs where tokens are accidentally held across frame boundaries.

**See Also**: `docs/vulkan/VULKAN_DEFERRED_LIGHTING_FLOW.md` for complete deferred lighting pipeline documentation.

---

## 4. Token Creation

### 4.1 RecordingFrame Creation

**Function**: `VulkanRenderer::beginRecording()`

**Called At**: Frame start, after acquiring swapchain image

**Implementation** (`VulkanRenderer.cpp:472-479`):
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

### 4.2 FrameCtx Creation

**Function**: `currentFrameCtx()` helper

**Location**: `VulkanGraphics.cpp:96-101`

**Implementation**:
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

**Key Point**: Can be called multiple times; returns new `FrameCtx` each time (holds references). Asserts that `flip()` has been called to start recording.

### 4.3 RenderCtx Creation

**Function**: `VulkanRenderer::ensureRenderingStarted(const FrameCtx& ctx)`

**Implementation** (`VulkanRenderer.cpp:496-506`):
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

**Lazy Initialization**: Render pass begins on first `ensureRenderingStarted()` call, not on target request. This allows setup operations between target selection and actual drawing.

### 4.4 Bound Frame Creation

**Functions**: `requireModelBound()`, `requireNanoVGBound()`, `requireDecalBound()`

**Usage Pattern**:
```cpp
void renderModel(const FrameCtx& frameCtx) {
    // Validate that ModelData UBO is bound and get proof token
    auto bound = requireModelBound(frameCtx);

    // Now we have compile-time proof that the UBO is bound
    // bound.modelSet, bound.modelUbo, etc. are guaranteed valid
}
```

**Key Point**: These functions perform runtime assertions at the boundary (validating UBO bindings) but return a token that serves as compile-time proof for downstream code.

### 4.5 UploadCtx Creation

**Function**: Internal, created during `beginFrame()`

**Usage**: Passed to `flushPendingUploads(UploadCtx)` at frame start

**Not Exposed**: Upload context is internal to texture upload system; engine code does not interact with it directly.

---

## 5. Token Consumption

### 5.1 Token Requirements by Function

**Functions Requiring FrameCtx**:
- `ensureRenderingStarted(FrameCtx)` - Begin render pass
- `setViewport(FrameCtx, ...)` - Set viewport
- `setScissor(FrameCtx, ...)` - Set scissor
- `pushDebugGroup(FrameCtx, ...)` - Debug labels
- `popDebugGroup(FrameCtx)` - End debug label
- `beginSceneTexture(FrameCtx, ...)` - Begin HDR scene rendering
- `endSceneTexture(FrameCtx, ...)` - End HDR scene rendering
- `copySceneEffectTexture(FrameCtx)` - Copy scene for effects
- `getBindlessTextureIndex(FrameCtx, ...)` - Texture lookup
- `setBitmapRenderTarget(FrameCtx, ...)` - Set render-to-texture target

**Functions Requiring RenderCtx**:
- Draw operations (implicit - `RenderCtx` contains `cmd`)
- Pipeline binding (implicit - uses `RenderCtx.cmd`)
- Descriptor binding (implicit - uses `RenderCtx.cmd`)

**Functions Requiring RecordingFrame**:
- `advanceFrame(RecordingFrame prev)` - Ends current frame, submits, begins next (takes by value, consumes)
- `endFrame(RecordingFrame& rec)` - End command buffer recording (takes by reference)
- `submitRecordedFrame(RecordingFrame& rec)` - Submit to GPU (takes by reference)
- `deferredLightingBegin(RecordingFrame&, bool)` - Begin deferred geometry pass
- `deferredLightingEnd(RecordingFrame&, DeferredGeometryCtx&&)` - Transition to lighting pass
- `deferredLightingFinish(RecordingFrame&, DeferredLightingCtx&&, vk::Rect2D)` - Complete deferred lighting

### 5.2 Token Validation

Tokens are validated to ensure they belong to the correct renderer instance:

**FrameCtx Validation Example**:
```cpp
void VulkanRenderer::beginSceneTexture(const FrameCtx& ctx, bool enableHdrPipeline)
{
    Assertion(&ctx.renderer == this,
        "beginSceneTexture called with FrameCtx from a different VulkanRenderer instance");
    // ...
}
```

**Key Point**: This validation pattern is used consistently across all `FrameCtx`-accepting methods, preventing accidental cross-renderer token usage.

### 5.3 Token Movement

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

**FrameCtx Exception**: `FrameCtx` is copyable because it holds references, not ownership. This allows it to be passed to multiple functions within the same frame.

---

## 6. Token Lifetime

### 6.1 RecordingFrame Lifetime

| Event | Description |
|-------|-------------|
| Created | `beginRecording()` at frame start |
| Valid | Throughout command recording |
| Destroyed | `advanceFrame()` consumes it, creates new one |

**Scope**: Single frame recording session

**Storage**: `std::optional<RecordingFrame> g_backend->recording`

### 6.2 FrameCtx Lifetime

| Event | Description |
|-------|-------------|
| Created | `currentFrameCtx()` - can be called anytime during recording |
| Valid | While `RecordingFrame` exists |
| Destroyed | When scope ends (but underlying frame remains valid) |

**Key Point**: Multiple `FrameCtx` instances can exist simultaneously (they're all just references to the same frame).

### 6.3 RenderCtx Lifetime

| Event | Description |
|-------|-------------|
| Created | `ensureRenderingStarted()` - begins render pass if needed |
| Valid | While render pass is active |
| Invalid | When render pass ends |

**Render pass ends automatically when**:
- Target switches (`requestSwapchainTarget()`, `requestPostLdrTarget()`, etc.)
- Frame ends (`endFrame()`)
- Rendering suspended (`suspendRendering()`)
- Different render target requested (`beginSceneTexture()`, `endSceneTexture()`)

### 6.4 Deferred Lighting Token Lifetime

```
deferredLightingBegin() --> DeferredGeometryCtx created
        |
        | (geometry rendering)
        v
deferredLightingEnd()   --> DeferredGeometryCtx consumed
                        --> DeferredLightingCtx created
        |
        | (lighting calculations)
        v
deferredLightingFinish()--> DeferredLightingCtx consumed
                        --> Back to normal rendering
```

### 6.5 Lifetime Pitfalls

**Pitfall 1: Holding RenderCtx Across Target Switch**
```cpp
// WRONG
auto renderCtx = ensureRenderingStarted(frameCtx);
// ... draw to swapchain ...
requestPostLdrTarget();  // Ends render pass!
renderCtx.cmd.draw(...);  // ERROR: Render pass ended, cmd invalid for drawing
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

**Pitfall 2: Holding FrameCtx After Frame End**
```cpp
// WRONG
auto ctx = currentFrameCtx();
flip();  // Advances frame, destroys RecordingFrame
ctx.frame();  // ERROR: RecordingFrame destroyed, reference invalid
```

**Fix**: Get new `FrameCtx` after frame advance:
```cpp
// CORRECT
flip();  // Advances frame
auto ctx = currentFrameCtx();  // New token for new frame
ctx.frame();  // OK
```

---

## 7. Common Patterns

### 7.1 Pattern: Basic Draw Operation

```cpp
void drawModel(const FrameCtx& frameCtx, ModelData& model) {
    // Get render context (begins pass if needed)
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);

    // Get pipeline using target format info
    PipelineKey key = buildPipelineKey(renderCtx.targetInfo);
    vk::Pipeline pipeline = getPipeline(key);

    // Bind pipeline and draw
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
}
```

### 7.2 Pattern: Multiple Draws in Same Pass

```cpp
void drawMultipleModels(const FrameCtx& frameCtx, const std::vector<ModelData>& models) {
    // Get render context once
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);

    // Bind pipeline once (if all models use same pipeline)
    vk::Pipeline pipeline = getPipeline(...);
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

    // Draw all models in the same pass
    for (const auto& model : models) {
        // ... set per-model uniforms, bind per-model descriptors ...
        renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
    }
}
```

### 7.3 Pattern: Target Switch

```cpp
void renderSceneThenPostProcess(const FrameCtx& frameCtx) {
    // Scene rendering to HDR target
    frameCtx.renderer.beginSceneTexture(frameCtx, true);
    auto sceneCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    // ... draw scene ...

    // Post-processing (target switch ends scene pass automatically)
    frameCtx.renderer.endSceneTexture(frameCtx, true);

    // Post-processing uses new target - get new RenderCtx
    auto postCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    // ... draw post-processing effects ...
}
```

### 7.4 Pattern: Bound Frame Validation

```cpp
void renderModelWithValidation(const FrameCtx& frameCtx, ModelData& model) {
    // Validate UBO bindings at the boundary
    auto bound = requireModelBound(frameCtx);

    // Now we have proof that ModelData UBO is bound
    auto renderCtx = bound.ctx.renderer.ensureRenderingStarted(bound.ctx);

    // Use the validated binding info
    renderCtx.cmd.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        layout,
        0,
        { bound.modelSet },
        { bound.transformDynamicOffset }
    );

    renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
}
```

### 7.5 Pattern: Deferred Lighting

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

## 8. Common Mistakes

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
// Token goes out of scope naturally
```

### Mistake 3: Copying Move-Only Token

```cpp
// WRONG
auto renderCtx = ensureRenderingStarted(frameCtx);
auto ctx2 = renderCtx;  // ERROR: RenderCtx is move-only
```

**Fix**: Move instead (if transfer of ownership is intended):
```cpp
// CORRECT
auto renderCtx = ensureRenderingStarted(frameCtx);
auto ctx2 = std::move(renderCtx);  // OK: Move semantics
// renderCtx is now in moved-from state, do not use
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

### Mistake 5: Forgetting to Validate Bound Frames

```cpp
// WRONG: Assumes UBO is bound without proof
void renderModel(const FrameCtx& frameCtx) {
    auto& frame = frameCtx.frame();
    // Using modelDescriptorSet() without validation
    auto set = frame.modelDescriptorSet();  // May be invalid!
}

// CORRECT: Validate and get proof token
void renderModel(const FrameCtx& frameCtx) {
    auto bound = requireModelBound(frameCtx);  // Validates bindings
    // bound.modelSet is guaranteed valid
}
```

### Mistake 6: Deferred Lighting Call Order Violation

```cpp
// WRONG: Skipping phases
gr_vulkan_deferred_lighting_begin(true);
gr_vulkan_deferred_lighting_finish();  // ERROR: Missing end()

// WRONG: Wrong order
gr_vulkan_deferred_lighting_end();  // ERROR: No begin() called

// CORRECT: Proper sequence
gr_vulkan_deferred_lighting_begin(true);
// ... geometry rendering ...
gr_vulkan_deferred_lighting_end();
// ... lighting calculations ...
gr_vulkan_deferred_lighting_finish();
```

---

## 9. Thread Safety

### 9.1 Single-Threaded Design

The capability token system is designed for single-threaded rendering. All tokens are created, used, and destroyed on the main render thread.

**Thread-Safety Guarantees**:
- Tokens themselves are not thread-safe
- Token creation, validation, and consumption must occur on the render thread
- Global backend state (`g_backend`) is accessed only from the render thread

### 9.2 Why Tokens Are Not Thread-Safe

Move-only semantics prevent duplication within a single thread but do not prevent data races across threads. If multi-threaded command recording is needed in the future, the token system would need to be extended with:
- Per-thread command buffer tokens
- Thread-safe token minting
- Synchronization primitives for phase transitions

### 9.3 GPU Synchronization

While the token system is single-threaded on the CPU, it coordinates with GPU work:

- `InFlightFrame` tracks GPU completion via timeline semaphores
- `SubmitInfo.timeline` is used to wait for previous frames before reusing resources
- Frame resources are not reused until the GPU signals completion

---

## 10. Error Handling and Debugging

### 10.1 Assertion-Based Validation

Tokens use assertions to catch programming errors:

```cpp
Assertion(g_backend->recording.has_value(),
    "Recording not started - flip() must be called first");
```

**Assertion Types**:
- **Precondition checks**: Validate that required state exists before creating a token
- **Cross-validation**: Ensure tokens belong to the correct renderer instance
- **Binding validation**: Verify UBO bindings before creating bound-frame tokens

### 10.2 Debugging Token Issues

**Symptom: "Recording not started" assertion**
- **Cause**: Attempting to use `currentFrameCtx()` or similar before `flip()` has been called
- **Fix**: Ensure frame initialization is complete before rendering

**Symptom: "FrameCtx from a different VulkanRenderer instance" assertion**
- **Cause**: Token created from one renderer instance passed to another
- **Fix**: Ensure consistent renderer usage (typically there's only one)

**Symptom: "UBO binding not set" assertion**
- **Cause**: Calling a `require*Bound()` function before binding the required uniform buffer
- **Fix**: Call `gr_bind_uniform_buffer()` with the appropriate type before rendering

**Symptom: "Deferred lighting end called without a matching begin" assertion**
- **Cause**: Deferred lighting phases called out of order
- **Fix**: Follow the correct sequence: `begin()` -> `end()` -> `finish()`

### 10.3 Debug Labels

Use debug groups to label rendering sections in GPU profilers (RenderDoc, etc.):

```cpp
auto ctx = currentFrameCtx();
ctx.renderer.pushDebugGroup(ctx, "Model Rendering");
// ... render models ...
ctx.renderer.popDebugGroup(ctx);
```

### 10.4 Validation Layers

When Vulkan validation layers are enabled, additional checks occur at the Vulkan API level. Token misuse that leads to invalid Vulkan calls (e.g., drawing outside a render pass) will trigger validation errors.

---

## 11. Integration with Legacy Code

### 11.1 Bridge Pattern

**File**: `code/graphics/vulkan/VulkanGraphics.cpp`

**Pattern**: Legacy engine code doesn't use tokens directly. Bridge functions create tokens from global state:

```cpp
void gr_vulkan_draw_something() {
    // Bridge: Create token from global state
    auto ctx = currentFrameCtx();

    // Call token-based API
    drawSomething(ctx);
}
```

### 11.2 Global State Access Functions

**Functions**: `currentFrameCtx()`, `currentFrame()`, `currentRecording()`, `currentRenderer()`

**Location**: `VulkanGraphics.cpp:76-101`

**Implementation**:
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

**Usage**: Called by legacy code paths that need frame access. These functions assert that recording is active, bridging legacy implicit state to explicit tokens.

### 11.3 Token Creation in Legacy Paths

**Pattern**: Legacy code calls `gr_vulkan_*` functions, which create tokens internally:

```cpp
// Legacy engine code
gr_vulkan_set_viewport(x, y, w, h);

// Bridge function in VulkanGraphics.cpp
void gr_vulkan_set_viewport(int x, int y, int w, int h) {
    auto ctx = currentFrameCtx();  // Create token from global state
    ctx.renderer.setViewport(ctx, viewport);  // Use token
}
```

### 11.4 Gradual Migration

When adding new rendering features:

1. **New code**: Use token-based APIs directly where possible
2. **Legacy integration**: Use bridge functions that create tokens from global state
3. **Validation**: The bridge functions' assertions catch misuse at runtime

Over time, more code can be migrated to use tokens directly, improving compile-time safety.

---

## References

- `code/graphics/vulkan/VulkanFrameCaps.h` - FrameCtx, ModelBoundFrame, NanoVGBoundFrame, DecalBoundFrame
- `code/graphics/vulkan/VulkanPhaseContexts.h` - UploadCtx, RenderCtx, DeferredGeometryCtx, DeferredLightingCtx
- `code/graphics/vulkan/VulkanFrameFlow.h` - RecordingFrame, InFlightFrame, SubmitInfo
- `code/graphics/vulkan/VulkanRenderer.h` - Token-consuming API declarations
- `code/graphics/vulkan/VulkanRenderer.cpp` - Token-consuming API implementations
- `code/graphics/vulkan/VulkanGraphics.cpp` - Token creation helpers and bridge functions
- `docs/DESIGN_PHILOSOPHY.md` - Capability token principles and type-driven design philosophy
- `docs/vulkan/VULKAN_DEFERRED_LIGHTING_FLOW.md` - Deferred lighting pipeline documentation
