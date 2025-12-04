# Vulkan validation issues review (Dec 2025)

Scope: verification of three reported validation items (irrmap/post pipeline, default-material via `render_primitives`, and fragment outputs with a single color attachment). No code changes were made.

## Irrmap/Post pipeline (shaderType 30)
- Shader inputs: `code/def_files/data/effects/post-v.sdr` declares `vertPosition` at location 0 and `vertTexCoord` at location 2.
- Pipeline setup: `gr_vulkan.cpp` builds the `SDR_TYPE_IRRADIANCE_MAP_GEN` pipeline with `vertexLayoutHash = 0` (no vertex input, fullscreen triangle via `gl_VertexIndex`) in lines 641-647, then issues `cmdBuffer.draw(3, …)` without binding vertex buffers.
- Conclusion: validation complaints about missing locations 0/2 are accurate. Provide a fullscreen quad layout with position+texcoord (e.g., POSITION2 + TEX_COORD2) or swap the shader to generate position/texcoord from `gl_VertexIndex` so the expected attributes are present.

## Default-material via `render_primitives` (shaderType 17)
- Shader inputs: `code/def_files/data/effects/default-material.vert` requires position@0 and texcoord@2; color comes from a uniform, not a vertex attribute.
- Caller layouts: some paths build layouts without texcoords and with per-vertex color instead, e.g., `g3_render_primitives_colored` in `code/render/3ddraw.cpp:400-416` adds position@0 + color@1 only. That layout maps to locations 0 and 1, leaving location 2 missing while providing an unused location 1.
- Conclusion: validation warnings about unused loc1 and missing loc2 match the code. Feeding texcoords for these draws (and optionally keeping color@1) so the layout supplies pos@0 + texcoord@2 aligns the pipeline with the shader.

## Fragment outputs to extra attachments
- Shader outputs: `code/def_files/data/effects/main-f.sdr` writes to locations 0–4.
- Render targets: Vulkan pipelines are created with a single color attachment (`createPipeline` in `code/graphics/vulkan/VulkanPipelineManager.cpp:552-558` sets `colorAttachmentCount = 1` with one format).
- Conclusion: when only one color target is bound, validation warnings about writes to locations 1–4 are correct. Either bind matching MRT attachments for passes using these shaders or trim/ifdef the shader so only location 0 is written when a single target is active.
