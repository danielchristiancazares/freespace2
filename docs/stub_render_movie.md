# GPT‑5.2 Pro Prompt: Vulkan `gf_render_movie` (SDR_TYPE_VIDEO_PROCESS)

## Hard constraints (must be obeyed)
You are producing a code‑only answer in the ChatGPT UI.

You can only inspect and edit Vulkan backend C++/H files under `code/graphics/vulkan/`. You must assume you cannot open any other engine files.

Do not refactor or redesign the renderer. Implement only what is explicitly requested.

## Output format (mandatory)
Return **only** code.

- For edited C++/H files: unified diffs.
- For new shader files: complete file contents with exact paths.

No prose.

## Goal
Make `gr_render_movie(...)` render the movie’s already‑decoded Y/U/V textures on Vulkan instead of calling a stub.

## Exact function signature (must match)
Implement this function in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_render_movie(movie_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int n_verts,
	gr_buffer_handle buffer,
	size_t buffer_offset);
```

Wire it in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_render_movie = gr_vulkan_render_movie;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_VIDEO_PROCESS` that loads:

- `video.vert.spv`
- `video.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

- **binding 0 (UBO, std140)**: `matrixData { mat4 modelViewMatrix; mat4 projMatrix; }`
- **binding 1 (UBO, std140)**: `movieData { float alpha; float pad[3]; }`
- **binding 2**: `ytex` (`sampler2DArray`)
- **binding 3**: `utex` (`sampler2DArray`)
- **binding 4**: `vtex` (`sampler2DArray`)

Do not create or use any other descriptor sets.

## VulkanDescriptorLayouts prerequisite (must exist or this cannot work)
File: `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

The standard per‑draw push descriptor set layout (**set 0**) must contain sampler bindings up to at least **binding 4**.

Implement it by expanding the per‑draw push layout to include sampler bindings **2..6** (`vk::DescriptorType::eCombinedImageSampler`, fragment stage), in addition to UBO bindings 0 and 1.

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/video.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 vertPosition;
layout(location = 2) in vec4 vertTexCoord;

layout(location = 0) out vec4 fragTexCoord;

layout(binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

void main()
{
	fragTexCoord = vertTexCoord;
	gl_Position = projMatrix * modelViewMatrix * vertPosition;
}
```

### `code/graphics/shaders/video.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2DArray ytex;
layout(binding = 3) uniform sampler2DArray utex;
layout(binding = 4) uniform sampler2DArray vtex;

layout(binding = 1, std140) uniform movieData {
	float alpha;
	float pad[3];
};

void main()
{
	float y = texture(ytex, vec3(fragTexCoord.st, 0.0)).r;
	float u = texture(utex, vec3(fragTexCoord.st, 0.0)).r;
	float v = texture(vtex, vec3(fragTexCoord.st, 0.0)).r;
	vec3 val = vec3(y - 0.0625, u - 0.5, v - 0.5);
	fragOut0.r = dot(val, vec3(1.1640625, 0.0, 1.59765625));
	fragOut0.g = dot(val, vec3(1.1640625, -0.390625, -0.8125));
	fragOut0.b = dot(val, vec3(1.1640625, 2.015625, 0.0));
	fragOut0.a = alpha;
}
```

Build system note (do not improvise): add `video.vert` and `video.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `video.*.spv` and are embedded.

## VulkanGraphics implementation requirements
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Implement `gr_vulkan_render_movie(...)` using the existing Vulkan draw pattern (`gr_vulkan_render_primitives`): pipeline key → pipeline bind → push descriptors → bind buffers → draw.

Mandatory behavior:

- Call `gr_set_2d_matrix()` at the start of this function.
- Call `gr_end_2d_matrix()` at the end of this function.
- Use the current frame and command buffer:
  - `VulkanFrame& frame = *g_currentFrame;`
  - `vk::CommandBuffer cmd = frame.commandBuffer();`
- Ensure a dynamic rendering scope is active before binding the pipeline and pushing descriptors:
  - `renderer_instance->ensureRenderingStarted(cmd);`
- Build the pipeline key explicitly (no guessing):
  - `type = SDR_TYPE_VIDEO_PROCESS`
  - `variant_flags = 0`
  - `color_format = renderer_instance->getSwapChainImageFormat()`
  - `depth_format = renderer_instance->getDepthFormat()`
  - `sample_count = renderer_instance->getSampleCount()`
  - `color_attachment_count = 1`
  - `blend_mode = material_info->get_blend_mode()`
  - `layout_hash = layout->hash()`
- Set dynamic state explicitly for a 2D overlay draw:
  - `cmd.setPrimitiveTopology(convertPrimitiveType(prim_type))`
  - `cmd.setCullMode(vk::CullModeFlagBits::eNone)`
  - `cmd.setFrontFace(vk::FrontFace::eClockwise)` (matches the renderer’s Y‑flip convention)
  - `cmd.setDepthTestEnable(VK_FALSE)` and `cmd.setDepthWriteEnable(VK_FALSE)`
- Allocate a `matrixData` UBO and a `movieData` UBO from `frame.uniformBuffer()` and push them to bindings 0 and 1.
  - `movieData.alpha = material_info->get_color().xyzw.w`
- Bind all three textures (bindings 2/3/4) using `renderer_instance->getTextureDescriptor(...)`.
- Bind the vertex buffer `buffer` with byte offset `buffer_offset`.
- Draw non‑indexed: `cmd.draw((uint32_t)n_verts, 1, 0, 0)`.

Do not implement any video decoding or CPU color conversion.

## Acceptance (done when)
- Vulkan no longer uses the movie stub for `gf_render_movie`.
- `VulkanShaderManager` loads `SDR_TYPE_VIDEO_PROCESS` without throwing.
- Movie draws bind Y/U/V as `sampler2DArray` and render correctly.
