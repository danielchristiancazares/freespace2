# NVIDIA DLSS (Super Resolution) Integration Plan (Vulkan)

This document describes a concrete, phased implementation plan to integrate NVIDIA DLSS Super Resolution into the FreeSpace Open Vulkan renderer. The plan aligns with the project's type-driven design philosophy and the existing Vulkan renderer architecture.

**Status:** Planning document (not yet implemented)

---

## Table of Contents

1. [Prerequisites and References](#prerequisites-and-references)
2. [Scope and Non-Goals](#scope-and-non-goals)
3. [Terminology and Glossary](#terminology-and-glossary)
4. [Invariants and Constraints](#invariants-and-constraints)
5. [Current Vulkan Frame Structure](#current-vulkan-frame-structure)
6. [Proposed Frame Flow with DLSS](#proposed-frame-flow-with-dlss)
7. [New Modules and Types](#new-modules-and-types)
8. [Resolution Decoupling](#resolution-decoupling)
9. [Motion Vectors](#motion-vectors)
10. [Projection Jitter](#projection-jitter)
11. [Transparency and HUD Policy](#transparency-and-hud-policy)
12. [DLSS Evaluation Pass](#dlss-evaluation-pass)
13. [Post-Processing and AA Interactions](#post-processing-and-aa-interactions)
14. [Texture LOD Bias](#texture-lod-bias)
15. [Build and Runtime Configuration](#build-and-runtime-configuration)
16. [Error Handling and Graceful Degradation](#error-handling-and-graceful-degradation)
17. [Performance Considerations](#performance-considerations)
18. [Testing and Validation](#testing-and-validation)
19. [Phased Implementation Checklist](#phased-implementation-checklist)
20. [Code Areas Expected to Change](#code-areas-expected-to-change)
21. [Open Questions](#open-questions)

---

## Prerequisites and References

### Required Reading

Before implementing this plan, review the following project documentation:

| Document | Key Concepts |
|----------|--------------|
| `docs/DESIGN_PHILOSOPHY.md` | Capability tokens, typestate, state-as-location, boundary-only conditionals |
| `docs/VULKAN_RENDER_PASS_STRUCTURE.md` | Dynamic rendering, `ActivePass` RAII guard, target typestates, G-buffer specification |
| `docs/VULKAN_SYNCHRONIZATION.md` | Synchronization2 barriers, `stageAccessForLayout()`, layout transitions, frames-in-flight model |
| `docs/VULKAN_DESCRIPTOR_SETS.md` | Push descriptors, descriptor set management |
| `docs/VULKAN_TEXTURE_BINDING.md` | Bindless textures, sampler cache, descriptor validity rules |
| `docs/VULKAN_HUD_RENDERING.md` | HUD/UI ordering, clip/scissor invariants, rendering after post-processing |
| `docs/VULKAN_POST_PROCESSING.md` | Post-processing pipeline stages, bloom, tonemapping, FXAA/SMAA |

### External References

| Resource | Purpose |
|----------|---------|
| [NVIDIA NGX SDK Documentation](https://developer.nvidia.com/rtx/ngx) | Official DLSS integration guide |
| [DLSS Programming Guide](https://github.com/NVIDIA/DLSS) | Reference implementation patterns |
| NVIDIA Vulkan Best Practices | Performance recommendations |

### Hardware Requirements

DLSS Super Resolution requires:

- NVIDIA GeForce RTX 20-series or newer GPU (Turing architecture or later)
- Tensor Core hardware for AI upscaling inference
- Vulkan 1.3+ with required extensions (see Section 15)

---

## Scope and Non-Goals

### In-Scope

| Feature | Description |
|---------|-------------|
| DLSS Super Resolution | Temporal upscaler with integrated TAA on supported NVIDIA GPUs |
| Resolution decoupling | Separate render resolution (3D scene) from display resolution (swapchain/UI) |
| Motion vectors | Per-pixel velocity buffer generation for temporal stability |
| Projection jitter | Subpixel jitter sequence for temporal accumulation |
| Quality modes | Quality, Balanced, Performance, Ultra Performance presets |

### Non-Goals (Separate Future Work)

| Feature | Reason for Exclusion |
|---------|---------------------|
| DLSS Frame Generation (DLSS 3 FG) | Different integration surface requiring frame pacing model |
| NVIDIA Reflex | Latency reduction requires separate pipeline integration |
| OpenGL backend support | Vulkan-only implementation initially |
| DLAA-only mode | Can be built on top of SR once SR is functional |
| FSR/XeSS support | Different SDKs; may share resolution decoupling infrastructure |

---

## Terminology and Glossary

### Resolution Terms

| Term | Definition |
|------|------------|
| **DisplayExtent** | Swapchain dimensions (what the OS presents). HUD/UI coordinates operate at this resolution. Corresponds to `VulkanDevice::swapchainExtent()`. |
| **RenderExtent** | Internal 3D rendering dimensions (G-buffer, depth, scene HDR before upscaling). Always <= DisplayExtent. |
| **RenderScale** | Ratio of RenderExtent to DisplayExtent (e.g., 0.5 for 50% resolution). |

### DLSS Terms

| Term | Definition |
|------|------------|
| **NGX** | NVIDIA Graphics Extensions framework that hosts DLSS features. |
| **Feature** | An NGX capability instance (DLSS SR is one feature type). |
| **Feature Key** | Configuration parameters that define when a feature must be recreated. |
| **Quality Mode** | DLSS preset controlling the render scale (Quality ~67%, Balanced ~58%, Performance ~50%, Ultra Performance ~33%). |
| **Motion Vector** | Per-pixel 2D velocity indicating where a pixel moved from the previous frame. |
| **Jitter Offset** | Subpixel camera offset applied per frame for temporal accumulation. |
| **Reset History** | Signal indicating DLSS should discard accumulated temporal data (camera cuts, scene loads). |

### Buffer Terms

| Term | Definition |
|------|------------|
| **ColorInHDR** | Low-resolution HDR scene color at RenderExtent (DLSS input). Maps to `sceneHdrImage()` in current architecture. |
| **ColorOutHDR** | Upscaled HDR scene color at DisplayExtent (DLSS output). New resource. |
| **MotionVectors** | Low-resolution velocity buffer at RenderExtent. New resource. |
| **Depth** | Low-resolution depth buffer at RenderExtent. Maps to `depthImage()`. |

---

## Invariants and Constraints

### Resolution Invariants

**Invariant A (HUD Resolution):**
HUD/UI must always render at DisplayExtent. UI coordinates, scissor rects, and text rendering are never affected by RenderExtent. Per `docs/VULKAN_HUD_RENDERING.md`, HUD rendering occurs after post-processing on `SwapchainNoDepthTarget`.

**Invariant B (Disabled Behavior):**
When DLSS is disabled, `RenderExtent == DisplayExtent`. The frame flow is identical to the current implementation with no observable behavior change.

**Invariant C (Aspect Ratio):**
RenderExtent preserves the aspect ratio of DisplayExtent. No anamorphic scaling.

### Design Constraints (from `docs/DESIGN_PHILOSOPHY.md`)

**Constraint 1 (Boundary-Only Conditionals):**
"Is DLSS available?" is decided once at initialization and when settings change. Draw paths must not contain scattered `if (dlss)` checks. The decision is made at the boundary, and internal code operates on the resulting configuration.

**Constraint 2 (Capability Tokens):**
DLSS evaluate must only be callable with an explicit token proving "resources exist and are ready." Functions requiring DLSS accept a capability type, not a nullable pointer. This pattern is already established with `RecordingFrame`, `RenderCtx`, `DeferredGeometryCtx`, and `DeferredLightingCtx` in `VulkanPhaseContexts.h`.

**Constraint 3 (State as Location):**
DLSS enabled/disabled state is modeled as a variant type owned by the renderer, not scattered boolean flags. An object's presence in a container or its type determines its state.

**Constraint 4 (No Inhabitant Branching):**
Internal logic must not branch on "does DLSS exist?" via null checks or optional inspection. The type system enforces validity.

---

## Current Vulkan Frame Structure

The Vulkan renderer architecture is documented in detail in `docs/VULKAN_RENDER_PASS_STRUCTURE.md` and `docs/VULKAN_ARCHITECTURE_OVERVIEW.md`. The following summarizes the integration points relevant to DLSS.

### Architecture Components

| Component | Source File | Description | Relevance to DLSS |
|-----------|-------------|-------------|-------------------|
| `VulkanRenderingSession` | `VulkanRenderingSession.h` | Owns target selection, layout transitions, `ActivePass` RAII guard | DLSS operates between render passes |
| `VulkanRenderTargets` | `VulkanRenderTargets.h` | Manages G-buffer (5 attachments), depth, scene HDR, bloom, post-process targets | Must support dual-extent allocation |
| `ActivePass` | `VulkanRenderingSession.h:107` | RAII guard for dynamic rendering begin/end | DLSS evaluates outside active passes |
| `RecordingFrame` | `VulkanFrameFlow.h:18` | Capability token for in-flight frame recording | DLSS evaluation needs frame context |
| Synchronization2 | `VulkanRenderingSession.cpp` | `vkCmdPipelineBarrier2` with `stageAccessForLayout()` | DLSS inputs/outputs require barriers |

### Current G-Buffer Layout

From `VulkanRenderTargets.h` and `model.frag`:

| Index | Output Name | Format | Content |
|-------|-------------|--------|---------|
| 0 | `outColor` | `R16G16B16A16Sfloat` | Diffuse albedo + roughness |
| 1 | `outNormal` | `R16G16B16A16Sfloat` | World normal + metalness |
| 2 | `outPosition` | `R16G16B16A16Sfloat` | World position + AO |
| 3 | `outSpecular` | `R16G16B16A16Sfloat` | Specular color + fresnel |
| 4 | `outEmissive` | `R16G16B16A16Sfloat` | Self-illumination |

The G-buffer count is defined as `kGBufferCount = 5` in `VulkanRenderTargets.h:21`.

### Current Frame Flow

From `VulkanRenderer.cpp` and `VulkanRenderingSession.cpp`:

```
1. beginFrame()
   |-- acquire swapchain image (VulkanDevice::acquireNextImage)
   |-- VulkanRenderingSession::beginFrame()
   |   |-- transitionSwapchainToAttachment()
   |   +-- transitionDepthToAttachment()

2. Deferred G-Buffer Pass (at DisplayExtent currently)
   |-- beginDeferredPass() selects DeferredGBufferTarget
   |-- ensureRendering() -> beginGBufferRenderingInternal()
   |-- record opaque geometry (model.frag writes 5 G-buffer outputs)
   +-- endActivePass() on target change

3. Deferred Lighting (at DisplayExtent currently)
   |-- endDeferredGeometry() -> transitionGBufferToShaderRead()
   |-- requestSwapchainNoDepthTarget() or requestSceneHdrNoDepthTarget()
   |-- recordDeferredLighting() with light volumes
   +-- deferredLightingFinish()

4. Post-Processing (at DisplayExtent currently)
   |-- beginSceneTexture() / endSceneTexture()
   |-- recordBloomBrightPass() -> generateBloomMipmaps() -> recordBloomBlurPass()
   |-- recordBloomCompositePass() (additive blend into scene HDR)
   |-- recordTonemappingToSwapchain() -> postLdr target
   +-- FXAA (recordFxaaPrepass/recordFxaaPass) or SMAA (3-pass)

5. HUD/UI
   |-- requestSwapchainNoDepthTarget()
   |-- NanoVG / interface draws (per VULKAN_HUD_RENDERING.md)
   +-- endActivePass()

6. endFrame()
   |-- VulkanRenderingSession::endFrame()
   |   +-- transitionSwapchainToPresent()
   +-- submitRecordedFrame() with timeline semaphore signal
```

### DLSS Integration Point

DLSS evaluation must run **after** the scene is fully rendered at RenderExtent (ColorInHDR complete) and **before** post-processing that assumes DisplayExtent input. This is between steps 3 and 4 above, specifically after deferred lighting completes but before bloom.

---

## Proposed Frame Flow with DLSS

### Frame Flow Diagram

```
+------------------+     +------------------+     +------------------+
|   RenderExtent   |     |  DLSS Evaluate   |     |  DisplayExtent   |
|   (Low-Res)      | --> |  (Upscale Pass)  | --> |  (Full-Res)      |
+------------------+     +------------------+     +------------------+
        |                        |                        |
        v                        v                        v
+----------------+       +----------------+       +----------------+
| G-Buffer       |       | ColorInHDR     |       | ColorOutHDR    |
| Depth          |  -->  | Depth          |  -->  | (DLSS Output)  |
| MotionVectors  |       | MotionVectors  |       +----------------+
+----------------+       | Jitter         |               |
                         | ResetHistory   |               v
                         +----------------+       +----------------+
                                                  | Post-Process   |
                                                  | Bloom          |
                                                  | Tonemap        |
                                                  +----------------+
                                                          |
                                                          v
                                                  +----------------+
                                                  | HUD/UI         |
                                                  | (DisplayExtent)|
                                                  +----------------+
                                                          |
                                                          v
                                                  +----------------+
                                                  | Present        |
                                                  +----------------+
```

### Detailed Frame Flow (DLSS Enabled)

```
1. beginFrame()
   |-- acquire swapchain image
   |-- transition depth -> depthAttachmentLayout() (RenderExtent)
   +-- transition MotionVectors -> eColorAttachmentOptimal (RenderExtent)

2. Deferred G-Buffer Pass (at RenderExtent)
   |-- beginDeferredPass() on sized DeferredGBufferTarget
   |-- record opaque geometry with motion vector output (location 5)
   +-- endActivePass()

3. Deferred Lighting (at RenderExtent)
   |-- transitionGBufferToShaderRead()
   |-- requestSceneHdrTarget() at RenderExtent (ColorInHDR)
   |-- record light volumes / fullscreen lighting
   +-- deferredLightingFinish()

4. Transparency (at RenderExtent)
   |-- render scene-affecting transparencies into ColorInHDR
   +-- (particles, effects that should be temporally stable)

5. DLSS Evaluation Pass (NEW)
   |-- suspendRendering() if active
   |-- Barrier: ColorInHDR -> eShaderReadOnlyOptimal
   |-- Barrier: Depth -> depthReadLayout()
   |-- Barrier: MotionVectors -> eShaderReadOnlyOptimal
   |-- Barrier: ColorOutHDR -> SDK-required layout (likely eGeneral)
   |-- evaluate DLSS (RenderExtent -> DisplayExtent)
   +-- Barrier: ColorOutHDR -> eShaderReadOnlyOptimal

6. Post-Processing (at DisplayExtent)
   |-- recordBloomBrightPass() on ColorOutHDR
   |-- bloom mip generation and blur
   |-- recordBloomCompositePass() into ColorOutHDR
   |-- recordTonemappingToSwapchain() -> postLdr
   +-- (FXAA/SMAA disabled when DLSS active)

7. HUD/UI (at DisplayExtent)
   |-- requestSwapchainNoDepthTarget()
   |-- NanoVG / interface draws
   +-- endActivePass()

8. endFrame()
   |-- transitionSwapchainToPresent()
   +-- submitRecordedFrame()
```

### Frame Flow (DLSS Disabled)

When DLSS is disabled, the flow is identical to current behavior:

- `RenderExtent == DisplayExtent`
- `ColorOutHDR` either unused or aliases `ColorInHDR`
- No motion vectors generated (shader variant without MV output)
- No jitter applied to projection
- Post-processing pipeline unchanged

---

## New Modules and Types

### Module Structure

Create a Vulkan-only DLSS module isolated from core rendering:

```
code/graphics/vulkan/dlss/
    DlssManager.h        // Public interface
    DlssManager.cpp      // NGX SDK integration
    DlssTypes.h          // Type definitions
    DlssJitter.h         // Jitter sequence generator
    DlssJitter.cpp
```

### DlssManager Interface

The `DlssManager` encapsulates all NGX/DLSS SDK calls and exposes an engine-friendly interface:

```cpp
// DlssManager.h

#include <variant>
#include <vulkan/vulkan.hpp>
#include "globalincs/pstypes.h"

namespace graphics::vulkan::dlss {

// Typestate: DLSS is either unavailable or ready
struct DlssUnavailable {
    SCP_string reason;  // Human-readable explanation, logged once
};

struct DlssReady {
    // Owns NGX context + feature handle
    // Private members - only accessible via DlssManager
};

using DlssState = std::variant<DlssUnavailable, DlssReady>;

// Quality mode selection
enum class DlssQualityMode : uint8_t {
    Quality,           // ~67% render scale
    Balanced,          // ~58% render scale
    Performance,       // ~50% render scale
    UltraPerformance   // ~33% render scale
};

// Feature recreation key - feature must be recreated when any field changes
struct DlssFeatureKey {
    vk::Extent2D renderExtent;
    vk::Extent2D displayExtent;
    DlssQualityMode mode;
    bool hdr;  // HDR pipeline configuration

    bool operator==(const DlssFeatureKey&) const = default;
};

class DlssManager {
public:
    // Initialize NGX context (called once at renderer startup)
    // Returns DlssUnavailable with reason if initialization fails
    static DlssState initialize(vk::Instance instance,
                                vk::PhysicalDevice physicalDevice,
                                vk::Device device);

    // Query optimal render extent for a quality mode
    static vk::Extent2D optimalRenderExtent(vk::Extent2D displayExtent,
                                            DlssQualityMode mode);

    // Create or recreate feature for given key
    // Called when DlssFeatureKey changes
    void ensureFeature(const DlssFeatureKey& key);

    // Evaluate DLSS - requires capability token
    void evaluate(const DlssEvaluateToken& token,
                  vk::CommandBuffer cmd,
                  const DlssEvaluateParams& params);

    // Shutdown and release NGX resources
    void shutdown();

private:
    // NGX context and feature handles (opaque)
};

} // namespace graphics::vulkan::dlss
```

### Capability Token for Evaluation

The evaluation capability token follows the pattern established in `VulkanPhaseContexts.h`:

```cpp
// DlssTypes.h

namespace graphics::vulkan::dlss {

// Token proving DLSS evaluation is valid
// Only constructible by VulkanRenderer at the evaluation boundary
class DlssEvaluateToken {
    friend class VulkanRenderer;  // Only renderer can construct

    DlssEvaluateToken() = default;

public:
    DlssEvaluateToken(const DlssEvaluateToken&) = delete;
    DlssEvaluateToken& operator=(const DlssEvaluateToken&) = delete;
    DlssEvaluateToken(DlssEvaluateToken&&) = default;
    DlssEvaluateToken& operator=(DlssEvaluateToken&&) = default;
};

// Parameters for DLSS evaluation
struct DlssEvaluateParams {
    // Input images (RenderExtent)
    vk::Image colorIn;           // HDR scene color (sceneHdrImage at RenderExtent)
    vk::ImageView colorInView;
    vk::Image depth;             // Depth buffer
    vk::ImageView depthView;
    vk::Image motionVectors;     // Velocity buffer
    vk::ImageView motionVectorsView;

    // Output image (DisplayExtent)
    vk::Image colorOut;          // Upscaled HDR output
    vk::ImageView colorOutView;

    // Per-frame parameters
    vec2d jitterOffset;          // Jitter in pixels (from JitterSequence)
    bool resetHistory;           // Discard temporal accumulation
    float sharpness;             // Optional sharpening (0.0 - 1.0)

    // Extents
    vk::Extent2D renderExtent;
    vk::Extent2D displayExtent;
};

} // namespace graphics::vulkan::dlss
```

### Resolution State

Centralized resolution configuration in `VulkanRenderer`:

```cpp
// VulkanRenderer.h (addition)

struct ResolutionState {
    vk::Extent2D displayExtent;  // Swapchain size (VulkanDevice::swapchainExtent())
    vk::Extent2D renderExtent;   // Internal 3D size
    float renderScale;           // renderExtent / displayExtent

    // Computed on construction
    bool isDlssActive() const { return renderExtent != displayExtent; }
};
```

---

## Resolution Decoupling

Resolution decoupling is required before DLSS SDK integration provides value. This phase proves the dual-extent infrastructure without NGX complexity.

### Resolution Policy

The resolution policy computes RenderExtent from DisplayExtent:

```cpp
// Resolution policy rules
vk::Extent2D computeRenderExtent(vk::Extent2D displayExtent,
                                 DlssQualityMode mode) {
    // 1. Get scale factor from quality mode
    float scale = qualityModeScale(mode);  // 0.33 - 0.67

    // 2. Compute raw render extent
    uint32_t width = static_cast<uint32_t>(displayExtent.width * scale);
    uint32_t height = static_cast<uint32_t>(displayExtent.height * scale);

    // 3. Clamp to minimum (at least 1x1)
    width = std::max(width, 1u);
    height = std::max(height, 1u);

    // 4. Apply SDK alignment requirements (query from SDK at init)
    // DLSS may require specific alignment (e.g., multiple of 8)
    width = alignUp(width, sdkAlignmentRequirement);
    height = alignUp(height, sdkAlignmentRequirement);

    // 5. Preserve aspect ratio (recompute height from aligned width)
    float aspect = static_cast<float>(displayExtent.width) /
                   static_cast<float>(displayExtent.height);
    height = static_cast<uint32_t>(width / aspect);
    height = alignUp(height, sdkAlignmentRequirement);

    return {width, height};
}

constexpr float qualityModeScale(DlssQualityMode mode) {
    switch (mode) {
    case DlssQualityMode::Quality:          return 0.67f;
    case DlssQualityMode::Balanced:         return 0.58f;
    case DlssQualityMode::Performance:      return 0.50f;
    case DlssQualityMode::UltraPerformance: return 0.33f;
    }
    return 1.0f;
}
```

### Render Target Ownership Changes

`VulkanRenderTargets` must support dual-extent resource allocation. Current resources from `VulkanRenderTargets.h`:

**RenderExtent Targets (allocated at internal resolution):**

| Target | Format | Purpose | Current Member |
|--------|--------|---------|----------------|
| G-Buffer[0-4] | `R16G16B16A16Sfloat` | Deferred material properties | `m_gbufferImages` |
| Depth | `D32SfloatS8Uint` or fallback | Scene depth | `m_depthImage` |
| MotionVectors | `R16G16_Sfloat` | Per-pixel velocity (NEW) | N/A |
| ColorInHDR | `R16G16B16A16Sfloat` | Scene HDR before upscaling | `m_sceneHdrImage` |

**DisplayExtent Targets (allocated at display resolution):**

| Target | Format | Purpose | Current Member |
|--------|--------|---------|----------------|
| ColorOutHDR | `R16G16B16A16Sfloat` | DLSS output (NEW) | N/A |
| Bloom[0-1] | `R16G16B16A16Sfloat` | Bloom ping-pong (4 mip levels) | `m_bloomImages` |
| PostLdr | `B8G8R8A8Unorm` | Tonemapped LDR | `m_postLdrImage` |
| SMAA targets | `B8G8R8A8Unorm` | AA intermediates | `m_smaa*` |
| Swapchain | Swapchain format | Final output | Via VulkanDevice |

### Target Reallocation Triggers

Render targets must be reallocated when:

1. DisplayExtent changes (swapchain resize/recreate via `VulkanRenderTargets::resize()`)
2. RenderExtent changes (DLSS mode change)
3. DLSS enabled/disabled toggle

The `DlssFeatureKey` captures all recreation triggers; when the key changes, both NGX feature and render targets are recreated.

---

## Motion Vectors

Motion vectors encode per-pixel velocity for temporal stability.

### Buffer Specification

| Property | Value | Notes |
|----------|-------|-------|
| Format | `VK_FORMAT_R16G16_SFLOAT` | 16-bit float per component |
| Size | RenderExtent | Matches scene rendering resolution |
| Usage | `eColorAttachment | eSampled` | Written by G-buffer pass, read by DLSS |
| Layout tracking | Via `VulkanRenderTargets` | Following existing pattern |

**Upgrade Path:** If SDK requires higher precision, use `VK_FORMAT_R32G32_SFLOAT`. Query SDK requirements at initialization and set format accordingly.

### Motion Vector Convention

**Internal Representation:**
- NDC delta (current frame position - previous frame position)
- Uses **unjittered** projections for stable math
- Range: typically small values near zero for static objects

**Conversion to SDK Format:**
- Convert to DLSS-required units only at the DLSS boundary
- SDK may require pixel-space or different NDC conventions
- Encapsulate conversion in `DlssManager::evaluate()`

### Motion Vector Generation

**Primary Producer:** Deferred G-buffer pass (opaque geometry via `model.frag`)

This approach covers most pixels from opaque geometry without requiring motion vectors everywhere. Transparent objects that need temporal stability can write motion vectors in future iterations.

**Shader Output:**

Add to `model.frag` (currently has outputs at locations 0-4):

```glsl
// Fragment shader addition for G-buffer pass
// Add after existing outputs (outColor, outNormal, outPosition, outSpecular, outEmissive)
layout(location = 5) out vec2 outMotionVector;

// Uniform: previous frame MVP (binding TBD, likely in existing matrixData or new UBO)
uniform mat4 prevMvpMatrix;

void main() {
    // ... existing G-buffer outputs ...

    // Motion vector: current screen position - previous screen position
    // fragPosition is the world-space position, already computed for outPosition

    // Current NDC (from gl_FragCoord, unjittered projection)
    vec2 currentNdc = gl_FragCoord.xy / vec2(renderExtent);

    // Previous NDC
    vec4 prevClip = prevMvpMatrix * vec4(fragPosition, 1.0);
    vec2 prevNdc = (prevClip.xy / prevClip.w) * 0.5 + 0.5;  // [-1,1] -> [0,1]

    outMotionVector = currentNdc - prevNdc;
}
```

**Note:** Shader variant selection (with/without motion vectors) should use existing `SDR_FLAG_*` mechanism to avoid runtime conditionals.

### Previous-Transform Cache

Per-object previous-frame transforms are required for correct motion (ships, weapons, debris, rotating submodels).

**Design (State as Location + Boundary Ownership):**

The renderer owns a cache mapping stable render keys to previous model matrices:

```cpp
// RenderKey uniquely identifies a transform history entry
struct RenderKey {
    uint32_t objectInstanceId;   // Object instance identifier
    uint32_t submodelId;         // Submodel within object (0 for main)
    uint32_t drawIndex;          // Disambiguator for multiple draws per object

    auto operator<=>(const RenderKey&) const = default;
};

// Hash specialization for unordered_map
struct RenderKeyHash {
    size_t operator()(const RenderKey& key) const {
        // Combine hashes
        size_t h = std::hash<uint32_t>{}(key.objectInstanceId);
        h ^= std::hash<uint32_t>{}(key.submodelId) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(key.drawIndex) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

// Cache structure
class PreviousTransformCache {
public:
    // Look up previous transform; returns current if not found (first frame)
    matrix4 getPrevious(const RenderKey& key, const matrix4& current);

    // Update cache after draw submission
    void update(const RenderKey& key, const matrix4& current);

    // Clear all entries (scene load, camera cut)
    void clear();

    // Remove stale entries (object destruction) - call at frame end
    void compact();

private:
    std::unordered_map<RenderKey, matrix4, RenderKeyHash> m_cache;
};
```

**Usage Pattern:**

```cpp
// At draw submission (in model rendering path)
matrix4 currentModel = computeModelMatrix(object);
matrix4 prevModel = m_prevTransformCache.getPrevious(renderKey, currentModel);

// Send both to shader via push constants or UBO
pushConstants.currentMvp = projection * view * currentModel;
pushConstants.previousMvp = prevProjection * prevView * prevModel;

// After draw
m_prevTransformCache.update(renderKey, currentModel);
```

### Camera Cuts and History Reset

DLSS must be told when temporal history is invalid:

| Trigger | Action |
|---------|--------|
| Mission start/load | Set `resetHistory = true`, clear transform cache |
| Cutscene transition | Set `resetHistory = true` |
| Viewpoint mode change | Set `resetHistory = true` |
| RenderExtent change | Set `resetHistory = true` (feature recreated) |
| DisplayExtent change | Set `resetHistory = true` (feature recreated) |

**Implementation:**

```cpp
// Per-frame reset detection
bool detectHistoryReset() {
    if (m_sceneJustLoaded) return true;
    if (m_viewpointChanged) return true;
    if (m_featureKeyChanged) return true;
    return false;
}
```

---

## Projection Jitter

Temporal upscaling requires subpixel jitter for sample accumulation.

### Jitter Sequence

Use a deterministic low-discrepancy sequence:

```cpp
// Halton sequence generator (base 2/3)
class JitterSequence {
public:
    static constexpr uint32_t kSequenceLength = 32;  // Or SDK recommendation

    // Get jitter for frame index (in pixel units at RenderExtent)
    vec2d getJitter(uint32_t frameIndex, vk::Extent2D renderExtent) const {
        uint32_t idx = frameIndex % kSequenceLength;

        // Halton base 2 and 3
        float x = halton(idx + 1, 2);  // +1 to avoid (0,0)
        float y = halton(idx + 1, 3);

        // Center around 0: [0,1] -> [-0.5, +0.5] pixels
        x -= 0.5f;
        y -= 0.5f;

        return {x, y};  // Pixel-space jitter
    }

private:
    static float halton(uint32_t index, uint32_t base) {
        float result = 0.0f;
        float f = 1.0f / base;
        uint32_t i = index;
        while (i > 0) {
            result += f * (i % base);
            i /= base;
            f /= base;
        }
        return result;
    }
};
```

### Jittered vs Unjittered Matrices

**Critical:** Do not replace the global projection with the jittered projection without preserving the unjittered version.

| Matrix | Purpose |
|--------|---------|
| Unjittered projection | Frustum culling, motion vector math, LOD selection |
| Jittered projection | Rasterization (what actually hits the G-buffer) |

**Implementation:**

```cpp
// VulkanRenderer frame setup
void setupFrameMatrices(const Camera& camera, uint32_t frameIndex) {
    // Unjittered (stable)
    m_viewMatrix = camera.viewMatrix();
    m_projectionMatrix = camera.projectionMatrix();

    // Previous frame matrices (stored before update)
    m_prevViewMatrix = m_viewMatrixPrev;
    m_prevProjectionMatrix = m_projectionMatrixPrev;

    // Jittered (for rasterization only when DLSS active)
    if (m_resolutionState.isDlssActive()) {
        vec2d jitter = m_jitterSequence.getJitter(frameIndex, m_resolutionState.renderExtent);
        m_jitteredProjection = applyJitter(m_projectionMatrix, jitter, m_resolutionState.renderExtent);
        m_currentJitter = jitter;
    } else {
        m_jitteredProjection = m_projectionMatrix;
        m_currentJitter = {0.0, 0.0};
    }

    // Store for next frame
    m_viewMatrixPrev = m_viewMatrix;
    m_projectionMatrixPrev = m_projectionMatrix;
}

matrix4 applyJitter(const matrix4& proj, vec2d jitterPx, vk::Extent2D extent) {
    // Convert pixel jitter to NDC offset
    float jitterNdcX = (2.0f * jitterPx.x) / extent.width;
    float jitterNdcY = (2.0f * jitterPx.y) / extent.height;

    // Apply to projection matrix (modify [2][0] and [2][1] for perspective)
    matrix4 jittered = proj;
    jittered[2][0] += jitterNdcX;
    jittered[2][1] += jitterNdcY;
    return jittered;
}
```

**Usage Rules:**
- Culling uses `m_projectionMatrix` (unjittered)
- Motion vector computation uses `m_projectionMatrix` and `m_prevProjectionMatrix` (both unjittered)
- Rasterization uses `m_jitteredProjection`

---

## Transparency and HUD Policy

### HUD/UI Policy

**Rule:** HUD/UI must not feed into DLSS input.

- HUD renders at DisplayExtent (after DLSS upscaling and post-processing)
- HUD elements are not temporally accumulated
- Consistent with `docs/VULKAN_HUD_RENDERING.md`: HUD renders on `SwapchainNoDepthTarget` after post-processing

### Transparency Policy

**Phase 1 (Initial Implementation):**

Render most scene-affecting transparencies into low-res `ColorInHDR` before upscaling:

| Element | Render Location | Rationale |
|---------|-----------------|-----------|
| Ship shields | RenderExtent (ColorInHDR) | Temporally stable with scene |
| Weapon effects | RenderExtent (ColorInHDR) | Part of 3D scene |
| Engine glows | RenderExtent (ColorInHDR) | Part of 3D scene |
| Explosions | RenderExtent (ColorInHDR) | Part of 3D scene |
| Particles | RenderExtent (ColorInHDR) | May show ghosting |

**Phase 2 (Quality Improvement):**

If Phase 1 produces visible ghosting around particles/transparencies:

- Add reactive/transparency mask support (SDK feature)
- Mask indicates pixels with high transparency or motion
- DLSS reduces temporal accumulation in masked regions

This is deferred until artifacts are observed and quantified.

---

## DLSS Evaluation Pass

### Placement in Frame

DLSS evaluation occurs:

- **After:** Low-res `ColorInHDR` is complete (all scene rendering including transparencies finished)
- **Before:** Post-processing that assumes display-res input (bloom, tonemapping)

### Dynamic Rendering Boundaries

DLSS evaluate must run **outside** an active dynamic rendering scope. Use the existing `suspendRendering()` pattern:

```cpp
void VulkanRenderer::evaluateDlss(vk::CommandBuffer cmd) {
    // Ensure no active rendering pass
    m_renderingSession->suspendRendering();  // Calls endActivePass() if needed

    // Transitions and DLSS evaluate
    performDlssEvaluation(cmd);
}
```

### Layout Transitions

All transitions follow the synchronization2 discipline from `docs/VULKAN_SYNCHRONIZATION.md`, using `stageAccessForLayout()` from `VulkanRenderingSession.cpp`:

**Before DLSS Evaluate:**

| Resource | Old Layout | New Layout | Src Stage | Dst Stage |
|----------|------------|------------|-----------|-----------|
| ColorInHDR | eColorAttachmentOptimal | eShaderReadOnlyOptimal | eColorAttachmentOutput | eComputeShader* |
| Depth | depthAttachmentLayout() | depthReadLayout() | eLateFragmentTests | eComputeShader* |
| MotionVectors | eColorAttachmentOptimal | eShaderReadOnlyOptimal | eColorAttachmentOutput | eComputeShader* |
| ColorOutHDR | eUndefined | eGeneral | eTopOfPipe | eComputeShader* |

*Note: Actual destination stage depends on NGX SDK requirements; may be vendor-specific or require `eAllCommands`.

**After DLSS Evaluate:**

| Resource | Old Layout | New Layout | Src Stage | Dst Stage |
|----------|------------|------------|-----------|-----------|
| ColorOutHDR | eGeneral | eShaderReadOnlyOptimal | eComputeShader* | eFragmentShader |

### Transition Implementation

Following the pattern in `VulkanRenderingSession.cpp`:

```cpp
void VulkanRenderingSession::transitionForDlssEvaluate(vk::CommandBuffer cmd) {
    std::array<vk::ImageMemoryBarrier2, 4> barriers{};
    uint32_t barrierCount = 0;

    // ColorInHDR: attachment -> shader read
    const auto sceneHdrOld = m_targets.sceneHdrLayout();
    const auto sceneHdrNew = vk::ImageLayout::eShaderReadOnlyOptimal;
    const auto srcSceneHdr = stageAccessForLayout(sceneHdrOld);
    const auto dstSceneHdr = stageAccessForLayout(sceneHdrNew);

    barriers[barrierCount] = vk::ImageMemoryBarrier2{};
    barriers[barrierCount].srcStageMask = srcSceneHdr.stageMask;
    barriers[barrierCount].srcAccessMask = srcSceneHdr.accessMask;
    barriers[barrierCount].dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barriers[barrierCount].dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    barriers[barrierCount].oldLayout = sceneHdrOld;
    barriers[barrierCount].newLayout = sceneHdrNew;
    barriers[barrierCount].image = m_targets.sceneHdrImage();
    barriers[barrierCount].subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    ++barrierCount;

    // Depth: attachment -> shader read
    const auto depthOld = m_targets.depthLayout();
    const auto depthNew = m_targets.depthReadLayout();
    const auto srcDepth = stageAccessForLayout(depthOld);

    barriers[barrierCount] = vk::ImageMemoryBarrier2{};
    barriers[barrierCount].srcStageMask = srcDepth.stageMask;
    barriers[barrierCount].srcAccessMask = srcDepth.accessMask;
    barriers[barrierCount].dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
    barriers[barrierCount].dstAccessMask = vk::AccessFlagBits2::eShaderSampledRead;
    barriers[barrierCount].oldLayout = depthOld;
    barriers[barrierCount].newLayout = depthNew;
    barriers[barrierCount].image = m_targets.depthImage();
    barriers[barrierCount].subresourceRange = {m_targets.depthAttachmentAspectMask(), 0, 1, 0, 1};
    ++barrierCount;

    // MotionVectors: attachment -> shader read
    // (Similar pattern for new motion vector image)

    // ColorOutHDR: undefined -> general
    // (Similar pattern for new DLSS output image)

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = barrierCount;
    dep.pImageMemoryBarriers = barriers.data();
    cmd.pipelineBarrier2(dep);

    // Update tracked layouts
    m_targets.setSceneHdrLayout(sceneHdrNew);
    m_targets.setDepthLayout(depthNew);
    // ... update other layouts
}
```

### Correctness Rule

The DLSS module must not "guess" current image layouts. The renderer boundary transitions resources and then hands them to DLSS evaluate with the capability token. Layout state is owned by `VulkanRenderTargets` and tracked explicitly (following the existing pattern for all image resources).

---

## Post-Processing and AA Interactions

### Anti-Aliasing Mode Interactions

DLSS Super Resolution includes temporal anti-aliasing. Running FXAA/SMAA on top is:

- Redundant (DLSS already handles edges)
- Potentially harmful (may blur DLSS output)

**Policy:**

| DLSS State | AA Mode |
|------------|---------|
| Enabled | Force AA to "None" (skip FXAA/SMAA passes in `endSceneTexture()`) |
| Disabled | User's selected AA mode (FXAA/SMAA/None) |

**Implementation:**

```cpp
// In VulkanRenderer::endSceneTexture()
AntiAliasMode effectiveAaMode() const {
    if (m_resolutionState.isDlssActive()) {
        return AntiAliasMode::None;  // DLSS provides AA
    }
    return m_userSelectedAaMode;  // From gr_is_fxaa_mode() / gr_is_smaa_mode()
}
```

### Post-Processing Resolution

When DLSS is enabled, post-processing operates on `ColorOutHDR` at DisplayExtent:

| Pass | Input | Resolution |
|------|-------|------------|
| Bloom downsample | ColorOutHDR | DisplayExtent -> mips (half-res base) |
| Bloom upsample | Bloom mips | DisplayExtent |
| Bloom composite | Bloom[0] + ColorOutHDR | DisplayExtent (additive) |
| Tonemapping | ColorOutHDR | DisplayExtent -> PostLdr |
| (FXAA/SMAA) | (Disabled) | - |
| Final resolve | PostLdr | -> Swapchain |

### Optional Sharpening

DLSS includes optional output sharpening. If users desire additional sharpening beyond DLSS:

- Expose a sharpening slider (0.0 - 1.0) in graphics options
- Pass to DLSS evaluation parameters (`DlssEvaluateParams::sharpness`)
- Consider CAS (Contrast Adaptive Sharpening) as post-DLSS option for non-DLSS quality modes

---

## Texture LOD Bias

Rendering at lower resolution changes mip selection; without LOD bias adjustment, textures appear overly blurry.

### LOD Bias Calculation

```cpp
float computeLodBias(float renderScale) {
    // log2(renderScale) gives the mip level offset
    // Negative bias sharpens (selects higher-res mip)
    return std::log2(renderScale);  // e.g., 0.5 -> -1.0
}
```

### Sampler Cache Integration

The current `VulkanTextureManager::SamplerKey` in `VulkanTextureManager.h:123` is:

```cpp
struct SamplerKey {
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

    bool operator==(const SamplerKey& other) const {
        return filter == other.filter && address == other.address;
    }
};
```

Extend to include LOD bias:

```cpp
// VulkanTextureManager sampler key extension
struct SamplerKey {
    // Existing fields
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

    // NEW: LOD bias (quantized to avoid float key issues)
    int8_t lodBiasQuantized = 0;  // lodBias * 8, clamped to [-16, 0]

    bool operator==(const SamplerKey& other) const {
        return filter == other.filter &&
               address == other.address &&
               lodBiasQuantized == other.lodBiasQuantized;
    }
};

// Hash must also be updated for the new field
```

Helper functions:

```cpp
float quantizedToLodBias(int8_t quantized) {
    return static_cast<float>(quantized) / 8.0f;
}

int8_t lodBiasToQuantized(float lodBias) {
    return static_cast<int8_t>(std::clamp(lodBias * 8.0f, -128.0f, 0.0f));
}
```

### Descriptor Validity

Per `docs/VULKAN_TEXTURE_BINDING.md`, all descriptor slots must remain valid. LOD bias changes require:

1. New sampler creation with updated LOD bias (via existing sampler cache)
2. Descriptor set update to reference new sampler
3. Previous sampler remains valid until frame completion (handled by existing `DeferredReleaseQueue`)

---

## Build and Runtime Configuration

### Build-Time Configuration

**CMake Option:**

```cmake
option(FSO_ENABLE_DLSS "Enable NVIDIA DLSS support (requires NGX SDK)" OFF)

if(FSO_ENABLE_DLSS)
    # Require developer-supplied SDK path
    if(NOT DEFINED NGX_SDK_PATH)
        message(FATAL_ERROR "FSO_ENABLE_DLSS requires NGX_SDK_PATH to be set")
    endif()

    # Verify SDK exists
    if(NOT EXISTS "${NGX_SDK_PATH}/include/nvsdk_ngx.h")
        message(FATAL_ERROR "NGX SDK not found at ${NGX_SDK_PATH}")
    endif()

    # Include SDK headers
    target_include_directories(code PRIVATE "${NGX_SDK_PATH}/include")

    # Link SDK library (platform-specific)
    if(WIN32)
        target_link_libraries(code PRIVATE "${NGX_SDK_PATH}/lib/x64/nvsdk_ngx.lib")
    endif()

    # Define preprocessor macro
    target_compile_definitions(code PRIVATE FSO_DLSS_ENABLED=1)
endif()
```

**Important:** Do not commit proprietary SDK binaries/headers to the repository. Add `NGX_SDK_PATH` to `.gitignore` documentation.

### Runtime Availability

DLSS transitions to `DlssReady` only if all conditions are met:

| Condition | Check Method |
|-----------|--------------|
| Build with `FSO_DLSS_ENABLED` | Compile-time (module exists) |
| NVIDIA GPU | `VulkanDevice::properties().vendorID == 0x10DE` |
| DLSS-capable hardware | NGX capability query |
| Runtime components present | NGX initialization success |
| Feature creation succeeds | NGX feature creation for current key |

**Availability Check Flow:**

```cpp
DlssState DlssManager::initialize(vk::Instance instance,
                                  vk::PhysicalDevice physicalDevice,
                                  vk::Device device) {
    // 1. Check GPU vendor
    vk::PhysicalDeviceProperties props;
    physicalDevice.getProperties(&props);
    if (props.vendorID != 0x10DE) {  // NVIDIA vendor ID
        return DlssUnavailable{"Non-NVIDIA GPU; DLSS requires NVIDIA RTX hardware"};
    }

    // 2. Initialize NGX
    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init(/* ... */);
    if (NVSDK_NGX_FAILED(result)) {
        return DlssUnavailable{formatNgxError("NGX initialization failed", result)};
    }

    // 3. Query DLSS capability
    int dlssAvailable = 0;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params);
    params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
    if (!dlssAvailable) {
        return DlssUnavailable{"DLSS not supported on this GPU"};
    }

    // 4. Success
    return DlssReady{/* ... */};
}
```

---

## Error Handling and Graceful Degradation

### Initialization Failures

| Failure Mode | Response |
|--------------|----------|
| Non-NVIDIA GPU | Log once via `mprintf()`, continue without DLSS |
| Unsupported GPU (pre-RTX) | Log once, continue without DLSS |
| NGX runtime missing | Log once, continue without DLSS |
| Feature creation fails | Log once, continue without DLSS |

**Logging Policy:**
- Log reason exactly once at startup or settings change
- Do not spam logs on every frame
- Include actionable information (e.g., "DLSS requires NVIDIA RTX GPU")
- Use existing logging infrastructure (`mprintf()`, `nprintf()`)

### Runtime Failures

| Failure Mode | Response |
|--------------|----------|
| Evaluate returns error | Log, fall back to bilinear upscale for this frame |
| Feature becomes invalid | Attempt recreation; if fails, disable DLSS |
| Resource allocation fails | Fall back to non-DLSS path |

### Graceful Degradation

When DLSS fails or is unavailable:

1. `RenderExtent == DisplayExtent` (no resolution decoupling)
2. Motion vectors not generated (shader variant)
3. Jitter not applied
4. Post-processing unchanged
5. `ResolutionState::isDlssActive()` returns `false`

The game remains fully playable without DLSS.

---

## Performance Considerations

### Frame Timing

DLSS evaluation adds GPU time but reduces overall frame time due to lower render resolution:

| Component | Without DLSS | With DLSS (Performance) |
|-----------|--------------|-------------------------|
| G-Buffer | 4.0ms (1080p) | 1.0ms (540p) |
| Lighting | 2.0ms (1080p) | 0.5ms (540p) |
| DLSS Evaluate | - | 1.5ms |
| Post-processing | 1.5ms (1080p) | 1.5ms (1080p) |
| **Total** | **7.5ms** | **4.5ms** |

*Example values; actual timing depends on scene complexity and GPU. Profile with RenderDoc or Nsight.*

### Memory Overhead

Additional allocations when DLSS is enabled (at 1080p display, Performance mode):

| Resource | Size |
|----------|------|
| MotionVectors (RenderExtent) | 540x270 x 4 bytes = ~0.6 MB |
| ColorOutHDR (DisplayExtent) | 1920x1080 x 8 bytes = ~16 MB |
| DLSS internal state | ~50-100 MB (SDK managed) |

### Latency Considerations

DLSS temporal accumulation can add ~1 frame of input latency:

- Consider NVIDIA Reflex integration (future work) for latency-sensitive scenarios
- Document latency impact for users in graphics options tooltip

---

## Testing and Validation

### Debug Visualizations

**Motion Vector View:**

```cpp
// Debug shader for motion vector visualization
// Enable via command-line flag or debug menu
vec3 visualizeMotion(vec2 mv) {
    // Scale motion for visibility
    vec2 scaled = mv * 10.0;

    // Encode direction as hue, magnitude as saturation
    float angle = atan(scaled.y, scaled.x);
    float magnitude = length(scaled);

    return hsv2rgb(vec3(angle / (2.0 * PI) + 0.5, min(magnitude, 1.0), 1.0));
}
```

**Expected Results:**
- Camera pan over static geometry: coherent vectors in pan direction (uniform color)
- Moving ships: vectors point in motion direction (colored by direction)
- Stationary objects: vectors near zero (dark/black)

**Jitter Validation:**

```cpp
// Debug: render with jitter but without DLSS
// Scene should wobble by < 1 pixel (subpixel amounts)
// Disable DLSS evaluate, keep jitter active -> visible jitter confirms it works
```

### Unit Tests

| Test | Purpose |
|------|---------|
| Jitter sequence determinism | Same frame index always produces same jitter |
| Jitter bounds | All jitter values within (-0.5, +0.5) pixel range |
| Resolution policy | RenderExtent correctly computed for all quality modes |
| Resolution alignment | RenderExtent respects SDK alignment requirements |
| Feature key equality | DlssFeatureKey comparison works correctly |
| LOD bias calculation | `computeLodBias()` returns expected values |

### Integration Tests

| Test | Purpose |
|------|---------|
| Target resize | RenderExtent change reallocates correct resources |
| No zero-size targets | RenderExtent always >= 1x1 |
| Extent split correctness | RenderExtent targets distinct from DisplayExtent targets |
| Motion vector format | Velocity attachment is writable and samplable |
| DLSS toggle | Enable/disable produces correct frame output without validation errors |
| Swapchain resize | DisplayExtent change triggers feature recreation |

### Visual Validation

| Scenario | Expected Result |
|----------|-----------------|
| Static camera, moving ship | Ship edges sharp, no ghosting |
| Moving camera, static scene | Scene stable, no jitter artifacts |
| Camera cut | No ghosting from previous scene |
| Quality mode switch | Clean transition, new resolution applied |
| Particle effects | Acceptable quality (may need reactive mask) |
| Mission load | Clean first frame, no history artifacts |

---

## Phased Implementation Checklist

### Phase A: Resolution Decoupling (No DLSS)

**Goal:** Prove dual-extent infrastructure without NGX complexity.

- [ ] Add `ResolutionState` (DisplayExtent, RenderExtent) to `VulkanRenderer`
- [ ] Teach `VulkanRenderTargets` to allocate dual-extent resources
- [ ] Modify `VulkanRenderTargets::create()` and `resize()` for RenderExtent awareness
- [ ] Add simple bilinear upsample pass (RenderExtent -> DisplayExtent)
- [ ] Verify HUD/UI renders at DisplayExtent (per `VULKAN_HUD_RENDERING.md`)
- [ ] Verify post-processing operates at correct resolution
- [ ] Add manual render scale control (debug command or cvar)

**Exit Criteria:**
RenderExtent can be set to a smaller size (e.g., 50%) and the game remains visually correct (with expected quality loss from bilinear upscale). No validation errors.

### Phase B: Motion Vectors + Jitter (No DLSS)

**Goal:** Generate correct temporal inputs without DLSS consumption.

- [ ] Add MotionVectors render target to `VulkanRenderTargets` (RenderExtent)
- [ ] Extend G-buffer shader (`model.frag`) with motion vector output (location 5)
- [ ] Add shader variant flag for motion vector output
- [ ] Implement `PreviousTransformCache` keyed by `RenderKey`
- [ ] Implement `JitterSequence` generator
- [ ] Add jittered/unjittered projection split in matrix setup
- [ ] Plumb `prevMvpMatrix` through model rendering path
- [ ] Add debug visualizations (motion vector color overlay)
- [ ] Implement `resetHistory` flag generation (scene load, camera cut detection)

**Exit Criteria:**
Motion vector debug view shows correct velocity for moving objects (consistent direction/color). Jitter wobble is visible when rendering at low res without upscale, and within expected range.

### Phase C: DLSS Module Integration (Boundary Only)

**Goal:** Initialize DLSS SDK and manage feature lifecycle.

- [ ] Add `code/graphics/vulkan/dlss/` module with CMake integration
- [ ] Add `FSO_ENABLE_DLSS` CMake option with SDK path handling
- [ ] Implement `DlssManager::initialize()` with availability checks
- [ ] Implement `DlssFeatureKey` and feature (re)creation logic
- [ ] Add `DlssState` variant (Unavailable/Ready) as renderer member
- [ ] Implement graceful degradation on failure
- [ ] Add logging for initialization success/failure

**Exit Criteria:**
On supported hardware, feature initializes and logs success. On unsupported hardware, logs clear reason once and continues without DLSS. No crashes or validation errors.

### Phase D: DLSS Evaluation Pass

**Goal:** Full DLSS integration producing upscaled output.

- [ ] Insert DLSS evaluate pass between scene and post-processing in frame flow
- [ ] Implement layout transitions for DLSS inputs/outputs via `VulkanRenderingSession`
- [ ] Wire all DLSS parameters (color, depth, motion, jitter, reset) to `DlssManager::evaluate()`
- [ ] Route post-processing to use `ColorOutHDR` when DLSS active
- [ ] Verify DLSS toggle on/off works correctly
- [ ] Verify no validation errors

**Exit Criteria:**
DLSS mode toggles correctly. Output is temporally stable with no validation errors. Visual quality is acceptable.

### Phase E: Quality Options and Polish

**Goal:** User-facing controls and quality refinements.

- [ ] Add UI option: enable/disable DLSS (in graphics options)
- [ ] Add quality mode selection UI (Quality/Balanced/Performance/UltraPerformance)
- [ ] Implement sampler LOD bias keyed by render scale
- [ ] Add optional sharpening control slider
- [ ] Disable FXAA/SMAA in `endSceneTexture()` when DLSS active
- [ ] Optional: reactive mask for transparency (if artifacts warrant)
- [ ] Performance profiling and optimization
- [ ] Update user documentation/tooltips

**Exit Criteria:**
Feature complete with user controls. Performance meets targets. Documentation updated.

---

## Code Areas Expected to Change

| File | Changes |
|------|---------|
| `code/graphics/vulkan/VulkanRenderTargets.h` | Add motion vectors, DLSS output HDR; dual-extent allocation support |
| `code/graphics/vulkan/VulkanRenderTargets.cpp` | Resource creation for new targets; extent-aware allocation |
| `code/graphics/vulkan/VulkanRenderingSession.h` | New target types for motion vectors; DLSS transition methods |
| `code/graphics/vulkan/VulkanRenderingSession.cpp` | Layout transitions for DLSS; `transitionForDlssEvaluate()` |
| `code/graphics/vulkan/VulkanRenderer.h` | `ResolutionState`, DLSS manager ownership, jitter state |
| `code/graphics/vulkan/VulkanRenderer.cpp` | Frame orchestration, jitter application, history reset, DLSS evaluate call |
| `code/graphics/vulkan/VulkanTextureManager.h` | `SamplerKey` LOD bias extension |
| `code/graphics/vulkan/VulkanTextureManager.cpp` | Sampler cache update for LOD bias |
| `code/graphics/shaders/model.frag` | Motion vector output (location 5) |
| `code/graphics/shaders/model.vert` | Potentially pass previous-frame data to fragment |
| **NEW:** `code/graphics/vulkan/dlss/DlssManager.h` | NGX SDK wrapper interface |
| **NEW:** `code/graphics/vulkan/dlss/DlssManager.cpp` | NGX initialization, feature management, evaluate |
| **NEW:** `code/graphics/vulkan/dlss/DlssTypes.h` | `DlssEvaluateToken`, `DlssEvaluateParams`, `DlssFeatureKey` |
| **NEW:** `code/graphics/vulkan/dlss/DlssJitter.h` | `JitterSequence` class |
| **NEW:** `code/graphics/vulkan/dlss/DlssJitter.cpp` | Halton sequence implementation |

---

## Open Questions

These must be resolved during implementation by consulting the specific NGX SDK version integrated:

| Question | Resolution Method |
|----------|-------------------|
| Motion vector units (pixels vs NDC vs other) | SDK documentation + experimentation |
| Depth range conventions (reversed-Z, linear vs non-linear) | SDK documentation; FSO uses reversed-Z |
| Jitter representation (pixels vs NDC, centered vs corner) | SDK documentation |
| Required Vulkan extensions for NGX | SDK headers / documentation |
| Exposure requirement for stable brightness | SDK documentation; may need auto-exposure input |
| Cockpit RTT displays at RenderExtent vs DisplayExtent | Per-RT policy decision; may render cockpit RTTs at DisplayExtent always |
| DLSS internal state memory location | SDK manages; verify no conflicts with VMA allocator |
| Minimum RenderExtent limits | SDK query or documentation |
| NGX SDK licensing for open-source distribution | Legal review; SDK binaries likely cannot be redistributed |

---

## Revision History

| Date | Changes |
|------|---------|
| Initial | Planning document created |
| 2025-01 | Documentation overhaul: improved code accuracy, added specific file/line references, verified against actual codebase, expanded implementation details |
