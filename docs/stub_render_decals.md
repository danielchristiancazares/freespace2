# GPT‑5.2 Pro Prompt: Vulkan decals (`gf_start_decal_pass` / `gf_render_decals` / `gf_stop_decal_pass`)

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
Make decals render on Vulkan using the existing engine submission path. The engine will call:

- `gr_bind_uniform_buffer(uniform_block_type::DecalGlobals, ...)` once
- `gf_start_decal_pass()`
- for each batch:
  - `gr_bind_uniform_buffer(uniform_block_type::DecalInfo, ...)`
  - `gf_render_decals(...)`
- `gf_stop_decal_pass()`

Vulkan must implement the backend hooks it expects.

## Exact function signatures (must match)
Implement these functions in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_start_decal_pass();

void gr_vulkan_render_decals(decal_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int num_elements,
	const indexed_vertex_source& buffers,
	const gr_buffer_handle& instance_buffer,
	int num_instances);

void gr_vulkan_stop_decal_pass();
```

Wire them in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_start_decal_pass = gr_vulkan_start_decal_pass;`
- `gr_screen.gf_render_decals = gr_vulkan_render_decals;`
- `gr_screen.gf_stop_decal_pass = gr_vulkan_stop_decal_pass;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_DECAL` that loads:

- `decal.vert.spv`
- `decal.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Frame‑local uniform buffer binding state (mandatory)
Decals use `gr_bind_uniform_buffer(...)` for two uniform blocks. Vulkan must remember those bindings until draw time.

### A) Add a reusable binding state struct
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

Add members to `class VulkanFrame`:

- `BoundUniformBuffer decalGlobals;`
- `BoundUniformBuffer decalInfo;`

### B) Populate it in `gr_vulkan_bind_uniform_buffer`
File: `code/graphics/vulkan/VulkanGraphics.cpp`

In `gr_vulkan_bind_uniform_buffer(...)`:

- When `type == uniform_block_type::DecalGlobals`, set:
  - `g_currentFrame->decalGlobals = { handle, offset, size, true }`
- When `type == uniform_block_type::DecalInfo`, set:
  - `g_currentFrame->decalInfo = { handle, offset, size, true }`

Do not emit an “unsupported block type” log for these.

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

### Uniform buffers
- **binding 0 (UBO, std140)**: `decalGlobalData` (from stored DecalGlobals binding state)
- **binding 1 (UBO, std140)**: `decalInfoData` (from stored DecalInfo binding state)

### Textures
- **binding 2**: `diffuseMap` (`sampler2DArray`)
- **binding 3**: `glowMap` (`sampler2DArray`)
- **binding 4**: `normalMap` (`sampler2DArray`)
- **binding 5**: `gDepthBuffer` (`sampler2D`, Vulkan depth)

## VulkanDescriptorLayouts prerequisite (must exist or this cannot work)
File: `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

The standard per‑draw push descriptor set layout (**set 0**) must contain sampler bindings up to at least **binding 5**.

Implement it by expanding the per‑draw push layout to include sampler bindings **2..6** (`vk::DescriptorType::eCombinedImageSampler`, fragment stage), in addition to UBO bindings 0 and 1.

## Explicitly NOT supported in this Vulkan implementation
The OpenGL decals path has a variant that samples the scene normal buffer (`gNormalBuffer`). Vulkan must not do that here (it would be a read/write feedback loop).

Rules:

- Do not declare or bind `gNormalBuffer` in the Vulkan decal shader.
- Do not use or branch on `SDR_FLAG_DECAL_USE_NORMAL_MAP`.
- In Vulkan C++ pipeline selection for decals, treat `variant_flags` as `0`.

## Depth sampling legality (this is the key Vulkan requirement)
The decal fragment shader samples depth **and** writes to the G‑buffer.

In Vulkan this must be legal:

- The depth image must be bound as a **read‑only depth attachment** (layout `vk::ImageLayout::eDepthReadOnlyOptimal`).
- Depth writes must be disabled.

This requires a dedicated render‑mode variant in `VulkanRenderingSession`.

## VulkanRenderingSession changes (mandatory)
Files: `code/graphics/vulkan/VulkanRenderingSession.h/.cpp`

### A) Add a new render mode
Add:

- `RenderMode::DeferredDecal`

### B) Add a dedicated begin function
Implement a new internal function (exact behavior):

- End any active dynamic rendering.
- Transition the depth image from `vk::ImageLayout::eDepthAttachmentOptimal` to `vk::ImageLayout::eDepthReadOnlyOptimal`.
  - Use an explicit image barrier (no guessing of stages/layouts):
    - `oldLayout = vk::ImageLayout::eDepthAttachmentOptimal`
    - `newLayout = vk::ImageLayout::eDepthReadOnlyOptimal`
    - `srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests`
    - `srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite`
    - `dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eFragmentShader`
    - `dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eShaderRead`
    - `image = m_targets.depthImage()`
    - `aspectMask = vk::ImageAspectFlagBits::eDepth`
    - `levelCount = 1`, `layerCount = 1`
- Begin dynamic rendering with:
  - the same 3 G‑buffer color attachments as `beginGBufferRendering` (loadOp = Load, storeOp = Store)
  - the depth attachment view with `imageLayout = vk::ImageLayout::eDepthReadOnlyOptimal` and `loadOp = Load`
  - no clears

### C) Integrate it
In `ensureRenderingActive(...)`, handle `RenderMode::DeferredDecal` by calling the new begin function.

When switching back from `RenderMode::DeferredDecal` to `RenderMode::DeferredGBuffer`, you must transition depth back to writable attachment layout before beginning G‑buffer rendering:

- barrier `oldLayout = vk::ImageLayout::eDepthReadOnlyOptimal` → `newLayout = vk::ImageLayout::eDepthAttachmentOptimal`
- `srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader | vk::PipelineStageFlagBits2::eEarlyFragmentTests`
- `srcAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eDepthStencilAttachmentRead`
- `dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests`
- `dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite`
- same `image`/`aspectMask`/subresource range as above

In `currentColorFormat()` and `currentColorAttachmentCount()`, treat `DeferredDecal` exactly like `DeferredGBuffer`:

- format = `gbufferFormat()`
- attachment count = 3

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/decal.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 vertPosition;
layout(location = 8) in mat4 vertModelMatrix;

layout(location = 0) flat out mat4 invModelMatrix;
layout(location = 4) flat out vec3 decalDirection;
layout(location = 5) flat out float normal_angle_cutoff;
layout(location = 6) flat out float angle_fade_start;
layout(location = 7) flat out float alpha_scale;

layout(binding = 0, std140) uniform decalGlobalData {
	mat4 viewMatrix;
	mat4 projMatrix;
	mat4 invViewMatrix;
	mat4 invProjMatrix;

	vec2 viewportSize;
};

void main()
{
	normal_angle_cutoff = vertModelMatrix[0][3];
	angle_fade_start = vertModelMatrix[1][3];
	alpha_scale = vertModelMatrix[2][3];

	mat4 modelMatrix = vertModelMatrix;
	modelMatrix[0][3] = 0.0;
	modelMatrix[1][3] = 0.0;
	modelMatrix[2][3] = 0.0;

	invModelMatrix = inverse(modelMatrix);
	decalDirection = mat3(viewMatrix) * modelMatrix[2].xyz;
	gl_Position = projMatrix * viewMatrix * modelMatrix * vertPosition;
}
```

### `code/graphics/shaders/decal.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

// NOTE: The technique and some of this code is based on this tutorial:
// http://martindevans.me/game-development/2015/02/27/Drawing-Stuff-On-Other-Stuff-With-Deferred-Screenspace-Decals/

#include "lighting.sdr"
#include "normals.sdr"
#include "gamma.sdr"

layout(location = 0) out vec4 fragOut0; // Diffuse buffer
layout(location = 1) out vec4 fragOut1; // Normal buffer
layout(location = 2) out vec4 fragOut2; // Emissive buffer

layout(location = 0) flat in mat4 invModelMatrix;
layout(location = 4) flat in vec3 decalDirection;
layout(location = 5) flat in float normal_angle_cutoff;
layout(location = 6) flat in float angle_fade_start;
layout(location = 7) flat in float alpha_scale;

layout(binding = 5) uniform sampler2D gDepthBuffer;

layout(binding = 2) uniform sampler2DArray diffuseMap;
layout(binding = 3) uniform sampler2DArray glowMap;
layout(binding = 4) uniform sampler2DArray normalMap;

layout(binding = 0, std140) uniform decalGlobalData {
	mat4 viewMatrix;
	mat4 projMatrix;
	mat4 invViewMatrix;
	mat4 invProjMatrix;

	vec2 viewportSize;
};

layout(binding = 1, std140) uniform decalInfoData {
	int diffuse_index;
	int glow_index;
	int normal_index;
	int diffuse_blend_mode;

	int glow_blend_mode;
};

vec3 computeViewPosition(vec2 textureCoord)
{
	vec4 clipSpaceLocation;
	vec2 normalizedCoord = textureCoord / viewportSize;

	clipSpaceLocation.xy = normalizedCoord * 2.0 - 1.0;
	// Vulkan NDC depth is 0..1, so use the sampled depth directly.
	clipSpaceLocation.z = texelFetch(gDepthBuffer, ivec2(textureCoord), 0).r;
	clipSpaceLocation.w = 1.0;

	vec4 homogenousLocation = invProjMatrix * clipSpaceLocation;
	return homogenousLocation.xyz / homogenousLocation.w;
}

vec3 getPixelNormal(vec3 frag_position, vec2 tex_coord, inout float alpha, out vec3 binormal, out vec3 tangent)
{
	// Always use screen-space derivatives (Vulkan implementation does not sample a scene normal buffer).
	vec3 pos_dx = dFdx(frag_position);
	vec3 pos_dy = dFdy(frag_position);
	vec3 normal = normalize(cross(pos_dx, pos_dy));

	binormal = normalize(pos_dx);
	tangent = normalize(pos_dy);

	// Calculate angle between surface normal and decal direction
	float angle = acos(dot(normal, decalDirection));

	if (angle > normal_angle_cutoff) {
		discard;
	}

	// Make a smooth alpha transition leading up to an edge
	alpha = alpha * (1.0 - smoothstep(angle_fade_start, normal_angle_cutoff, angle));

	return normal;
}

vec2 getDecalTexCoord(vec3 view_pos, inout float alpha)
{
	vec4 object_pos = invModelMatrix * invViewMatrix * vec4(view_pos, 1.0);

	bvec3 invalidComponents = greaterThan(abs(object_pos.xyz), vec3(0.5));
	bvec4 nanComponents = isnan(object_pos);

	if (any(invalidComponents) || any(nanComponents)) {
		discard;
	}

	// Fade out the texture when it gets close to the top or bottom of the decal box
	alpha = alpha * (1.0 - smoothstep(0.4, 0.5, abs(object_pos.z)));

	return object_pos.xy + 0.5;
}

void main()
{
	vec3 frag_position = computeViewPosition(gl_FragCoord.xy);
	float alpha = alpha_scale;
	vec2 tex_coord = getDecalTexCoord(frag_position, alpha);

	vec3 binormal;
	vec3 tangent;
	vec3 normal = getPixelNormal(frag_position, gl_FragCoord.xy, alpha, binormal, tangent);

	vec4 diffuse_out = vec4(0.0);
	vec4 emissive_out = vec4(0.0);
	vec3 normal_out = vec3(0.0);

	if (diffuse_index >= 0) {
		vec4 color = texture(diffuseMap, vec3(tex_coord, float(diffuse_index)));
		color.rgb = srgb_to_linear(color.rgb);

		if (diffuse_blend_mode == 0) {
			diffuse_out = vec4(color.rgb, color.a * alpha);
		} else {
			diffuse_out = vec4(color.rgb * alpha, 1.0);
		}
	}

	if (glow_index >= 0) {
		vec4 color = texture(glowMap, vec3(tex_coord, float(glow_index)));
		color.rgb = srgb_to_linear(color.rgb) * GLOW_MAP_SRGB_MULTIPLIER;
		color.rgb *= GLOW_MAP_INTENSITY;

		if (glow_blend_mode == 0) {
			emissive_out = vec4(color.rgb + emissive_out.rgb * emissive_out.a, color.a * alpha);
		} else {
			emissive_out.rgb += color.rgb * alpha;
		}
	}

	if (normal_index >= 0) {
		vec3 decalNormal = unpackNormal(texture(normalMap, vec3(tex_coord, float(normal_index))).ag);

		mat3 tangentToView;
		tangentToView[0] = tangent;
		tangentToView[1] = binormal;
		tangentToView[2] = normal;

		normal_out = tangentToView * decalNormal * alpha;
	}

	fragOut0 = diffuse_out;
	fragOut1 = vec4(normal_out, 0.0);
	fragOut2 = emissive_out;
}
```

Build system note (do not improvise): add `decal.vert` and `decal.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `decal.*.spv` and are embedded.

## VulkanGraphics implementation requirements
File: `code/graphics/vulkan/VulkanGraphics.cpp`

### A) `gr_vulkan_start_decal_pass()`
- Use the current frame command buffer:
  - `vk::CommandBuffer cmd = g_currentFrame->commandBuffer();`
- Switch the rendering session to the decals render mode:
  - `renderer_instance->renderingSession()->setMode(RenderMode::DeferredDecal)`
- Force dynamic rendering to re-begin with the decal configuration:
  - `renderer_instance->renderingSession()->endRendering(cmd)`
  - `renderer_instance->ensureRenderingStarted(cmd)` (this must begin the `DeferredDecal` rendering variant)

### B) `gr_vulkan_render_decals(...)`
Mandatory behavior:

- Hard assert `g_currentFrame->decalGlobals.valid` and `g_currentFrame->decalInfo.valid`.
- Use `shaderType = SDR_TYPE_DECAL` and treat `variant_flags` as `0`.
- Build the pipeline key explicitly (no guessing):
  - `type = SDR_TYPE_DECAL`
  - `variant_flags = 0`
  - `color_format = renderer_instance->getCurrentColorFormat()` (must be the G‑buffer format in decal mode)
  - `depth_format = renderer_instance->getDepthFormat()`
  - `sample_count = renderer_instance->getSampleCount()`
  - `color_attachment_count = renderer_instance->getCurrentColorAttachmentCount()` (must be 3 in decal mode)
  - `blend_mode = material_info->get_blend_mode()` (single blend mode applied to all attachments)
  - `layout_hash = layout->hash()`
- Descriptor writes (set 0):
  - binding 0: DecalGlobals UBO (buffer/offset/size from stored state)
  - binding 1: DecalInfo UBO (buffer/offset/size from stored state)
  - binding 2: diffuseMap (`TM_BASE_TYPE`)
  - binding 3: glowMap (`TM_GLOW_TYPE`)
  - binding 4: normalMap (`TM_NORMAL_TYPE`)
  - binding 5: depth sampler:
    - sampler = `renderTargets()->gbufferSampler()`
    - imageView = `renderTargets()->depthSampledView()`
    - imageLayout = `vk::ImageLayout::eDepthReadOnlyOptimal`
- Bind vertex buffers:
  - binding 0: `buffers.Vbuffer_handle`
  - binding 1: `instance_buffer`
- Bind index buffer:
  - `buffers.Ibuffer_handle`, type `vk::IndexType::eUint32`
- Draw:
  - `cmd.drawIndexed((uint32_t)num_elements, (uint32_t)num_instances, 0, 0, 0)`

Depth writes must be disabled.

### C) `gr_vulkan_stop_decal_pass()`
- Use the current frame command buffer:
  - `vk::CommandBuffer cmd = g_currentFrame->commandBuffer();`
- Restore the rendering session to normal deferred G‑buffer rendering:
  - `renderer_instance->renderingSession()->setMode(RenderMode::DeferredGBuffer)`
  - `renderer_instance->renderingSession()->endRendering(cmd)`
  - `renderer_instance->ensureRenderingStarted(cmd)`

## Acceptance (done when)
- Vulkan no longer uses decal stubs (`gf_start_decal_pass`, `gf_render_decals`, `gf_stop_decal_pass`).
- Decals draw into the 3‑attachment G‑buffer with depth bound read‑only.
- No validation errors about sampling depth while depth is writable.
- No “unsupported DecalGlobals/DecalInfo” logs.
