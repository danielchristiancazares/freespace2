# Invisible Ships Bug - Verified Facts

## Observed Behavior

1. Ship 3D models do not appear in the main 3D scene
2. Ship thrusters/glow effects ARE visible
3. Skybox renders correctly
4. HUD renders correctly
5. Weapons (lasers, blasters) render correctly
6. Targeting reticle (bottom-left HUD element) shows ship textures

## Debug Shader Test Results

**Test:** Added `outColor = vec4(1.0, 0.0, 1.0, 1.0);` (magenta) to `model.frag`

**Result:** Magenta appeared ONLY in the targeting box, nowhere else in the scene

**Conclusion from test:**
- `model.frag` executes
- G-buffer receives writes
- Output is spatially constrained to a small region

## Code Facts

### VulkanRenderingSession.cpp

`applyDynamicState()` (lines 339-387):
- Sets viewport using `m_device.swapchainExtent()`
- Does NOT set scissor

**Confirmed by code review:** "Scissor state: `applyDynamicState()` sets viewport but not scissor. Your deferred lighting path should explicitly set scissor (otherwise you inherit whatever last draw used)."

### VulkanGraphics.cpp

`createFullScreenViewport()` (lines 39-48):
- Uses `gr_screen.max_w` and `gr_screen.max_h`

`createClipScissor()` (lines 51-58):
- Uses `gr_screen.clip_width` and `gr_screen.clip_height` via `getClipScissorFromScreen()`

### VulkanShaderManager.cpp

`loadModule()` (lines 80-118):
- Tries embedded resources first via `defaults_get_all()`
- Falls back to filesystem path if not found in embedded

### shaders.cmake

- Shaders compile to `code/graphics/shaders/compiled/`
- Shaders are embedded into executable via `target_embed_files()`

## G-Buffer Configuration

- `kGBufferCount = 3` (VulkanRenderTargets.h)
- `model.frag` declares 5 outputs: outColor, outNormal, outPosition, outSpecular, outEmissive

## Render Target Info

G-buffer pass returns:
- `colorAttachmentCount = 3` (kGBufferCount)
- `colorFormat = gbufferFormat()`
- `depthFormat = depthFormat()`

## Uniform Buffer Setup

`convert_model_material()` in uniforms.cpp populates model_uniform_data:
- `modelViewMatrix` = `gr_view_matrix * scaled_model_matrix`
- `projMatrix` = `gr_projection_matrix`
- These are global variables set by engine before rendering


## What Has NOT Been Verified

- Actual runtime values of `gr_screen.clip_*` during model rendering
- Actual runtime values of scissor rect during model rendering
- Render order of targeting reticle vs main 3D scene
- Whether scissor state persists between render passes
- Whether targeting reticle uses same render path as main 3D scene
- Whether targeting reticle uses same uniform buffer setup as main 3D scene
- Actual values of `gr_view_matrix` and `gr_projection_matrix` during main scene vs targeting reticle
