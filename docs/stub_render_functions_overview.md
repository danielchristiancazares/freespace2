# Vulkan render stub prompts (GPT‑5.2 Pro) — self‑contained, tangent‑proof overview

This file exists to make the individual stub prompts deterministic. It states the shared, non‑negotiable assumptions and the one‑time Vulkan‑side prerequisites those prompts rely on.

## Hard constraints (must be obeyed)
You are generating a code‑only answer in the ChatGPT UI.

You can only inspect and edit the Vulkan backend C++/H files (everything under `code/graphics/vulkan/`). You must assume you **cannot** open or inspect any other engine code (OpenGL backend, gameplay, build scripts, `.sdr` shader sources, etc.).

You must not “improve architecture”, refactor, or introduce new systems. Implement only what is explicitly requested.

## Output format (mandatory)
Return **only** code.

- For existing C++/H files: output unified diffs.
- For any new files (such as Vulkan GLSL sources): output complete file contents with the exact repo path.

No prose explanations, no additional suggestions.

## One‑time prerequisites shared by all stub prompts
### 1) Expand the per‑draw push descriptor set layout (set 0)
File: `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

The engine’s non‑model draw paths use a single standard push‑descriptor set: **set 0**.

Change `m_perDrawPushLayout` so set 0 contains **exactly these bindings**:

- binding 0: `vk::DescriptorType::eUniformBuffer`, `stageFlags = eVertex | eFragment`
- binding 1: `vk::DescriptorType::eUniformBuffer`, `stageFlags = eVertex | eFragment`
- binding 2: `vk::DescriptorType::eCombinedImageSampler`, `stageFlags = eFragment`
- binding 3: `vk::DescriptorType::eCombinedImageSampler`, `stageFlags = eFragment`
- binding 4: `vk::DescriptorType::eCombinedImageSampler`, `stageFlags = eFragment`
- binding 5: `vk::DescriptorType::eCombinedImageSampler`, `stageFlags = eFragment`
- binding 6: `vk::DescriptorType::eCombinedImageSampler`, `stageFlags = eFragment`

Do **not** add new descriptor sets. Do **not** renumber sets.

### 2) Add VulkanShaderManager mappings for the stub shader types (ignore variants)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

For every shader type listed below, add a `case` that loads the listed `*.spv` filenames.

**Variant flags must be ignored for module lookup and caching.** Concretely: in each new `case`, force the cache key to treat flags as `0` (the same approach as the existing `SDR_TYPE_MODEL` case).

Add mappings:

- `SDR_TYPE_EFFECT_PARTICLE` → `effect.vert.spv` / `effect.frag.spv`
- `SDR_TYPE_EFFECT_DISTORTION` → `effect-distort.vert.spv` / `effect-distort.frag.spv`
- `SDR_TYPE_VIDEO_PROCESS` → `video.vert.spv` / `video.frag.spv`
- `SDR_TYPE_NANOVG` → `nanovg.vert.spv` / `nanovg.frag.spv`
- `SDR_TYPE_ROCKET_UI` → `rocketui.vert.spv` / `rocketui.frag.spv`
- `SDR_TYPE_DECAL` → `decal.vert.spv` / `decal.frag.spv`
- `SDR_TYPE_SHIELD_DECAL` → `shield-impact.vert.spv` / `shield-impact.frag.spv`

### 3) Wire the Vulkan function pointers (otherwise stubs stay active)
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Inside `graphics::vulkan::init_function_pointers()` you must override the following pointers to Vulkan implementations:

- `gf_render_primitives_particle`
- `gf_render_primitives_distortion`
- `gf_render_movie`
- `gf_render_nanovg`
- `gf_render_rocket_primitives`
- `gf_start_decal_pass`
- `gf_render_decals`
- `gf_stop_decal_pass`
- `gf_render_shield_impact`
- `gf_copy_effect_texture` (used by distortion)

If you do not wire them here, the engine will continue calling the stubs from `code/graphics/grstub.cpp`.

### 4) Frame‑local uniform‑buffer binding state (NanoVG + decals)
Files: `code/graphics/vulkan/VulkanFrame.h`, `code/graphics/vulkan/VulkanGraphics.cpp`

Some higher‑level systems bind uniform buffers via `gr_bind_uniform_buffer(...)` and expect the backend to remember that binding until the draw call.

You must implement frame‑local tracking for these uniform block types:

- `uniform_block_type::NanoVGData`
- `uniform_block_type::DecalGlobals`
- `uniform_block_type::DecalInfo`

Required shape:

- Add a small struct to `VulkanFrame.h`:
  - `gr_buffer_handle handle` (engine handle)
  - `size_t offset`
  - `size_t size`
  - `bool valid`
- Add `VulkanFrame` members:
  - `nanovgData`, `decalGlobals`, `decalInfo`
- Update `gr_vulkan_bind_uniform_buffer(...)` in `VulkanGraphics.cpp` to populate those members.
- Do **not** log “unsupported block type” for these.

### 5) Depth sampling legality (do not violate this)
If a shader samples the Vulkan depth image (`renderTargets()->depthSampledView()`), then at draw time the depth image must be in a shader‑readable layout (`vk::ImageLayout::eShaderReadOnlyOptimal` or `vk::ImageLayout::eDepthReadOnlyOptimal`) and must **not** be simultaneously attached as a writable depth attachment.

- **Decals** are the special case: they must write to the 3 G‑buffer attachments while sampling depth, so they require a dedicated deferred “decal pass” render mode where the depth attachment is bound read‑only.

Do not invent any additional render‑graph redesign. Implement only the minimal render‑mode additions explicitly demanded by the decals prompt.

## Set‑0 binding maps (push descriptors)
All bindings below are **set 0**.

- **Particles (`SDR_TYPE_EFFECT_PARTICLE`)**
  - b0: `matrixData`
  - b1: `genericData` = `effect_data`
  - b2: `baseMap` (`sampler2DArray`)
  - b3: `depthMap` (`sampler2D`, Vulkan depth image)

- **Distortion (`SDR_TYPE_EFFECT_DISTORTION`)**
  - b0: `matrixData`
  - b1: `genericData` = `effect_distort_data`
  - b2: `baseMap` (`sampler2DArray`)
  - b3: `depthMap` (`sampler2D`)
  - b4: `frameBuffer` (`sampler2D`, Vulkan scene copy)
  - b5: `distMap` (`sampler2D`, neutral/static map)

- **Movie (`SDR_TYPE_VIDEO_PROCESS`)**
  - b0: `matrixData` (after `gr_set_2d_matrix()`)
  - b1: `movieData` (`alpha`)
  - b2: `ytex` (`sampler2DArray`)
  - b3: `utex` (`sampler2DArray`)
  - b4: `vtex` (`sampler2DArray`)

- **NanoVG (`SDR_TYPE_NANOVG`)**
  - b1: `NanoVGUniformData` (from `uniform_block_type::NanoVGData` binding state)
  - b2: `nvg_tex` (`sampler2DArray`)

- **Rocket UI (`SDR_TYPE_ROCKET_UI`)**
  - b1: `genericData` = `rocketui_data`
  - b2: `baseMap` (`sampler2DArray`)

- **Decals (`SDR_TYPE_DECAL`)**
  - b0: `decalGlobalData` (from `uniform_block_type::DecalGlobals` binding state)
  - b1: `decalInfoData` (from `uniform_block_type::DecalInfo` binding state)
  - b2: `diffuseMap` (`sampler2DArray`)
  - b3: `glowMap` (`sampler2DArray`)
  - b4: `normalMap` (`sampler2DArray`)
  - b5: `gDepthBuffer` (`sampler2D`, Vulkan depth)

- **Shield impact (`SDR_TYPE_SHIELD_DECAL`)**
  - b0: `matrixData`
  - b1: `genericData` = `shield_impact_data`
  - b2: `shieldMap` (`sampler2DArray`)

## Explicit tangent traps (do not do these)
Do not implement video decoding, NanoVG scene generation, libRocket UI layout, new descriptor set systems, a new render graph, or any OpenGL compatibility layer. Do not touch non‑Vulkan code.
