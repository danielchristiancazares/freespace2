### Overview
Based on a comprehensive review of the provided CONTEXT.md (describing architecture, known bugs, and the "garbage scene rendering" issue), shader sources/disassemblies, code snippets (e.g., VulkanRenderer.cpp, VulkanGraphics.cpp, VulkanTexture.cpp, VulkanPipelineManager.cpp, VulkanShader.cpp, VulkanBuffer.cpp, VulkanFramebuffer.cpp, VulkanPostProcessing.cpp), web search results on common Vulkan bugs, and the Khronos Vulkan specification's "common mistakes" section, I've identified potential causes for the symptoms: **invisible ships** (likely model rendering failure), **distorted skybox showing as squares** (cubemap mishandling), and **red-tinged color space** (format/conversion errors). These appear interconnected, with root causes likely in texture/descriptor binding, shader mismatches, image layouts, and color handling—aligning with CONTEXT.md's "garbage rectangles" from wrong/uninitialized textures.

Symptoms match cross-platform issues (not MoltenVK-specific, per CONTEXT.md). I've prioritized high-likelihood causes based on evidence, with fixes. Issues are categorized by symptom, then by type (e.g., shaders, textures). This is exhaustive but focused on evidence; add validation layers (e.g., VK_LAYER_KHRONOS_validation) for runtime confirmation.

### 1. Invisible Ships (Models Not Rendering)
Likely due to model-specific failures in pipelines, bindings, or depth/culling. Ships use model shaders (SDR_TYPE_MODEL per VulkanShader.cpp), which may fail silently.

#### Shader/Pipeline Issues
- **Mismatch between source and compiled shaders**: default-material.frag source lacks lighting (uses texture + mix), but default-material.frag.spv.glsl disassembly includes SceneLightingData/LightingValues with dot products—suggesting wrong shader variant bound (perhaps OpenGL remnant). This could discard fragments or output black/transparent. (Evidence: Document mismatches; Khronos: Invalid shader module.)
  - **Fix**: Verify VulkanShaderCache loads correct SPIR-V for SDR_TYPE_MODEL. Compare hashes of preprocessed GLSL vs. cached SPIR-V. Recompile with shaderc ensuring no #include "gamma.sdr" mismatches.

- **Clip plane discarding geometry**: default-material.vert sets gl_ClipDistance if clipEnabled, but disassembly defaults to 1.0 if disabled. If clipEquation is invalid or clipEnabled mismatches, ships clip out. (Evidence: VulkanRenderer.h tracks clipEnabled; Khronos: Geometry discarded outside clip volume.)
  - **Fix**: Audit clipEnabled in VulkanGraphics.cpp (s_clip_enabled=false default); ensure dot(modelMatrix * vertPosition, clipEquation) >0 for visible ships.

- **Culling/depth misconfiguration**: s_cull_mode=1 (backface) in VulkanGraphics.cpp, but pipelines inherit this. Wrong winding or depthMode could cull ships. (Evidence: Web search: Models "inside out" from culling; Khronos: Cull mode discards faces.)
  - **Fix**: Set cullMode=VK_CULL_MODE_NONE in PipelineKey for models; verify depthMode in VulkanPipelineManager.cpp matches OpenGL behavior.

- **Vertex buffer binding failures**: VulkanBufferManager may not bind/update buffers if frame sync (lastUsedFrame) mismatches, leading to no geometry. (Evidence: Khronos: Vertex buffers not bound; CONTEXT.md: Buffer manager issues possible.)
  - **Fix**: In VulkanGraphics.cpp, ensure gr_vulkan_bind_uniform_buffer calls bindBufferData with correct offsets; add logging for boundVertexBuffer in DrawState.

#### Texture/Descriptor Issues
- **Uninitialized/missing textures**: Ships invisible if baseMap unbound or in UNDEFINED layout. (Evidence: CONTEXT.md: Garbage from uninitialized textures; Khronos: Descriptor set not bound.)
  - **Fix**: Audit VulkanTextureManager::bindTexture; ensure createRenderTarget sets VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL before binding.

- **Descriptor set mismatches**: VulkanDescriptorManager uses single pool; if sets not updated/bound, shaders read garbage (invisible/black). (Evidence: CONTEXT.md next steps: Investigate descriptors; Khronos: Descriptor layout mismatch.)
  - **Fix**: In VulkanPipelineManager::bindPipeline, verify vkCmdBindDescriptorSets includes all sets from ShaderReflectionData; log bound textures.

#### Other
- **Render pass attachment issues**: Scene pass uses dynamic rendering (no VkFramebuffer); if currentColorView/currentDepthView undefined, models skip. (Evidence: VulkanRenderer.cpp: m_currentDepthView; Khronos: Invalid attachment layout.)
  - **Fix**: In beginScenePass, ensure m_currentDepthFormat != eUndefined and transition to DEPTH_STENCIL_ATTACHMENT_OPTIMAL.

### 2. Distorted Skybox Showing as Squares
Skyboxes use cubemaps (VulkanTexture.cpp supports cubemap creation). Distortion as "squares" suggests 2D sampling of cubemap or view type errors, rendering faces as flat quads.

#### Cubemap-Specific Issues
- **Non-cube view binding**: samplerCube bound to non-cube view (fixed per CONTEXT.md, but perhaps incomplete). If image lacks VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT or viewType != VK_IMAGE_VIEW_TYPE_CUBE, renders as 2D squares. (Evidence: CONTEXT.md bug fix; Web snippets: MoltenVK distortion, flipped faces; Khronos: View type mismatch VUID-07752.)
  - **Fix**: In VulkanTexture::create, enforce flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT for arrayLayers=6; verify viewType in createImageView.

- **Coordinate transformation errors**: Vulkan cubemaps need Z scaled by -1 for sampling (vs. OpenGL). Disassembled vulkan.vert.spv.glsl uses _20 positions without flip. (Evidence: Web snippets: SaschaWillems issues #679/#232—transform skybox coords; Khronos: Shader coordinate system flip.)
  - **Fix**: In skybox shader (likely SDR_TYPE_EFFECT_DISTORTION or model), add vec3(pos.x, pos.y, -pos.z) before sampling textureCube.

- **Layer/face order mismatches**: Cubemap layers not consecutive or ordered wrong (e.g., +X/-X swapped), causing squared distortion. (Evidence: Web snippets: Flipped Yokohama images; Khronos: baseArrayLayer wrong.)
  - **Fix**: In VulkanTextureManager::createCubemap, ensure arrayLayers=6 and baseArrayLayer=0; log face uploads.

#### Sampling/Texture Issues
- **Sampler mismatch**: sampler2D bound to 2D_ARRAY (fixed per CONTEXT.md VUID-07752), but if lingering, cubemaps sample as arrays (squares). Unnormalized coords with cubemaps distort. (Evidence: CONTEXT.md fix; Khronos: unnormalizedCoordinates VUID-08609.)
  - **Fix**: In VulkanTextureManager::getSampler, set unnormalizedCoordinates=VK_FALSE for cubemaps; audit bindSampler.

- **Mipmap/layout failures**: No mipmaps or layout not SHADER_READ_ONLY_OPTIMAL per face, causing pixelated squares. (Evidence: Web snippets: Pixelated skybox at 1024x1024; Khronos: Incomplete mip chain.)
  - **Fix**: Generate mipLevels>1 in create; transition each face separately in recordLayoutTransition.

#### Render Pass Issues
- **Depth/multisampling incompatibility**: Skybox render pass lacks depth (auxiliaryPassActive), but scene framebuffer has it—causing incompatibility when blitting. (Evidence: Web snippets: Skybox glitches without depth; Khronos: Attachment mismatch.)
  - **Fix**: Use dummy depth attachment for skybox compatibility in VulkanFramebuffer::create; disable depth for skybox pipeline.

### 3. Red-Tinged Color Space (Overall Tint/Garbage)
Red/yellow blobs in rectangles match CONTEXT.md's garbage (wrong textures), but tint suggests color format or gamma bugs. Passthrough blit ignores conversions.

#### Color Format/Conversion Issues
- **sRGB/linear mismatch**: Scene in linear (R16G16B16A16_SFLOAT per VulkanPostProcessing.h), but swapchain sRGB—passthrough in vulkan_blit.frag causes washed-out/red tint (no gamma). default-material.frag has srgb_to_linear, but if srgb=0 incorrectly, skips. (Evidence: Web search: Color space different on swapchain; Khronos: No gamma correction.)
  - **Fix**: In recordBlitToSwapchain, add gamma correction (pow(color, 1/2.2)); set swapchain colorSpace=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR.

- **Format swapping (BGRA/RGBA)**: If scene BGRA but treated as RGBA, red/blue swap—red tint if blue-dominant (e.g., skies). CONTEXT.md says format-agnostic, but perhaps not fully tested. (Evidence: CONTEXT.md: BGRA/RGBA same wrong output; Khronos: Swapchain format mismatch.)
  - **Fix**: In VulkanRenderer::recreateSwapChain, query preferred format; add optional R/B swap in blit if detected.

- **Baked-in tint from effects**: Bloom/tonemapping over-amplifies red (m_bloomIntensity=0.25f). If bloom textures uninitialized, red blobs. (Evidence: Web snippets: Red tint baked in cubemaps from damage; VulkanPostProcessing.cpp: Bloom enabled.)
  - **Fix**: Disable bloom (m_bloomEnabled=false) for testing; ensure bloom textures initialized in initialize.

#### Texture/Binding Garbage
- **Wrong/uninitialized texture bindings**: Garbage rectangles from sampling unbound/uninit VRAM (red/yellow common for GPU defaults). (Evidence: CONTEXT.md key findings: Samplers bound to wrong textures; Khronos: Garbage from UNDEFINED layout.)
  - **Fix**: Implement CONTEXT.md next steps: Add logging in VulkanTextureManager::bindTexture for handles; audit initialization in createTexture.

- **Layout transition failures**: Scene color stays COLOR_ATTACHMENT_OPTIMAL post-render, not transitioned for sampling—garbage/tint. Failed attempts in CONTEXT.md didn't fix, but extended tracking hung. (Evidence: CONTEXT.md: Layout agnostic, but symptom not root; Khronos: No transition after render pass.)
  - **Fix**: In endScenePass, insert VkImageMemoryBarrier (old= COLOR_ATTACHMENT_OPTIMAL, new=SHADER_READ_ONLY_OPTIMAL) using m_currentColorImage; avoid infinite loops by limiting barriers.

#### Descriptor/Sync Issues
- **Descriptor binding errors**: Sets not updated after texture changes, reading stale/red data. (Evidence: CONTEXT.md: Investigate descriptors; Khronos: Descriptor not bound VUID-08878.)
  - **Fix**: In VulkanDescriptorManager::allocateDescriptorSet, log bindings; ensure vkUpdateDescriptorSets post-texture upload.

- **Synchronization misses**: No barriers between scene writes and blit reads, causing garbage. (Evidence: CONTEXT.md: Timing issue corrupts state; Khronos: No barrier for compute/graphics.)
  - **Fix**: In submitTransfers, add VkMemoryBarrier (ACCESS_SHADER_WRITE_BIT to READ_BIT); use semaphores in flip.

### Cross-Cutting Issues
- **Dynamic rendering bugs**: Pipelines use renderPass=nullptr but specify formats via pNext; if m_currentColorFormat undefined, all rendering fails. (Evidence: VulkanRenderer.cpp: Dynamic state; Khronos: Attachment format mismatch.)
  - **Fix**: Set formats explicitly in ensureRenderPassActive.

- **Post-processing interference**: Bloom/tonemapping (VulkanPostProcessing.cpp) could tint red if exposure=1.0f wrong; uninit bloom textures cause blobs. (Evidence: Web search: Blending disabled on textures; Khronos: Blend on non-blendable format.)
  - **Fix**: Test with post-processing disabled (isInitialized=false).

- **Validation/feature gaps**: No mention of enabled features (e.g., nullDescriptor for unbound); could crash/invisible. (Evidence: Khronos: Feature not enabled.)
  - **Fix**: In PhysicalDeviceValues, enable Vulkan 1.4 features; run with validation layers.

### Recommendations
- **Immediate Tests**: Compare GL vs. Vulkan outputs (per CONTEXT.md); disable features one-by-one (e.g., bloom, culling).
- **Debugging**: Add logging for bindings/layouts (as suggested); use RenderDoc for frame capture.
- **Potential Root**: Descriptor/texture binding + layout mismatches, causing all symptoms via garbage sampling.
If more code/docs needed, provide for deeper analysis.
