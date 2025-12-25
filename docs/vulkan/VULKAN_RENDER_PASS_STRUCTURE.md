# Vulkan Render Pass Structure - Complete Reference

This document provides comprehensive documentation of the Vulkan dynamic rendering architecture.

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Render Target Types](#render-target-types)
3. [G-Buffer Content Specification](#g-buffer-content-specification)
4. [Active Pass (RAII)](#active-pass-raii)
5. [Layout Transition System](#layout-transition-system)
6. [Dynamic State Management](#dynamic-state-management)
7. [Scene Color Copy Pipeline](#scene-color-copy-pipeline)
8. [Bitmap Target and Cubemap Rendering](#bitmap-target-and-cubemap-rendering)
9. [Clear Operations](#clear-operations)
10. [Frame Lifecycle](#frame-lifecycle)
11. [Code Reference Summary](#code-reference-summary)

---

## Architecture Overview

The renderer uses **Vulkan dynamic rendering** (`vkCmdBeginRendering`/`vkCmdEndRendering`) instead of traditional `VkRenderPass` objects. This design:

- Avoids `VkSubpass` complexity and implicit subpass dependencies
- Uses explicit layout transitions via `pipelineBarrier2()` with `VkImageMemoryBarrier2`
- Leverages the **synchronization2** extension for fine-grained memory barriers
- Employs RAII pattern (`ActivePass`) for automatic pass lifecycle management

**Key Source Files:**

| File | Purpose |
|------|---------|
| `VulkanRenderingSession.h` | Render pass state machine, target type definitions |
| `VulkanRenderingSession.cpp` | Dynamic rendering, layout transitions, barrier logic |
| `VulkanRenderTargets.h` | G-buffer image ownership and definitions |
| `VulkanRenderTargetInfo.h` | Attachment format contract interface |
| `VulkanRenderer.cpp` | Deferred lighting pipeline, frame management |

---

## Render Target Types

The render target state machine uses polymorphic `RenderTargetState` classes (`VulkanRenderingSession.h`):

### 1. SwapchainWithDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | Swapchain format (`B8G8R8A8Srgb` typical) | Final composited output |
| Depth | `D32Sfloat` or `D24UnormS8Uint` | 3D scene depth testing |

**Usage:** Main 3D scene rendering, post-deferred composition with depth.

### 2. SceneHdrWithDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | HDR scene color buffer |
| Depth | `D32Sfloat` (shared) | 3D scene depth testing |

**Usage:** Main scene rendering when HDR post-processing is enabled.

### 3. SceneHdrNoDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | HDR scene color buffer |

**Usage:** HDR compositing passes that do not require depth testing.

### 4. PostLdrTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R8G8B8A8Unorm` | LDR post-processing target |

**Usage:** Post-tonemapping effects (film grain, color grading).

### 5. PostLuminanceTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R8G8B8A8Unorm` | Luminance for FXAA |

**Usage:** FXAA prepass luminance calculation.

### 6. SmaaEdgesTarget / SmaaBlendTarget / SmaaOutputTarget

| Target | Attachment | Format | Purpose |
|--------|------------|--------|---------|
| Edges | Color 0 | `R8G8B8A8Unorm` | Edge detection output |
| Blend | Color 0 | `R8G8B8A8Unorm` | Blending weight calculation |
| Output | Color 0 | Swapchain/LDR format | Final SMAA output |

**Usage:** SMAA anti-aliasing pipeline stages.

### 7. BloomMipTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | Bloom ping-pong mip level |

**Usage:** Bloom downsample/upsample passes with configurable mip level and ping-pong index.

### 8. DeferredGBufferTarget

| Attachment | Index | Format |
|------------|-------|--------|
| Albedo | 0 | `R16G16B16A16Sfloat` |
| Normal | 1 | `R16G16B16A16Sfloat` |
| Position | 2 | `R16G16B16A16Sfloat` |
| Specular | 3 | `R16G16B16A16Sfloat` |
| Emissive | 4 | `R16G16B16A16Sfloat` |
| Depth | - | `D32Sfloat` (shared) |

**Usage:** Deferred geometry pass - renders material properties to G-buffer.

### 9. SwapchainNoDepthTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | Swapchain format | 2D overlay rendering |

**Key:** `depthFormat = vk::Format::eUndefined`

**Usage:** Deferred lighting composition, HUD/UI overlay, NanoVG rendering.

### 10. GBufferEmissiveTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | `R16G16B16A16Sfloat` | Emissive G-buffer only |

**Usage:** Pre-deferred scene color snapshot capture.

### 11. BitmapTarget

| Attachment | Format | Purpose |
|------------|--------|---------|
| Color 0 | Texture format (varies) | Render-to-texture output |
| Depth | Optional | RTT with depth testing |

**Usage:** Cockpit displays, cubemap faces, dynamic texture generation.

---

## G-Buffer Content Specification

**File:** `VulkanRenderTargets.h:15-22`

Complete channel definitions for all G-buffer attachments:

### Index 0: Albedo/Color (`R16G16B16A16Sfloat`)

```
.rgb = Diffuse albedo color (linear color space)
.a   = Surface roughness (0.0 = mirror smooth, 1.0 = fully rough)
```

### Index 1: Normal (`R16G16B16A16Sfloat`)

```
.rgb = World-space surface normal (normalized, range -1.0 to +1.0)
.a   = Metalness factor (0.0 = dielectric/non-metal, 1.0 = pure metal)
```

### Index 2: Position (`R16G16B16A16Sfloat`)

```
.rgb = World-space fragment position (absolute world coordinates)
.a   = Linear depth (eye-space Z distance from camera)
```

### Index 3: Specular (`R16G16B16A16Sfloat`)

```
.rgb = Specular reflectance color (F0 for dielectrics, albedo-tinted for metals)
.a   = Ambient occlusion factor (1.0 = fully lit, 0.0 = fully occluded)
```

### Index 4: Emissive (`R16G16B16A16Sfloat`)

```
.rgb = Self-illumination color (HDR capable, values > 1.0 supported)
.a   = Emission intensity multiplier
```

**G-Buffer Memory Layout:**

```cpp
// VulkanRenderTargets.h:65-91
class VulkanRenderTargets {
    std::array<VkImage, 5> m_gbufferImages;      // All 5 G-buffer images
    std::array<VkImageView, 5> m_gbufferViews;   // Corresponding views
    VkDeviceMemory m_gbufferMemory;              // Single allocation block

    VkImage m_depthImage;                         // Shared depth buffer
    VkImageView m_depthView;
    VkDeviceMemory m_depthMemory;

    std::vector<VkImage> m_sceneColorImages;      // Per-swapchain-index
    std::vector<VkImageView> m_sceneColorViews;
};
```

---

## Active Pass (RAII)

**File:** `VulkanRenderingSession.h:102-129`

The `ActivePass` struct implements RAII-based render pass lifecycle management:

```cpp
struct ActivePass {
    vk::CommandBuffer cmd{};

    // Constructor: stores command buffer for endRendering()
    explicit ActivePass(vk::CommandBuffer c) : cmd(c) {}

    // Destructor: automatically calls cmd.endRendering() if cmd is valid
    ~ActivePass() {
        if (cmd) {
            cmd.endRendering();
        }
    }

    // Move-only semantics (no copy)
    ActivePass(const ActivePass&) = delete;
    ActivePass& operator=(const ActivePass&) = delete;
    ActivePass(ActivePass&& other) noexcept : cmd(other.cmd) {
        other.cmd = vk::CommandBuffer{};
    }
    ActivePass& operator=(ActivePass&& other) noexcept;
};
```

**Lifecycle Guarantees:**

1. **Automatic Termination:** Destructor ensures `vkCmdEndRendering()` is called
2. **Exception Safety:** Pass ends correctly even during stack unwinding
3. **Move Semantics:** Guard ownership can transfer between scopes
4. **Lazy Initialization:** Pass begins on first draw/clear, not guard construction

**Usage Pattern:**

```cpp
void renderScene(VulkanRenderingSession& session, vk::CommandBuffer cmd) {
    // Guard created but pass not yet begun
    session.requestSwapchainTarget();

    // First draw triggers beginRendering() internally
    session.ensureRendering(cmd);  // Called by draw functions
    cmd.draw(...);

    // Additional draws reuse active pass
    cmd.draw(...);

}  // Destructor calls vkCmdEndRendering() automatically
```

---

## Layout Transition System

**File:** `VulkanRenderingSession.cpp:520-702`

All layout transitions use the **synchronization2** extension with `VkImageMemoryBarrier2`:

### Transition Functions

| Function | Line | Source Layout | Destination Layout |
|----------|------|---------------|-------------------|
| `transitionSwapchainToAttachment()` | 522 | `ePresentSrcKHR` or `eUndefined` | `eColorAttachmentOptimal` |
| `transitionDepthToAttachment()` | 545 | `eShaderReadOnlyOptimal` or `eUndefined` | `eDepthStencilAttachmentOptimal` |
| `transitionSwapchainToPresent()` | 569 | `eColorAttachmentOptimal` | `ePresentSrcKHR` |
| `transitionGBufferToAttachment()` | 594 | `eShaderReadOnlyOptimal` or `eUndefined` | `eColorAttachmentOptimal` |
| `transitionGBufferToShaderRead()` | 623 | `eColorAttachmentOptimal` | `eShaderReadOnlyOptimal` |

### Barrier Structure

```cpp
// Typical barrier using synchronization2
vk::ImageMemoryBarrier2 barrier{
    .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
    .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
    .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
    .dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead,
    .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
    .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    .image = gbufferImage,
    .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}
};

vk::DependencyInfo depInfo{
    .imageMemoryBarrierCount = 1,
    .pImageMemoryBarriers = &barrier
};

cmd.pipelineBarrier2(depInfo);
```

### Layout Tracking State

**File:** `VulkanRenderingSession.h:73-104`

```cpp
// Per-image layout tracking
std::vector<vk::ImageLayout> m_swapchainLayouts;  // Line 200, per swapchain image
std::array<vk::ImageLayout, 5> m_gbufferLayouts;  // Line 80, per G-buffer
vk::ImageLayout m_depthLayout;                     // Line 73, single depth buffer
std::vector<vk::ImageLayout> m_sceneColorLayouts;  // Line 89, per swapchain index
```

---

## Dynamic State Management

**File:** `VulkanRenderingSession.cpp:706-742`

The renderer uses Vulkan dynamic state extensively to reduce pipeline permutations:

### Viewport and Scissor (Lines 706-720)

```cpp
void setViewportScissor(vk::CommandBuffer cmd, const vk::Extent2D& extent) {
    vk::Viewport viewport{
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    cmd.setViewport(0, 1, &viewport);

    vk::Rect2D scissor{
        .offset = {0, 0},
        .extent = extent
    };
    cmd.setScissor(0, 1, &scissor);
}
```

### Cull Mode (Lines 721-728)

```cpp
void setCullMode(vk::CommandBuffer cmd, vk::CullModeFlags mode) {
    cmd.setCullMode(mode);  // VK_EXT_extended_dynamic_state
}
```

**Supported Modes:**
- `vk::CullModeFlagBits::eNone` - HUD/UI rendering
- `vk::CullModeFlagBits::eBack` - Standard 3D geometry
- `vk::CullModeFlagBits::eFront` - Inside-out rendering (cubemaps)

### Depth State (Lines 729-742)

```cpp
void setDepthState(vk::CommandBuffer cmd, bool testEnable, bool writeEnable,
                   vk::CompareOp compareOp) {
    cmd.setDepthTestEnable(testEnable ? VK_TRUE : VK_FALSE);
    cmd.setDepthWriteEnable(writeEnable ? VK_TRUE : VK_FALSE);
    cmd.setDepthCompareOp(compareOp);
}
```

### Blend State

Blend enable and equations set per draw via `VK_EXT_extended_dynamic_state3`:

```cpp
cmd.setColorBlendEnableEXT(0, blendEnable);
cmd.setColorBlendEquationEXT(0, blendEquation);
```

---

## Scene Color Copy Pipeline

**File:** `VulkanRenderingSession.cpp:50-56` (image definitions)

The scene color copy captures rendered content before deferred lighting for background compositing:

### Image Resources

```cpp
// One image per swapchain index
std::vector<VkImage> m_sceneColorImages;      // Lines 50-56
std::vector<VkImageView> m_sceneColorViews;
// Format: R16G16B16A16Sfloat (HDR capable)
```

### Copy Pipeline Stages

```
Stage 1: Prepare Destination
    m_sceneColorImages[index]: eUndefined -> eTransferDstOptimal

Stage 2: Execute Copy
    vkCmdCopyImage(cmd,
        swapchainImage, eTransferSrcOptimal,
        sceneColorImage, eTransferDstOptimal,
        region);

Stage 3: Prepare for Sampling
    m_sceneColorImages[index]: eTransferDstOptimal -> eShaderReadOnlyOptimal

Stage 4: Bind in Deferred Lighting
    Descriptor updated to reference scene color image
    Shader samples for emissive background blend
```

---

## Bitmap Target and Cubemap Rendering

**File:** `VulkanRenderingSession.h:129-139`

### BitmapTarget Structure

```cpp
struct BitmapTargetState {
    VkImage targetImage;           // Destination texture
    VkImageView targetView;        // View for specific mip/layer
    uint32_t cubemapFace;          // 0-5 for cubemap, 0 for 2D
    uint32_t mipLevel;             // Target mip level
    vk::Extent2D dimensions;       // Render area size
    vk::Format colorFormat;        // Target color format
    bool hasDepth;                 // Depth attachment presence
};
```

### Cubemap Face Indices

| Index | Face | GL Constant | View Direction |
|-------|------|-------------|----------------|
| 0 | +X | `GL_TEXTURE_CUBE_MAP_POSITIVE_X` | Right |
| 1 | -X | `GL_TEXTURE_CUBE_MAP_NEGATIVE_X` | Left |
| 2 | +Y | `GL_TEXTURE_CUBE_MAP_POSITIVE_Y` | Up |
| 3 | -Y | `GL_TEXTURE_CUBE_MAP_NEGATIVE_Y` | Down |
| 4 | +Z | `GL_TEXTURE_CUBE_MAP_POSITIVE_Z` | Front |
| 5 | -Z | `GL_TEXTURE_CUBE_MAP_NEGATIVE_Z` | Back |

### RTT Rendering Flow

```cpp
// Request RTT target
session.requestBitmapTarget(textureImage, textureView,
    cubemapFace, mipLevel, extent, format, hasDepth);

// Render to texture
session.ensureRendering(cmd);
cmd.bindPipeline(...);
cmd.draw(...);

// Return to swapchain (ends RTT pass automatically)
session.requestSwapchainTarget();

// Transition RTT for sampling
transitionImageLayout(textureImage,
    eColorAttachmentOptimal, eShaderReadOnlyOptimal);
```

---

## Clear Operations

**File:** `VulkanRenderingSession.h:141-160`

### ClearOps Structure

```cpp
struct ClearOps {
    vk::AttachmentLoadOp colorLoadOp;      // eClear or eLoad
    vk::AttachmentLoadOp depthLoadOp;      // eClear or eLoad
    vk::AttachmentLoadOp stencilLoadOp;    // eClear or eLoad
    vk::ClearValue colorClear;             // RGBA clear color
    vk::ClearValue depthStencilClear;      // Depth + stencil values
};
```

### Factory Methods

```cpp
static ClearOps clearAll();              // Clear everything
static ClearOps loadAll();               // Preserve all contents
static ClearOps withDepthStencilClear(); // Clear depth/stencil, load color
```

### One-Shot Clear Behavior

Clears automatically revert to load after first use within a pass:

```cpp
// VulkanRenderingSession.cpp:378, 429, 482, 517
void afterBeginRendering() {
    m_clearOps.colorLoadOp = vk::AttachmentLoadOp::eLoad;
    m_clearOps.depthLoadOp = vk::AttachmentLoadOp::eLoad;
    m_clearOps.stencilLoadOp = vk::AttachmentLoadOp::eLoad;
}
```

This prevents redundant clears across multiple draw calls in the same pass.

---

## Frame Lifecycle

### Complete Frame Flow

```
flip()
|-- wait for GPU (fence from previous frame)
|-- acquire swapchain image (imageAvailableSemaphore)
|-- beginFrame()
|   |-- transition swapchain -> eColorAttachmentOptimal
|   |-- transition depth -> eDepthStencilAttachmentOptimal
|   +-- set target to SwapchainWithDepthTarget
|
|-- [First draw/clear]
|   +-- beginRendering() via ensureRendering()
|
|-- [3D Scene Rendering]
|   |-- geometry draws to swapchain
|   +-- (or to DeferredGBufferTarget if deferred enabled)
|
|-- [Deferred Path - if enabled]
|   |-- beginDeferredPass()
|   |   +-- switch to DeferredGBufferTarget
|   |-- record geometry to G-buffer
|   |-- endDeferredGeometry()
|   |   |-- transition G-buffer -> eShaderReadOnlyOptimal
|   |   +-- switch to SwapchainNoDepthTarget
|   +-- deferredLightingFinish()
|       +-- light volumes to swapchain
|
|-- [HUD Rendering]
|   |-- (already on SwapchainNoDepthTarget)
|   |-- NanoVG text/vector graphics
|   +-- interface elements
|
+-- endFrame()
    |-- end active rendering pass
    |-- transition swapchain -> ePresentSrcKHR
    +-- submit (renderFinishedSemaphore signaled)
```

---

## Code Reference Summary

| Component | File | Lines | Notes |
|-----------|------|-------|-------|
| Render pass state machine | `VulkanRenderingSession.h` | 18-202 | Target types, state tracking |
| Target type enum | `VulkanRenderingSession.h` | - | 11 target definitions |
| Active Pass | `VulkanRenderingSession.h` | 102-129 | RAII pass management |
| Clear operations | `VulkanRenderingSession.h` | 141-160 | Load/store config |
| Dynamic rendering begin | `VulkanRenderingSession.cpp` | 332-518 | Per-target begin logic |
| Layout transitions | `VulkanRenderingSession.cpp` | 520-702 | Barrier functions |
| Dynamic state | `VulkanRenderingSession.cpp` | 706-742 | Viewport/scissor/cull |
| G-buffer definitions | `VulkanRenderTargets.h` | 15-22, 41-48 | Image formats |
| G-buffer ownership | `VulkanRenderTargets.h` | 65-91 | Memory allocation |
| Attachment contract | `VulkanRenderTargetInfo.h` | 11-15 | Format specification |
| Deferred lighting | `VulkanRenderer.cpp` | 588-639 | Light volume rendering |

---

## Design Principles

1. **Type-Driven State:** Target types encode valid attachment configurations, preventing invalid transitions at compile time.

2. **Explicit Synchronization:** All layout transitions use explicit barriers rather than implicit subpass dependencies.

3. **RAII Lifecycle:** `ActivePass` ensures proper pass termination even during exceptions.

4. **Lazy Initialization:** Passes begin on first draw/clear, not on target request.

5. **Layout Tracking:** Per-image layout state prevents redundant transitions.

6. **One-Shot Clears:** Clear operations automatically become loads after first use.
