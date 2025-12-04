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

## Active validation error: sampler2D bound to 2D_ARRAY views

**Date**: 2025-12-03  
**Status**: ACTIVE (causing rendering corruption)

### Validation Error

```
vkCmdDraw(): the combined image sampler descriptor [VkDescriptorSet 0x690000000069, Set 1, Binding 0, Index 0] 
VkImageViewType is VK_IMAGE_VIEW_TYPE_2D_ARRAY but the OpTypeImage has (Dim = 2D) and (Arrayed = 0).
Either fix in shader or update the VkImageViewType to VK_IMAGE_VIEW_TYPE_2D.
VUID-vkCmdDraw-viewType-07752
```

### Root Cause

**VulkanTexture.cpp:89-117** creates TWO image views per texture:
- `m_imageView` (2D) - correct type for single-layer textures
- `m_arrayImageView` (2D_ARRAY) - for sampler2DArray compatibility

BUT: The code ALWAYS binds the 2D_ARRAY view to descriptors, even when shaders declare `sampler2D`.

**The Problem**:
1. Shaders use `layout(set = 1, binding = 0) uniform sampler2D baseMap;` (Dim=2D, Arrayed=0)
2. VulkanTexture binds `m_arrayImageView` (VK_IMAGE_VIEW_TYPE_2D_ARRAY)
3. Vulkan validation rejects this (undefined behavior)

### Impact

Rendering corruption:
- Multiple scenes/frames appear simultaneously on screen
- Static geometry invisible/corrupt
- Objects only appear when animated
- Tiled/repeated viewport artifacts

### Fix

Change descriptor binding to use the correct view type based on shader expectations:

**Option 1** (Recommended): Detect which view is needed
- Check if descriptor binding expects `sampler2D` or `sampler2DArray` (via shader reflection)
- Bind `m_imageView` for `sampler2D`, `m_arrayImageView` for `sampler2DArray`

**Option 2** (Quick fix): Always use 2D views for single-layer textures
- In `VulkanTextureManager::bindTextureToDescriptor()`, use `m_imageView` instead of `getImageView()` (which returns array view)

**Option 3** (Nuclear): Change all shaders to `sampler2DArray`
- DO NOT DO THIS - breaks OpenGL renderer, makes menus black (see "Mistakes to avoid" section)

### Location

- **Code**: `code/graphics/vulkan/VulkanTexture.cpp:89-117`
- **Binding site**: Where textures are bound to descriptor sets (likely in `gr_vulkan.cpp` material binding)
- **Logs**: `fs2_open.log` line 879+ shows repeated VUID-07752 errors
