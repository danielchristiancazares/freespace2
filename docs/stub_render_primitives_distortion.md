# GPT‑5.2 Pro Prompt: Vulkan `gf_render_primitives_distortion` + `gf_copy_effect_texture` (SDR_TYPE_EFFECT_DISTORTION)

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
Implement Vulkan so distortion primitives render instead of calling the stub. This requires:

- `gf_copy_effect_texture` producing a valid sampled 2D `frameBuffer` image.
- `gf_render_primitives_distortion` binding the required descriptors and drawing.

## Exact function signatures (must match)
Implement these functions in `code/graphics/vulkan/VulkanGraphics.cpp`:

```cpp
void gr_vulkan_copy_effect_texture();

void gr_vulkan_render_primitives_distortion(distortion_material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int offset,
	int n_verts,
	gr_buffer_handle buffer_handle);
```

Wire them in `graphics::vulkan::init_function_pointers()`:

- `gr_screen.gf_copy_effect_texture = gr_vulkan_copy_effect_texture;`
- `gr_screen.gf_render_primitives_distortion = gr_vulkan_render_primitives_distortion;`

## Required VulkanShaderManager support (must not throw)
File: `code/graphics/vulkan/VulkanShaderManager.cpp`

Add a `case` for `SDR_TYPE_EFFECT_DISTORTION` that loads:

- `effect-distort.vert.spv`
- `effect-distort.frag.spv`

Variant flags must be ignored for cache/module lookup (treat flags as `0`).

## Descriptor binding contract (set 0 push descriptors)
Use **set 0** bindings exactly:

- **binding 0 (UBO, std140)**: `matrixData { mat4 modelViewMatrix; mat4 projMatrix; }`
- **binding 1 (UBO, std140)**: `genericData` (see shader source below)
- **binding 2**: `baseMap` (`sampler2DArray`)
- **binding 3**: `depthMap` (`sampler2D`, Vulkan depth image)
- **binding 4**: `frameBuffer` (`sampler2D`, Vulkan scene composite copy)
- **binding 5**: `distMap` (`sampler2D`, neutral/static distortion map)

Do not create or use any other descriptor sets.

## VulkanDescriptorLayouts prerequisite (must exist or this cannot work)
File: `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

The standard per‑draw push descriptor set layout (**set 0**) must contain sampler bindings up to at least **binding 5**.

Implement it by expanding the per‑draw push layout to include sampler bindings **2..6** (`vk::DescriptorType::eCombinedImageSampler`, fragment stage), in addition to UBO bindings 0 and 1.

## Depth sampling legality (must be explicit)
This shader samples the Vulkan depth image.

To keep this implementation bounded and non‑tangential, distortion draws must obey these rules:

- **Do not attach the depth image** while rendering distortion. Rendering must be swapchain‑color‑only (no depth attachment) so the depth image can remain in `vk::ImageLayout::eShaderReadOnlyOptimal` for sampling.
- Depth test and depth writes must be disabled for this draw.

## Vulkan resources you must add (VulkanRenderTargets)
File: `code/graphics/vulkan/VulkanRenderTargets.h/.cpp`

Add two persistent, non‑bitmap Vulkan images owned by `VulkanRenderTargets`:

### A) Scene composite image (sampled 2D)
Purpose: this is the `frameBuffer` sampler input (binding 4).

Requirements:

- Extent: swapchain extent
- Format: `m_gbufferFormat` (same as G‑buffer 0)
- View type: `vk::ImageViewType::e2D`
- Usage flags: `vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc`

Add accessors:

- `vk::Image sceneCompositeImage() const`
- `vk::ImageView sceneCompositeView() const`

Initialization state (mandatory, to avoid incorrect barrier oldLayout):

- Add `bool m_sceneCompositeInitialized = false;` to `VulkanRenderTargets`.
- Add methods:
  - `bool isSceneCompositeInitialized() const`
  - `void markSceneCompositeInitialized()`

### B) Neutral distortion map (sampled 2D)
Purpose: `distMap` sampler input (binding 5). This avoids undefined sampling when you do not implement the animated/ping‑pong distortion system yet.

Requirements:

- Size: 32×32
- Format: `vk::Format::eR8G8B8A8Unorm`
- View type: `vk::ImageViewType::e2D`
- Usage flags: `vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst`
- Contents: RGBA = (128, 128, 0, 255) for every texel (so `(distortion - 0.5)` becomes ~0)

Add accessors:

- `vk::Image neutralDistortionImage() const`
- `vk::ImageView neutralDistortionView() const`

Initialization requirement (no ambiguity): after creating the neutral distortion image, you must clear/fill it once so its contents are deterministic.

Do this with Vulkan commands (not CPU writes): add `bool m_neutralDistortionInitialized = false;` and accessors in `VulkanRenderTargets`, and initialize it in `gr_vulkan_copy_effect_texture()` by recording:

- Barrier `neutralDistortionImage`: `eUndefined` → `eTransferDstOptimal`
- `vkCmdClearColorImage` to `vec4(0.5, 0.5, 0.0, 1.0)` over the full subresource range
- Barrier `neutralDistortionImage`: `eTransferDstOptimal` → `eShaderReadOnlyOptimal`
- `markNeutralDistortionInitialized()`

## Vulkan GLSL shader sources to create (exact contents)
Create these two files with exactly the following contents.

### `code/graphics/shaders/effect-distort.vert`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 vertPosition;
layout(location = 2) in vec4 vertTexCoord;
layout(location = 1) in vec4 vertColor;
layout(location = 6) in float vertRadius;

layout(location = 0) out vec4 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) out float fragOffset;

layout(binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

layout(binding = 1, std140) uniform genericData {
	float window_width;
	float window_height;
	float use_offset;
};

void main()
{
	gl_Position = projMatrix * modelViewMatrix * vertPosition;
	fragOffset = vertRadius * use_offset;
	fragTexCoord = vec4(vertTexCoord.xyz, 0.0);
	fragColor = vertColor;
}
```

### `code/graphics/shaders/effect-distort.frag`

```glsl
#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec4 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragOffset;

layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2DArray baseMap;
layout(binding = 3) uniform sampler2D depthMap;
layout(binding = 5) uniform sampler2D distMap;
layout(binding = 4) uniform sampler2D frameBuffer;

layout(binding = 1, std140) uniform genericData {
	float window_width;
	float window_height;
	float use_offset;
};

void main()
{
	vec2 depthCoord = vec2(gl_FragCoord.x / window_width, gl_FragCoord.y / window_height);
	vec4 fragmentColor = texture(baseMap, fragTexCoord.xyz) * fragColor.a;
	vec2 distortion = texture(distMap, fragTexCoord.xy + vec2(0.0, fragOffset)).rg;
	float alpha = clamp(dot(fragmentColor.rgb, vec3(0.3333)) * 10.0, 0.0, 1.0);
	distortion = ((distortion - 0.5) * 0.01) * alpha;
	fragOut0 = texture(frameBuffer, depthCoord + distortion);
	fragOut0.a = alpha;
}
```

Build system note (do not improvise): add `effect-distort.vert` and `effect-distort.frag` to the `VULKAN_SHADERS` list in `code/shaders.cmake` so they compile to `effect-distort.*.spv` and are embedded.

## Implement `gf_copy_effect_texture` (Vulkan)
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Implement `gr_vulkan_copy_effect_texture()` to produce a valid `sceneComposite` image:

- Use the current frame’s command buffer (`g_currentFrame->commandBuffer()`).
- End any active dynamic rendering scope first by calling `renderer_instance->renderingSession()->endRendering(cmd)`.
- Copy **G‑buffer attachment 0** (`renderTargets()->gbufferImage(0)`) into `renderTargets()->sceneCompositeImage()`.
- This function is defined to run while the G‑buffer is still being rendered, i.e. `gbuffer0` is in `vk::ImageLayout::eColorAttachmentOptimal` when the copy begins.
- Perform explicit layout transitions around the copy:
  - gbuffer0: `eColorAttachmentOptimal` → `eTransferSrcOptimal` → `eColorAttachmentOptimal`
  - sceneComposite:
    - if `renderTargets()->isSceneCompositeInitialized() == false`: `eUndefined` → `eTransferDstOptimal` → `eShaderReadOnlyOptimal`
    - else: `eShaderReadOnlyOptimal` → `eTransferDstOptimal` → `eShaderReadOnlyOptimal`
- Copy full extents via `vkCmdCopyImage` (no scaling).

This is the Vulkan analogue of the OpenGL path’s “copy current color attachment 0 into effect texture”.

## VulkanGraphics implementation requirements (distortion draw)
File: `code/graphics/vulkan/VulkanGraphics.cpp`

Mandatory behavior in `gr_vulkan_render_primitives_distortion(...)`:

- Use `shaderType = SDR_TYPE_EFFECT_DISTORTION`.
- Ensure deferred geometry has been ended before sampling depth:
  - call `renderer_instance->endDeferredGeometry(cmd)` before drawing (idempotent).
- Begin swapchain rendering without depth attachment for this draw:
  - Add this Vulkan‑only helper (exact signature) so `VulkanGraphics.cpp` can start the no‑depth swapchain rendering variant without needing access to the swapchain image index:
    - `code/graphics/vulkan/VulkanRenderer.h`: `void beginSwapchainRenderingNoDepth(vk::CommandBuffer cmd);`
    - `code/graphics/vulkan/VulkanRenderer.cpp`: implement it as `m_renderingSession->beginSwapchainRenderingNoDepth(cmd, m_recordingImage);`
  - Call `renderer_instance->beginSwapchainRenderingNoDepth(cmd)` before binding the pipeline/descriptors.
- Build the pipeline key explicitly (no guessing, and no depth attachment):
  - `type = SDR_TYPE_EFFECT_DISTORTION`
  - `variant_flags = 0`
  - `color_format = renderer_instance->getSwapChainImageFormat()`
  - `depth_format = VK_FORMAT_UNDEFINED`
  - `sample_count = renderer_instance->getSampleCount()`
  - `color_attachment_count = 1`
  - `blend_mode = material_info->get_blend_mode()`
  - `layout_hash = layout->hash()`
- Populate `genericData` (binding 1):
  - `window_width = (float)gr_screen.max_w`
  - `window_height = (float)gr_screen.max_h`
  - `use_offset = material_info->get_thruster_rendering() ? 1.0f : 0.0f`
- Bind textures:
  - binding 2 (`baseMap`): `material_info->get_texture_map(TM_BASE_TYPE)` via `renderer_instance->getTextureDescriptor(...)`
  - binding 3 (`depthMap`): `renderTargets()->depthSampledView()` with `renderTargets()->gbufferSampler()` and `imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal`
  - binding 4 (`frameBuffer`): `renderTargets()->sceneCompositeView()` with `renderTargets()->gbufferSampler()` and `imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal`
  - binding 5 (`distMap`): `renderTargets()->neutralDistortionView()` with `renderTargets()->gbufferSampler()` and `imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal`
- Bind the vertex buffer `buffer_handle` (byte offset 0) and draw:
  - `cmd.draw((uint32_t)n_verts, 1, (uint32_t)offset, 0)`

Depth test and depth writes must be disabled.

## Acceptance (done when)
- Vulkan no longer uses the distortion stub for `gf_render_primitives_distortion`.
- Vulkan no longer uses the stub for `gf_copy_effect_texture`.
- `VulkanShaderManager` loads `SDR_TYPE_EFFECT_DISTORTION` without throwing.
- Distortion draws have valid `frameBuffer` and `distMap` inputs and produce output without validation errors.
