#include "VulkanRenderer.h"

#include "VulkanClip.h"
#include "graphics/matrix.h"

#include <array>
#include <cmath>
#include <type_traits>
#include <vector>

namespace graphics {
namespace vulkan {

void VulkanRenderer::beginDeferredLighting(graphics::vulkan::RecordingFrame &rec, bool clearNonColorBufs) {
  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "beginDeferredLighting called with null command buffer");

  // Preserve the current clip scissor across the internal fullscreen copy pass. Model draw paths
  // don't currently set scissor themselves.
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D restoreScissor{};
  restoreScissor.offset = vk::Offset2D{clip.x, clip.y};
  restoreScissor.extent = vk::Extent2D{clip.width, clip.height};

  const bool canCaptureSwapchain =
      (m_vulkanDevice->swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc) != vk::ImageUsageFlags{};

  const bool sceneHdrTarget = m_renderingSession->targetIsSceneHdr();
  const bool swapchainTarget = m_renderingSession->targetIsSwapchain();

  bool preserveEmissive = false;
  if (sceneHdrTarget) {
    // Scene rendering targets the HDR offscreen image, so preserve pre-deferred content from there (not swapchain).
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSceneHdrToShaderRead(cmd);

    m_renderingSession->requestGBufferEmissiveTarget();
    const auto emissiveRender = ensureRenderingStartedRecording(rec);
    recordPreDeferredSceneHdrCopy(emissiveRender);

    // Restore scissor for subsequent geometry draws.
    cmd.setScissor(0, 1, &restoreScissor);
    preserveEmissive = true;
  } else if (swapchainTarget && canCaptureSwapchain) {
    // End any active swapchain rendering and snapshot the current swapchain image.
    m_renderingSession->captureSwapchainColorToSceneCopy(cmd, rec.imageIndex);

    // Copy the captured scene color into the emissive G-buffer attachment (OpenGL parity).
    m_renderingSession->requestGBufferEmissiveTarget();
    const auto emissiveRender = ensureRenderingStartedRecording(rec);
    recordPreDeferredSceneColorCopy(emissiveRender, rec.imageIndex);

    // Restore scissor for subsequent geometry draws.
    cmd.setScissor(0, 1, &restoreScissor);
    preserveEmissive = true;
  }

  m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
  // Begin dynamic rendering immediately so clears execute even if no geometry draws occur.
  (void)ensureRenderingStartedRecording(rec);
}

void VulkanRenderer::endDeferredGeometry(vk::CommandBuffer cmd) { m_renderingSession->endDeferredGeometry(cmd); }

DeferredGeometryCtx VulkanRenderer::deferredLightingBegin(graphics::vulkan::RecordingFrame &rec,
                                                          bool clearNonColorBufs) {
  beginDeferredLighting(rec, clearNonColorBufs);
  return DeferredGeometryCtx{m_frameCounter};
}

DeferredLightingCtx VulkanRenderer::deferredLightingEnd(graphics::vulkan::RecordingFrame &rec,
                                                        DeferredGeometryCtx &&geometry) {
  Assertion(geometry.frameIndex == m_frameCounter,
            "deferredLightingEnd called with mismatched frameIndex (got %u, expected %u)", geometry.frameIndex,
            m_frameCounter);
  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "deferredLightingEnd called with null command buffer");

  endDeferredGeometry(cmd);
  if (m_sceneTexture.has_value()) {
    // Deferred lighting output should land in the scene HDR target during scene texture mode.
    m_renderingSession->requestSceneHdrNoDepthTarget();
  }
  return DeferredLightingCtx{m_frameCounter};
}

void VulkanRenderer::deferredLightingFinish(graphics::vulkan::RecordingFrame &rec, DeferredLightingCtx &&lighting,
                                            const vk::Rect2D &restoreScissor) {
  Assertion(lighting.frameIndex == m_frameCounter,
            "deferredLightingFinish called with mismatched frameIndex (got %u, expected %u)", lighting.frameIndex,
            m_frameCounter);

  VulkanFrame &frame = rec.ref();
  vk::Buffer uniformBuffer = frame.uniformBuffer().buffer();

  // Build lights from engine state (boundary: conditionals live here only).
  std::vector<DeferredLight> lights =
      buildDeferredLights(frame, uniformBuffer, gr_view_matrix, gr_projection_matrix, getMinUniformBufferAlignment());

  if (!lights.empty()) {
    // Activate swapchain rendering without depth (target set by endDeferredGeometry).
    auto render = ensureRenderingStartedRecording(rec);
    recordDeferredLighting(render, uniformBuffer, rec.ref().globalDescriptorSet(), lights);
  }

  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "deferredLightingFinish called with null command buffer");
  cmd.setScissor(0, 1, &restoreScissor);

  requestMainTargetWithDepth();
}

void VulkanRenderer::bindDeferredGlobalDescriptors(vk::DescriptorSet dstSet) {
  Assertion(dstSet, "bindDeferredGlobalDescriptors called with null descriptor set");
  std::vector<vk::WriteDescriptorSet> writes;
  std::vector<vk::DescriptorImageInfo> infos;
  writes.reserve(6);
  infos.reserve(6);

  // G-buffer 0..2
  for (uint32_t i = 0; i < 3; ++i) {
    vk::DescriptorImageInfo info{};
    info.sampler = m_renderTargets->gbufferSampler();
    info.imageView = m_renderTargets->gbufferView(i);
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    infos.push_back(info);

    vk::WriteDescriptorSet write{};
    write.dstSet = dstSet;
    write.dstBinding = i;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &infos.back();
    writes.push_back(write);
  }

  // Depth (binding 3) - uses nearest-filter sampler (linear often unsupported for depth)
  vk::DescriptorImageInfo depthInfo{};
  depthInfo.sampler = m_renderTargets->depthSampler();
  depthInfo.imageView = m_renderTargets->depthSampledView();
  depthInfo.imageLayout = m_renderTargets->depthReadLayout();
  infos.push_back(depthInfo);

  vk::WriteDescriptorSet depthWrite{};
  depthWrite.dstSet = dstSet;
  depthWrite.dstBinding = 3;
  depthWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  depthWrite.descriptorCount = 1;
  depthWrite.pImageInfo = &infos.back();
  writes.push_back(depthWrite);

  // Specular (binding 4): G-buffer attachment 3
  {
    vk::DescriptorImageInfo info{};
    info.sampler = m_renderTargets->gbufferSampler();
    info.imageView = m_renderTargets->gbufferView(3);
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    infos.push_back(info);

    vk::WriteDescriptorSet write{};
    write.dstSet = dstSet;
    write.dstBinding = 4;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &infos.back();
    writes.push_back(write);
  }

  // Emissive (binding 5): G-buffer attachment 4
  {
    vk::DescriptorImageInfo info{};
    info.sampler = m_renderTargets->gbufferSampler();
    info.imageView = m_renderTargets->gbufferView(4);
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    infos.push_back(info);

    vk::WriteDescriptorSet write{};
    write.dstSet = dstSet;
    write.dstBinding = 5;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &infos.back();
    writes.push_back(write);
  }

  m_vulkanDevice->device().updateDescriptorSets(writes, {});
}

void VulkanRenderer::recordPreDeferredSceneColorCopy(const RenderCtx &render, uint32_t imageIndex) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordPreDeferredSceneColorCopy called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordPreDeferredSceneColorCopy requires render targets");
  Assertion(m_bufferManager != nullptr, "recordPreDeferredSceneColorCopy requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordPreDeferredSceneColorCopy requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordPreDeferredSceneColorCopy requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  // Fullscreen draw state: independent of current clip/scissor.
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

  // Push the scene-color snapshot as the per-draw texture (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneColorSampler();
  sceneInfo.imageView = m_renderTargets->sceneColorView(imageIndex);
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.pImageInfo = &sceneInfo;

  std::array<vk::WriteDescriptorSet, 1> writes{write};
  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, writes);

  // Fullscreen triangle (same vertex buffer as deferred ambient).
  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &offset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordPreDeferredSceneHdrCopy(const RenderCtx &render) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordPreDeferredSceneHdrCopy called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordPreDeferredSceneHdrCopy requires render targets");
  Assertion(m_bufferManager != nullptr, "recordPreDeferredSceneHdrCopy requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordPreDeferredSceneHdrCopy requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordPreDeferredSceneHdrCopy requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  // Fullscreen draw state: independent of current clip/scissor.
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

  // Push the scene HDR color as the per-draw texture (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneHdrSampler();
  sceneInfo.imageView = m_renderTargets->sceneHdrView();
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.pImageInfo = &sceneInfo;

  std::array<vk::WriteDescriptorSet, 1> writes{write};
  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, writes);

  // Fullscreen triangle (same vertex buffer as deferred ambient).
  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &offset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::createDeferredLightingResources() {
  // Fullscreen triangle (covers entire screen with 3 vertices, no clipping)
  // Positions are in clip space: vertex shader passes through directly
  struct FullscreenVertex {
    float x, y, z;
  };
  static const FullscreenVertex fullscreenVerts[] = {{-1.0f, -1.0f, 0.0f}, {3.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}};

  m_fullscreenMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_fullscreenMesh.vbo, sizeof(fullscreenVerts), fullscreenVerts);
  m_fullscreenMesh.indexCount = 3;

  // Sphere mesh (icosphere-like approximation)
  // Using octahedron subdivided once for reasonable approximation
  std::vector<float> sphereVerts;
  std::vector<uint32_t> sphereIndices;

  // Octahedron base vertices
  const float oct[] = {
      0.0f,  1.0f,  0.0f, // top
      0.0f,  -1.0f, 0.0f, // bottom
      1.0f,  0.0f,  0.0f, // +X
      -1.0f, 0.0f,  0.0f, // -X
      0.0f,  0.0f,  1.0f, // +Z
      0.0f,  0.0f,  -1.0f // -Z
  };

  // Octahedron faces (8 triangles)
  const uint32_t octFaces[] = {
      0, 4, 2, 0, 2, 5, 0, 5, 3, 0, 3, 4, // top half
      1, 2, 4, 1, 5, 2, 1, 3, 5, 1, 4, 3  // bottom half
  };

  for (int i = 0; i < 18; i++) {
    sphereVerts.push_back(oct[i]);
  }
  for (int i = 0; i < 24; i++) {
    sphereIndices.push_back(octFaces[i]);
  }

  m_sphereMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_sphereMesh.vbo, sphereVerts.size() * sizeof(float), sphereVerts.data());
  m_sphereMesh.ibo = m_bufferManager->createBuffer(BufferType::Index, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_sphereMesh.ibo, sphereIndices.size() * sizeof(uint32_t), sphereIndices.data());
  m_sphereMesh.indexCount = static_cast<uint32_t>(sphereIndices.size());

  // Cylinder mesh (capped cylinder along -Z axis)
  // The model matrix will position and scale it
  std::vector<float> cylVerts;
  std::vector<uint32_t> cylIndices;

  const int segments = 12;
  const float twoPi = 6.283185307f;

  // Generate ring vertices at z=0 and z=-1
  for (int ring = 0; ring < 2; ++ring) {
    float z = (ring == 0) ? 0.0f : -1.0f;
    for (int i = 0; i < segments; ++i) {
      float angle = twoPi * i / segments;
      cylVerts.push_back(cosf(angle)); // x
      cylVerts.push_back(sinf(angle)); // y
      cylVerts.push_back(z);           // z
    }
  }

  // Center vertices for caps
  uint32_t capTop = static_cast<uint32_t>(cylVerts.size() / 3);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);

  uint32_t capBot = static_cast<uint32_t>(cylVerts.size() / 3);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(-1.0f);

  // Side faces (quads as two triangles)
  for (int i = 0; i < segments; ++i) {
    uint32_t i0 = i;
    uint32_t i1 = (i + 1) % segments;
    uint32_t i2 = i + segments;
    uint32_t i3 = ((i + 1) % segments) + segments;

    // Two triangles per quad
    cylIndices.push_back(i0);
    cylIndices.push_back(i2);
    cylIndices.push_back(i1);

    cylIndices.push_back(i1);
    cylIndices.push_back(i2);
    cylIndices.push_back(i3);
  }

  // Top cap (z=0)
  for (int i = 0; i < segments; ++i) {
    cylIndices.push_back(capTop);
    cylIndices.push_back((i + 1) % segments);
    cylIndices.push_back(i);
  }

  // Bottom cap (z=-1)
  for (int i = 0; i < segments; ++i) {
    cylIndices.push_back(capBot);
    cylIndices.push_back(i + segments);
    cylIndices.push_back(((i + 1) % segments) + segments);
  }

  m_cylinderMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_cylinderMesh.vbo, cylVerts.size() * sizeof(float), cylVerts.data());
  m_cylinderMesh.ibo = m_bufferManager->createBuffer(BufferType::Index, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_cylinderMesh.ibo, cylIndices.size() * sizeof(uint32_t), cylIndices.data());
  m_cylinderMesh.indexCount = static_cast<uint32_t>(cylIndices.size());
}

void VulkanRenderer::recordDeferredLighting(const RenderCtx &render, vk::Buffer uniformBuffer,
                                            vk::DescriptorSet globalSet, const std::vector<DeferredLight> &lights) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordDeferredLighting called with null command buffer");
  Assertion(globalSet, "recordDeferredLighting called with null global descriptor set");

  // Deferred lighting pass owns full-screen viewport/scissor and disables depth.
  {
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
    cmd.setFrontFace(vk::FrontFace::eClockwise); // Matches Y-flipped viewport convention
    cmd.setDepthTestEnable(VK_FALSE);
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setDepthCompareOp(vk::CompareOp::eAlways);
    cmd.setStencilTestEnable(VK_FALSE);
  }

  // Pipelines are cached by VulkanPipelineManager; we still build the key per frame since the render target contract
  // can vary.
  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_DEFERRED_LIGHTING);

  static const vertex_layout deferredLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0); // Position only for volume meshes
    return layout;
  }();

  const auto &rt = render.targetInfo;
  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_DEFERRED_LIGHTING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(rt.colorFormat);
  key.depth_format = static_cast<VkFormat>(rt.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = rt.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = deferredLayout.hash();

  // Ambient pipeline (no blend, overwrites undefined swapchain)
  PipelineKey ambientKey = key;
  ambientKey.blend_mode = ALPHA_BLEND_NONE;

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, deferredLayout);
  vk::Pipeline ambientPipeline = m_pipelineManager->getPipeline(ambientKey, modules, deferredLayout);

  DeferredDrawContext ctx{};
  ctx.cmd = cmd;
  ctx.layout = m_descriptorLayouts->deferredPipelineLayout();
  ctx.uniformBuffer = uniformBuffer;
  ctx.pipeline = pipeline;
  ctx.ambientPipeline = ambientPipeline;
  ctx.dynamicBlendEnable =
      m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable;

  // Bind global (set=1) deferred descriptor set using the *deferred* pipeline layout.
  // Binding via the standard pipeline layout is not descriptor-set compatible because set 0 differs.
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.layout, 1, 1, &globalSet, 0, nullptr);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::Buffer sphereVB = m_bufferManager->getBuffer(m_sphereMesh.vbo);
  vk::Buffer sphereIB = m_bufferManager->getBuffer(m_sphereMesh.ibo);
  vk::Buffer cylinderVB = m_bufferManager->getBuffer(m_cylinderMesh.vbo);
  vk::Buffer cylinderIB = m_bufferManager->getBuffer(m_cylinderMesh.ibo);

  for (const auto &light : lights) {
    std::visit(
        [&](const auto &l) {
          using T = std::decay_t<decltype(l)>;
          if constexpr (std::is_same_v<T, FullscreenLight>) {
            l.record(ctx, fullscreenVB);
          } else if constexpr (std::is_same_v<T, SphereLight>) {
            l.record(ctx, sphereVB, sphereIB, m_sphereMesh.indexCount);
          } else if constexpr (std::is_same_v<T, CylinderLight>) {
            l.record(ctx, cylinderVB, cylinderIB, m_cylinderMesh.indexCount);
          }
        },
        light);
  }
  // Note: render pass ends at explicit session boundaries (target changes/frame end).
}

} // namespace vulkan
} // namespace graphics
