# Vulkan Backend Context (FSO)

> This file exists **only** to feed context to LLM-based tooling.

---

## Architecture

- **API version**: Vulkan 1.4 (`VK_API_VERSION_1_4` in `VulkanRenderer.cpp`)
- **Dynamic rendering**:
  - Uses `vkCmdBeginRendering` + `VkRenderingAttachmentInfo`
  - `VkFramebuffer` objects are *not* created (`VulkanFramebuffer::getFramebuffer()` returns `nullptr`)
  - Render passes are still created (`createRenderTargetRenderPass`) but graphics pipelines use `renderPass = nullptr` and specify formats via `pNext`
- **Key managers**:
  - `VulkanRenderer` – main renderer, swapchain, dynamic rendering, owns other managers
  - `VulkanTextureManager` – textures, render targets (2D + cubemap), sampler cache
  - `VulkanDescriptorManager` – single descriptor pool, allocation/free, tracking
  - `VulkanPipelineManager` – pipeline creation/caching, descriptor set layouts
  - `VulkanBufferManager` – buffer allocation (vertex/index/uniform/staging)

---

## Bug Fixes

### Off-center reticle
See [`docs/vulkan-off-center-reticle-fix.md`](../docs/vulkan-off-center-reticle-fix.md) for details.

### ImGui SDL backend shutdown when using Vulkan
See [`docs/vulkan-imgui-shutdown-fix.md`](../docs/vulkan-imgui-shutdown-fix.md) for details.

### Null std::function `gf_calculate_irrmap`
See [`docs/vulkan-gf-calculate-irrmap-fix.md`](../docs/vulkan-gf-calculate-irrmap-fix.md) for details.
**Note**: This fix resolved the null crash, but there is a known timing issue that can corrupt render state. See the external doc and FAILED ATTEMPT sections below for details.

### sampler2D bound to 2D_ARRAY views (VUID-07752)
See [`docs/vulkan-sampler2d-2darray-fix.md`](../docs/vulkan-sampler2d-2darray-fix.md) for details.

### `samplerCube` bound to non-cube view
See [`docs/vulkan-samplercube-noncube-fix.md`](../docs/vulkan-samplercube-noncube-fix.md) for details.


---

## Mistakes to avoid (DO NOT REPEAT)

### Sampler type mismatch: `sampler2DArray` in default material

- **Problem**: Changing `default-material.frag` to use `sampler2DArray` made all menus/UI invisible (black).
- **Why**:
  - Menu textures are 2D with `VK_IMAGE_VIEW_TYPE_2D` views.
  - `sampler2DArray` expects `VK_IMAGE_VIEW_TYPE_2D_ARRAY`.
  - Binding 2D views to a `sampler2DArray` uniform silently fails (no validation error, nothing rendered).
- **DO NOT**:
  - Change `code/def_files/data/effects/default-material.frag` to `sampler2DArray`.
  - Add array-view accessors (e.g. `getImageViewArray()`) to `VulkanTexture` for default materials.
  - Bind 2D-array views to descriptor slots intended for `sampler2D`.
- **DO**:
  - Keep default material shaders using `sampler2D`:
    - `default-material.frag`: `layout(set = 1, binding = 0) uniform sampler2D baseMap;`
    - `default-material.vert`: `fragColor = color;` (uniform only, no `vertColor` input).
  - In `gr_vulkan.cpp` material binding:
    - Use `texture->getImageView()` (2D views) for default materials.
  - Treat NanoVG separately:
    - NanoVG shaders (`nanovg-f.sdr`, `nanovg-v.sdr`) legitimately use `sampler2DArray` and need array textures.

---

## File map (for symbol lookup)

- **Core Vulkan renderer**:
  - `code/graphics/vulkan/VulkanRenderer.cpp/.h`
  - `code/graphics/vulkan/RenderFrame.cpp/.h`
  - `code/graphics/vulkan/gr_vulkan.cpp/.h`
  - `code/graphics/vulkan/vulkan_stubs.cpp`
- **Resources & pipelines**:
  - `code/graphics/vulkan/VulkanTexture.cpp/.h`
  - `code/graphics/vulkan/VulkanDescriptorManager.cpp/.h`
  - `code/graphics/vulkan/VulkanPipelineManager.cpp/.h`
  - `code/graphics/vulkan/VulkanBuffer.cpp/.h`
  - `code/graphics/vulkan/VulkanFramebuffer.cpp/.h`
  - `code/graphics/vulkan/VulkanShader.cpp/.h`
  - `code/graphics/vulkan/VulkanPostProcessing.cpp/.h`
- **Irradiance/envmap path**:
  - `code/starfield/starfield.cpp` – `irradiance_map_gen()`, `stars_setup_environment_mapping()`
  - `code/graphics/2d.h` – `gf_calculate_irrmap` declaration
  - `code/graphics/opengl/gropengldraw.cpp` – OpenGL reference (`gr_opengl_calculate_irrmap`)
  - `code/graphics/vulkan/gr_vulkan.cpp/.h` – `gr_vulkan_calculate_irrmap` implementation
  - `code/graphics/vulkan/VulkanShader.cpp` – `SDR_TYPE_IRRADIANCE_MAP_GEN` mapping
- **Default material / UI shaders**:
  - `code/def_files/data/effects/default-material.frag`
  - `code/def_files/data/effects/default-material.vert`
  - `code/def_files/data/effects/nanovg-f.sdr`
  - `code/def_files/data/effects/nanovg-v.sdr`

---

## Testing & debugging

### Running a Build (Must be done inside game folder)
```cmd
build\bin\Debug\fs2_26_0_0.exe -vulkan -window
```

### Enable validation layers
```cmd
set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
set VK_LOADER_DEBUG=all
build\bin\Debug\fs2_26_0_0.exe -vulkan -noshadercache
```

### Crash dump analysis (CDB)
```cmd
cdbx64.exe -z "<dump>.mdmp" -c ".symfix; .sympath+ build\\bin\\Debug; .reload; !analyze -v; .ecxr; kv; q"
```
- Tip: `cdbx64.exe` ships with the Windows SDK under `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`. Add that directory to `PATH` or invoke the tool via its full path when running the command above.

### Unit Tests

```cmd
cd build
ctest -C Debug --output-on-failure -VV
```

### Logfile Locations
- Main log: `%APPDATA%\HardLightProductions\FreeSpaceOpen\data\fs2_open.log`
- Validation log: `vulkan_debug.log` (game directory, written by `debugReportCallback`) Note: Logs here should also be in the main `fs2_open.log` file 
- HDR surface debug: `vulkan_hdr_debug.txt` (game directory)
- The application does not stdout or stderr anything directly.

---

## Historical validation errors (for pattern matching)

### Descriptor pool exhaustion
- Error: `vk::Device::allocateDescriptorSets: ErrorOutOfPoolMemory`
- Likely site: `bindMaterialDescriptors` in `gr_vulkan.cpp` (label `"MaterialTextures"`)
- Current pool is large (`POOL_SIZE_COMBINED_IMAGE_SAMPLER = 65536`, `POOL_MAX_SETS = 4096`), so this may not repro now.
- If it does: consider caching/reusing per-material descriptor sets instead of allocating per draw.
### Framebuffer attachment count mismatch (pre–dynamic rendering):
- Error: `vkCreateFramebuffer(): pCreateInfo->attachmentCount 1 does not match attachmentCount of 2`
- Came from old code that actually created `VkFramebuffer` objects for render targets.
- With dynamic rendering, check `vkCmdBeginRendering` attachments instead (color/depth views + formats) if a similar error reappears.

---

## FAILED ATTEMPT #7: Track recorded scene pass and reuse command buffer for HUD

**Status**: FAILED (models still invisible)

### Approach

- Added `m_scenePassRecorded` flag set by `endScenePass()`.
- `ensureRenderPassActive()` now detects a recorded scene, blits it to the swapchain without transitioning to present, then starts a direct pass with `loadOp = eLoad` so HUD draws over the blitted scene while reusing the same command buffer.
- `recordBlitToSwapchain()` can skip the final present transition so the swapchain stays in color-attachment layout for the HUD direct pass.
- `flip()` now presents whenever a scene pass was recorded (even if ended earlier) so the scene buffer is submitted.

### Result

- Reported as failed; scene/model visibility is still broken. Need log inspection to confirm state/layout correctness.

### Notes / Suspicions

- The blit + reload path may still be dropping the scene command buffer, or layouts may be invalid when the HUD direct pass begins.
- More logging around `ensureRenderPassActive()`, `flip()`, and image layouts is needed to pinpoint where the scene content is lost.

---

## Instrumentation (ongoing)

- Added verbose vk_debugf/mprintf tracing for:
  - `gr_vulkan_calculate_irrmap` entry, framebuffer/images per cubemap face, aux pass begin/end, and submit/skip decisions.
  - `VulkanRenderer::ensureRenderPassActive` (includes `m_scenePassRecorded` and formats), HUD blit/direct path, and command buffer reuse.
  - `VulkanRenderer::flip` (records `m_scenePassRecorded` state) and `submitAuxiliaryCommandBuffer` (state before/after).
  - `beginAuxiliaryRenderPass` logs framebuffer image handles and rejection reasons when scene/direct passes are active.
- Use these logs alongside validation output to pinpoint where images stay in `VK_IMAGE_LAYOUT_UNDEFINED` or where command buffers are dropped.

---

## CRITICAL BUG: No geometry rendering (models invisible)

**Status**: UNRESOLVED

### Symptoms

- 3D models (ships, etc.) are not rendering - screen shows only UI/menus
- UI elements using `SDR_TYPE_DEFAULT_MATERIAL` (shader type 17) and `SDR_TYPE_NANOVG` (18) render correctly
- Model rendering using `SDR_TYPE_MODEL` (shader type 0) is called but produces no visible output

### Investigation Findings

1. **Log analysis** showed `render_model` calls happening with `depthFmt=0` (no depth buffer):
   ```
   VulkanTrace: render_model entry texi=0
   VulkanTrace: ensureRenderPassActive entry scene=0 direct=1 aux=0
   VulkanTrace: getOrCreatePipeline entry shaderType=0 colorFmt=64 depthFmt=0
   ```

2. **Possible Causes - Unconfirmed (KEEP UP TO DATE)**: Irradiance map generation corrupts scene pass state
   - `beginAuxiliaryRenderPass` is called while a scene/direct pass is already active
     - ✅ **Addressed**: Function returns early (line 1775-1779 in VulkanRenderer.cpp)
   - But `gr_vulkan_calculate_irrmap` continues to call `submitAuxiliaryCommandBuffer()`
     - ⚠️ **Partially addressed**: ATTEMPT #1 tried to check return value and abort, but didn't fix the issue
   - This frees the scene command buffer but leaves `m_scenePassActive = true`
     - ❌ **NOT addressed**: `submitAuxiliaryCommandBuffer()` (line 2253-2254) frees the buffer but doesn't reset `m_scenePassActive`
   - Later, `beginScenePass` sees `m_scenePassActive = true` and returns early
     - ❌ **NOT addressed**: No fix to prevent stale `m_scenePassActive` state
   - `ensureRenderPassActive` detects inconsistency and starts a **direct pass** (no depth)
     - ⚠️ **Symptom addressed**: ATTEMPT #2-4 tried to fix the direct pass issue, but not the root cause
   - Models render to direct pass without depth buffer → invisible/broken

3. **Log evidence**:
   ```
   Vulkan: Generating irradiance map from envmap
   VulkanRenderer: beginAuxiliaryRenderPass called while scene/direct pass active - this is not supported (frame=0)
   ...
   Vulkan: Irradiance map generation complete
   VulkanRenderer: renderState[beginScenePass entry]: scene=1 direct=0 cmd=0000000000000000
   VulkanRenderer: beginScenePass called while scene pass already active (frame=0)
   VulkanRenderer: ensureRenderPassActive: starting direct pass frame=0 swapImage=2
   ```

### PROPOSED FIX (pending validation)

- Root cause appears to be the scene command buffer getting discarded when HUD/UI triggers a direct pass after `gr_scene_texture_end()`. The direct pass allocates a new command buffer and never blits the recorded scene.
- New state flag `m_scenePassRecorded` tracks when `endScenePass()` finishes recording. When HUD calls `ensureRenderPassActive()`, the renderer now:
  - Reuses the existing scene command buffer, blits the scene texture to the swapchain without transitioning to present, and starts a direct pass with `loadOp = eLoad` so HUD draws over the blit.
  - Clears `m_scenePassRecorded` once consumed; direct-pass submissions also clear it.
- `flip()` now presents the scene path whenever either `m_scenePassActive` **or** `m_scenePassRecorded` is set, so scene content is submitted even if the pass ended earlier for HUD rendering.
- Motivation: previously `m_scenePassActive` was cleared by `gr_scene_texture_end()`, HUD drew to a fresh direct-pass command buffer, and `flip()` skipped the scene blit entirely → models never appeared.

### ATTEMPT #1

1. Changed `beginAuxiliaryRenderPass` to return `bool` indicating success/failure
2. Modified `gr_vulkan_calculate_irrmap` to check return value and abort early if auxiliary pass fails
3. Changed `render_model` to call `beginScenePass()` directly instead of `ensureRenderPassActive()`

**Result**: Did not fix the issue. The underlying problem is more complex - the scene/direct pass state management is fundamentally broken when irradiance map generation is called at the wrong time.

---

## ATTEMPT #2: Restart scene pass for HUD rendering

**Status**: FAILED - made the problem worse

### Symptoms (before this attempt)

- Skybox shows briefly (~0.5 seconds) then disappears
- HUD renders correctly
- 3D models (ships) are black/invisible

### Analysis

Identified that the scene texture was never being blitted to swapchain:

1. `gr_scene_texture_begin()` starts scene pass, sets `m_scenePassActive = true`
2. 3D content renders to scene framebuffer
3. `gr_scene_texture_end()` ends scene pass, sets `m_scenePassActive = false`
4. HUD rendering calls `ensureRenderPassActive()`
5. Since `m_scenePassActive` is false, starts a **direct pass** to swapchain
6. `flip()` checks `m_scenePassActive` (false), takes direct pass path
7. Direct pass path does NOT call `recordBlitToSwapchain()`
8. Scene texture with 3D content is never composited

### Fix Attempt

Added `m_scenePassWasUsedThisFrame` flag to track if scene pass was used. In `ensureRenderPassActive()`, if flag was set, call `beginScenePass()` to restart scene pass instead of starting direct pass.

**Changes made:**
1. Added `bool m_scenePassWasUsedThisFrame = false;` to `VulkanRenderer.h`
2. Set flag in `beginScenePass()` after `m_scenePassActive = true`
3. In `ensureRenderPassActive()`, if `m_scenePassWasUsedThisFrame` is true, call `beginScenePass()` and return
4. Reset flag at end of `flip()`

### Why it failed

**Critical bug**: `beginScenePass()` allocates a NEW command buffer every time (line 1521-1522):
```cpp
auto allocatedBuffers = m_device->allocateCommandBuffers(cmdBufferAlloc);
m_sceneCommandBuffer = allocatedBuffers.front();
```

When called the second time (for HUD after scene ended), this **overwrites** `m_sceneCommandBuffer`, losing all the 3D rendering commands from the first scene pass. The first command buffer (with skybox, ships, etc.) is abandoned without being submitted.

**Result**: Made problem worse - skybox no longer even appeared briefly. Screen was completely black except for HUD.

### Lesson learned

Cannot simply "restart" the scene pass - the command buffer architecture doesn't support it. Need a different approach that either:
1. Blits scene to swapchain BEFORE starting direct pass for HUD
2. Keeps HUD rendering in the same command buffer as scene (composite in scene framebuffer)
3. Uses separate command buffers that are properly sequenced

---

## FAILED ATTEMPT #3: Blit scene before direct pass in ensureRenderPassActive

**Status**: FAILED - no change from original symptom

### Approach

Modified `ensureRenderPassActive()` to blit scene to swapchain BEFORE starting the direct pass for HUD:

1. Check if `m_scenePassWasUsedThisFrame` is true
2. If so, call `recordBlitToSwapchain()` to blit scene texture to swapchain
3. Transition swapchain from `PRESENT_SRC_KHR` back to `COLOR_ATTACHMENT_OPTIMAL`
4. Start direct pass with `loadOp = eLoad` (preserve blitted content) instead of `eClear`
5. HUD renders on top of blitted scene

**Changes made in `ensureRenderPassActive()`:**
```cpp
bool blitSceneFirst = m_scenePassWasUsedThisFrame && m_sceneFramebuffer && m_sceneFramebuffer->getColorImageView(0);
if (blitSceneFirst) {
    recordBlitToSwapchain(m_sceneCommandBuffer);
}
// ... barrier with oldLayout based on blitSceneFirst ...
colorAttachment.loadOp = blitSceneFirst ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear;
```

### Why it failed

Back to original symptom: skybox appears for ~0.5 seconds then disappears. The blit approach did not work - likely because:

1. The scene framebuffer content may be empty/invalid when `ensureRenderPassActive()` is called
2. The scene pass may have already ended and the scene texture transitioned to wrong layout
3. Or there's something else clearing/overwriting the content

### Key observation

The skybox appearing briefly then disappearing suggests the FIRST frame renders correctly but subsequent frames do not. This points to a per-frame state issue rather than a fundamental rendering architecture problem.

### Potential next steps

1. **Investigate first frame vs subsequent frames**: Why does first frame work but later frames don't?
2. **Check scene framebuffer state**: Is scene framebuffer content valid when blit is attempted?
3. **Check image layout transitions**: Are layouts correct throughout the frame?
4. **Consider NOT ending scene pass**: Keep scene pass active through HUD rendering instead of ending it early

---

## FAILED ATTEMPT #4: Reuse scene pass command buffer in ensureRenderPassActive

**Status**: FAILED - did not fix the issue

### Analysis

Identified that `ensureRenderPassActive()` was always allocating a NEW command buffer when no pass was active, even when the scene pass command buffer still existed with all the 3D rendering recorded:

```cpp
// After endScenePass():
// - m_scenePassActive = false
// - m_sceneCommandBuffer = valid (contains 3D rendering)
// 
// ensureRenderPassActive() checks:
if ((m_scenePassActive || m_directPassActive) && m_sceneCommandBuffer) {
    return;  // early return only if pass is ACTIVE
}
// Falls through to allocate NEW command buffer, overwriting the existing one!
```

### Fix Attempt

Modified `ensureRenderPassActive()` to check if we already have a command buffer from scene pass:

```cpp
bool reuseExistingCmdBuffer = m_scenePassWasUsedThisFrame && m_sceneCommandBuffer;

if (!reuseExistingCmdBuffer) {
    // Allocate NEW command buffer (menu-only path)
    auto allocatedBuffers = m_device->allocateCommandBuffers(cmdBufferAlloc);
    m_sceneCommandBuffer = allocatedBuffers.front();
    m_sceneCommandBuffer.begin(beginInfo);
} else {
    // REUSE existing command buffer from scene pass
    // Command buffer is already recording, just continue using it
}
```

### Why it failed

Unknown - the fix seemed logically correct but did not resolve the rendering issue. Possible causes:
1. The command buffer state may be invalid after `endScenePass()` (already ended?)
2. The scene framebuffer content may be getting cleared/overwritten elsewhere
3. There may be synchronization issues between the scene pass and the blit
4. The scene texture layout transition may be incorrect

### Lesson learned

The command buffer lifecycle is more complex than initially understood. Simply reusing the command buffer pointer is not sufficient - need to understand the full state of the command buffer after `endScenePass()`.

---

## FAILED ATTEMPT #5: Skip irradiance generation when any pass is active

**Status**: FAILED - regression (skybox never appears)

### Change

- Added a guard in `gr_vulkan_calculate_irrmap` to bail out if scene/direct/aux passes are active and to restore the previous render target.
- Tracked auxiliary work with `m_auxiliaryCommandsRecorded` so `submitAuxiliaryCommandBuffer` skips when no aux work is recorded or when a scene/direct pass is active.

### Result

- The regression worsened: skybox/scene never appears (black immediately), not even for the first frame. The underlying state issue remains unresolved.

### Note on contradictory guidance

This attempt contradicts the suggestion in FAILED ATTEMPT #1 to "call irradiance generation BEFORE any scene/direct pass starts". The failure here suggests that simply skipping irradiance generation when passes are active doesn't solve the problem - the issue may be that irradiance generation is being called at the wrong time in the frame lifecycle, or there's a deeper architectural problem with how auxiliary passes interact with scene/direct passes.

---

## FAILED ATTEMPT #6: Reset pass state flags in submitAuxiliaryCommandBuffer

**Status**: FAILED - regression (skybox never appears)

### Approach

Attempted to fix the stale state issue by resetting all pass state flags in `submitAuxiliaryCommandBuffer()`:

1. Changed `beginAuxiliaryRenderPass()` to return `bool` indicating success/failure
2. Modified `gr_vulkan_calculate_irrmap()` to track whether any auxiliary passes were started
3. Only call `submitAuxiliaryCommandBuffer()` if auxiliary work was actually recorded
4. Reset `m_scenePassActive`, `m_directPassActive`, `m_auxiliaryPassActive`, and format/view tracking in `submitAuxiliaryCommandBuffer()`

**Changes made:**
- `beginAuxiliaryRenderPass()` now returns `bool` (false if pass couldn't start)
- `gr_vulkan_calculate_irrmap()` tracks `anyAuxiliaryPassesStarted` flag
- `submitAuxiliaryCommandBuffer()` resets all pass state flags when freeing command buffer

### Why it failed

**Critical bug**: `submitAuxiliaryCommandBuffer()` should NOT reset `m_scenePassActive` or `m_directPassActive`. These flags are managed by `endScenePass()` and the direct pass end logic. Resetting them in `submitAuxiliaryCommandBuffer()` corrupts legitimate scene/direct pass state.

**Result**: Skybox never appears - worse than before. The fix was too aggressive and broke legitimate scene pass state management.

### Lesson learned

- `submitAuxiliaryCommandBuffer()` should only reset `m_auxiliaryPassActive` (and related auxiliary state)
- `m_scenePassActive` and `m_directPassActive` must only be reset by their respective end functions
- The root cause is more subtle - need to understand when `submitAuxiliaryCommandBuffer()` is called relative to scene pass lifecycle

### Potential next steps (KEEP UP TO DATE)

1. **Revert the state flag resets**: Only reset `m_auxiliaryPassActive` in `submitAuxiliaryCommandBuffer()`, not scene/direct pass flags
2. **Investigate when submitAuxiliaryCommandBuffer is called**: Is it being called at the wrong time in the frame lifecycle?
3. **Check if scene pass state is already corrupted before submitAuxiliaryCommandBuffer**: Maybe the issue is earlier in the flow
4. **Add more logging**: Log the exact state of command buffer, scene framebuffer, and image layouts at each step
5. **Compare with OpenGL flow**: Understand how OpenGL handles the scene texture → swapchain blit

---

## CURRENT BUG: Everything renders red/wrong colors

**Status**: UNRESOLVED

### Symptoms

- All rendered content has a heavy red tint
- Textures ARE rendering with correct shapes/detail but wrong colors
- Font atlas is red
- HUD elements are red
- Menu backgrounds show red-tinted imagery
- Green clear color test confirmed: clear color works (green background visible), red content is being drawn ON TOP

### Investigation

1. **Green clear color test**: Modified scene pass clear color to green. Result: green background with red content on top. This confirms:
   - Scene pass and clear are working
   - The problem is in what's being DRAWN, not the render pass itself
   - Textures/geometry are rendering but with wrong colors

2. **Blit shader R/B swap test**: Added `outColor = vec4(sceneColor.b, sceneColor.g, sceneColor.r, sceneColor.a)` to swap red and blue channels. Result: No change - still red. This suggests the swap isn't happening in the blit shader OR the problem is upstream.

3. **Texture format analysis**:
   - `VulkanTexture.cpp` comment says: "bmpman stores pixels as BGRA in memory... Use BGRA format so Vulkan interprets the bytes correctly. The blit shader then swaps R/B for the swapchain"
   - But the blit shader does NOT swap - it just passes through `outColor = sceneColor`
   - Texture format selection returns `vk::Format::eB8G8R8A8Unorm` for 32-bit textures

### Hypotheses

1. **Channel mismatch in texture sampling**: Textures are BGRA but being sampled as RGBA somewhere
2. **Scene framebuffer format issue**: Scene FB is R16G16B16A16_SFLOAT (RGBA order) but receiving BGRA data
3. **Shader uniform/UBO mismatch**: Color multiplier or material data is wrong
4. **Descriptor binding issue**: Wrong texture bound to sampler slots

### Failed fixes

1. **Blit shader R/B swap**: No effect - problem is upstream of blit
2. **VulkanFramebuffer external images**: Extended `createFromImageViews` to accept `vk::Image` handles, stored them in `m_externalColorImages`/`m_externalDepthImage`, fixed `getColorImage()` to return external handles when attachments are external, and passed the images at the cubemap render target, swapchain framebuffer, and bloom framebuffer call sites. Result: red tint remained.

### Next steps

1. Check where BGRA->RGBA conversion should happen (at texture upload? at sampling? at blit?)
2. Investigate if model/material shaders need channel swizzling
3. Check if scene framebuffer is receiving correct color data
4. Verify descriptor set bindings are correct

---

## Investigation Session: 2025-12-04

### Current Symptoms
- **Skybox**: Scrambled geometry - triangles scattered randomly, but visible with correct nebula texture
- **Ships**: Completely invisible
- **HUD**: Renders correctly
- Both skybox and ships use `SDR_TYPE_MODEL` (shader type 0)

### Verified/Ruled Out

#### std140 Layout (FIXED)
- `sizeof(model_light)` = 80 bytes (matches std140)
- `sizeof(model_uniform_data)` = 1600 bytes
- Padding was added to fix alignment
- **Not the cause** of current symptoms (still broken after fix)

#### Uniform Buffer Binding Numbers
- `uniform_block_type::ModelData` has enum value `1`
- Descriptor layout puts ModelData at binding index 1
- Shader expects `modelData` at Set 0, Binding 1
- **Match confirmed** - not the issue

#### Uniform Buffer Offset Flow
- `build_uniform_buffer()` calculates offset correctly
- `render_buffer()` binds the correct offset
- `bindUniformDescriptors()` passes offset to `vkCmdBindDescriptorSets`
- Offset calculation verified as correct

#### Memory Visibility
- Uniform buffers use `HOST_VISIBLE | HOST_COHERENT`
- Writes immediately visible to GPU
- No explicit flush needed

#### Descriptor Set Creation
- `m_uniformDescriptorSet` created during initialization
- Initialized with placeholder buffers
- Updates when buffer changes

### NOT Verified

#### Draw Calls Actually Happening
- Need to confirm `vkCmdDrawIndexed` is called for ships
- Add logging before draw call to verify

#### Vertex Buffer Binding
- Offset and stride not verified
- Could explain scrambled skybox geometry

#### Uniform Data Content
- Data written correctly to shadow buffer
- `submitData()` called at correct time
- But actual matrix values not logged

### Failed OpenGL Comparison Attempt

1. **Problem**: Shader changes added `layout(set = ...)` which is Vulkan-only
2. **Fix attempt**: Macro to conditionally emit set qualifier
3. **Result**: GL compiles but renders completely black
4. **Root cause**: Original GL shaders had NO binding qualifiers - GL sets bindings programmatically via `glUniform1i`/`glUniformBlockBinding`
5. **Abandoned**: Can't use GL as reference implementation

### Key Insight: Two Separate Bugs

1. **Scrambled skybox** = geometry visible but wrong vertex positions
   - Matrices at struct start should be correct
   - Suggests uniform buffer offset or vertex buffer issue

2. **Invisible ships** = nothing rendered at all
   - Same shader path as skybox
   - Different failure mode = different cause
   - Could be: alpha=0, clipped, depth fail, draw not issued

### Next Steps

1. Add draw call logging to verify ships issue draws
2. Log matrix values being written to uniform buffer
3. Check vertex buffer binding (offset/stride)

---

## FAILED ATTEMPT #8: Remove texture manager fallback from beginScenePass

**Status**: FAILED - no change (not the root cause)

### Investigation

Log analysis showed some `render_model` calls had `depthFmt=0` (no depth buffer):
```
beginScenePass target framebuffer=... extent=512x512 activeRT=0 textureRT=1 ... colorFmt=37 depthFmt=0
```

Hypothesis: The texture manager fallback in `beginScenePass` was latching stale render targets (e.g., irrmap 512x512 RT with no depth) when `gr_vulkan_calculate_irrmap` ran, causing models to render without depth testing.

### Code Analysis

`beginScenePass()` had two ways to select a render target:
1. `m_activeRenderTarget.isActive && m_activeRenderTarget.framebuffer` - explicit renderer state
2. `m_textureManager->isRenderingToTexture()` fallback - grab RT from texture manager

### Critical Discovery: Dead Code

**`VulkanRenderer::setActiveRenderTarget()` is NEVER CALLED anywhere in the codebase.**

- The function exists (lines 2897-2911 in VulkanRenderer.cpp)
- It's declared in the header
- But no code ever calls it
- Therefore `m_activeRenderTarget.isActive` is **always false**
- The first condition in `beginScenePass` was never true
- Only the texture manager fallback could ever trigger

### Fix Attempt

Removed the texture manager fallback from `beginScenePass()`:
```cpp
// BEFORE:
if (m_activeRenderTarget.isActive && m_activeRenderTarget.framebuffer) {
    // use explicit RT
} else if (m_textureManager && m_textureManager->isRenderingToTexture()) {
    // fallback to texture manager RT
}

// AFTER:
if (m_activeRenderTarget.isActive && m_activeRenderTarget.framebuffer) {
    // use explicit RT (never true - dead code path)
}
// Fallback removed
```

### Result

**Nothing changed.** Skybox still scrambled, ships still invisible.

### Conclusions

1. The texture manager fallback was either:
   - Not being hit during the problematic rendering
   - Being hit but not causing the visual issues

2. The `depthFmt=0` log entries were likely from auxiliary passes that don't affect model rendering

3. **Render target tracking is broken** (dead `setActiveRenderTarget()`) but this is a separate bug, not the cause of scrambled skybox / invisible ships

4. Root cause must be elsewhere: uniforms, vertex data, descriptors, or pipeline state

### Commits

- `560d62366` - Add instrumentation to debug beginScenePass render target hijacking
- `e106729df` - Remove texture manager fallback from beginScenePass

---

## Main Model Shader Interface (Reference)

### main-v.sdr (Vertex Shader)

**Inputs:**
| Location | Name | Type |
|----------|------|------|
| 0 | vertPosition | vec4 |
| 2 | vertTexCoord | vec4 |
| 3 | vertNormal | vec3 |
| 4 | vertTangent | vec4 |
| 5 | vertModelID | float |

**Outputs (VertexOutput block, location 0):**
- `tangentMatrix` (mat3)
- `fogDist` (float) - conditional
- `position` (vec4)
- `normal` (vec3)
- `texCoord` (vec4)
- `shadowUV[4]` (vec4) - conditional
- `shadowPos` (vec4) - conditional

**UBOs:**
| Set | Binding | Name | Size (approx) |
|-----|---------|------|---------------|
| 0 | 1 | modelData | ~1600 bytes |
| 0 | 9 | transform_tex | samplerBuffer (conditional) |

### main-f.sdr (Fragment Shader)

**Inputs:** VertexOutput block from vertex shader

**Outputs:**
| Location | Name | Purpose |
|----------|------|---------|
| 0 | fragOut0 | Base color / final color |
| 1 | fragOut1 | Position + AO (deferred) |
| 2 | fragOut2 | Normal + gloss (deferred) |
| 3 | fragOut3 | Spec color + fresnel (deferred) |
| 4 | fragOut4 | Emissive (deferred) |

**Textures (Set 1):**
| Binding | Name | Type | Conditional |
|---------|------|------|-------------|
| 0 | sBasemap | sampler2DArray | MODEL_SDR_FLAG_DIFFUSE |
| 1 | sGlowmap | sampler2DArray | MODEL_SDR_FLAG_GLOW |
| 2 | sNormalmap | sampler2DArray | MODEL_SDR_FLAG_NORMAL |
| 3 | sSpecmap | sampler2DArray | MODEL_SDR_FLAG_SPEC |
| 8 | shadow_map | sampler2DArray | MODEL_SDR_FLAG_SHADOWS |
| 9 | sAmbientmap | sampler2DArray | MODEL_SDR_FLAG_AMBIENT |
| 10 | sMiscmap | sampler2DArray | MODEL_SDR_FLAG_MISC |
| 11 | sFramebuffer | sampler2D | always |

### modelData UBO members (binding 1)

- Matrices: modelViewMatrix, modelMatrix, viewMatrix, projMatrix, textureMatrix, shadow_mv_matrix, shadow_proj_matrix[4]
- color (vec4)
- lights[8] (model_light struct - 80 bytes each)
- fogStart, fogScale, fogColor
- clip_equation, use_clip_plane
- n_lights, defaultGloss, alphaGloss, gammaSpec, envGloss
- ambientFactor, diffuseFactor, emissionFactor
- Texture indices: sBasemapIndex, sGlowmapIndex, sSpecmapIndex, sNormalmapIndex, sAmbientmapIndex, sMiscmapIndex
- Team colors: base_color, stripe_color, team_glow_enabled
- Shadow distances: veryneardist, neardist, middist, fardist
- Viewport: vpwidth, vpheight, znear, zfar
- effect_num, anim_timer, alphaMult, flags, etc.
