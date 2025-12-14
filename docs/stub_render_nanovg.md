# GPT‑5.2 Pro Prompt: Vulkan `gf_render_nanovg` (SDR_TYPE_NANOVG)

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
NanoVG UI rendering must work on Vulkan: NanoVG code binds a uniform buffer via `gr_bind_uniform_buffer(uniform_block_type::NanoVGData, ...)` and then calls `gr_screen.gf_render_nanovg(...)`. Vulkan must respect that binding contract and draw.

## Exact function signature (must match)
Implement this function in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_render_nanovg(nanovg_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int offset,
	int n_verts,
	gr_buffer_handle buffer_handle);
```

Wire it in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_render_nanovg = gr_vulkan_render_nanovg;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_NANOVG` that loads:

- `nanovg.vert.spv`
- `nanovg.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Uniform buffer binding plumbing (mandatory)
### A) Store the currently bound NanoVG uniform block per frame
File: `code/graphics/vulkan/VulkanFrame.h`

Add this struct definition inside `namespace graphics::vulkan` (exact fields):

```cpp
struct BoundUniformBuffer {
	gr_buffer_handle handle{};
	size_t offset = 0;
	size_t size = 0;
	bool valid = false;
};
```

Add a member to `class VulkanFrame`:

- `BoundUniformBuffer nanovgData;`

### B) Populate it in `gr_vulkan_bind_uniform_buffer`
File: `code/graphics/vulkan/VulkanGraphics.cpp`

In `gr_vulkan_bind_uniform_buffer(...)`:

- When `type == uniform_block_type::NanoVGData`, set:
  - `g_currentFrame->nanovgData = { handle, offset, size, true }`
- Do not ignore NanoVGData.
- Do not emit an “unsupported block type” log for NanoVGData.

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

- **binding 1 (UBO, std140)**: `NanoVGUniformData` (buffer/offset/size comes from the stored NanoVGData binding state)
- **binding 2**: `nvg_tex` (`sampler2DArray`)

Do not create or use any other descriptor sets.

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/nanovg.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1, std140) uniform NanoVGUniformData {
	mat3 scissorMat;

	mat3 paintMat;

	vec4 innerCol;

	vec4 outerCol;

	vec2 scissorExt;
	vec2 scissorScale;

	vec2 extent;
	float radius;
	float feather;

	float strokeMult;
	float strokeThr;
	int texType;
	int type;

	vec2 viewSize;
	int texArrayIndex;
};

layout(location = 0) in vec2 vertPosition;
layout(location = 2) in vec2 vertTexCoord;

layout(location = 0) out vec2 ftcoord;
layout(location = 1) out vec2 fpos;

void main(void)
{
	ftcoord = vertTexCoord;
	fpos = vertPosition;
	gl_Position = vec4(2.0 * vertPosition.x / viewSize.x - 1.0,
		1.0 - 2.0 * vertPosition.y / viewSize.y,
		0.0,
		1.0);
}
```

### `code/graphics/shaders/nanovg.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

#define EDGE_AA

layout(binding = 1, std140) uniform NanoVGUniformData {
	mat3 scissorMat;

	mat3 paintMat;

	vec4 innerCol;

	vec4 outerCol;

	vec2 scissorExt;
	vec2 scissorScale;

	vec2 extent;
	float radius;
	float feather;

	float strokeMult;
	float strokeThr;
	int texType;
	int type;

	vec2 viewSize;
	int texArrayIndex;
};

layout(binding = 2) uniform sampler2DArray nvg_tex;

layout(location = 0) in vec2 ftcoord;
layout(location = 1) in vec2 fpos;
layout(location = 0) out vec4 outColor;

float sdroundrect(vec2 pt, vec2 ext, float rad)
{
	vec2 ext2 = ext - vec2(rad, rad);
	vec2 d = abs(pt) - ext2;
	return min(max(d.x, d.y), 0.0) + length(max(d, 0.0)) - rad;
}

// Scissoring
float scissorMask(vec2 p)
{
	vec2 sc = (abs((scissorMat * vec3(p, 1.0)).xy) - scissorExt);
	sc = vec2(0.5, 0.5) - sc * scissorScale;
	return clamp(sc.x, 0.0, 1.0) * clamp(sc.y, 0.0, 1.0);
}

#ifdef EDGE_AA
// Stroke - from [0..1] to clipped pyramid, where the slope is 1px.
float strokeMask()
{
	return min(1.0, (1.0 - abs(ftcoord.x * 2.0 - 1.0)) * strokeMult) * min(1.0, ftcoord.y);
}
#endif

void main(void)
{
	vec4 result;
	float scissor = scissorMask(fpos);

#ifdef EDGE_AA
	float strokeAlpha = strokeMask();
#else
	float strokeAlpha = 1.0;
#endif

#ifdef EDGE_AA
	if (strokeAlpha < strokeThr) {
		discard;
	}
#endif

	if (type == 0) { // Gradient
		// Calculate gradient color using box gradient
		vec2 pt = (paintMat * vec3(fpos, 1.0)).xy;
		float d = clamp((sdroundrect(pt, extent, radius) + feather * 0.5) / feather, 0.0, 1.0);
		vec4 color = mix(innerCol, outerCol, d);
		// Combine alpha
		color *= strokeAlpha * scissor;
		result = color;
	}
	else if (type == 1) { // Image
		// Calculate color from texture
		vec2 pt = (paintMat * vec3(fpos, 1.0)).xy / extent;
		vec4 color = texture(nvg_tex, vec3(pt, float(texArrayIndex)));
		// Apply color tint and alpha.
		if (texType == 1)
			color = vec4(color.xyz * color.w, color.w);
		if (texType == 2)
			color = vec4(color.r);
		color *= innerCol;
		// Combine alpha
		color *= strokeAlpha * scissor;
		result = color;
	}
	else if (type == 2) { // Stencil fill
		result = vec4(1, 1, 1, 1);
	}
	else if (type == 3) { // Textured tris
		vec4 color = texture(nvg_tex, vec3(ftcoord, float(texArrayIndex)));
		if (texType == 1)
			color = vec4(color.xyz * color.w, color.w);
		if (texType == 2)
			color = vec4(color.x);
		color *= scissor;
		result = color * innerCol;
	}

	outColor = result;
}
```

Build system note (do not improvise): add `nanovg.vert` and `nanovg.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `nanovg.*.spv` and are embedded.

## VulkanGraphics implementation requirements
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Implementation rules:

- Hard assert `g_currentFrame != nullptr` and `g_currentFrame->nanovgData.valid`.
- Use the current frame and command buffer:
  - `VulkanFrame& frame = *g_currentFrame;`
  - `vk::CommandBuffer cmd = frame.commandBuffer();`
- Ensure a dynamic rendering scope is active before binding the pipeline and pushing descriptors:
  - `renderer_instance->ensureRenderingStarted(cmd);`
- Build the pipeline key explicitly (no guessing):
  - `type = SDR_TYPE_NANOVG`
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
- Create push descriptor writes (set 0):
  - binding 1: the currently bound NanoVGData uniform buffer
  - binding 2: `nvg_tex` from `material_info->get_texture_map(TM_BASE_TYPE)` via `renderer_instance->getTextureDescriptor(...)`
- Bind the vertex buffer `buffer_handle` with byte offset **0**.
- Draw using the provided **vertex start** `offset`:
  - `cmd.draw((uint32_t)n_verts, 1, (uint32_t)offset, 0)`

Do not recompute NanoVG uniforms inside this function.

## Acceptance (done when)
- Vulkan no longer uses the NanoVG stub for `gf_render_nanovg`.
- NanoVGData is bound via the stored `gr_bind_uniform_buffer` state.
- `VulkanShaderManager` loads `SDR_TYPE_NANOVG` without throwing.
