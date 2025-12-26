# Vulkan Post-Processing Pipeline

This document describes the post-processing pipeline in the Vulkan renderer, including the multi-pass rendering flow, render target transitions, and uniform data flow.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Quick Reference](#2-quick-reference)
3. [Pipeline Flow](#3-pipeline-flow)
4. [Render Targets](#4-render-targets)
5. [Pass Details](#5-pass-details)
6. [Uniform Data Flow](#6-uniform-data-flow)
7. [Target Transitions](#7-target-transitions)
8. [Conditional Passes](#8-conditional-passes)
9. [Debugging and Troubleshooting](#9-debugging-and-troubleshooting)

---

## 1. Overview

The post-processing pipeline transforms the rendered 3D scene (stored in an HDR offscreen target) into the final swapchain image. The pipeline consists of multiple fullscreen passes that apply effects in sequence, converting from HDR linear color space to the final sRGB output.

### Entry Point

Post-processing is triggered by `VulkanRenderer::endSceneTexture()`:

```cpp
void VulkanRenderer::endSceneTexture(const FrameCtx& ctx, bool enablePostProcessing);
```

**Preconditions**:

- Scene rendering must have been started via `beginSceneTexture(const FrameCtx& ctx, bool enableHdrPipeline)`
- Scene HDR target contains the rendered 3D scene
- `enablePostProcessing` flag controls whether effects are applied

**Postconditions**:

- Scene HDR target is released (`m_sceneTexture.reset()`)
- Swapchain image contains the final composited result
- Scissor state is restored for subsequent UI rendering

### Pipeline Stages

The post-processing chain executes in this order:

1. **Bloom** (HDR, optional)
   - Bright pass -> Mip generation -> Blur (vertical/horizontal x2) -> Additive composite
2. **Tonemapping** (HDR -> LDR)
   - Converts HDR scene to LDR linear color; always executes
3. **Anti-Aliasing** (LDR, optional)
   - SMAA (3-pass) or FXAA (2-pass), mutually exclusive
4. **Lightshafts** (LDR, optional)
   - Additive radial blur effect using scene and cockpit depth
5. **Post Effects / Final Resolve** (LDR -> Swapchain)
   - Applies color grading effects OR performs a simple copy

---

## 2. Quick Reference

### Key Functions

| Function | Purpose |
|----------|---------|
| `VulkanRenderer::endSceneTexture()` | Main post-processing entry point |
| `recordBloomBrightPass()` | Extracts bright pixels for bloom |
| `recordBloomBlurPass()` | Gaussian blur (vertical or horizontal) |
| `recordBloomCompositePass()` | Blends bloom back into scene |
| `recordTonemappingToSwapchain()` | HDR to LDR conversion |
| `recordSmaaEdgePass()` | SMAA edge detection |
| `recordSmaaBlendWeightsPass()` | SMAA blending weight calculation |
| `recordSmaaNeighborhoodPass()` | SMAA final blending |
| `recordFxaaPrepass()` | Computes luminance for FXAA |
| `recordFxaaPass()` | Applies FXAA anti-aliasing |
| `recordLightshaftsPass()` | Renders radial light shafts |
| `recordPostEffectsPass()` | Applies color grading effects |
| `recordCopyToSwapchain()` | Simple texture copy to swapchain |

### Key Constants

| Constant | Value | Location |
|----------|-------|----------|
| `kBloomMipLevels` | 4 | `VulkanRenderTargets.h:110` |
| `kBloomPingPongCount` | 2 | `VulkanRenderTargets.h:109` |
| Max lightshaft samples | 128 | Clamped in `endSceneTexture()` |

### Enable Conditions Summary

| Pass | Condition |
|------|-----------|
| Bloom | `enablePostProcessing && gr_bloom_intensity() > 0` |
| Tonemapping | Always (passthrough if HDR disabled) |
| SMAA | `enablePostProcessing && gr_is_smaa_mode(Gr_aa_mode)` |
| FXAA | `enablePostProcessing && gr_is_fxaa_mode(Gr_aa_mode)` |
| Lightshafts | See [Section 5.5](#55-lightshafts-pass) |
| Post Effects | `enablePostProcessing && doPostEffects` |

---

## 3. Pipeline Flow

### High-Level Flow Diagram

```
beginSceneTexture()
    |
[3D Scene Rendering into Scene HDR]
    |
endSceneTexture()
    |
    +-- Suspend rendering, transition Scene HDR to shader-read
    |
[Bloom Pass] (if enabled)
    +-- Bright Pass: Scene HDR -> bloom[0] mip0 (half-res)
    +-- Generate Mip Chain: bloom[0] mip0 -> mip1 -> mip2 -> mip3
    +-- Blur Iteration 1:
    |       Vertical:   bloom[0] -> bloom[1] (per mip)
    |       Horizontal: bloom[1] -> bloom[0] (per mip)
    +-- Blur Iteration 2: (repeat)
    +-- Composite: bloom[0] -> Scene HDR (additive blend)
    |
[Tonemapping Pass] (always)
    Scene HDR -> postLdr (HDR linear -> LDR linear)
    |
[SMAA Pass] (if enabled, mutually exclusive with FXAA)
    +-- Edge Detection:       postLdr -> smaaEdges
    +-- Blending Weights:     smaaEdges + lookup textures -> smaaBlend
    +-- Neighborhood Blending: postLdr + smaaBlend -> smaaOutput
    |
[FXAA Pass] (if enabled, mutually exclusive with SMAA)
    +-- Prepass: postLdr -> postLuminance (RGB -> RGBL)
    +-- FXAA:    postLuminance -> postLdr (overwrites)
    |
[Lightshafts Pass] (if enabled)
    Depth buffers -> postLdr or smaaOutput (additive blend)
    |
[Post Effects / Final Resolve]
    +-- If effects enabled: LDR + depth -> swapchain (SDR_TYPE_POST_PROCESS_MAIN)
    +-- Else:               LDR -> swapchain (SDR_TYPE_COPY)
    |
[Restore scissor for UI]
```

### Color Space Pipeline

The pipeline manages color space transitions as follows:

1. **Scene HDR**: Linear HDR (`R16G16B16A16_SFLOAT`)
2. **Post LDR**: Linear LDR after tonemapping (`B8G8R8A8_UNORM`)
3. **Swapchain**: sRGB output (`B8G8R8A8_SRGB`)

The swapchain uses an sRGB format, so the hardware automatically performs linear-to-sRGB conversion on write. This means post-processing shaders output linear color values.

---

## 4. Render Targets

### Target Types

| Target | Format | Purpose | Dimensions |
|--------|--------|---------|------------|
| **Scene HDR** | `R16G16B16A16_SFLOAT` | 3D scene rendering output (linear HDR) | Full resolution |
| **Bloom[0]** | `R16G16B16A16_SFLOAT` | Bloom ping-pong buffer 0 (4 mip levels) | Half resolution |
| **Bloom[1]** | `R16G16B16A16_SFLOAT` | Bloom ping-pong buffer 1 (4 mip levels) | Half resolution |
| **Post LDR** | `B8G8R8A8_UNORM` | Tonemapped scene (linear LDR) | Full resolution |
| **Post Luminance** | `B8G8R8A8_UNORM` | FXAA prepass output (RGBL) | Full resolution |
| **SMAA Edges** | `B8G8R8A8_UNORM` | SMAA edge detection output | Full resolution |
| **SMAA Blend** | `B8G8R8A8_UNORM` | SMAA blending weights | Full resolution |
| **SMAA Output** | `B8G8R8A8_UNORM` | SMAA final anti-aliased output | Full resolution |
| **Swapchain** | `B8G8R8A8_SRGB` | Final output (sRGB) | Full resolution |

### Auxiliary Resources

| Resource | Format | Purpose |
|----------|--------|---------|
| **Main Depth** | `D32_SFLOAT` or `D24_UNORM_S8_UINT` | Scene depth buffer |
| **Cockpit Depth** | Same as main depth | Separate cockpit depth for lightshafts |
| **SMAA Area Texture** | `R8G8_UNORM` | Pre-computed area lookup (160 x 560) |
| **SMAA Search Texture** | `R8_UNORM` | Pre-computed search lookup (64 x 16) |

### Bloom Target Dimensions

Bloom operates at half resolution with a 4-level mip chain:

| Mip Level | Width | Height |
|-----------|-------|--------|
| 0 | `swapchain.width >> 1` | `swapchain.height >> 1` |
| 1 | `swapchain.width >> 2` | `swapchain.height >> 2` |
| 2 | `swapchain.width >> 3` | `swapchain.height >> 3` |
| 3 | `swapchain.width >> 4` | `swapchain.height >> 4` |

---

## 5. Pass Details

### 5.1 Bloom Pass

**Purpose**: Extract bright areas from the HDR scene, blur them at multiple scales, and additively composite back into the scene to create a glow effect.

**Shaders**:
- `SDR_TYPE_POST_PROCESS_BRIGHTPASS` (`brightpass.frag`)
- `SDR_TYPE_POST_PROCESS_BLUR` (`blur_v.frag`, `blur_h.frag`)
- `SDR_TYPE_POST_PROCESS_BLOOM_COMP` (`bloom_comp.frag`)

#### 5.1.1 Bright Pass

Extracts pixels exceeding a luminance threshold.

- **Input**: Scene HDR (binding 2)
- **Output**: `bloom[0]` mip level 0 (half resolution)
- **Clear**: Target is cleared before writing
- **Uniform**: None (samples Scene HDR directly)

#### 5.1.2 Mip Generation

Generates a mip chain from the bright pass output using hardware blits.

```cpp
generateBloomMipmaps(cmd, 0, bloomExtent);
// Uses vkCmdBlitImage: mip0 -> mip1 -> mip2 -> mip3
```

#### 5.1.3 Blur Passes

Two iterations of separable Gaussian blur, ping-ponging between bloom buffers.

- **Iteration Pattern** (per mip level):
  1. Vertical blur: `bloom[0]` -> `bloom[1]`
  2. Horizontal blur: `bloom[1]` -> `bloom[0]`
- **Shader Variants**: `SDR_FLAG_BLUR_VERTICAL` or `SDR_FLAG_BLUR_HORIZONTAL`
- **Uniform**: `generic_data::blur_data` (binding 1)
  - `texSize`: Inverse of blur dimension (`1.0 / width` or `1.0 / height`)
  - `level`: Current mip level (0-3)

#### 5.1.4 Composite Pass

Blends the blurred bloom result back into the HDR scene.

- **Input**: `bloom[0]` full mip chain view (binding 2)
- **Output**: Scene HDR with `ALPHA_BLEND_ADDITIVE`
- **Uniform**: `generic_data::bloom_composition_data` (binding 1)
  - `bloom_intensity`: `gr_bloom_intensity() / 100.0f` (0.0 - 1.0)
  - `levels`: Number of mip levels (4)

**Key Points**:
- Half-resolution processing improves performance
- Multi-scale blur via mip chain creates smooth, wide glow
- Two blur iterations produce higher quality results
- Additive blending preserves original scene content

---

### 5.2 Tonemapping Pass

**Purpose**: Convert HDR linear color to LDR linear color using a selected tonemapping algorithm. This pass always executes; when HDR is disabled, it uses a linear (passthrough) tonemapper with exposure 1.0.

**Shader**: `SDR_TYPE_POST_PROCESS_TONEMAPPING` (`tonemapping.frag`)

**Flow**:
- **Input**: Scene HDR (binding 2)
- **Output**: `postLdr`
- **Uniform**: `generic_data::tonemapping_data` (binding 1)

#### Uniform Fields

| Field | Type | Description |
|-------|------|-------------|
| `exposure` | float | Exposure multiplier (applied before tonemapping) |
| `tonemapper` | int | Algorithm selector (see table below) |
| `x0`, `y0` | float | PPC toe/linear junction point |
| `x1` | float | PPC linear/shoulder junction x |
| `toe_B`, `toe_lnA` | float | PPC toe segment parameters |
| `sh_B`, `sh_lnA` | float | PPC shoulder segment parameters |
| `sh_offsetX`, `sh_offsetY` | float | PPC shoulder offset values |

#### Tonemapper Algorithms

| Value | Enum Name | Description |
|-------|-----------|-------------|
| 0 | `Linear` | Passthrough with clamp (used when HDR disabled) |
| 1 | `Uncharted` | Uncharted 2 filmic curve (John Hable) |
| 2 | `Aces` | Full ACES RRT+ODT fit |
| 3 | `Aces_Approx` | Faster ACES approximation |
| 4 | `Cineon` | Optimized Cineon/filmic (Hejl-Burgess-Dawson) |
| 5 | `Reinhard_Jodie` | Luma-based Reinhard variant |
| 6 | `Reinhard_Extended` | Extended Reinhard with white point |
| 7 | `PPC` | Piecewise Power Curve (luma-based) |
| 8 | `PPC_RGB` | Piecewise Power Curve (per-channel) |

The tonemapper selection is defined in `lighting_profiles::TonemapperAlgorithm` and configured via `profiles.tbl`.

**Key Points**:
- Output is LDR linear color (sRGB conversion happens at swapchain write)
- Exposure is applied as a pre-multiplier: `color *= exposure`
- PPC tonemappers use the additional curve parameters for custom response curves

---

### 5.3 SMAA Pass

**Purpose**: Subpixel Morphological Anti-Aliasing (SMAA) - a high-quality post-process AA technique that detects edges and blends pixels based on morphological patterns.

**Shaders**:
- `SDR_TYPE_POST_PROCESS_SMAA_EDGE` (`smaa_edge.frag`)
- `SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT` (`smaa_blend.frag`)
- `SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING` (`smaa_neighborhood.frag`)

#### 5.3.1 Edge Detection

Detects edges using luma differences.

- **Input**: `postLdr` (binding 2)
- **Output**: `smaaEdges`
- **Uniform**: `generic_data::smaa_data` (binding 1)
  - `smaa_rt_metrics`: `vec2d` containing `(1.0/width, 1.0/height)`

#### 5.3.2 Blending Weights

Calculates blending weights by matching edge patterns against pre-computed lookup textures.

- **Inputs**:
  - `smaaEdges` (binding 2)
  - SMAA area texture (binding 3) - 160 x 560, `R8G8_UNORM`
  - SMAA search texture (binding 4) - 64 x 16, `R8_UNORM`
- **Output**: `smaaBlend`
- **Uniform**: `generic_data::smaa_data` (binding 1)

#### 5.3.3 Neighborhood Blending

Blends the original image using the calculated weights.

- **Inputs**:
  - `postLdr` (binding 2)
  - `smaaBlend` (binding 3)
- **Output**: `smaaOutput`
- **Uniform**: `generic_data::smaa_data` (binding 1)

**Key Points**:
- Three-pass algorithm with moderate GPU cost
- Lookup textures are loaded once at initialization via `loadSmaaTextures()`
- Output stored in `smaaOutput`; subsequent passes use this instead of `postLdr`
- Mutually exclusive with FXAA

---

### 5.4 FXAA Pass

**Purpose**: Fast Approximate Anti-Aliasing - a lightweight single-pass AA technique that smooths edges based on local contrast.

**Shaders**:
- `SDR_TYPE_POST_PROCESS_FXAA_PREPASS` (`fxaa_prepass.frag`)
- `SDR_TYPE_POST_PROCESS_FXAA` (`fxaa.frag`)

#### 5.4.1 Prepass

Computes luminance and stores it in the alpha channel (RGB -> RGBL conversion).

- **Input**: `postLdr` (binding 2)
- **Output**: `postLuminance`
- **Uniform**: None

#### 5.4.2 FXAA Pass

Applies the FXAA algorithm using the luminance information.

- **Input**: `postLuminance` (binding 2)
- **Output**: `postLdr` (overwrites original)
- **Uniform**: `generic_data::fxaa_data` (binding 1)
  - `rt_w`: Render target width (float)
  - `rt_h`: Render target height (float)

**Key Points**:
- Two-pass algorithm; faster than SMAA but lower quality
- Overwrites `postLdr` directly (unlike SMAA which uses separate output)
- Subsequent passes continue using `postLdr`
- Mutually exclusive with SMAA

---

### 5.5 Lightshafts Pass

**Purpose**: Renders radial light shafts (god rays) emanating from the sun position, using depth information to determine occlusion.

**Shader**: `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` (`lightshafts.frag`)

**Enable Conditions** (all must be true):
- `enablePostProcessing == true`
- `!Game_subspace_effect` (not in subspace)
- `gr_sunglare_enabled()` (sunglare effect enabled)
- `gr_lightshafts_enabled()` (lightshafts enabled in settings)
- `Sun_spot > 0.0f` (sun is visible)
- Light has glare (`light_has_glare(idx)`)
- Light is facing camera (`dot > 0.7f`)

**Flow**:
- **Inputs**:
  - Main depth buffer (binding 2)
  - Cockpit depth buffer (binding 3)
- **Output**: Current LDR buffer (`postLdr` or `smaaOutput`) with `ALPHA_BLEND_ADDITIVE`
- **Uniform**: `generic_data::lightshaft_data` (binding 1)

#### Uniform Fields

| Field | Type | Description | Default |
|-------|------|-------------|---------|
| `sun_pos` | `vec2d` | Sun position in screen space (0-1 range) | Computed per-frame |
| `density` | float | Radial blur density | 0.5 |
| `weight` | float | Sample weight | 0.02 |
| `falloff` | float | Distance falloff | 1.0 |
| `intensity` | float | Effect intensity (scaled by `Sun_spot`) | 0.5 |
| `cp_intensity` | float | Cockpit intensity (scaled by `Sun_spot`) | 0.5 |
| `samplenum` | int | Number of samples (clamped 1-128) | 50 |

**Key Points**:
- Only processes the first qualifying glare light (matches OpenGL behavior)
- Uses both main and cockpit depth buffers for proper occlusion
- Sample count is defensively clamped to prevent GPU stalls
- Parameters sourced from `Post_processing_manager->getLightshaftParams()` if available
- Renders into current LDR buffer, not directly to swapchain

---

### 5.6 Post Effects Pass

**Purpose**: Apply final color grading effects (saturation, brightness, contrast, film grain, etc.) and resolve to the swapchain.

**Shader**: `SDR_TYPE_POST_PROCESS_MAIN` (`post_effects.frag`)

**Flow**:
- **Inputs**:
  - Current LDR buffer (binding 2)
  - Main depth buffer (binding 3)
- **Output**: Swapchain
- **Uniform**: `generic_data::post_data` (binding 1)

#### Uniform Fields

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `timer` | float | milliseconds | Time value for animated effects |
| `noise_amount` | float | 0.0 | Noise effect intensity |
| `saturation` | float | 1.0 | Color saturation multiplier |
| `brightness` | float | 1.0 | Brightness multiplier |
| `contrast` | float | 1.0 | Contrast multiplier |
| `film_grain` | float | 0.0 | Film grain intensity |
| `tv_stripes` | float | 0.0 | TV stripe effect intensity |
| `cutoff` | float | 0.0 | Effect cutoff threshold |
| `tint` | `vec3d` | (0,0,0) | Color tint |
| `dither` | float | 0.0 | Dithering amount |
| `custom_effect_vec3_a` | `vec3d` | (0,0,0) | Custom effect vector A |
| `custom_effect_float_a` | float | 0.0 | Custom effect scalar A |
| `custom_effect_vec3_b` | `vec3d` | (0,0,0) | Custom effect vector B |
| `custom_effect_float_b` | float | 0.0 | Custom effect scalar B |

**Key Points**:
- Only executes if `doPostEffects` is true (at least one effect has `always_on` or non-default intensity)
- Effect parameters sourced from `Post_processing_manager->getPostEffects()`
- Depth buffer provided for depth-dependent effects
- If no effects are active, falls back to `recordCopyToSwapchain()`

---

### 5.7 Copy to Swapchain Pass

**Purpose**: Simple texture copy from LDR buffer to swapchain when no post effects are active.

**Shader**: `SDR_TYPE_COPY` (`copy.frag`)

**Flow**:
- **Input**: Current LDR buffer (binding 2)
- **Output**: Swapchain
- **Uniform**: None

**Key Points**:
- Used as fallback when `doPostEffects` is false
- Simple texture sampling with no additional processing

---

## 6. Uniform Data Flow

### Allocation Pattern

Each post-processing pass allocates uniforms from the per-frame ring buffer with proper alignment:

```cpp
const size_t uboAlignment = renderer.getMinUniformOffsetAlignment();
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(tonemapping_data), uboAlignment);
std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

vk::DescriptorBufferInfo genericInfo{};
genericInfo.buffer = frame.uniformBuffer().buffer();
genericInfo.offset = uniformAlloc.offset;
genericInfo.range = sizeof(tonemapping_data);
```

All uniform structs use the `genericData` binding (binding 1) with push descriptors.

### Uniform Struct Sizes

| Pass | Uniform Struct | Size (bytes) |
|------|---------------|--------------|
| Bloom Bright Pass | None | - |
| Bloom Blur | `generic_data::blur_data` | 16 |
| Bloom Composite | `generic_data::bloom_composition_data` | 16 |
| Tonemapping | `generic_data::tonemapping_data` | 48 |
| SMAA (all passes) | `generic_data::smaa_data` | 16 |
| FXAA Prepass | None | - |
| FXAA | `generic_data::fxaa_data` | 16 |
| Lightshafts | `generic_data::lightshaft_data` | 32 |
| Post Effects | `generic_data::post_data` | 80 |
| Copy | None | - |

Sizes reflect std140 layout alignment requirements (16-byte alignment for all members).

---

## 7. Target Transitions

### Transition Pattern

Each pass follows this synchronization pattern:

1. **Suspend Rendering**: End any active render pass (`suspendRendering()`)
2. **Transition Source**: Transition source image to `SHADER_READ_ONLY_OPTIMAL`
3. **Request Target**: Request new render target (transitions to `COLOR_ATTACHMENT_OPTIMAL`)
4. **Begin Rendering**: Start new render pass (`ensureRenderingStarted()`)
5. **Draw**: Execute fullscreen quad draw
6. **Suspend Rendering**: End render pass
7. **Transition Output**: Transition output for next pass (if needed)

### Example: Bloom Blur Transition

```cpp
// End previous pass
m_renderingSession->suspendRendering();

// Transition source to shader-read
m_renderingSession->transitionBloomToShaderRead(cmd, 0);

// Request destination target
m_renderingSession->requestBloomMipTarget(1, mip);

// Begin rendering
auto render = ensureRenderingStarted(ctx);

// Draw blur pass
recordBloomBlurPass(render, frame, ...);

// End pass
m_renderingSession->suspendRendering();
```

### Critical Transitions Summary

| Transition | From | To | When |
|------------|------|-----|------|
| Scene HDR -> Shader Read | `COLOR_ATTACHMENT_OPTIMAL` | `SHADER_READ_ONLY_OPTIMAL` | After scene render, before bloom/tonemap |
| Bloom[n] -> Shader Read | `COLOR_ATTACHMENT_OPTIMAL` | `SHADER_READ_ONLY_OPTIMAL` | After blur, before next blur/composite |
| Post LDR -> Shader Read | `COLOR_ATTACHMENT_OPTIMAL` | `SHADER_READ_ONLY_OPTIMAL` | After tonemap, before AA/resolve |
| Main Depth -> Shader Read | `DEPTH_ATTACHMENT_OPTIMAL` | `DEPTH_READ_ONLY_OPTIMAL` | Before lightshafts/post effects |
| Cockpit Depth -> Shader Read | `DEPTH_ATTACHMENT_OPTIMAL` | `DEPTH_READ_ONLY_OPTIMAL` | Before lightshafts |
| SMAA Output -> Shader Read | `COLOR_ATTACHMENT_OPTIMAL` | `SHADER_READ_ONLY_OPTIMAL` | After SMAA, before resolve |

---

## 8. Conditional Passes

### Pass Enablement Logic

| Pass | Enable Condition |
|------|------------------|
| **Bloom** | `enablePostProcessing && gr_bloom_intensity() > 0` |
| **Tonemapping** | Always (uses `Linear` tonemapper if HDR disabled) |
| **SMAA** | `enablePostProcessing && gr_is_smaa_mode(Gr_aa_mode)` |
| **FXAA** | `enablePostProcessing && gr_is_fxaa_mode(Gr_aa_mode)` |
| **Lightshafts** | Multiple conditions (see [Section 5.5](#55-lightshafts-pass)) |
| **Post Effects** | `enablePostProcessing && doPostEffects` |
| **Copy** | Fallback when Post Effects is disabled |

**Note**: SMAA and FXAA are mutually exclusive; only one AA mode is active at a time.

### LDR Buffer Selection

The pipeline tracks which buffer contains the current LDR result:

```cpp
bool ldrIsSmaaOutput = false;

// After SMAA
if (smaa_enabled) {
    ldrInfo.imageView = m_renderTargets->smaaOutputView();
    ldrIsSmaaOutput = true;
}

// Lightshafts renders into correct buffer
if (ldrIsSmaaOutput) {
    m_renderingSession->requestSmaaOutputTarget();
} else {
    m_renderingSession->requestPostLdrTarget();
}
```

### Scissor Preservation

The post-processing chain preserves scissor state for subsequent UI rendering:

```cpp
// Save scissor before post-processing
auto clip = getClipScissorFromScreen(gr_screen);
clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
vk::Rect2D restoreScissor{...};

// ... post-processing passes ...

// Restore scissor after post-processing
cmd.setScissor(0, 1, &restoreScissor);
```

---

## 9. Debugging and Troubleshooting

### RenderDoc Integration

Use RenderDoc to capture frames and inspect each post-processing pass. Key things to verify:

- Correct input/output textures at each stage
- Proper image layout transitions (no validation errors)
- Uniform buffer contents match expected values
- Blend states are correct (especially for additive passes)

### Common Issues and Solutions

#### Bloom appears too bright or too dim

- **Check**: `gr_bloom_intensity()` returns expected value (0-100 scale)
- **Check**: `bloom_composition_data.bloom_intensity` is `intensity / 100.0f`
- **Verify**: Bright pass threshold is appropriate for scene content
- **Verify**: Blend mode is `ALPHA_BLEND_ADDITIVE` for composite

#### SMAA not working

- **Check**: SMAA lookup textures are loaded (`m_smaaAreaTex.view`, `m_smaaSearchTex.view`)
- **Check**: `gr_is_smaa_mode(Gr_aa_mode)` returns true
- **Verify**: All three targets (`smaaEdges`, `smaaBlend`, `smaaOutput`) are properly transitioned
- **Verify**: Area texture is 160x560 `R8G8_UNORM`, search texture is 64x16 `R8_UNORM`

#### Tonemapping produces incorrect colors

- **Check**: `m_sceneTexture->hdrEnabled` matches expected HDR state
- **Check**: `tonemapping_data.tonemapper` contains correct algorithm index (0-8)
- **Check**: `tonemapping_data.exposure` is reasonable (typically 0.5 - 4.0)
- **Verify**: Scene HDR format is `R16G16B16A16_SFLOAT`
- **Verify**: PPC parameters are valid if using tonemapper 7 or 8

#### Lightshafts not appearing

- **Check all conditions**: `gr_sunglare_enabled()`, `gr_lightshafts_enabled()`, `!Game_subspace_effect`, `Sun_spot > 0.0f`
- **Check**: Light has glare (`light_has_glare()`) and faces camera (`dot > 0.7`)
- **Verify**: `sun_pos` is in 0-1 screen space range
- **Verify**: Both depth buffers are transitioned to shader-read layout
- **Check**: Sample count is reasonable (not clamped from extreme value)

#### Post effects not applied

- **Check**: `doPostEffects` is true (at least one effect has `always_on` or non-default intensity)
- **Check**: `Post_processing_manager` is not null
- **Verify**: Effect uniform values in `post_data` are non-default
- **Review**: Effect configuration in post-processing tables

#### Target transition validation errors

- **Check**: `suspendRendering()` called before all transitions
- **Check**: Target requests happen before `ensureRenderingStarted()`
- **Verify**: Image layouts match expected state
- **Enable**: Vulkan validation layers for detailed error messages

#### Incorrect LDR buffer used

- **Check**: `ldrIsSmaaOutput` flag is set correctly after SMAA
- **Verify**: Lightshafts renders to correct buffer based on AA mode
- **Verify**: Final resolve samples from correct LDR source

### Performance Considerations

- **Bloom**: Half-resolution processing reduces bandwidth; consider reducing mip levels for low-end GPUs
- **SMAA vs FXAA**: SMAA is higher quality but ~2x the cost; FXAA is faster but may blur fine detail
- **Lightshafts**: Sample count directly affects performance; 50 is default, 128 is max
- **Post effects**: Minimal overhead when effects have default values (identity transforms)

---

## References

### Source Files

| File | Description |
|------|-------------|
| `code/graphics/vulkan/VulkanRenderer.cpp:701-1042` | `endSceneTexture()` implementation |
| `code/graphics/vulkan/VulkanRenderTargets.h` | Render target definitions |
| `code/graphics/vulkan/VulkanRenderTargets.cpp` | Render target creation |
| `code/graphics/util/uniform_structs.h:210-420` | Uniform struct definitions (`generic_data` namespace) |
| `code/graphics/post_processing.h` | Post-processing manager and global functions |
| `code/graphics/2d.h` | `gr_is_fxaa_mode()`, `gr_is_smaa_mode()` |
| `code/lighting/lighting_profiles.h` | `TonemapperAlgorithm` enum |

### Shader Files

Located in `code/graphics/shaders/`:

| Shader | Pass |
|--------|------|
| `brightpass.frag` | Bloom bright pass |
| `blur_v.frag` | Bloom vertical blur |
| `blur_h.frag` | Bloom horizontal blur |
| `bloom_comp.frag` | Bloom composite |
| `tonemapping.frag` | HDR to LDR tonemapping |
| `smaa_edge.frag` | SMAA edge detection |
| `smaa_blend.frag` | SMAA blending weights |
| `smaa_neighborhood.frag` | SMAA neighborhood blending |
| `fxaa_prepass.frag` | FXAA luminance prepass |
| `fxaa.frag` | FXAA anti-aliasing |
| `lightshafts.frag` | Lightshafts effect |
| `post_effects.frag` | Final color grading |
| `copy.frag` | Simple texture copy |

### Related Documentation

- `VULKAN_ARCHITECTURE_OVERVIEW.md` - Overall Vulkan renderer architecture
- `VULKAN_RENDER_PASS_STRUCTURE.md` - Render pass organization
- `VULKAN_SYNCHRONIZATION.md` - Synchronization primitives
- `VULKAN_UNIFORM_BINDINGS.md` - Descriptor binding conventions
