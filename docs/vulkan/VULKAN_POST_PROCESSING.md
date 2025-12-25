# Vulkan Post-Processing Pipeline

This document describes the post-processing pipeline chain in the Vulkan renderer, including the multi-pass rendering flow, target transitions, and uniform data flow.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Pipeline Flow](#2-pipeline-flow)
3. [Render Targets](#3-render-targets)
4. [Pass Details](#4-pass-details)
5. [Uniform Data Flow](#5-uniform-data-flow)
6. [Target Transitions](#6-target-transitions)
7. [Conditional Passes](#7-conditional-passes)

---

## 1. Overview

The post-processing pipeline transforms the rendered 3D scene (stored in an HDR offscreen target) into the final swapchain image. The pipeline consists of multiple fullscreen passes that apply effects in sequence.

### Entry Point

Post-processing is triggered by `VulkanRenderer::endSceneTexture()`:

```cpp
void VulkanRenderer::endSceneTexture(const FrameCtx& ctx, bool enablePostProcessing);
```

**Preconditions**:
- Scene rendering must have been started via `beginSceneTexture(const FrameCtx& ctx, bool enableHdrPipeline)`
- Scene HDR target contains the rendered 3D scene
- `enablePostProcessing` flag controls whether effects are applied

### Pipeline Stages

The post-processing chain executes in this order:

1. **Bloom** (HDR, optional)
   - Bright pass -> Blur (vertical/horizontal) -> Composite
2. **Tonemapping** (HDR -> LDR)
   - Converts HDR scene to LDR for subsequent passes
3. **Anti-Aliasing** (LDR, optional)
   - SMAA or FXAA
4. **Lightshafts** (LDR, optional)
   - Additive lightshaft effect into LDR buffer
5. **Post Effects / Final Resolve** (LDR -> Swapchain)
   - Applies saturation, brightness, contrast, film grain, etc. OR simple copy

---

## 2. Pipeline Flow

### High-Level Flow Diagram

```
beginSceneTexture()
    |
[3D Scene Rendering]
    |
endSceneTexture()
    |
[Bloom Pass] (if enabled)
    +-- Bright Pass -> bloom[0] mip0
    +-- Generate Mip Chain (4 levels)
    +-- Blur Vertical (bloom[0] -> bloom[1]) x2 iterations
    +-- Blur Horizontal (bloom[1] -> bloom[0]) x2 iterations
    +-- Composite -> Scene HDR (additive)
    |
[Tonemapping Pass]
    Scene HDR -> postLdr
    |
[SMAA/FXAA] (if enabled)
    +-- SMAA: Edge Detection -> Blending Weights -> Neighborhood Blending -> smaaOutput
    +-- FXAA: Prepass -> FXAA Pass -> postLdr (overwrites)
    |
[Lightshafts] (if enabled)
    Additive pass -> postLdr or smaaOutput (current LDR buffer)
    |
[Post Effects / Final Resolve]
    +-- If effects enabled: postLdr/smaaOutput + depth -> swapchain (via SDR_TYPE_POST_PROCESS_MAIN)
    +-- Else: postLdr/smaaOutput -> swapchain (simple copy via SDR_TYPE_COPY)
```

### Code Flow

The implementation in `VulkanRenderer::endSceneTexture()` follows this structure:

```cpp
// 1. End scene rendering, transition HDR to shader-read
m_renderingSession->suspendRendering();
m_renderingSession->transitionSceneHdrToShaderRead(cmd);

// 2. Bloom chain (if enabled)
if (enablePostProcessing && gr_bloom_intensity() > 0) {
    // Bright pass, mip generation, blur iterations, composite
}

// 3. Tonemapping (always runs)
m_renderingSession->requestPostLdrTarget();
recordTonemappingToSwapchain(..., hdrEnabled);

// 4. SMAA/FXAA (if enabled)
if (enablePostProcessing && gr_is_smaa_mode(Gr_aa_mode)) {
    // Edge detection -> Blending weights -> Neighborhood blending
} else if (enablePostProcessing && gr_is_fxaa_mode(Gr_aa_mode)) {
    // Prepass (RGB->RGBL) -> FXAA pass
}

// 5. Lightshafts (if enabled) - renders into LDR buffer, not swapchain
if (enablePostProcessing && !Game_subspace_effect &&
    gr_sunglare_enabled() && gr_lightshafts_enabled() && Sun_spot > 0.0f) {
    // Additive pass into current LDR buffer (postLdr or smaaOutput)
}

// 6. Final resolve to swapchain
m_renderingSession->requestSwapchainNoDepthTarget();
if (enablePostProcessing && doPostEffects) {
    recordPostEffectsPass(...);  // SDR_TYPE_POST_PROCESS_MAIN
} else {
    recordCopyToSwapchain(...);  // SDR_TYPE_COPY
}
```

---

## 3. Render Targets

### Target Types

| Target | Format | Purpose | Lifetime |
|--------|--------|---------|----------|
| **Scene HDR** | `R16G16B16A16_SFLOAT` | 3D scene rendering output | Per-frame |
| **Bloom[0]** | `R16G16B16A16_SFLOAT` | Bloom ping-pong buffer 0 (4 mip levels) | Per-frame |
| **Bloom[1]** | `R16G16B16A16_SFLOAT` | Bloom ping-pong buffer 1 (4 mip levels) | Per-frame |
| **Post LDR** | `B8G8R8A8_UNORM` | Tonemapped scene (LDR) | Per-frame |
| **Post Luminance** | `B8G8R8A8_UNORM` | FXAA prepass output (RGBL) | Per-frame |
| **SMAA Edges** | `B8G8R8A8_UNORM` | SMAA edge detection | Per-frame |
| **SMAA Blend** | `B8G8R8A8_UNORM` | SMAA blending weights | Per-frame |
| **SMAA Output** | `B8G8R8A8_UNORM` | SMAA final output | Per-frame |
| **Swapchain** | `B8G8R8A8_SRGB` | Final output | Per-frame |

**Note**: All LDR post-processing targets share the same format (`B8G8R8A8_UNORM`) for consistency. The SMAA area lookup texture uses `R8G8_UNORM` and the search texture uses `R8_UNORM`, but these are separate lookup textures, not render targets.

### Target Dimensions

- **Scene HDR**: Full resolution (swapchain extent)
- **Bloom**: Half resolution (`width >> 1`, `height >> 1`) with `kBloomMipLevels` (4) mip levels
- **Post LDR**: Full resolution
- **SMAA targets**: Full resolution
- **Swapchain**: Full resolution

---

## 4. Pass Details

### 4.1 Bloom Pass

**Purpose**: Extract bright areas, blur them, and additively composite back into the scene.

**Shaders**: `SDR_TYPE_POST_PROCESS_BRIGHTPASS`, `SDR_TYPE_POST_PROCESS_BLUR`, `SDR_TYPE_POST_PROCESS_BLOOM_COMP`

**Flow**:

1. **Bright Pass**
   - **Input**: Scene HDR (binding 2)
   - **Output**: `bloom[0]` mip level 0 (half-res)
   - **Shader**: `brightpass.frag`
   - **Uniform**: None (samples Scene HDR directly)

2. **Mip Generation**
   - Generates mip chain for `bloom[0]` via `generateBloomMipmaps()`
   - Uses `vkCmdBlitImage` for each mip level (0 -> 1 -> 2 -> 3)
   - `kBloomMipLevels` = 4 total levels

3. **Blur Passes** (2 iterations, ping-pong per mip level)
   - **Iteration 1**:
     - Vertical blur: `bloom[0]` -> `bloom[1]` (each mip separately)
     - Horizontal blur: `bloom[1]` -> `bloom[0]` (each mip separately)
   - **Iteration 2**: Repeat
   - **Shader**: `blur_v.frag` (vertical) / `blur_h.frag` (horizontal)
   - **Variant Flags**: `SDR_FLAG_BLUR_VERTICAL` / `SDR_FLAG_BLUR_HORIZONTAL`
   - **Uniform**: `generic_data::blur_data`
     - `texSize`: Inverse of blur dimension (1.0 / width or 1.0 / height)
     - `level`: Current mip level

4. **Composite Pass**
   - **Input**: `bloom[0]` full mip chain view (binding 2)
   - **Output**: Scene HDR (additive composite via `ALPHA_BLEND_ADDITIVE`)
   - **Shader**: `bloom_comp.frag`
   - **Uniform**: `generic_data::bloom_composition_data`
     - `bloom_intensity`: `gr_bloom_intensity() / 100.0f`
     - `levels`: Number of mip levels (4)

**Key Points**:
- Bloom operates at half resolution for performance
- Mip chain allows multi-scale blur (4 levels)
- Two blur iterations create smoother result
- Final composite uses additive blending into Scene HDR

### 4.2 Tonemapping Pass

**Purpose**: Convert HDR scene to LDR for subsequent post-processing.

**Shader**: `SDR_TYPE_POST_PROCESS_TONEMAPPING`

**Flow**:
- **Input**: Scene HDR (binding 2)
- **Output**: `postLdr`
- **Uniform**: `generic_data::tonemapping_data` (binding 1)
  - `exposure`: Exposure value
  - `tonemapper`: Tonemapper algorithm enum (from `lighting_profiles::TonemapperAlgorithm`)
  - `x0`, `y0`, `x1`: PPC curve control points
  - `toe_B`, `toe_lnA`: Toe segment parameters
  - `sh_B`, `sh_lnA`, `sh_offsetX`, `sh_offsetY`: Shoulder segment parameters

**Tonemapper Types** (from `lighting_profiles::TonemapperAlgorithm`):
- Linear (passthrough, used when HDR disabled)
- Reinhard
- ACES
- Uncharted 2
- Custom PPC (Piecewise Power Curve)

**Key Points**:
- Always executes (even if HDR disabled, uses Linear tonemapper with exposure 1.0)
- Shader file: `tonemapping.frag`
- Output is LDR (`B8G8R8A8_UNORM`)
- Subsequent passes sample `postLdr`

### 4.3 SMAA Pass

**Purpose**: Subpixel morphological anti-aliasing (SMAA).

**Shaders**: `SDR_TYPE_POST_PROCESS_SMAA_EDGE`, `SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT`, `SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING`

**Flow**:

1. **Edge Detection**
   - **Input**: `postLdr` (binding 2)
   - **Output**: `smaaEdges`
   - **Shader**: `smaa_edge.frag`
   - **Uniform**: `generic_data::smaa_data` (binding 1)
     - `smaa_rt_metrics`: `vec2d` containing render target metrics

2. **Blending Weights**
   - **Input**: `smaaEdges` (binding 2) + SMAA area texture (binding 3) + SMAA search texture (binding 4)
   - **Output**: `smaaBlend`
   - **Shader**: `smaa_blend.frag`
   - **Uniform**: `generic_data::smaa_data`

3. **Neighborhood Blending**
   - **Input**: `postLdr` (binding 2) + `smaaBlend` (binding 3)
   - **Output**: `smaaOutput`
   - **Shader**: `smaa_neighborhood.frag`
   - **Uniform**: `generic_data::smaa_data`

**Key Points**:
- Requires SMAA area texture (`R8G8_UNORM`, 160x560) and search texture (`R8_UNORM`, 64x16)
- Lookup textures loaded at initialization via `loadSmaaTextures()`
- Three-pass algorithm
- Output is `smaaOutput`; subsequent passes use this instead of `postLdr`

### 4.4 FXAA Pass

**Purpose**: Fast approximate anti-aliasing (FXAA).

**Shaders**: `SDR_TYPE_POST_PROCESS_FXAA_PREPASS`, `SDR_TYPE_POST_PROCESS_FXAA`

**Flow**:

1. **Prepass**
   - **Input**: `postLdr` (binding 2)
   - **Output**: `postLuminance` (RGB -> RGBL conversion)
   - **Shader**: `fxaa_prepass.frag`
   - **Uniform**: None

2. **FXAA Pass**
   - **Input**: `postLuminance` (binding 2)
   - **Output**: `postLdr` (overwrites)
   - **Shader**: `fxaa.frag`
   - **Uniform**: `generic_data::fxaa_data` (binding 1)
     - `rt_w`: Render target width
     - `rt_h`: Render target height

**Key Points**:
- Two-pass algorithm
- Overwrites `postLdr` directly (unlike SMAA which creates separate `smaaOutput`)
- Simpler than SMAA but lower quality
- Subsequent passes continue using `postLdr`

### 4.5 Lightshafts Pass

**Purpose**: Additive lightshaft effect using scene depth and sun position.

**Shader**: `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS`

**Flow**:
- **Input**: Main depth buffer (binding 2) + Cockpit depth buffer (binding 3)
- **Output**: Current LDR buffer (`postLdr` or `smaaOutput`) with additive blending
- **Uniform**: `generic_data::lightshaft_data` (binding 1)
  - `sun_pos`: `vec2d` - Sun position in screen space (0-1 range)
  - `density`: Radial blur density
  - `weight`: Sample weight
  - `falloff`: Distance falloff
  - `intensity`: Effect intensity (scaled by `Sun_spot`)
  - `cp_intensity`: Cockpit intensity (scaled by `Sun_spot`)
  - `samplenum`: Number of samples (clamped to 1-128)

**Key Points**:
- Only executes if all conditions met: `gr_sunglare_enabled()`, `gr_lightshafts_enabled()`, `!Game_subspace_effect`, `Sun_spot > 0.0f`
- Shader file: `lightshafts.frag`
- Uses `ALPHA_BLEND_ADDITIVE` blend mode
- Renders into the current LDR buffer (NOT directly to swapchain)
- Parameters sourced from `Post_processing_manager->getLightshaftParams()` if available
- Sample count defensively clamped to prevent GPU stalls (max 128)

### 4.6 Post Effects Pass

**Purpose**: Apply final color grading effects (saturation, brightness, contrast, film grain, etc.) and resolve to swapchain.

**Shader**: `SDR_TYPE_POST_PROCESS_MAIN`

**Flow**:
- **Input**: Current LDR buffer (`postLdr` or `smaaOutput`) (binding 2) + Main depth buffer (binding 3)
- **Output**: Swapchain
- **Uniform**: `generic_data::post_data` (binding 1)
  - `timer`: Time-based value for animated effects
  - `noise_amount`: Noise effect intensity
  - `saturation`: Color saturation multiplier
  - `brightness`: Brightness multiplier
  - `contrast`: Contrast multiplier
  - `film_grain`: Film grain intensity
  - `tv_stripes`: TV stripe effect intensity
  - `cutoff`: Effect cutoff threshold
  - `tint`: `vec3d` color tint
  - `dither`: Dithering amount
  - `custom_effect_vec3_a`, `custom_effect_float_a`: Custom effect parameters A
  - `custom_effect_vec3_b`, `custom_effect_float_b`: Custom effect parameters B

**Key Points**:
- Only executes if any post effect is enabled (`doPostEffects` flag)
- Shader file: `post_effects.frag`
- Effect parameters sourced from `Post_processing_manager->getPostEffects()`
- Effects enabled when `always_on` OR `intensity != default_intensity`
- If no effects are active, uses `recordCopyToSwapchain()` with `SDR_TYPE_COPY` shader instead

### 4.7 Copy to Swapchain Pass

**Purpose**: Simple copy from LDR buffer to swapchain when no post effects are active.

**Shader**: `SDR_TYPE_COPY`

**Flow**:
- **Input**: Current LDR buffer (`postLdr` or `smaaOutput`) (binding 2)
- **Output**: Swapchain
- **Uniform**: None

**Key Points**:
- Used as fallback when `doPostEffects` is false
- Shader file: `copy.frag`
- Simple texture sampling with no additional processing

---

## 5. Uniform Data Flow

### Uniform Allocation Pattern

Each post-processing pass allocates uniforms from the per-frame ring buffer:

```cpp
// Example: Tonemapping pass
const size_t uboAlignment = renderer.getMinUniformOffsetAlignment();
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(tonemapping_data), uboAlignment);
std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

// Push descriptor
vk::DescriptorBufferInfo genericInfo{};
genericInfo.buffer = frame.uniformBuffer().buffer();
genericInfo.offset = uniformAlloc.offset;
genericInfo.range = sizeof(tonemapping_data);
```

### Uniform Structs by Pass

| Pass | Uniform Struct | Binding | Size |
|------|---------------|---------|------|
| Bloom Bright Pass | None | - | - |
| Bloom Blur | `generic_data::blur_data` | 1 | 16 bytes |
| Bloom Composite | `generic_data::bloom_composition_data` | 1 | 16 bytes |
| Tonemapping | `generic_data::tonemapping_data` | 1 | 48 bytes |
| SMAA Edge | `generic_data::smaa_data` | 1 | 16 bytes |
| SMAA Blend | `generic_data::smaa_data` | 1 | 16 bytes |
| SMAA Neighborhood | `generic_data::smaa_data` | 1 | 16 bytes |
| FXAA Prepass | None | - | - |
| FXAA | `generic_data::fxaa_data` | 1 | 16 bytes |
| Lightshafts | `generic_data::lightshaft_data` | 1 | 32 bytes |
| Post Effects | `generic_data::post_data` | 1 | 80 bytes |
| Copy | None | - | - |

**Note**: All uniform structs use `genericData` binding (binding 1) with push descriptors. Sizes reflect std140 layout alignment requirements.

---

## 6. Target Transitions

### Transition Pattern

Each pass follows this pattern:

1. **Suspend Rendering**: End any active render pass
2. **Transition Source**: Transition source image to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
3. **Request Target**: Request new render target (automatically transitions to `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`)
4. **Begin Rendering**: Start new render pass
5. **Draw**: Execute fullscreen quad draw
6. **Suspend Rendering**: End render pass
7. **Transition Output**: Transition output to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (if used as input to next pass)

### Example: Bloom Blur Transition

```cpp
// End previous pass
m_renderingSession->suspendRendering();

// Transition source to shader-read
m_renderingSession->transitionBloomToShaderRead(cmd, 0);

// Request destination target (automatically transitions to attachment layout)
m_renderingSession->requestBloomMipTarget(1, mip);

// Begin rendering
auto render = ensureRenderingStarted(ctx);

// Draw blur pass
recordBloomBlurPass(render, frame, ...);

// End pass
m_renderingSession->suspendRendering();
```

### Critical Transitions

| Transition | From Layout | To Layout | When |
|------------|-------------|-----------|------|
| Scene HDR → Shader Read | `COLOR_ATTACHMENT` | `SHADER_READ_ONLY` | After scene rendering, before bloom/tonemapping |
| Bloom → Shader Read | `COLOR_ATTACHMENT` | `SHADER_READ_ONLY` | After blur pass, before next blur/composite |
| Post LDR → Shader Read | `COLOR_ATTACHMENT` | `SHADER_READ_ONLY` | After tonemapping, before SMAA/FXAA |
| SMAA Output → Shader Read | `COLOR_ATTACHMENT` | `SHADER_READ_ONLY` | After SMAA, before final resolve |

---

## 7. Conditional Passes

### Pass Enablement Logic

| Pass | Enable Condition |
|------|------------------|
| **Bloom** | `enablePostProcessing && gr_bloom_intensity() > 0` |
| **Tonemapping** | Always (uses Linear tonemapper if HDR disabled) |
| **SMAA** | `enablePostProcessing && gr_is_smaa_mode(Gr_aa_mode)` |
| **FXAA** | `enablePostProcessing && gr_is_fxaa_mode(Gr_aa_mode)` |
| **Lightshafts** | `enablePostProcessing && !Game_subspace_effect && gr_sunglare_enabled() && gr_lightshafts_enabled() && Sun_spot > 0.0f` |
| **Post Effects** | `enablePostProcessing && doPostEffects` (any effect has `always_on` OR non-default intensity) |
| **Copy** | Fallback when Post Effects is disabled |

**Note**: SMAA and FXAA are mutually exclusive; only one AA mode is active at a time.

### Scissor Preservation

The post-processing chain preserves scissor state across passes:

```cpp
// Save scissor before post-processing
auto clip = getClipScissorFromScreen(gr_screen);
clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
vk::Rect2D restoreScissor{...};

// ... post-processing passes ...

// Restore scissor after post-processing
cmd.setScissor(0, 1, &restoreScissor);
```

This ensures UI and other draw paths that rely on scissor continue to work correctly.

---

## Appendix: Debugging Tips

### RenderDoc Integration

Use RenderDoc to capture frames and inspect each post-processing pass. Look for:
- Correct input/output textures at each stage
- Proper image layout transitions
- Uniform buffer contents

### Common Issues

**Issue**: Bloom appears too bright or too dim
- **Check**: `gr_bloom_intensity()` value (0-100 scale, divided by 100 in shader)
- **Check**: Bloom composite uniform data (`bloom_composition_data.bloom_intensity`)

**Issue**: SMAA not working
- **Check**: SMAA area/search textures are loaded (`m_smaaAreaTex.view`, `m_smaaSearchTex.view`)
- **Check**: `gr_is_smaa_mode(Gr_aa_mode)` returns true
- **Check**: `smaaEdges` and `smaaBlend` targets are properly transitioned

**Issue**: Tonemapping produces incorrect colors
- **Check**: HDR enable state (`m_sceneTexture->hdrEnabled`)
- **Check**: Tonemapper type in uniform data (`tonemapping_data.tonemapper`)
- **Check**: Exposure value (`tonemapping_data.exposure`)

**Issue**: Lightshafts not appearing
- **Check**: All enable conditions: `gr_sunglare_enabled()`, `gr_lightshafts_enabled()`, `!Game_subspace_effect`, `Sun_spot > 0.0f`
- **Check**: Depth buffers are transitioned to shader-read before the pass
- **Check**: Sun position calculation (`sun_pos` should be in 0-1 screen space range)

**Issue**: Post effects not applied
- **Check**: `doPostEffects` flag (requires at least one effect with `always_on` or non-default intensity)
- **Check**: `Post_processing_manager` is not null
- **Check**: Effect uniform values in `post_data`

**Issue**: Target transition validation errors
- **Check**: All `suspendRendering()` calls before transitions
- **Check**: Target requests happen before `ensureRenderingStarted()`
- **Check**: Image layouts match expected state (use validation layers)

---

## References

- `code/graphics/vulkan/VulkanRenderer.cpp:700-1041` - `endSceneTexture()` post-processing implementation
- `code/graphics/vulkan/VulkanRenderer.cpp:1348-2665` - Individual pass recording functions
- `code/graphics/vulkan/VulkanRenderTargets.h` - Render target definitions and `kBloomMipLevels`
- `code/graphics/vulkan/VulkanRenderTargets.cpp:418-510` - Post-process target creation
- `code/graphics/util/uniform_structs.h:210-420` - Uniform struct definitions (`generic_data` namespace)
- `code/graphics/post_processing.h` - `gr_bloom_intensity()`, `gr_lightshafts_enabled()`, `gr_sunglare_enabled()`
- `code/graphics/2d.h:69-70` - `gr_is_fxaa_mode()`, `gr_is_smaa_mode()`

**Shader Files** (in `code/graphics/shaders/`):
- `brightpass.frag` - Bloom bright pass
- `blur_v.frag`, `blur_h.frag` - Bloom blur passes (vertical/horizontal)
- `bloom_comp.frag` - Bloom composite
- `tonemapping.frag` - HDR to LDR tonemapping
- `smaa_edge.frag`, `smaa_blend.frag`, `smaa_neighborhood.frag` - SMAA passes
- `fxaa_prepass.frag`, `fxaa.frag` - FXAA passes
- `lightshafts.frag` - Lightshafts effect
- `post_effects.frag` - Final post effects (saturation, contrast, etc.)
- `copy.frag` - Simple texture copy

