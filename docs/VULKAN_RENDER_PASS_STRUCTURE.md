# Vulkan Render Pass Structure - Complete Reference

This document provides comprehensive documentation of the Vulkan dynamic rendering
architecture used in the FreeSpace 2 Vulkan backend.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Render Target Types](#render-target-types)
3. [G-Buffer Specification](#g-buffer-specification)
4. [Depth Attachment System](#depth-attachment-system)
5. [Scene Capture Resources](#scene-capture-resources)
6. [Active Pass (RAII)](#active-pass-raii)
7. [Layout Transition System](#layout-transition-system)
8. [Dynamic State Management](#dynamic-state-management)
9. [Clear Operations](#clear-operations)
10. [Frame Lifecycle](#frame-lifecycle)
11. [Code Reference Summary](#code-reference-summary)
12. [Design Principles](#design-principles)

---

## Architecture Overview

The renderer uses **Vulkan dynamic rendering** (`vkCmdBeginRendering` / `vkCmdEndRendering`)
instead of traditional `VkRenderPass` objects. This design:

- Avoids `VkSubpass` complexity and implicit subpass dependencies
- Uses explicit layout transitions via `pipelineBarrier2()` with `VkImageMemoryBarrier2`
- Leverages the **synchronization2** extension for fine-grained memory barriers
- Employs RAII pattern (`ActivePass`) for automatic pass lifecycle management
- Supports selectable depth attachments (main scene depth vs cockpit depth)

**Key Source Files:**

| File | Purpose |
|------|---------|
| `VulkanRenderingSession.h` | Render pass state machine, target type definitions, RAII guard |
| `VulkanRenderingSession.cpp` | Dynamic rendering, layout transitions, barrier logic |
| `VulkanRenderTargets.h` | G-buffer, depth, and post-process image ownership |
| `VulkanRenderTargetInfo.h` | Pipeline-compatible attachment format contract |
| `VulkanPhaseContexts.h` | Capability tokens (`RenderCtx`, `UploadCtx`, `DeferredGeometryCtx`, etc.) |
| `VulkanRendererDeferredLighting.cpp` | Deferred lighting pipeline |
| `VulkanRendererFrameFlow.cpp` | Frame management |
| `VulkanTextureManager.h/cpp` | Bitmap render target management for RTT |

---

## Render Target Types

The render target state machine uses polymorphic `RenderTargetState` classes defined
in `VulkanRenderingSession.h`. Each target type specifies the attachment configuration
through the `RenderTargetInfo` contract used for pipeline cache keying.

### Pipeline Contract Structure

```cpp
// VulkanRenderTargetInfo.h
struct RenderTargetInfo {
    vk::Format colorFormat = vk::Format::eUndefined;
    uint32_t colorAttachmentCount = 1;
    vk::Format depthFormat = vk::Format::eUndefined;  // eUndefined => no depth
};
```

### 1. SwapchainWithDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | Swapchain format (typically `B8G8R8A8Srgb`) | Final composited output |
| Depth | Device-selected (see [Depth Format Selection](#depth-format-selection)) | 3D scene depth testing |

**Request:** `requestSwapchainTarget()`

**Usage:** Main 3D scene rendering, post-deferred composition with depth. This is
the default target at frame start.

### 2. SwapchainNoDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | Swapchain format | 2D overlay rendering |

**Request:** `requestSwapchainNoDepthTarget()`

**Usage:** Deferred lighting composition, HUD/UI overlay, NanoVG rendering.
Automatically selected after `endDeferredGeometry()`.

### 3. SceneHdrWithDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | HDR scene color buffer |
| Depth | Device-selected (shared) | 3D scene depth testing |

**Request:** `requestSceneHdrTarget()`

**Usage:** Main scene rendering when HDR post-processing is enabled (scene texture mode).

### 4. SceneHdrNoDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | HDR scene color buffer |

**Request:** `requestSceneHdrNoDepthTarget()`

**Usage:** HDR compositing passes that do not require depth testing. Also used for
deferred lighting output when scene texture mode is active.

### 5. DeferredGBufferTarget

| Attachment | Index | Format | Content |
|------------|-------|--------|---------|
| Albedo | 0 | `R16G16B16A16Sfloat` | Diffuse color + roughness |
| Normal | 1 | `R16G16B16A16Sfloat` | World normal + metalness |
| Position | 2 | `R16G16B16A16Sfloat` | World position + linear depth |
| Specular | 3 | `R16G16B16A16Sfloat` | Specular color + AO |
| Emissive | 4 | `R16G16B16A16Sfloat` | Self-illumination + intensity |
| Depth | - | Device-selected (shared) | Scene depth |

**Request:** `beginDeferredPass(clearNonColorBufs, preserveEmissive)`

**Alternative:** `requestDeferredGBufferTarget()` - Selects the G-buffer target without
modifying clear/load ops (used by decal pass to restore G-buffer target after individual
attachment rendering).

**Usage:** Deferred geometry pass - renders material properties to G-buffer.

**Parameters:**
- `clearNonColorBufs`: Reserved for API parity; Vulkan always clears non-emissive attachments
- `preserveEmissive`: When `true`, loads emissive attachment (for pre-deferred scene content)

### 6. GBufferAttachmentTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | Single G-buffer attachment |

**Request:** `requestGBufferAttachmentTarget(uint32_t gbufferIndex)`

**Usage:** Decal rendering - renders into a single G-buffer attachment with
alpha blending. Always uses `eLoad` for load op to preserve existing content.
No depth attachment is bound for this target.

### 7. GBufferEmissiveTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | Emissive G-buffer only |

**Request:** `requestGBufferEmissiveTarget()`

**Usage:** Pre-deferred scene color snapshot capture. Uses `eDontCare` load op
since the fullscreen copy shader overwrites all pixels. No depth attachment.

### 8. PostLdrTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `B8G8R8A8Unorm` | LDR post-processing target |

**Request:** `requestPostLdrTarget()`

**Usage:** Post-tonemapping effects (film grain, color grading).

### 9. PostLuminanceTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `B8G8R8A8Unorm` | Luminance for FXAA |

**Request:** `requestPostLuminanceTarget()`

**Usage:** FXAA prepass luminance calculation.

### 10. SmaaEdgesTarget / SmaaBlendTarget / SmaaOutputTarget

| Target | Attachment | Format | Purpose |
|--------|------------|--------|---------|
| Edges | Color 0 | `B8G8R8A8Unorm` | Edge detection output |
| Blend | Color 0 | `B8G8R8A8Unorm` | Blending weight calculation |
| Output | Color 0 | `B8G8R8A8Unorm` | Final SMAA output |

**Request:** `requestSmaaEdgesTarget()`, `requestSmaaBlendTarget()`, `requestSmaaOutputTarget()`

**Usage:** SMAA anti-aliasing pipeline stages.

### 11. BloomMipTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | Bloom ping-pong mip level |

**Request:** `requestBloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel)`

**Constraints:**
- `pingPongIndex`: 0 or 1 (see `kBloomPingPongCount`)
- `mipLevel`: 0 to 3 (see `kBloomMipLevels`)
- Base resolution is half of swapchain extent, further halved per mip level

**Usage:** Bloom downsample/upsample passes with configurable mip level and
ping-pong index.

### 12. BitmapTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | Texture format (varies) | Render-to-texture output |

**Request:** `requestBitmapTarget(int bitmapHandle, int face)`

**Parameters:**
- `bitmapHandle`: Engine bitmap handle (must be a valid render target)
- `face`: Cubemap face index (0-5) or 0 for 2D textures

**Usage:** Cockpit displays, cubemap faces, dynamic texture generation.

### Target Introspection and State Query

The rendering session provides methods to query the current target state and rendering status:

```cpp
// Target introspection (used by VulkanRenderer to pick capture source)
bool targetIsSceneHdr() const;      // True if current target is SceneHdrWithDepth or SceneHdrNoDepth
bool targetIsSwapchain() const;     // True if current target is SwapchainWithDepth or SwapchainNoDepth
const char* debugTargetName() const; // Returns human-readable target name for debugging

// Rendering state
bool renderingActive() const;       // True if dynamic rendering pass is currently active
```

**Usage:**
- `targetIsSceneHdr()` / `targetIsSwapchain()`: Used by the deferred lighting system to determine
  whether to capture from the scene HDR buffer or the swapchain for pre-deferred emissive content.
- `debugTargetName()`: Returns strings like `"swapchain+depth"`, `"gbuffer"`, `"bloom_mip"` for
  debug logging and validation messages.
- `renderingActive()`: Allows checking if `suspendRendering()` is needed before transfer operations.

### Additional Capture Methods

```cpp
// Capture swapchain content to a bitmap render target (RTT).
// No-op if swapchain lacks TRANSFER_SRC usage or target dimensions mismatch.
void captureSwapchainColorToRenderTarget(vk::CommandBuffer cmd, uint32_t imageIndex, int renderTargetHandle);
```

This method copies the current swapchain image to a bitmap render target for effects like
saved-screen captures. The target must have matching dimensions with the swapchain.

---

## G-Buffer Specification

**Constants from `VulkanRenderTargets.h`:**

```cpp
static constexpr uint32_t kGBufferCount = 5;
static constexpr uint32_t kGBufferEmissiveIndex = 4;
```

### Channel Definitions

All G-buffer attachments use `R16G16B16A16Sfloat` format.

**Index 0: Albedo/Color**
```
.rgb = Diffuse albedo color (linear color space)
.a   = Surface roughness (0.0 = mirror smooth, 1.0 = fully rough)
```

**Index 1: Normal**
```
.rgb = World-space surface normal (normalized, range -1.0 to +1.0)
.a   = Metalness factor (0.0 = dielectric/non-metal, 1.0 = pure metal)
```

**Index 2: Position**
```
.rgb = World-space fragment position (absolute world coordinates)
.a   = Linear depth (eye-space Z distance from camera)
```

**Index 3: Specular**
```
.rgb = Specular reflectance color (F0 for dielectrics, albedo-tinted for metals)
.a   = Ambient occlusion factor (1.0 = fully lit, 0.0 = fully occluded)
```

**Index 4: Emissive**
```
.rgb = Self-illumination color (HDR capable, values > 1.0 supported)
.a   = Emission intensity multiplier
```

### Memory Organization

```cpp
// VulkanRenderTargets.h (private members)
std::array<vk::UniqueImage, kGBufferCount> m_gbufferImages;
std::array<vk::UniqueDeviceMemory, kGBufferCount> m_gbufferMemories;
std::array<vk::UniqueImageView, kGBufferCount> m_gbufferViews;
std::array<vk::ImageLayout, kGBufferCount> m_gbufferLayouts{};
vk::UniqueSampler m_gbufferSampler;  // Linear filtering
vk::Format m_gbufferFormat = vk::Format::eR16G16B16A16Sfloat;
```

Each G-buffer image has its own memory allocation for flexibility. The sampler uses
linear filtering for the lighting pass.

---

## Depth Attachment System

The renderer supports two selectable depth attachments to enable proper depth
handling for cockpit rendering (OpenGL parity):

### Depth Selection API

```cpp
void useMainDepthAttachment();      // Scene depth (default)
void useCockpitDepthAttachment();   // Cockpit-only depth
```

### Depth Resources

| Resource | Purpose |
|----------|---------|
| Main Depth | Primary scene depth buffer, shared between swapchain/deferred targets |
| Cockpit Depth | Separate depth for cockpit geometry, populated between save/restore zbuffer calls |

### Depth Format Selection

The depth format is selected at initialization from a preference order that prioritizes
formats with stencil support for future extensibility:

```cpp
// VulkanRenderTargets.cpp - findDepthFormat()
const std::vector<vk::Format> candidates = {
    vk::Format::eD32SfloatS8Uint,   // 32-bit depth + 8-bit stencil (preferred)
    vk::Format::eD24UnormS8Uint,    // 24-bit depth + 8-bit stencil
    vk::Format::eD32Sfloat,         // 32-bit depth only (fallback)
};
```

The first format that supports both `eDepthStencilAttachment` and `eSampledImage`
features is selected. This ensures the depth buffer can be used both for rendering
and for deferred lighting/post-processing sampling.

### Depth Layout Helpers

```cpp
bool depthHasStencil() const;
// Returns: true if depth format includes stencil (eD32SfloatS8Uint or eD24UnormS8Uint)

vk::ImageLayout depthAttachmentLayout() const;
// Returns: eDepthStencilAttachmentOptimal (if stencil) or eDepthAttachmentOptimal

vk::ImageLayout depthReadLayout() const;
// Returns: eDepthStencilReadOnlyOptimal (if stencil) or eShaderReadOnlyOptimal

vk::ImageAspectFlags depthAttachmentAspectMask() const;
// Returns: eDepth (if no stencil) or eDepth | eStencil
```

The layout helpers account for whether the depth format includes a stencil component.
When the depth format has stencil, the rendering session automatically includes
stencil attachment info in `vk::RenderingInfo` during `beginRendering()` calls.

### Depth Sampler

Depth sampling uses nearest-neighbor filtering because linear filtering is often
unsupported for depth formats:

```cpp
vk::SamplerCreateInfo depthSamplerInfo{};
depthSamplerInfo.magFilter = vk::Filter::eNearest;
depthSamplerInfo.minFilter = vk::Filter::eNearest;
```

---

## Scene Capture Resources

### Scene Color Snapshot (Pre-Deferred)

Captures swapchain content before deferred pass for emissive background compositing.

```cpp
// One image per swapchain index
SCP_vector<vk::UniqueImage> m_sceneColorImages;
SCP_vector<vk::UniqueImageView> m_sceneColorViews;
// Format: matches swapchain format
```

**Capture:** `captureSwapchainColorToSceneCopy(cmd, imageIndex)`

**Requirement:** Swapchain must be created with `TRANSFER_SRC` usage flag.

### Scene HDR Color

Float-format scene color for HDR post-processing pipeline.

```cpp
vk::UniqueImage m_sceneHdrImage;
vk::UniqueImageView m_sceneHdrView;
vk::UniqueSampler m_sceneHdrSampler;
vk::Format m_sceneHdrFormat = vk::Format::eR16G16B16A16Sfloat;
```

**Transition:** `transitionSceneHdrToShaderRead(cmd)`

### Scene Effect Snapshot

Mid-scene capture for distortion and effects that sample the scene.

```cpp
vk::UniqueImage m_sceneEffectImage;
vk::UniqueImageView m_sceneEffectView;
vk::UniqueSampler m_sceneEffectSampler;
// Format: matches sceneHdrFormat
```

**Copy:** `copySceneHdrToEffect(cmd)` - ends active pass, copies, transitions for sampling

### Bloom Resources

**Constants:**
```cpp
static constexpr uint32_t kBloomPingPongCount = 2;
static constexpr uint32_t kBloomMipLevels = 4;
```

```cpp
std::array<vk::UniqueImage, kBloomPingPongCount> m_bloomImages;
std::array<vk::UniqueImageView, kBloomPingPongCount> m_bloomViews;  // Full mip chain views
std::array<std::array<vk::UniqueImageView, kBloomMipLevels>, kBloomPingPongCount> m_bloomMipViews;
// Format: R16G16B16A16Sfloat
// Resolution: half of swapchain, with 4 mip levels
```

Each bloom image has both a full-image view (`m_bloomViews`) for sampling the entire
mip chain, and per-mip views (`m_bloomMipViews`) for rendering to individual mip levels.

---

## Active Pass (RAII)

**Location:** `VulkanRenderingSession.h`, private struct

The `ActivePass` struct implements RAII-based render pass lifecycle management:

```cpp
struct ActivePass {
    vk::CommandBuffer cmd{};

    explicit ActivePass(vk::CommandBuffer c) : cmd(c) {}

    ~ActivePass() {
        if (cmd) {
            cmd.endRendering();
        }
    }

    // Move-only semantics
    ActivePass(const ActivePass&) = delete;
    ActivePass& operator=(const ActivePass&) = delete;
    ActivePass(ActivePass&& other) noexcept;
    ActivePass& operator=(ActivePass&& other) noexcept;
};
```

### Storage and State

```cpp
// VulkanRenderingSession.h (private members)
std::optional<ActivePass> m_activePass;
RenderTargetInfo m_activeInfo{};
```

### Lifecycle Guarantees

1. **Automatic Termination:** Destructor ensures `vkCmdEndRendering()` is called
2. **Exception Safety:** Pass ends correctly even during stack unwinding
3. **Move Semantics:** Guard ownership can transfer between scopes
4. **Lazy Initialization:** Pass begins on first `ensureRendering()` call

### Usage Pattern

```cpp
RenderTargetInfo VulkanRenderingSession::ensureRendering(
    vk::CommandBuffer cmd, uint32_t imageIndex)
{
    if (m_activePass.has_value()) {
        return m_activeInfo;  // Already rendering
    }

    auto info = m_target->info(m_device, m_targets);
    m_target->begin(*this, cmd, imageIndex);
    m_activeInfo = info;
    applyDynamicState(cmd);
    m_activePass.emplace(cmd);  // Store guard
    return info;
}
```

**Suspension for Transfers:**

```cpp
void suspendRendering() { endActivePass(); }  // For texture updates
```

---

## Layout Transition System

All layout transitions use the **synchronization2** extension with `VkImageMemoryBarrier2`.

### Stage/Access Mapping

```cpp
// VulkanRenderingSession.cpp - anonymous namespace
StageAccess stageAccessForLayout(vk::ImageLayout layout) {
    switch (layout) {
    case eUndefined:
        return {eTopOfPipe, {}};
    case eColorAttachmentOptimal:
        return {eColorAttachmentOutput, eColorAttachmentRead | eColorAttachmentWrite};
    case eDepthAttachmentOptimal:
    case eDepthStencilAttachmentOptimal:
        return {eEarlyFragmentTests | eLateFragmentTests,
                eDepthStencilAttachmentRead | eDepthStencilAttachmentWrite};
    case eShaderReadOnlyOptimal:
        return {eFragmentShader, eShaderRead};
    case eTransferSrcOptimal:
        return {eTransfer, eTransferRead};
    case eTransferDstOptimal:
        return {eTransfer, eTransferWrite};
    case eDepthStencilReadOnlyOptimal:
        return {eFragmentShader, eShaderRead};
    case ePresentSrcKHR:
        return {{}, {}};  // External to pipeline
    default:
        return {eAllCommands, eMemoryRead | eMemoryWrite};
    }
}
```

### Public Transition Methods

| Method | Description |
|--------|-------------|
| `transitionSceneHdrToShaderRead(cmd)` | Scene HDR to shader-read for post-process |
| `transitionMainDepthToShaderRead(cmd)` | Main depth for lightshafts/effects |
| `transitionCockpitDepthToShaderRead(cmd)` | Cockpit depth for depth-aware effects |
| `transitionPostLdrToShaderRead(cmd)` | LDR target for subsequent passes |
| `transitionPostLuminanceToShaderRead(cmd)` | Luminance for FXAA |
| `transitionSmaaEdgesToShaderRead(cmd)` | SMAA edges for blend pass |
| `transitionSmaaBlendToShaderRead(cmd)` | SMAA blend weights for output |
| `transitionSmaaOutputToShaderRead(cmd)` | SMAA output for final composite |
| `transitionBloomToShaderRead(cmd, index)` | Bloom ping-pong for sampling |

### Internal Transition Methods

| Method | Source Layout | Destination Layout |
|--------|---------------|-------------------|
| `transitionSwapchainToAttachment()` | `ePresentSrcKHR` / `eUndefined` | `eColorAttachmentOptimal` |
| `transitionSwapchainToPresent()` | `eColorAttachmentOptimal` | `ePresentSrcKHR` |
| `transitionDepthToAttachment()` | varies | `eDepthStencilAttachmentOptimal` or `eDepthAttachmentOptimal` |
| `transitionGBufferToAttachment()` | `eShaderReadOnlyOptimal` / `eUndefined` | `eColorAttachmentOptimal` |
| `transitionGBufferToShaderRead()` | `eColorAttachmentOptimal` | `eShaderReadOnlyOptimal` |

### Barrier Example

```cpp
vk::ImageMemoryBarrier2 barrier{};
const auto src = stageAccessForLayout(oldLayout);
const auto dst = stageAccessForLayout(newLayout);
barrier.srcStageMask = src.stageMask;
barrier.srcAccessMask = src.accessMask;
barrier.dstStageMask = dst.stageMask;
barrier.dstAccessMask = dst.accessMask;
barrier.oldLayout = oldLayout;
barrier.newLayout = newLayout;
barrier.image = image;
barrier.subresourceRange = {aspectMask, 0, 1, 0, 1};

vk::DependencyInfo dep{};
dep.imageMemoryBarrierCount = 1;
dep.pImageMemoryBarriers = &barrier;
cmd.pipelineBarrier2(dep);
```

### Layout Tracking

```cpp
// VulkanRenderingSession.h
std::vector<vk::ImageLayout> m_swapchainLayouts;  // Per swapchain image
uint64_t m_swapchainGeneration = 0;               // Detects swapchain recreation

// VulkanRenderTargets.h
std::array<vk::ImageLayout, kGBufferCount> m_gbufferLayouts{};
vk::ImageLayout m_depthLayout = vk::ImageLayout::eUndefined;
vk::ImageLayout m_cockpitDepthLayout = vk::ImageLayout::eUndefined;
vk::ImageLayout m_sceneHdrLayout = vk::ImageLayout::eUndefined;
SCP_vector<vk::ImageLayout> m_sceneColorLayouts;  // Per swapchain index
std::array<vk::ImageLayout, kBloomPingPongCount> m_bloomLayouts{};
// ... and other post-process image layouts
```

**Swapchain Recreation Handling:** The rendering session tracks `m_swapchainGeneration`
(obtained from `VulkanDevice::swapchainGeneration()`) to detect when the swapchain has
been recreated (e.g., due to window resize). When the generation changes, `beginFrame()`
resets all swapchain layout tracking to `eUndefined` since the old images are invalid.

---

## Dynamic State Management

The renderer uses Vulkan dynamic state extensively to reduce pipeline permutations.

### State Tracking

```cpp
// VulkanRenderingSession.h (private members)
vk::CullModeFlagBits m_cullMode = vk::CullModeFlagBits::eBack;
bool m_depthTest = true;
bool m_depthWrite = true;
```

### Public State API

```cpp
void setCullMode(vk::CullModeFlagBits mode);
void setDepthTest(bool enable);
void setDepthWrite(bool enable);

vk::CullModeFlagBits cullMode() const;
bool depthTestEnabled() const;
bool depthWriteEnabled() const;
```

### Dynamic State Application

Applied automatically when `ensureRendering()` begins a pass:

```cpp
void VulkanRenderingSession::applyDynamicState(vk::CommandBuffer cmd) {
    const uint32_t attachmentCount = m_activeInfo.colorAttachmentCount;
    const bool hasDepthAttachment = (m_activeInfo.depthFormat != vk::Format::eUndefined);

    cmd.setCullMode(m_cullMode);
    cmd.setFrontFace(vk::FrontFace::eClockwise);  // CW for negative viewport Y-flip
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);

    const bool depthTest = hasDepthAttachment && m_depthTest;
    const bool depthWrite = hasDepthAttachment && m_depthWrite;
    cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
    cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
    cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
    cmd.setStencilTestEnable(VK_FALSE);

    // Extended dynamic state 3 (if supported)
    if (m_device.supportsExtendedDynamicState3()) {
        const auto& caps = m_device.extDyn3Caps();
        if (caps.colorBlendEnable) {
            // Baseline: blending OFF. Draw paths enable per-material.
            std::array<vk::Bool32, kGBufferCount> blendEnables{};
            blendEnables.fill(VK_FALSE);
            cmd.setColorBlendEnableEXT(0, attachmentCount, blendEnables.data());
        }
        if (caps.colorWriteMask) {
            // Full RGBA write mask for all attachments
            cmd.setColorWriteMaskEXT(0, attachmentCount, ...);
        }
        if (caps.polygonMode) {
            cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
        }
        if (caps.rasterizationSamples) {
            cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
        }
    }
}
```

### Supported Cull Modes

| Mode | Usage |
|------|-------|
| `eNone` | HUD/UI rendering, double-sided geometry |
| `eBack` | Standard 3D geometry (default) |
| `eFront` | Inside-out rendering (skybox/cubemaps) |

---

## Clear Operations

### ClearOps Structure

```cpp
// VulkanRenderingSession.h (private)
struct ClearOps {
    vk::AttachmentLoadOp color = vk::AttachmentLoadOp::eLoad;
    vk::AttachmentLoadOp depth = vk::AttachmentLoadOp::eLoad;
    vk::AttachmentLoadOp stencil = vk::AttachmentLoadOp::eLoad;

    static ClearOps clearAll() {
        return {eClear, eClear, eClear};
    }

    static ClearOps loadAll() {
        return {eLoad, eLoad, eLoad};
    }

    ClearOps withDepthStencilClear() const {
        return {color, eClear, eClear};
    }
};
```

### Clear State

```cpp
std::array<float, 4> m_clearColor{0.f, 0.f, 0.f, 1.f};
float m_clearDepth = 1.0f;
ClearOps m_clearOps = ClearOps::clearAll();
std::array<vk::AttachmentLoadOp, kGBufferCount> m_gbufferLoadOps{};
```

### Public Clear API

```cpp
void requestClear();          // Sets clearAll()
void requestDepthClear();     // Sets withDepthStencilClear()
void setClearColor(float r, float g, float b, float a);
```

### One-Shot Clear Behavior

Clears automatically revert to load after first use within a pass:

```cpp
// Inside beginSwapchainRenderingInternal, beginGBufferRenderingInternal, etc.
cmd.beginRendering(renderingInfo);

// Reset after consumption
m_clearOps = ClearOps::loadAll();
m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eLoad);
```

This prevents redundant clears across multiple draw calls if rendering is suspended
and resumed (e.g., for texture updates).

---

## Frame Lifecycle

### Complete Frame Flow

```
VulkanRenderer::beginFrame()
|
+-- VulkanRenderingSession::beginFrame(cmd, imageIndex)
|   |-- Resize swapchain layout tracking if needed
|   |-- endActivePass() (safety reset)
|   |-- Set target to SwapchainWithDepthTarget
|   |-- Set depth attachment to Main
|   |-- Set clearOps to clearAll()
|   |-- transitionSwapchainToAttachment()
|   +-- transitionDepthToAttachment()
|
+-- [First draw/clear]
|   +-- ensureRendering() -> beginRendering() + applyDynamicState()
|
+-- [3D Scene Rendering]
|   |-- Geometry draws to swapchain (or SceneHdr if HDR enabled)
|   +-- (or beginDeferredPass for deferred)
|
+-- [Deferred Path - if enabled]
|   |
|   +-- VulkanRenderer::deferredLightingBegin(rec, clearNonColorBufs) -> DeferredGeometryCtx
|   |   |-- captureSwapchainColorToSceneCopy() (if swapchain has TRANSFER_SRC)
|   |   |   OR transitionSceneHdrToShaderRead() (if scene texture mode)
|   |   |-- requestGBufferEmissiveTarget() + fullscreen copy
|   |   |-- beginDeferredPass(clearNonColorBufs, preserveEmissive)
|   |   +-- ensureRendering() (begin G-buffer rendering)
|   |
|   +-- [Geometry Pass]
|   |   |-- Record geometry to G-buffer (5 MRT attachments)
|   |   +-- Decals via requestGBufferAttachmentTarget (per-attachment blending)
|   |
|   +-- VulkanRenderer::deferredLightingEnd(rec, geometryCtx) -> DeferredLightingCtx
|   |   |-- VulkanRenderingSession::endDeferredGeometry(cmd)
|   |   |   |-- endActivePass()
|   |   |   |-- transitionGBufferToShaderRead() (includes depth)
|   |   |   +-- Switch to SwapchainNoDepthTarget
|   |   +-- If scene texture mode: requestSceneHdrNoDepthTarget()
|   |
|   +-- [Lighting Pass]
|   |   +-- VulkanRenderer::deferredLightingFinish(rec, lightingCtx, restoreScissor)
|   |       |-- bindDeferredGlobalDescriptors() (G-buffer + depth)
|   |       |-- buildDeferredLights() + recordDeferredLighting()
|   |       +-- requestSwapchainTarget() (restore scene target with depth)
|
+-- [Post-Processing - if enabled]
|   |-- Scene HDR -> shader read
|   |-- Bloom (ping-pong mip chain)
|   |-- Tonemapping -> PostLdrTarget or swapchain
|   |-- FXAA (PostLuminanceTarget) or SMAA (edges/blend/output)
|   +-- Final composite to swapchain
|
+-- [HUD Rendering]
|   |-- requestSwapchainNoDepthTarget() (if needed)
|   |-- NanoVG text/vector graphics
|   +-- Interface elements
|
+-- VulkanRenderingSession::endFrame(cmd, imageIndex)
    |-- endActivePass()
    +-- transitionSwapchainToPresent()
```

### Deferred Lighting API (Typestate Pattern)

The deferred lighting API uses context types to enforce correct sequencing:

```cpp
// VulkanPhaseContexts.h - Context token definitions (move-only, friend-constructed)
struct DeferredGeometryCtx {
    uint32_t frameIndex = 0;
    // Move-only, only constructible by VulkanRenderer
};
struct DeferredLightingCtx {
    uint32_t frameIndex = 0;
    // Move-only, only constructible by VulkanRenderer
};

// VulkanRenderer.h - Typestate API (begin -> end -> finish)
DeferredGeometryCtx deferredLightingBegin(RecordingFrame& rec, bool clearNonColorBufs);
DeferredLightingCtx deferredLightingEnd(RecordingFrame& rec, DeferredGeometryCtx&& geometry);
void deferredLightingFinish(RecordingFrame& rec, DeferredLightingCtx&& lighting, const vk::Rect2D& restoreScissor);
```

Context objects are move-only with private constructors (friend to `VulkanRenderer`), ensuring
they can only be obtained through the proper API calls. Frame index validation triggers
assertions on mismatched contexts.

---

## Code Reference Summary

| Component | File | Key Elements |
|-----------|------|--------------|
| Target state machine | `VulkanRenderingSession.h` | `RenderTargetState` base class, 12+ target types |
| Pipeline contract | `VulkanRenderTargetInfo.h` | `RenderTargetInfo` struct |
| Capability tokens | `VulkanPhaseContexts.h` | `RenderCtx`, `UploadCtx`, `DeferredGeometryCtx`, `DeferredLightingCtx` |
| RAII pass guard | `VulkanRenderingSession.h` | `ActivePass` struct |
| Clear operations | `VulkanRenderingSession.h` | `ClearOps` struct |
| Dynamic rendering | `VulkanRenderingSession.cpp` | `begin*RenderingInternal()` methods |
| Layout transitions | `VulkanRenderingSession.cpp` | `transition*()` methods, `stageAccessForLayout()` |
| Dynamic state | `VulkanRenderingSession.cpp` | `applyDynamicState()` |
| G-buffer constants | `VulkanRenderTargets.h` | `kGBufferCount`, `kGBufferEmissiveIndex` |
| Bloom constants | `VulkanRenderTargets.h` | `kBloomPingPongCount`, `kBloomMipLevels` |
| Depth format selection | `VulkanRenderTargets.cpp` | `findDepthFormat()`, `depthHasStencil()` |
| Image ownership | `VulkanRenderTargets.h` | Memory allocation, layout tracking |
| Deferred lighting | `VulkanRendererDeferredLighting.cpp` | `deferredLightingBegin()`, `deferredLightingEnd()`, `deferredLightingFinish()` |
| Bitmap RTT | `VulkanTextureManager.h` | Render target management |

---

## Design Principles

1. **Type-Driven State:** Target types encode valid attachment configurations,
   preventing invalid combinations. Each target's `info()` method returns a
   consistent `RenderTargetInfo` for pipeline cache keying.

2. **Explicit Synchronization:** All layout transitions use explicit barriers
   (`pipelineBarrier2`) with precise stage/access masks rather than implicit
   subpass dependencies. The `stageAccessForLayout()` helper centralizes this logic.

3. **RAII Lifecycle:** `ActivePass` stored in `std::optional` ensures proper pass
   termination even during exceptions. The guard is move-only to prevent accidental
   copies.

4. **Lazy Initialization:** Passes begin on first `ensureRendering()` call, not on
   target request. This allows target switching without beginning empty passes.

5. **Layout Tracking:** Per-image layout state (in `VulkanRenderTargets` and
   `VulkanRenderingSession`) prevents redundant transitions. Each transition method
   checks current layout before issuing barriers.

6. **One-Shot Clears:** Clear operations automatically revert to load after
   consumption. This handles the case where rendering is suspended mid-pass
   (e.g., for texture updates) and must resume without re-clearing.

7. **Depth Attachment Selection:** The dual-depth system (main + cockpit) enables
   proper depth handling for cockpit rendering without complex depth buffer copying.

8. **Decoupled Ownership:** `VulkanRenderTargets` owns image resources while
   `VulkanRenderingSession` manages render state. This separation allows resource
   resizing without state machine changes.

9. **Typestate Enforcement:** The deferred lighting API uses move-only context
   types (`DeferredGeometryCtx`, `DeferredLightingCtx`) to enforce correct call
   sequencing at compile time, with frame index validation at runtime.
