# GPT‑5.2 Pro Prompt: Vulkan `gf_render_rocket_primitives` (SDR_TYPE_ROCKET_UI)

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
libRocket UI rendering must work on Vulkan: `gr_render_rocket_primitives(...)` must draw indexed UI geometry instead of calling the stub.

## Exact function signature (must match)
Implement this function in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_render_rocket_primitives(interface_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int n_indices,
	gr_buffer_handle vertex_buffer,
	gr_buffer_handle index_buffer);
```

Wire it in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_render_rocket_primitives = gr_vulkan_render_rocket_primitives;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_ROCKET_UI` that loads:

- `rocketui.vert.spv`
- `rocketui.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

- **binding 1 (UBO, std140)**: `genericData` with the exact std140 layout used by the shader (see shader source below)
- **binding 2**: `baseMap` (`sampler2DArray`)

Do not create or use any other descriptor sets.

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/rocketui.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 vertPosition;
layout(location = 1) in vec4 vertColor;
layout(location = 2) in vec2 vertTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out vec2 fragScreenPosition;

layout(binding = 1, std140) uniform genericData {
	mat4 projMatrix;

	vec2 offset;
	bool textured;
	int baseMapIndex;

	float horizontalSwipeOffset;
};

void main()
{
	fragTexCoord = vertTexCoord;
	fragColor = vertColor;

	vec4 position = vec4(vertPosition + offset, 0.0, 1.0);

	fragScreenPosition = position.xy;
	gl_Position = projMatrix * position;
}
```

### `code/graphics/shaders/rocketui.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec2 fragScreenPosition;

layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2DArray baseMap;

layout(binding = 1, std140) uniform genericData {
	mat4 projMatrix;

	vec2 offset;
	bool textured;
	int baseMapIndex;

	float horizontalSwipeOffset;
};

void main()
{
	if (fragScreenPosition.x > horizontalSwipeOffset) {
		discard;
	}

	float distance = horizontalSwipeOffset - fragScreenPosition.x;

	vec4 color;
	if (textured) {
		color = texture(baseMap, vec3(fragTexCoord, float(baseMapIndex))) * fragColor;
	} else {
		color = fragColor;
	}

	if (distance < 10.0) {
		// Only change the colors but not the alpha channel to preserve the transparent part of text
		color.xyz = vec3(1.0);
	}

	fragOut0 = color;
}
```

Build system note (do not improvise): add `rocketui.vert` and `rocketui.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `rocketui.*.spv` and are embedded.

## VulkanGraphics implementation requirements
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Mandatory behavior:

- Call `gr_set_2d_matrix()` at the start of `gr_vulkan_render_rocket_primitives`.
- Call `gr_end_2d_matrix()` at the end.
- Use the current frame and command buffer:
  - `VulkanFrame& frame = *g_currentFrame;`
  - `vk::CommandBuffer cmd = frame.commandBuffer();`
- Ensure a dynamic rendering scope is active before binding the pipeline and pushing descriptors:
  - `renderer_instance->ensureRenderingStarted(cmd);`
- Use `shaderType = SDR_TYPE_ROCKET_UI`.
- Build the pipeline key explicitly (no guessing):
  - `type = SDR_TYPE_ROCKET_UI`
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
- Build the `genericData` block with the following values:
  - `projMatrix = gr_projection_matrix` **after** `gr_set_2d_matrix()`
  - `offset = material_info->get_offset()`
  - `textured = material_info->is_textured() ? true : false`
  - `baseMapIndex = textured ? bm_get_array_index(material_info->get_texture_map(TM_BASE_TYPE)) : 0`
  - `horizontalSwipeOffset = material_info->get_horizontal_swipe()`
- Allocate that block from `frame.uniformBuffer()` and bind it to **binding 1**.
- If `textured`, bind **binding 2** (`baseMap`) using `renderer_instance->getTextureDescriptor(...)` on `TM_BASE_TYPE`.
- Bind buffers:
  - vertex buffer: `vertex_buffer`
  - index buffer: `index_buffer` (type `vk::IndexType::eUint32`)
- Draw indexed:
  - `cmd.drawIndexed((uint32_t)n_indices, 1, 0, 0, 0)`

## Acceptance (done when)
- Vulkan no longer uses the Rocket stub for `gf_render_rocket_primitives`.
- `VulkanShaderManager` loads `SDR_TYPE_ROCKET_UI` without throwing.
- Rocket UI draws render with correct 2D projection and texture sampling.
