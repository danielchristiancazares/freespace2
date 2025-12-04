# Vulkan Vulkan Backend Context (FSO)

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

## Crash fix: `gf_calculate_irrmap` (null std::function)

- **Problem**: `std::bad_function_call` in `stars_setup_environment_mapping` when calling `gr_screen.gf_calculate_irrmap()`.
- **Root cause**:
  - `gf_calculate_irrmap` is declared in `code/graphics/2d.h` and used in `code/starfield/starfield.cpp`.
  - OpenGL assigns it in `code/graphics/opengl/gropengldraw.cpp` (`gr_opengl_calculate_irrmap`).
  - Vulkan never assigned it; `std::function` remained empty → null-call crash.
- **Fix**:
  - Implemented `gr_vulkan_calculate_irrmap()` in `code/graphics/vulkan/gr_vulkan.cpp`.
  - Declared in `code/graphics/vulkan/gr_vulkan.h`.
  - Wired in `code/graphics/vulkan/vulkan_stubs.cpp`:
    - `gr_screen.gf_calculate_irrmap = gr_vulkan_calculate_irrmap;`
  - Implementation:
    - Uses `VulkanTextureManager` to fetch envmap (`ENVMAP`) and irrmap render target.
    - For each cubemap face:
      - `bm_set_render_target(gr_screen.irrmap_render_target, face)`
      - `VulkanRenderer::beginAuxiliaryRenderPass(...)` with that face’s `VulkanFramebuffer`
      - Bind `SDR_TYPE_IRRADIANCE_MAP_GEN` pipeline via `VulkanPipelineManager`
      - Bind envmap texture + sampler in a descriptor set
      - Draw fullscreen triangle (no vertex buffer, `gl_VertexIndex`-style)
      - End auxiliary render pass
- **Status**: Null `gf_calculate_irrmap` crash is **fixed**. Missions can still crash from other causes (not assumed solved here).

---

## Crash fix: ImGui SDL backend shutdown when using Vulkan

- **Problem**: Exiting the game with the Vulkan renderer enabled showed a Microsoft Visual C++ Runtime assertion dialog:
  - `Assertion failed! ... imgui_impl_sdl.cpp Line: 423`
  - Expression: `bd != nullptr && "No platform backend to shutdown, or already shutdown?"`
- **Root cause**:
  - `SDLGraphicsOperations::~SDLGraphicsOperations` always called `ImGui_ImplSDL2_Shutdown()` and `ImGui_ImplOpenGL3_Shutdown()`.
  - The ImGui SDL/OpenGL backends are only initialized in the OpenGL path (`createOpenGLContext` via `ImGui_ImplSDL2_InitForOpenGL` / `ImGui_ImplOpenGL3_Init`).
  - When running with `-vulkan`, those backends are never initialized, so `ImGui_ImplSDL2_GetBackendData()` returned `nullptr` and the shutdown assert fired.
- **Fix**:
  - In `freespace2/SDLGraphicsOperations.cpp`, guard the ImGui shutdown calls so they only run for the OpenGL renderer:
    - Keep `SDL_QuitSubSystem(SDL_INIT_VIDEO);` unconditional.
    - Under `#if SDL_VERSION_ATLEAST(2, 0, 6)`, only call `ImGui_ImplSDL2_Shutdown()` and `ImGui_ImplOpenGL3_Shutdown()` when `!Cmdline_vulkan`.
- **Status**: Vulkan builds now exit cleanly without hitting the ImGui SDL backend assertion. If a dedicated Vulkan ImGui backend is added later, revisit this section to ensure its init/shutdown are balanced.

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

- **Run Vulkan build (Debug, windowed)**:

```cmd
build\bin\Debug\fs2_26_0_0.exe -vulkan -window
```

- **Enable validation layers**:

```cmd
set VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
set VK_LOADER_DEBUG=all
build\bin\Debug\fs2_26_0_0.exe -vulkan -noshadercache
```

- **Crash dump analysis (CDB)**:

```cmd
cdbx64.exe -z "<dump>.mdmp" -c ".symfix; .sympath+ build\\bin\\Debug; .reload; !analyze -v; .ecxr; kv; q"
```

> Tip: `cdbx64.exe` ships with the Windows SDK under `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`. Add that directory to `PATH` or invoke the tool via its full path when running the command above.

- **Unit tests**:

```cmd
cd build
ctest -C Debug --output-on-failure -VV
```

- **Logs**:
  - Main log: `%APPDATA%\HardLightProductions\FreeSpaceOpen\data\fs2_open.log`
  - Validation log: `vulkan_debug.log` (game directory, written by `debugReportCallback`)
  - HDR surface debug: `vulkan_hdr_debug.txt` (game directory)
  - Helper scripts: `scripts/vulkan_debug_session.sh` (bash) and `scripts/vulkan_debug_session.ps1` (PowerShell) run the Vulkan exe and snapshot these logs into `out/vulkan_debug/<timestamp>/` for easier sharing with LLM tooling.

---

## Bug fix: Off-center reticle (Vulkan)

- **Problem**: Targeting reticle started centered for ~0.5 seconds, then moved to top-left corner. Other HUD elements remained correctly positioned.

### Root cause

Mid-frame render-to-texture operations (cockpit displays, radar) called `gr_set_viewport()` with small dimensions (e.g., `131x112`), which set `gr_screen.clip_width/clip_height` to those values. When `g3_start_frame()` was later called, it picked up these small values and set `Canvas_width/Canvas_height` accordingly. The `HUD_get_nose_coordinates()` function used `g3_project_vertex()`, which projected using the tiny canvas dimensions, producing coordinates like `(65, 56)` instead of the correct `(1920, 1080)`.

**Call chain that caused the bug**:
1. `gr_set_viewport(0, 2048, 131, 112)` – sets small clip region for render target
2. `g3_start_frame()` – copies `gr_screen.clip_width/height` to `Canvas_width/Canvas_height`
3. `HUD_get_nose_coordinates()` → `g3_project_vertex()` – projects using wrong canvas size
4. Reticle renders at wrong position (top-left)

### Fix

Modified `HUD_get_nose_coordinates()` in `code/hud/hud.cpp` to manually project using `gr_screen.max_w/max_h` (always full screen dimensions) instead of calling `g3_project_vertex()` (which uses potentially-corrupted `Canvas_width/Canvas_height`).

```cpp
// Project vertex manually using full screen dimensions instead of g3_project_vertex(),
// which uses Canvas_width/Canvas_height that may be set to a small render target.
float screen_w = static_cast<float>(gr_screen.max_w);
float screen_h = static_cast<float>(gr_screen.max_h);

float w = 1.0f / v0.world.xyz.z;
x_nose = (screen_w + (v0.world.xyz.x * screen_w * w)) * 0.5f;
y_nose = (screen_h - (v0.world.xyz.y * screen_h * w)) * 0.5f;

float x_center = screen_w * 0.5f;
float y_center = screen_h * 0.5f;

*x = fl2i(x_nose - x_center);
*y = fl2i(y_nose - y_center);
```

- **Status**: **FIXED**. The reticle now stays centered correctly.

### Investigation notes (for future reference)

Failed approaches before finding root cause:
1. Viewport tracking (`gr_vulkan_set_viewport`) – viewport values were correct, not the issue
2. DrawState dirty flag logic – did not fix
3. Scene pass timing – did not fix

The key insight was tracing the log to find `gr_set_viewport(0, 2048, 131, 112)` being called mid-frame, which corrupted the projection state used by HUD calculations

---

## Historical validation errors (for pattern matching)

- **Descriptor pool exhaustion** (historical):
  - Error: `vk::Device::allocateDescriptorSets: ErrorOutOfPoolMemory`
  - Likely site: `bindMaterialDescriptors` in `gr_vulkan.cpp` (label `"MaterialTextures"`)
  - Current pool is large (`POOL_SIZE_COMBINED_IMAGE_SAMPLER = 65536`, `POOL_MAX_SETS = 4096`), so this may not repro now.
  - If it does: consider caching/reusing per-material descriptor sets instead of allocating per draw.

- **Framebuffer attachment count mismatch** (pre–dynamic rendering):
  - Error: `vkCreateFramebuffer(): pCreateInfo->attachmentCount 1 does not match attachmentCount of 2`
  - Came from old code that actually created `VkFramebuffer` objects for render targets.
  - With dynamic rendering, check `vkCmdBeginRendering` attachments instead (color/depth views + formats) if a similar error reappears.

---

## Bug fix: sampler2D bound to 2D_ARRAY views (VUID-07752)

**Status**: **FIXED**

### Problem

Validation error `VUID-vkCmdDraw-viewType-07752`: shaders declared `sampler2D` but received `VK_IMAGE_VIEW_TYPE_2D_ARRAY` views, causing undefined behavior and rendering corruption.

### Root Cause

`bindMaterialDescriptors()` used a hardcoded list of bindings (0,1,2,3,9,10) to determine which needed array views. This didn't account for different shader types having different expectations per binding.

### Fix

Added `shaderUsesArrayView(shader_type, binding)` in `code/graphics/vulkan/gr_vulkan.cpp` to determine correct view type based on shader type:

```cpp
static bool shaderUsesArrayView(shader_type shaderType, uint32_t binding)
{
    switch (shaderType) {
        case SDR_TYPE_MODEL:
            return (binding <= 3) || binding == 8 || binding == 9 || binding == 10;
        case SDR_TYPE_EFFECT_PARTICLE:
        case SDR_TYPE_EFFECT_DISTORTION:
        case SDR_TYPE_BATCHED_BITMAP:
        case SDR_TYPE_NANOVG:
        case SDR_TYPE_ROCKET_UI:
        case SDR_TYPE_SHIELD_DECAL:
            return binding == 0;
        case SDR_TYPE_DECAL:
            return binding >= 2 && binding <= 4;
        case SDR_TYPE_VIDEO_PROCESS:
            return binding <= 2;
        case SDR_TYPE_COPY:
        case SDR_TYPE_COPY_WORLD:
            return binding == 0;
        default:
            return false;
    }
}
```

Updated `bindMaterialDescriptors()` to use `mat->get_shader_type()` and call `shaderUsesArrayView()` instead of hardcoded binding list

---

## Bug fix: `samplerCube` bound to non-cube view

**Status**: **FIXED**

### Problem

`gr_vulkan_calculate_irrmap` updated envmap binding 4 with `getImageView()` (2D/array view) even though the shader expects `samplerCube`, triggering viewType validation errors and incorrect sampling.

### Root Cause

1. `VulkanTexture::create()` never set `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` flag
2. `createRenderTarget()` computed cube flags but didn't pass them to `create()`
3. No cube view (`VK_IMAGE_VIEW_TYPE_CUBE`) was ever created
4. Irradiance generation bound a 2D/2D_ARRAY view to a samplerCube descriptor

### Fix

1. Added `vk::ImageCreateFlags flags` parameter to `VulkanTexture::create()`
2. Set `imageInfo.flags = flags` when creating the image
3. Create `m_cubeImageView` with `VK_IMAGE_VIEW_TYPE_CUBE` when `cubeCompatible && arrayLayers >= 6`
4. Added `getCubeImageView()` accessor
5. Updated `createRenderTarget()` and `createTexture()` to pass cube flags for 6-layer cubemaps
6. Changed `gr_vulkan_calculate_irrmap()` to use `getCubeImageView()` for envmap binding
7. Added Vulkan layout qualifiers to `irrmap-f.sdr` shader

---

## FAILED ATTEMPT: No geometry rendering (models invisible)

**Status**: UNRESOLVED - approach below did not work

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

2. **Root cause identified**: Irradiance map generation corrupts scene pass state
   - `beginAuxiliaryRenderPass` is called while a scene/direct pass is already active
   - It logs error and returns early WITHOUT doing anything
   - But `gr_vulkan_calculate_irrmap` continues to call `submitAuxiliaryCommandBuffer()`
   - This frees the scene command buffer but leaves `m_scenePassActive = true`
   - Later, `beginScenePass` sees `m_scenePassActive = true` and returns early
   - `ensureRenderPassActive` detects inconsistency and starts a **direct pass** (no depth)
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

### Failed Fix Attempt

1. Changed `beginAuxiliaryRenderPass` to return `bool` indicating success/failure
2. Modified `gr_vulkan_calculate_irrmap` to check return value and abort early if auxiliary pass fails
3. Changed `render_model` to call `beginScenePass()` directly instead of `ensureRenderPassActive()`

**Result**: Did not fix the issue. The underlying problem is more complex - the scene/direct pass state management is fundamentally broken when irradiance map generation is called at the wrong time.

### Potential Next Steps (not yet attempted)

1. **Fix the caller**: Ensure irradiance map generation is called BEFORE any scene/direct pass starts, not during rendering
2. **End active pass before auxiliary**: If a scene/direct pass is active, properly end it before starting auxiliary rendering
3. **Separate command buffers**: Use a completely separate command buffer for auxiliary rendering operations
4. **Investigate why scene pass is active**: Find what starts the scene/direct pass before irradiance map generation

---

## FAILED ATTEMPT #2: Restart scene pass for HUD rendering

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

### Potential next steps

1. **Check if command buffer is still recording**: After `endScenePass()`, is the command buffer still in recording state or was it ended?
2. **Add more logging**: Log the exact state of command buffer, scene framebuffer, and image layouts at each step
3. **Compare with OpenGL flow**: Understand how OpenGL handles the scene texture → swapchain blit
4. **Consider different architecture**: Maybe the scene pass and HUD should use separate command buffers that are properly sequenced
