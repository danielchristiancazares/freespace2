# GPT‑5.2 Pro Prompt: Vulkan `gf_render_primitives_particle` (SDR_TYPE_EFFECT_PARTICLE)

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
Implement the Vulkan backend so `gr_render_primitives_particle(...)` renders particles (soft particles via depth sampling) instead of calling the stub.

## Exact function signature (must match)
Implement this function in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_render_primitives_particle(particle_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int offset,
	int n_verts,
	gr_buffer_handle buffer_handle);
```

Wire it in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_render_primitives_particle = gr_vulkan_render_primitives_particle;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_EFFECT_PARTICLE` that loads:

- `effect.vert.spv`
- `effect.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

- **binding 0 (UBO, std140)**: `matrixData { mat4 modelViewMatrix; mat4 projMatrix; }`
- **binding 1 (UBO, std140)**: `genericData` with the exact std140 layout used by the shader (see shader source below)
- **binding 2**: `baseMap` (`sampler2DArray`)
- **binding 3**: `depthMap` (`sampler2D`, Vulkan depth image)

Do not create or use any other descriptor sets.

## VulkanDescriptorLayouts prerequisite (must exist or this cannot work)
File: `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

The standard per‑draw push descriptor set layout (**set 0**) must contain sampler bindings up to at least **binding 3**.

Implement it by expanding the per‑draw push layout to include sampler bindings **2..6** (`vk::DescriptorType::eCombinedImageSampler`, fragment stage), in addition to UBO bindings 0 and 1.

## Depth sampling legality (must be explicit)
This shader samples the Vulkan depth image.

To keep this implementation bounded and non‑tangential, particle draws must obey these rules:

- **Do not attach the depth image** while rendering particles. Rendering must be swapchain‑color‑only (no depth attachment) so the depth image can remain in `vk::ImageLayout::eShaderReadOnlyOptimal` for sampling.
- Depth test and depth writes must be disabled for this draw.

This matches the existing deferred lighting pass strategy (swapchain rendering without depth attachment).

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/effect.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 vertPosition;
layout(location = 2) in vec4 vertTexCoord;
layout(location = 1) in vec4 vertColor;
layout(location = 6) in float vertRadius;

#ifdef FLAG_EFFECT_GEOMETRY
layout(location = 7) in vec3 vertUvec;
layout(location = 0) out vec3 geoUvec;
layout(location = 1) out float geoRadius;
layout(location = 2) out vec4 geoColor;
layout(location = 3) out float geoArrayIndex;
#else
layout(location = 0) out float fragRadius;
layout(location = 1) out vec4 fragPosition;
layout(location = 2) out vec4 fragTexCoord;
layout(location = 3) out vec4 fragColor;
#endif

layout(binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

void main()
{
#ifdef FLAG_EFFECT_GEOMETRY
	geoRadius = vertRadius;
	geoUvec = vertUvec;
	gl_Position = modelViewMatrix * vertPosition;
	geoColor = vertColor;
	geoArrayIndex = vertTexCoord.z;
#else
	fragRadius = vertRadius;
	gl_Position = projMatrix * modelViewMatrix * vertPosition;
	fragPosition = modelViewMatrix * vertPosition;
	fragTexCoord = vec4(vertTexCoord.xyz, 0.0);
	fragColor = vertColor;
#endif
}
```

### `code/graphics/shaders/effect.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout(location = 0) in float fragRadius;
layout(location = 1) in vec4 fragPosition;
layout(location = 2) in vec4 fragTexCoord;
layout(location = 3) in vec4 fragColor;

layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2DArray baseMap;
layout(binding = 3) uniform sampler2D depthMap;

layout(binding = 1, std140) uniform genericData {
	float window_width;
	float window_height;
	float nearZ;
	float farZ;
	int linear_depth;
	int srgb;
	int blend_alpha;
};

void main()
{
	vec4 fragmentColor = texture(baseMap, fragTexCoord.xyz);
	fragmentColor.rgb = mix(fragmentColor.rgb, srgb_to_linear(fragmentColor.rgb), float(srgb));
	fragmentColor *= mix(fragColor, vec4(srgb_to_linear(fragColor.rgb), fragColor.a), float(srgb));
	vec2 offset = vec2(fragRadius * abs(0.5 - fragTexCoord.x) * 2.0, fragRadius * abs(0.5 - fragTexCoord.y) * 2.0);
	float offset_len = length(offset);
	if (offset_len > fragRadius) {
		fragOut0 = vec4(0.0, 0.0, 0.0, 0.0);
		return;
	}
	vec2 depthCoord = vec2(gl_FragCoord.x / window_width, gl_FragCoord.y / window_height);
	vec4 sceneDepth = texture(depthMap, depthCoord);
	float sceneDepthLinear;
	float fragDepthLinear;
	if (linear_depth == 1) {
		sceneDepthLinear = -sceneDepth.z;
		fragDepthLinear = -fragPosition.z;
	} else {
		sceneDepthLinear = (2.0 * farZ * nearZ) / (farZ + nearZ - sceneDepth.x * (farZ - nearZ));
		fragDepthLinear = (2.0 * farZ * nearZ) / (farZ + nearZ - gl_FragCoord.z * (farZ - nearZ));
	}
	// assume UV of 0.5, 0.5 is the centroid of this sphere volume
	float depthOffset = sqrt((fragRadius * fragRadius) - (offset_len * offset_len));
	float frontDepth = fragDepthLinear - depthOffset;
	float backDepth = fragDepthLinear + depthOffset;
	float intensity = smoothstep(max(nearZ, frontDepth), backDepth, sceneDepthLinear);
	fragmentColor.rgb *= (srgb == 1) ? 1.5 : 1.0;
	fragmentColor = (blend_alpha == 1) ? vec4(fragmentColor.rgb, fragmentColor.a * intensity) : vec4(fragmentColor.rgb * intensity, fragmentColor.a);
	fragOut0 = max(fragmentColor, vec4(0.0));
}
```

Build system note (do not improvise): add `effect.vert` and `effect.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `effect.*.spv` and are embedded.

## VulkanGraphics implementation requirements
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Mandatory behavior:

- Use `shaderType = SDR_TYPE_EFFECT_PARTICLE`.
- Ensure deferred geometry has been ended before sampling depth:
  - call `renderer_instance->endDeferredGeometry(cmd)` before drawing (idempotent).
- Begin swapchain rendering without depth attachment for this draw:
  - Add this Vulkan‑only helper (exact signature) so `VulkanGraphics.cpp` can start the no‑depth swapchain rendering variant without needing access to the swapchain image index:
    - `code/graphics/vulkan/VulkanRenderer.h`: `void beginSwapchainRenderingNoDepth(vk::CommandBuffer cmd);`
    - `code/graphics/vulkan/VulkanRenderer.cpp`: implement it as `m_renderingSession->beginSwapchainRenderingNoDepth(cmd, m_recordingImage);`
  - Call `renderer_instance->beginSwapchainRenderingNoDepth(cmd)` before binding the pipeline/descriptors.
- Build the pipeline key explicitly (no guessing, and no depth attachment):
  - `type = SDR_TYPE_EFFECT_PARTICLE`
  - `variant_flags = 0`
  - `color_format = renderer_instance->getSwapChainImageFormat()`
  - `depth_format = VK_FORMAT_UNDEFINED`
  - `sample_count = renderer_instance->getSampleCount()`
  - `color_attachment_count = 1`
  - `blend_mode = material_info->get_blend_mode()`
  - `layout_hash = layout->hash()`
- Populate `genericData` (binding 1) exactly:
  - `window_width = (float)gr_screen.max_w`
  - `window_height = (float)gr_screen.max_h`
  - `nearZ = Min_draw_distance`
  - `farZ = Max_draw_distance`
  - `srgb = High_dynamic_range ? 1 : 0`
  - `blend_alpha = (material_info->get_blend_mode() != ALPHA_BLEND_ADDITIVE) ? 1 : 0`
  - `linear_depth = 0` (do not guess a linear depth buffer)
- Bind textures:
  - binding 2 (`baseMap`): `material_info->get_texture_map(TM_BASE_TYPE)` via `renderer_instance->getTextureDescriptor(...)`
  - binding 3 (`depthMap`): `renderTargets()->depthSampledView()` with `renderTargets()->gbufferSampler()` and `imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal`
- Bind the vertex buffer `buffer_handle` (byte offset 0) and draw:
  - `cmd.draw((uint32_t)n_verts, 1, (uint32_t)offset, 0)`

Depth test and depth writes must be disabled.

## Acceptance (done when)
- Vulkan no longer uses the particle stub for `gf_render_primitives_particle`.
- `VulkanShaderManager` loads `SDR_TYPE_EFFECT_PARTICLE` without throwing.
- Particle draws sample depth legally (no depth attachment) and render without validation errors.
