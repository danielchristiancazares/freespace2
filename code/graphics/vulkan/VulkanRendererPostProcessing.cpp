#include "VulkanClip.h"
#include "VulkanFrameCaps.h"
#include "VulkanRenderer.h"
#include "VulkanSync2Helpers.h"
#include "freespace.h"
#include "graphics/matrix.h"
#include "graphics/util/uniform_structs.h"
#include "io/timer.h"
#include "lighting/lighting.h"
#include "lighting/lighting_profiles.h"
#include "osapi/outwnd.h"

// Reuse the engine's canonical SMAA lookup textures (precomputed area/search).
// These are tiny immutable resources and are backend-agnostic.
#include "graphics/opengl/SmaaAreaTex.h"
#include "graphics/opengl/SmaaSearchTex.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace graphics {
namespace vulkan {

void VulkanRenderer::beginSceneTexture(const FrameCtx &ctx, bool enableHdrPipeline) {
  Assertion(&ctx.renderer == this, "beginSceneTexture called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "beginSceneTexture called before rendering session initialization");

  // Mirror OpenGL's Scene_framebuffer_in_frame guard: if already active, ignore.
  if (m_sceneTexture.has_value()) {
    return;
  }

  // Boundary state (used by deferred to decide output target).
  m_sceneTexture = SceneTextureState{enableHdrPipeline};

  // Clear the scene target at frame start (color+depth); OpenGL does this in scene_texture_begin.
  m_renderingSession->requestClear();

  // Route subsequent rendering to the scene HDR target; tonemapping becomes passthrough when HDR is disabled.
  m_renderingSession->requestSceneHdrTarget();

  // Begin rendering immediately so the requested clear executes even if the scene draws nothing.
  (void)ensureRenderingStarted(ctx);
}

void VulkanRenderer::copySceneEffectTexture(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this,
            "copySceneEffectTexture called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "copySceneEffectTexture called before rendering session initialization");

  if (!m_sceneTexture.has_value()) {
    // OpenGL is a no-op if not in a scene framebuffer.
    return;
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  m_renderingSession->copySceneHdrToEffect(cmd);
}

void VulkanRenderer::endSceneTexture(const FrameCtx &ctx, bool enablePostProcessing) {
  Assertion(&ctx.renderer == this, "endSceneTexture called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "endSceneTexture called before rendering session initialization");

  if (!m_sceneTexture.has_value()) {
    return;
  }
  const bool hdrEnabled = m_sceneTexture->hdrEnabled;

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  // Preserve scissor across the internal fullscreen passes; UI and some draw paths rely on it.
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D restoreScissor{};
  restoreScissor.offset = vk::Offset2D{clip.x, clip.y};
  restoreScissor.extent = vk::Extent2D{clip.width, clip.height};

  // End any active scene rendering. Post-processing is a chain of fullscreen passes.
  m_renderingSession->suspendRendering();
  m_renderingSession->transitionSceneHdrToShaderRead(cmd);

  VulkanFrame &frame = ctx.frame();

  // Bloom pass (HDR): Scene HDR -> bloom (half-res) -> blur -> additive composite back into Scene HDR.
  if (enablePostProcessing && gr_bloom_intensity() > 0) {
    const auto fullExtent = m_vulkanDevice->swapchainExtent();
    const vk::Extent2D bloomExtent{std::max(1u, fullExtent.width >> 1), std::max(1u, fullExtent.height >> 1)};

    // Bright pass -> bloom[0] mip0 (clear before writing).
    m_renderingSession->requestClear();
    m_renderingSession->requestBloomMipTarget(0, 0);
    auto bright = ensureRenderingStarted(ctx);
    recordBloomBrightPass(bright, frame);

    // Generate mip chain from bright-pass output (required for blur).
    m_renderingSession->suspendRendering();
    generateBloomMipmaps(cmd, 0, bloomExtent);

    // Blur passes: ping-pong between bloom[0] and bloom[1] across mips.
    for (int iteration = 0; iteration < 2; ++iteration) {
      // Vertical: src=0 -> dst=1
      m_renderingSession->transitionBloomToShaderRead(cmd, 0);
      for (uint32_t mip = 0; mip < VulkanRenderTargets::kBloomMipLevels; ++mip) {
        const uint32_t bw = std::max(1u, bloomExtent.width >> mip);
        const uint32_t bh = std::max(1u, bloomExtent.height >> mip);
        m_renderingSession->requestBloomMipTarget(1, mip);
        auto passRender = ensureRenderingStarted(ctx);
        recordBloomBlurPass(passRender, frame, 0, SDR_FLAG_BLUR_VERTICAL, static_cast<int>(mip), bw, bh);
      }
      m_renderingSession->suspendRendering();

      // Horizontal: src=1 -> dst=0
      m_renderingSession->transitionBloomToShaderRead(cmd, 1);
      for (uint32_t mip = 0; mip < VulkanRenderTargets::kBloomMipLevels; ++mip) {
        const uint32_t bw = std::max(1u, bloomExtent.width >> mip);
        const uint32_t bh = std::max(1u, bloomExtent.height >> mip);
        m_renderingSession->requestBloomMipTarget(0, mip);
        auto passRender = ensureRenderingStarted(ctx);
        recordBloomBlurPass(passRender, frame, 1, SDR_FLAG_BLUR_HORIZONTAL, static_cast<int>(mip), bw, bh);
      }
      m_renderingSession->suspendRendering();
    }

    // Composite bloom back into the HDR scene target.
    m_renderingSession->transitionBloomToShaderRead(cmd, 0);
    m_renderingSession->requestSceneHdrNoDepthTarget();
    auto composite = ensureRenderingStarted(ctx);
    recordBloomCompositePass(composite, frame, VulkanRenderTargets::kBloomMipLevels);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSceneHdrToShaderRead(cmd);
  }

  // Tonemapping: Scene HDR -> post LDR (RGBA8).
  m_renderingSession->requestPostLdrTarget();
  auto ldrRender = ensureRenderingStarted(ctx);

  // If the HDR pipeline is disabled, the tonemapper is set to passthrough.
  recordTonemappingToSwapchain(ldrRender, frame, hdrEnabled);

  // End tonemapping pass; subsequent post steps sample the LDR target.
  m_renderingSession->suspendRendering();
  m_renderingSession->transitionPostLdrToShaderRead(cmd);

  vk::DescriptorImageInfo ldrInfo{};
  ldrInfo.sampler = m_renderTargets->postLinearSampler();
  ldrInfo.imageView = m_renderTargets->postLdrView();
  ldrInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  bool ldrIsSmaaOutput = false;

  // SMAA (LDR): optional edge detection + blending weights + neighborhood blending.
  if (enablePostProcessing && gr_is_smaa_mode(Gr_aa_mode)) {
    Assertion(m_smaaAreaTex.view, "SMAA area texture must be initialized");
    Assertion(m_smaaSearchTex.view, "SMAA search texture must be initialized");

    // Edge detection: postLdr -> smaaEdges
    m_renderingSession->requestSmaaEdgesTarget();
    auto edgeRender = ensureRenderingStarted(ctx);
    recordSmaaEdgePass(edgeRender, frame, ldrInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSmaaEdgesToShaderRead(cmd);

    vk::DescriptorImageInfo edgesInfo{};
    edgesInfo.sampler = m_renderTargets->postLinearSampler();
    edgesInfo.imageView = m_renderTargets->smaaEdgesView();
    edgesInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::DescriptorImageInfo areaInfo{};
    areaInfo.sampler = m_renderTargets->postLinearSampler();
    areaInfo.imageView = m_smaaAreaTex.view.get();
    areaInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::DescriptorImageInfo searchInfo{};
    searchInfo.sampler = m_renderTargets->postLinearSampler();
    searchInfo.imageView = m_smaaSearchTex.view.get();
    searchInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    // Blending weights: edges -> smaaBlend
    m_renderingSession->requestSmaaBlendTarget();
    auto blendRender = ensureRenderingStarted(ctx);
    recordSmaaBlendWeightsPass(blendRender, frame, edgesInfo, areaInfo, searchInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSmaaBlendToShaderRead(cmd);

    vk::DescriptorImageInfo blendInfo{};
    blendInfo.sampler = m_renderTargets->postLinearSampler();
    blendInfo.imageView = m_renderTargets->smaaBlendView();
    blendInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    // Neighborhood blending: postLdr + blend -> smaaOutput
    m_renderingSession->requestSmaaOutputTarget();
    auto nbRender = ensureRenderingStarted(ctx);
    recordSmaaNeighborhoodPass(nbRender, frame, ldrInfo, blendInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSmaaOutputToShaderRead(cmd);

    // Use SMAA output for final resolve.
    ldrInfo.imageView = m_renderTargets->smaaOutputView();
    ldrIsSmaaOutput = true;
  } else if (enablePostProcessing && gr_is_fxaa_mode(Gr_aa_mode)) {
    // FXAA (LDR): prepass converts RGB -> RGBL, then FXAA writes back into postLdr.
    m_renderingSession->requestPostLuminanceTarget();
    auto pre = ensureRenderingStarted(ctx);
    recordFxaaPrepass(pre, frame, ldrInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionPostLuminanceToShaderRead(cmd);

    vk::DescriptorImageInfo lumInfo{};
    lumInfo.sampler = m_renderTargets->postLinearSampler();
    lumInfo.imageView = m_renderTargets->postLuminanceView();
    lumInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    m_renderingSession->requestPostLdrTarget();
    auto fxaa = ensureRenderingStarted(ctx);
    recordFxaaPass(fxaa, frame, lumInfo);

    m_renderingSession->suspendRendering();
    m_renderingSession->transitionPostLdrToShaderRead(cmd);
  }

  // Lightshafts (LDR): additive pass using scene + cockpit depth.
  {
    // These are declared in freespace.cpp and used by OpenGL postprocessing.
    extern float Sun_spot;

    if (enablePostProcessing && !Game_subspace_effect && gr_sunglare_enabled() && gr_lightshafts_enabled() &&
        Sun_spot > 0.0f) {
      const int n_lights = light_get_global_count();
      for (int idx = 0; idx < n_lights; ++idx) {
        vec3d light_dir;
        if (!light_get_global_dir(&light_dir, idx)) {
          continue;
        }
        if (!light_has_glare(idx)) {
          continue;
        }

        const float dot = vm_vec_dot(&light_dir, &Eye_matrix.vec.fvec);
        if (dot <= 0.7f) {
          continue;
        }

        const float x = asinf_safe(vm_vec_dot(&light_dir, &Eye_matrix.vec.rvec)) / PI * 1.5f + 0.5f;
        const float y =
            asinf_safe(vm_vec_dot(&light_dir, &Eye_matrix.vec.uvec)) / PI * 1.5f * gr_screen.clip_aspect + 0.5f;

        graphics::generic_data::lightshaft_data ls{};
        ls.sun_pos.x = x;
        ls.sun_pos.y = y;
        if (graphics::Post_processing_manager != nullptr) {
          const auto &ls_params = graphics::Post_processing_manager->getLightshaftParams();
          ls.density = ls_params.density;
          ls.falloff = ls_params.falloff;
          ls.weight = ls_params.weight;
          ls.intensity = Sun_spot * ls_params.intensity;
          ls.cp_intensity = Sun_spot * ls_params.cpintensity;
          ls.samplenum = ls_params.samplenum;
        } else {
          // Reasonable defaults if the table wasn't loaded.
          ls.density = 0.5f;
          ls.falloff = 1.0f;
          ls.weight = 0.02f;
          ls.intensity = Sun_spot * 0.5f;
          ls.cp_intensity = Sun_spot * 0.5f;
          ls.samplenum = 50;
        }

        // Defensive clamp: avoid pathological stalls/hangs if a table sets an extreme sample count.
        // OpenGL bakes SAMPLE_NUM into the shader at compile time; Vulkan uses a uniform loop bound.
        const int requestedSamples = ls.samplenum;
        CLAMP(ls.samplenum, 1, 128);
        if (ls.samplenum != requestedSamples) {
          mprintf(("Vulkan lightshafts: clamping sample count %d -> %d\n", requestedSamples, ls.samplenum));
        }

        // Depth textures must be shader-readable for sampling.
        m_renderingSession->transitionMainDepthToShaderRead(cmd);
        m_renderingSession->transitionCockpitDepthToShaderRead(cmd);

        // Render into the current LDR buffer (postLdr or SMAA output) with additive blending.
        if (ldrIsSmaaOutput) {
          m_renderingSession->requestSmaaOutputTarget();
        } else {
          m_renderingSession->requestPostLdrTarget();
        }
        auto lsRender = ensureRenderingStarted(ctx);
        recordLightshaftsPass(lsRender, frame, ls);

        m_renderingSession->suspendRendering();
        if (ldrIsSmaaOutput) {
          m_renderingSession->transitionSmaaOutputToShaderRead(cmd);
        } else {
          m_renderingSession->transitionPostLdrToShaderRead(cmd);
        }

        // Only render the first qualifying glare light (OpenGL parity).
        break;
      }
    }
  }

  // Post effects: apply configured post-processing effects to swapchain. If no effects are active, copy the LDR buffer.
  bool doPostEffects = false;
  graphics::generic_data::post_data post{};
  post.timer = i2fl(timer_get_milliseconds() % 100 + 1);
  post.noise_amount = 0.0f;
  // Identity defaults (effects disabled unless explicitly enabled)
  post.saturation = 1.0f;
  post.brightness = 1.0f;
  post.contrast = 1.0f;
  post.film_grain = 0.0f;
  post.tv_stripes = 0.0f;
  post.cutoff = 0.0f;
  post.tint = vmd_zero_vector;
  post.dither = 0.0f;
  post.custom_effect_vec3_a = vmd_zero_vector;
  post.custom_effect_float_a = 0.0f;
  post.custom_effect_vec3_b = vmd_zero_vector;
  post.custom_effect_float_b = 0.0f;

  if (enablePostProcessing && graphics::Post_processing_manager != nullptr) {
    const auto &effects = graphics::Post_processing_manager->getPostEffects();
    for (const auto &eff : effects) {
      // Match OpenGL semantics: effects are only applied when flagged on (always_on OR intensity != default).
      const bool enabled = eff.always_on || eff.intensity != eff.default_intensity;
      if (!enabled) {
        continue;
      }
      doPostEffects = true;

      switch (eff.uniform_type) {
      case graphics::PostEffectUniformType::NoiseAmount:
        post.noise_amount = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Saturation:
        post.saturation = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Brightness:
        post.brightness = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Contrast:
        post.contrast = eff.intensity;
        break;
      case graphics::PostEffectUniformType::FilmGrain:
        post.film_grain = eff.intensity;
        break;
      case graphics::PostEffectUniformType::TvStripes:
        post.tv_stripes = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Cutoff:
        post.cutoff = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Dither:
        post.dither = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Tint:
        post.tint = eff.rgb;
        break;
      case graphics::PostEffectUniformType::CustomEffectVEC3A:
        post.custom_effect_vec3_a = eff.rgb;
        break;
      case graphics::PostEffectUniformType::CustomEffectFloatA:
        post.custom_effect_float_a = eff.intensity;
        break;
      case graphics::PostEffectUniformType::CustomEffectVEC3B:
        post.custom_effect_vec3_b = eff.rgb;
        break;
      case graphics::PostEffectUniformType::CustomEffectFloatB:
        post.custom_effect_float_b = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Invalid:
      default:
        break;
      }
    }
  }

  // Ensure the main scene depth is shader-readable for any custom effects.
  m_renderingSession->transitionMainDepthToShaderRead(cmd);

  vk::DescriptorImageInfo depthInfo{};
  depthInfo.sampler = m_renderTargets->depthSampler();
  depthInfo.imageView = m_renderTargets->depthSampledView();
  depthInfo.imageLayout = m_renderTargets->depthReadLayout();

  // Final: write to swapchain (no depth attachment; post passes may sample depth).
  m_renderingSession->requestSwapchainNoDepthTarget();
  auto swap = ensureRenderingStarted(ctx);
  if (enablePostProcessing && doPostEffects) {
    recordPostEffectsPass(swap, frame, post, ldrInfo, depthInfo);
  } else {
    recordCopyToSwapchain(swap, ldrInfo);
  }

  // Restore clip scissor for any subsequent UI draws.
  cmd.setScissor(0, 1, &restoreScissor);

  // Exit scene texture mode: subsequent UI draws go directly to swapchain.
  m_sceneTexture.reset();
}

void VulkanRenderer::recordTonemappingToSwapchain(const RenderCtx &render, VulkanFrame &frame, bool hdrEnabled) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordTonemappingToSwapchain called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordTonemappingToSwapchain requires render targets");
  Assertion(m_bufferManager != nullptr, "recordTonemappingToSwapchain requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordTonemappingToSwapchain requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordTonemappingToSwapchain requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  // Preserve scissor across the internal fullscreen pass.
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D restoreScissor{};
  restoreScissor.offset = vk::Offset2D{clip.x, clip.y};
  restoreScissor.extent = vk::Extent2D{clip.width, clip.height};

  // Fullscreen draw state.
  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise); // Matches Y-flipped viewport convention
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_TONEMAPPING);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_TONEMAPPING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Build tonemapping uniforms (genericData binding 1).
  graphics::generic_data::tonemapping_data data{};
  if (hdrEnabled) {
    const auto ppc = lighting_profiles::current_piecewise_intermediates();
    data.tonemapper = static_cast<int>(lighting_profiles::current_tonemapper());
    data.sh_B = ppc.sh_B;
    data.sh_lnA = ppc.sh_lnA;
    data.sh_offsetX = ppc.sh_offsetX;
    data.sh_offsetY = ppc.sh_offsetY;
    data.toe_B = ppc.toe_B;
    data.toe_lnA = ppc.toe_lnA;
    data.x0 = ppc.x0;
    data.x1 = ppc.x1;
    data.y0 = ppc.y0;
    data.exposure = lighting_profiles::current_exposure();
  } else {
    // Passthrough: no HDR pipeline => clamp only.
    data.tonemapper = static_cast<int>(lighting_profiles::TonemapperAlgorithm::Linear);
    data.exposure = 1.0f;
  }

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  // Scene HDR sampler (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneHdrSampler();
  sceneInfo.imageView = m_renderTargets->sceneHdrView();
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &sceneInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);

  // Restore clip scissor for any subsequent UI draws.
  cmd.setScissor(0, 1, &restoreScissor);
}

void VulkanRenderer::recordBloomBrightPass(const RenderCtx &render, VulkanFrame & /*frame*/) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordBloomBrightPass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordBloomBrightPass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordBloomBrightPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordBloomBrightPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordBloomBrightPass requires pipeline manager");

  // Bright pass renders at half resolution (mip 0 of bloom texture).
  const auto fullExtent = m_vulkanDevice->swapchainExtent();
  const vk::Extent2D extent{std::max(1u, fullExtent.width >> 1), std::max(1u, fullExtent.height >> 1)};

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_BRIGHTPASS);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_BRIGHTPASS;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Input: scene HDR (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneHdrSampler();
  sceneInfo.imageView = m_renderTargets->sceneHdrView();
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.descriptorCount = 1;
  write.pImageInfo = &sceneInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, 1, &write);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordBloomBlurPass(const RenderCtx &render, VulkanFrame &frame, uint32_t srcPingPongIndex,
                                         uint32_t variantFlags, int mipLevel, uint32_t bloomWidth,
                                         uint32_t bloomHeight) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordBloomBlurPass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordBloomBlurPass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordBloomBlurPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordBloomBlurPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordBloomBlurPass requires pipeline manager");

  const vk::Extent2D extent{bloomWidth, bloomHeight};

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_BLUR, variantFlags);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_BLUR;
  key.variant_flags = variantFlags;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // genericData (binding 1): blur params.
  graphics::generic_data::blur_data data{};
  if (variantFlags & SDR_FLAG_BLUR_HORIZONTAL) {
    data.texSize = (bloomWidth > 0) ? (1.0f / static_cast<float>(bloomWidth)) : 0.0f;
  } else {
    data.texSize = (bloomHeight > 0) ? (1.0f / static_cast<float>(bloomHeight)) : 0.0f;
  }
  data.level = mipLevel;

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  // Input bloom texture (binding 2): full mip chain view.
  vk::DescriptorImageInfo bloomInfo{};
  bloomInfo.sampler = m_renderTargets->postLinearSampler();
  bloomInfo.imageView = m_renderTargets->bloomView(srcPingPongIndex);
  bloomInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &bloomInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordBloomCompositePass(const RenderCtx &render, VulkanFrame &frame, int mipLevels) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordBloomCompositePass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordBloomCompositePass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordBloomCompositePass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordBloomCompositePass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordBloomCompositePass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_BLOOM_COMP);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_BLOOM_COMP;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Enable blending for the single attachment if supported by dynamic state.
  if (m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable) {
    vk::Bool32 enable = VK_TRUE;
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &enable));
  }

  graphics::generic_data::bloom_composition_data data{};
  data.levels = mipLevels;
  data.bloom_intensity = gr_bloom_intensity() / 100.0f;

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  vk::DescriptorImageInfo bloomInfo{};
  bloomInfo.sampler = m_renderTargets->postLinearSampler();
  bloomInfo.imageView = m_renderTargets->bloomView(0);
  bloomInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &bloomInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);

  // Restore blending disabled for subsequent passes if dynamic state is used.
  if (m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable) {
    vk::Bool32 disable = VK_FALSE;
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &disable));
  }
}

void VulkanRenderer::recordSmaaEdgePass(const RenderCtx &render, VulkanFrame &frame,
                                        const vk::DescriptorImageInfo &colorInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordSmaaEdgePass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordSmaaEdgePass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordSmaaEdgePass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordSmaaEdgePass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_SMAA_EDGE);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_SMAA_EDGE;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::smaa_data data{};
  data.smaa_rt_metrics.x = static_cast<float>(extent.width);
  data.smaa_rt_metrics.y = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &colorInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordSmaaBlendWeightsPass(const RenderCtx &render, VulkanFrame &frame,
                                                const vk::DescriptorImageInfo &edgesInput,
                                                const vk::DescriptorImageInfo &areaTex,
                                                const vk::DescriptorImageInfo &searchTex) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordSmaaBlendWeightsPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordSmaaBlendWeightsPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordSmaaBlendWeightsPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordSmaaBlendWeightsPass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::smaa_data data{};
  data.smaa_rt_metrics.x = static_cast<float>(extent.width);
  data.smaa_rt_metrics.y = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 4> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &edgesInput;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &areaTex;

  writes[3].dstBinding = 4;
  writes[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[3].descriptorCount = 1;
  writes[3].pImageInfo = &searchTex;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordSmaaNeighborhoodPass(const RenderCtx &render, VulkanFrame &frame,
                                                const vk::DescriptorImageInfo &colorInput,
                                                const vk::DescriptorImageInfo &blendTex) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordSmaaNeighborhoodPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordSmaaNeighborhoodPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordSmaaNeighborhoodPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordSmaaNeighborhoodPass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::smaa_data data{};
  data.smaa_rt_metrics.x = static_cast<float>(extent.width);
  data.smaa_rt_metrics.y = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 3> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &colorInput;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &blendTex;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordFxaaPrepass(const RenderCtx &render, VulkanFrame & /*frame*/,
                                       const vk::DescriptorImageInfo &ldrInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordFxaaPrepass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordFxaaPrepass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordFxaaPrepass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordFxaaPrepass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_FXAA_PREPASS);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_FXAA_PREPASS;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.descriptorCount = 1;
  write.pImageInfo = &ldrInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, 1, &write);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordFxaaPass(const RenderCtx &render, VulkanFrame &frame,
                                    const vk::DescriptorImageInfo &luminanceInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordFxaaPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordFxaaPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordFxaaPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordFxaaPass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_FXAA);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_FXAA;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::fxaa_data data{};
  data.rt_w = static_cast<float>(extent.width);
  data.rt_h = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &luminanceInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordLightshaftsPass(const RenderCtx &render, VulkanFrame &frame,
                                           const graphics::generic_data::lightshaft_data &params) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordLightshaftsPass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordLightshaftsPass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordLightshaftsPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordLightshaftsPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordLightshaftsPass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_LIGHTSHAFTS);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_LIGHTSHAFTS;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Our pipelines use VK_EXT_extended_dynamic_state3 for colorBlendEnable when available.
  // VulkanRenderingSession::applyDynamicState sets a baseline of blending OFF at pass start,
  // so we must explicitly enable it for additive passes like lightshafts.
  if (m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable) {
    vk::Bool32 enable = VK_TRUE;
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &enable));
  }

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(params), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &params, sizeof(params));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(params);

  vk::DescriptorImageInfo sceneDepth{};
  sceneDepth.sampler = m_renderTargets->depthSampler();
  sceneDepth.imageView = m_renderTargets->depthSampledView();
  sceneDepth.imageLayout = m_renderTargets->depthReadLayout();

  vk::DescriptorImageInfo cockpitDepth{};
  cockpitDepth.sampler = m_renderTargets->depthSampler();
  cockpitDepth.imageView = m_renderTargets->cockpitDepthSampledView();
  cockpitDepth.imageLayout = m_renderTargets->depthReadLayout();

  std::array<vk::WriteDescriptorSet, 3> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &sceneDepth;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &cockpitDepth;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordPostEffectsPass(const RenderCtx &render, VulkanFrame &frame,
                                           const graphics::generic_data::post_data &params,
                                           const vk::DescriptorImageInfo &ldrInput,
                                           const vk::DescriptorImageInfo &depthInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordPostEffectsPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordPostEffectsPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordPostEffectsPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordPostEffectsPass requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_MAIN);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_MAIN;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(params), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &params, sizeof(params));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(params);

  std::array<vk::WriteDescriptorSet, 4> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &ldrInput;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &depthInput;

  // Binding 4 is unused by the built-in shader but reserved for future/custom effects.
  writes[3].dstBinding = 4;
  writes[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[3].descriptorCount = 1;
  writes[3].pImageInfo = &depthInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::generateBloomMipmaps(vk::CommandBuffer cmd, uint32_t pingPongIndex, vk::Extent2D baseExtent) {
  Assertion(cmd, "generateBloomMipmaps called with null command buffer");
  Assertion(m_renderTargets != nullptr, "generateBloomMipmaps requires render targets");
  Assertion(pingPongIndex < VulkanRenderTargets::kBloomPingPongCount, "Invalid bloom ping-pong index %u",
            pingPongIndex);

  // Ensure no dynamic rendering is active. Mipmap generation uses blits (transfer ops).
  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  const vk::Image image = m_renderTargets->bloomImage(pingPongIndex);
  const uint32_t mipLevels = VulkanRenderTargets::kBloomMipLevels;

  // Query blit support; fall back to nearest if linear isn't supported for this format.
  vk::Filter filter = vk::Filter::eLinear;
  const auto props = m_vulkanDevice->physicalDevice().getFormatProperties(vk::Format::eR16G16B16A16Sfloat);
  const bool canLinear =
      (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) != vk::FormatFeatureFlags{};
  if (!canLinear) {
    filter = vk::Filter::eNearest;
  }

  // Transition all mips to TRANSFER_DST (we will move each mip to SRC as we go).
  {
    const auto oldLayout = m_renderTargets->bloomLayout(pingPongIndex);
    submitImageBarrier(cmd, makeImageBarrier(image, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite,
                                             vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                                             oldLayout, vk::ImageLayout::eTransferDstOptimal,
                                             vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1));

    m_renderTargets->setBloomLayout(pingPongIndex, vk::ImageLayout::eTransferDstOptimal);
  }

  // Transition mip 0 to TRANSFER_SRC.
  {
    submitImageBarrier(cmd, makeImageLayoutBarrier(image, vk::ImageLayout::eTransferDstOptimal,
                                                   vk::ImageLayout::eTransferSrcOptimal,
                                                   vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
  }

  uint32_t srcW = std::max(1u, baseExtent.width);
  uint32_t srcH = std::max(1u, baseExtent.height);

  for (uint32_t mip = 1; mip < mipLevels; ++mip) {
    const uint32_t dstW = std::max(1u, srcW >> 1);
    const uint32_t dstH = std::max(1u, srcH >> 1);

    vk::ImageBlit blit{};
    blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.srcSubresource.mipLevel = mip - 1;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.srcOffsets[1] = vk::Offset3D{static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};

    blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.dstSubresource.mipLevel = mip;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{static_cast<int32_t>(dstW), static_cast<int32_t>(dstH), 1};

    // Layout invariants for this loop:
    // - Source mip (mip-1) is already TRANSFER_SRC (mip0 pre-transition, subsequent mips become SRC below)
    // - Destination mip (mip) is still TRANSFER_DST from the initial "all mips to DST" transition.

    cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal, 1, &blit,
                  filter);

    // Transition destination mip to TRANSFER_SRC so it can serve as source for the next step.
    {
      submitImageBarrier(cmd, makeImageLayoutBarrier(image, vk::ImageLayout::eTransferDstOptimal,
                                                     vk::ImageLayout::eTransferSrcOptimal,
                                                     vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1));
    }

    srcW = dstW;
    srcH = dstH;
  }

  // Transition all mips to shader-read for sampling in blur passes.
  {
    submitImageBarrier(cmd,
                       makeImageBarrier(image, vk::PipelineStageFlagBits2::eTransfer,
                                        vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite,
                                        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
                                        vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
                                        vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1));
  }

  m_renderTargets->setBloomLayout(pingPongIndex, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderer::recordCopyToSwapchain(const RenderCtx &render, vk::DescriptorImageInfo src) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordCopyToSwapchain called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordCopyToSwapchain requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordCopyToSwapchain requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordCopyToSwapchain requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, 1, &viewport);

  vk::Rect2D scissor{{0, 0}, extent};
  cmd.setScissor(0, 1, &scissor);

  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_COPY);

  static const vertex_layout copyLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_COPY;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = copyLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, copyLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.descriptorCount = 1;
  write.pImageInfo = &src;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, 1, &write);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::createSmaaLookupTextures(const InitCtx &init) {
  Assertion(m_vulkanDevice != nullptr, "createSmaaLookupTextures requires VulkanDevice");

  auto createLookupTexture = [&](const uint8_t *pixels, size_t sizeBytes, uint32_t width, uint32_t height,
                                 vk::Format format, SmaaLookupTexture &out) {
    Assertion(pixels != nullptr, "createSmaaLookupTextures pixel pointer must be valid");
    Assertion(width > 0 && height > 0, "createSmaaLookupTextures invalid extent %ux%u", width, height);
    Assertion(sizeBytes > 0, "createSmaaLookupTextures invalid size");

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = format;
    imageInfo.extent = vk::Extent3D(width, height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;

    out.image = m_vulkanDevice->device().createImageUnique(imageInfo);

    const auto memReqs = m_vulkanDevice->device().getImageMemoryRequirements(out.image.get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex =
        m_vulkanDevice->findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    out.memory = m_vulkanDevice->device().allocateMemoryUnique(allocInfo);
    m_vulkanDevice->device().bindImageMemory(out.image.get(), out.memory.get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = out.image.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    out.view = m_vulkanDevice->device().createImageViewUnique(viewInfo);

    // Staging buffer (host visible).
    vk::BufferCreateInfo bufInfo{};
    bufInfo.size = sizeBytes;
    bufInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
    bufInfo.sharingMode = vk::SharingMode::eExclusive;
    auto staging = m_vulkanDevice->device().createBufferUnique(bufInfo);

    const auto bufReqs = m_vulkanDevice->device().getBufferMemoryRequirements(staging.get());
    vk::MemoryAllocateInfo bufAlloc{};
    bufAlloc.allocationSize = bufReqs.size;
    bufAlloc.memoryTypeIndex = m_vulkanDevice->findMemoryType(
        bufReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    auto stagingMem = m_vulkanDevice->device().allocateMemoryUnique(bufAlloc);
    m_vulkanDevice->device().bindBufferMemory(staging.get(), stagingMem.get(), 0);

    void *mapped = m_vulkanDevice->device().mapMemory(stagingMem.get(), 0, sizeBytes);
    std::memcpy(mapped, pixels, sizeBytes);
    m_vulkanDevice->device().unmapMemory(stagingMem.get());

    submitInitCommandsAndWait(init, [&](vk::CommandBuffer cmd) {
      // Undefined -> transfer dst
      submitImageBarrier(cmd, makeImageLayoutBarrier(out.image.get(), vk::ImageLayout::eUndefined,
                                                     vk::ImageLayout::eTransferDstOptimal,
                                                     vk::ImageAspectFlagBits::eColor, 1, 1));

      vk::BufferImageCopy region{};
      region.bufferOffset = 0;
      region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = vk::Extent3D(width, height, 1);
      cmd.copyBufferToImage(staging.get(), out.image.get(), vk::ImageLayout::eTransferDstOptimal, 1, &region);

      // Transfer dst -> shader read
      submitImageBarrier(cmd, makeImageLayoutBarrier(out.image.get(), vk::ImageLayout::eTransferDstOptimal,
                                                     vk::ImageLayout::eShaderReadOnlyOptimal,
                                                     vk::ImageAspectFlagBits::eColor, 1, 1));
    });
  };

  // areaTex: R8G8_UNORM, 160x560
  createLookupTexture(areaTexBytes, AREATEX_SIZE, static_cast<uint32_t>(AREATEX_WIDTH),
                      static_cast<uint32_t>(AREATEX_HEIGHT), vk::Format::eR8G8Unorm, m_smaaAreaTex);

  // searchTex: R8_UNORM, 64x16
  createLookupTexture(searchTexBytes, SEARCHTEX_SIZE, static_cast<uint32_t>(SEARCHTEX_WIDTH),
                      static_cast<uint32_t>(SEARCHTEX_HEIGHT), vk::Format::eR8Unorm, m_smaaSearchTex);
}

} // namespace vulkan
} // namespace graphics
