# GPT‑5.2 Pro Prompt: Vulkan `gf_render_shield_impact` (SDR_TYPE_SHIELD_DECAL)

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
Shield impact effects must render on Vulkan: `gr_render_shield_impact(...)` must draw instead of calling the stub.

## Exact function signature (must match)
Implement this function in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_render_shield_impact(shield_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	gr_buffer_handle buffer_handle,
	int n_verts);
```

Wire it in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_render_shield_impact = gr_vulkan_render_shield_impact;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_SHIELD_DECAL` that loads:

- `shield-impact.vert.spv`
- `shield-impact.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

- **binding 0 (UBO, std140)**: `matrixData { mat4 modelViewMatrix; mat4 projMatrix; }`
- **binding 1 (UBO, std140)**: `genericData` with the exact std140 layout used by the shader (see shader source below)
- **binding 2**: `shieldMap` (`sampler2DArray`)

Do not create or use any other descriptor sets.

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/shield-impact.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 vertPosition;
layout(location = 3) in vec3 vertNormal;

layout(location = 0) out vec4 fragImpactUV;
layout(location = 1) out float fragNormOffset;

layout(binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

layout(binding = 1, std140) uniform genericData {
	mat4 shieldModelViewMatrix;
	mat4 shieldProjMatrix;

	vec3 hitNormal;
	int srgb;

	vec4 color;

	int shieldMapIndex;
};

void main()
{
	gl_Position = projMatrix * modelViewMatrix * vertPosition;
	fragNormOffset = dot(hitNormal, vertNormal);
	fragImpactUV = shieldProjMatrix * shieldModelViewMatrix * vertPosition;
	fragImpactUV += 1.0;
	fragImpactUV *= 0.5;
}
```

### `code/graphics/shaders/shield-impact.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

const float EMISSIVE_GAIN = 2.0;

layout(location = 0) in vec4 fragImpactUV;
layout(location = 1) in float fragNormOffset;

layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2DArray shieldMap;

layout(binding = 1, std140) uniform genericData {
	mat4 shieldModelViewMatrix;
	mat4 shieldProjMatrix;

	vec3 hitNormal;
	int srgb;

	vec4 color;

	int shieldMapIndex;
};

void main()
{
	if (fragNormOffset < 0.0)
		discard;
	if (fragImpactUV.x < 0.0 || fragImpactUV.x > 1.0 || fragImpactUV.y < 0.0 || fragImpactUV.y > 1.0)
		discard;

	vec4 shieldColor = texture(shieldMap, vec3(fragImpactUV.xy, float(shieldMapIndex)));
	shieldColor.rgb = (srgb == 1) ? srgb_to_linear(shieldColor.rgb) * EMISSIVE_GAIN : shieldColor.rgb;

	vec4 blendColor = color;
	blendColor.rgb = (srgb == 1) ? srgb_to_linear(blendColor.rgb) * EMISSIVE_GAIN : blendColor.rgb;

	fragOut0 = shieldColor * blendColor;
}
```

Build system note (do not improvise): add `shield-impact.vert` and `shield-impact.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `shield-impact.*.spv` and are embedded.

## VulkanGraphics implementation requirements
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Mandatory behavior:

- Use `shaderType = SDR_TYPE_SHIELD_DECAL`.
- Use the current frame and command buffer:
  - `VulkanFrame& frame = *g_currentFrame;`
  - `vk::CommandBuffer cmd = frame.commandBuffer();`
- Ensure a dynamic rendering scope is active before binding the pipeline and pushing descriptors:
  - `renderer_instance->ensureRenderingStarted(cmd);`
- Compute the impact matrices **exactly** as the OpenGL implementation does (verbatim logic, just in Vulkan code):

- Build the pipeline key explicitly (no guessing):
  - `type = SDR_TYPE_SHIELD_DECAL`
  - `variant_flags = 0`
  - `color_format = renderer_instance->getCurrentColorFormat()`
  - `depth_format = renderer_instance->getDepthFormat()`
  - `sample_count = renderer_instance->getSampleCount()`
  - `color_attachment_count = renderer_instance->getCurrentColorAttachmentCount()`
  - `blend_mode = material_info->get_blend_mode()`
  - `layout_hash = layout->hash()`

```cpp
matrix4 impact_transform;
matrix4 impact_projection;
vec3d min;
vec3d max;

float radius = material_info->get_impact_radius();
min.xyz.x = min.xyz.y = min.xyz.z = -radius;
max.xyz.x = max.xyz.y = max.xyz.z = radius;

vm_matrix4_set_orthographic(&impact_projection, &max, &min);

matrix impact_orient = material_info->get_impact_orient();
vec3d impact_pos = material_info->get_impact_pos();

vm_matrix4_set_inverse_transform(&impact_transform, &impact_orient, &impact_pos);
```

- Determine `shieldMapIndex`:
  - default `0`
  - if `material_info->get_texture_map(TM_BASE_TYPE) >= 0`, set `shieldMapIndex = bm_get_array_index(material_info->get_texture_map(TM_BASE_TYPE))`
- Fill the generic uniform block (binding 1) with:
  - `hitNormal = impact_orient.vec.fvec`
  - `shieldProjMatrix = impact_projection`
  - `shieldModelViewMatrix = impact_transform`
  - `shieldMapIndex = ...` (above)
  - `srgb = High_dynamic_range ? 1 : 0`
  - `color = material_info->get_color()`
- Fill the matrix uniform block (binding 0) from current globals:
  - `modelViewMatrix = gr_model_view_matrix`
  - `projMatrix = gr_projection_matrix`
- Bind `shieldMap` (binding 2) using `renderer_instance->getTextureDescriptor(...)` for `TM_BASE_TYPE`.
- Bind the vertex buffer `buffer_handle` (byte offset 0) and draw:
  - `cmd.draw((uint32_t)n_verts, 1, 0, 0)`

## Acceptance (done when)
- Vulkan no longer uses the shield impact stub for `gf_render_shield_impact`.
- `VulkanShaderManager` loads `SDR_TYPE_SHIELD_DECAL` without throwing.
- Shield impacts render with correct projection and texture indexing.
