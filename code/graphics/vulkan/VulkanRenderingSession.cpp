#include "VulkanRenderingSession.h"
#include "VulkanDebug.h"
#include "osapi/outwnd.h"
#include <fstream>
#include <chrono>
#include <atomic>
#include <string>

namespace graphics {
namespace vulkan {

namespace {
struct StageAccess {
  vk::PipelineStageFlags2 stageMask{};
  vk::AccessFlags2 accessMask{};
};

StageAccess stageAccessForLayout(vk::ImageLayout layout)
{
  StageAccess out{};
  switch (layout) {
  case vk::ImageLayout::eUndefined:
    out.stageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
    out.accessMask = {};
    break;
  case vk::ImageLayout::eColorAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    out.accessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    break;
  case vk::ImageLayout::eDepthAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    out.accessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    break;
  case vk::ImageLayout::eDepthStencilAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    out.accessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    break;
  case vk::ImageLayout::eShaderReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::ePresentSrcKHR:
    out.stageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
    out.accessMask = {};
    break;
  default:
    out.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    out.accessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    break;
  }
  return out;
}

} // namespace

VulkanRenderingSession::VulkanRenderingSession(VulkanDevice& device,
    VulkanRenderTargets& targets)
    : m_device(device)
    , m_targets(targets)
{
  m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
  m_target = std::make_unique<SwapchainWithDepthTarget>();
  m_clearOps = ClearOps::clearAll();
}

void VulkanRenderingSession::beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex) {
  if (m_swapchainLayouts.size() != m_device.swapchainImageCount()) {
    m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
  }

  endActivePass();
  m_target = std::make_unique<SwapchainWithDepthTarget>();

  m_clearOps = ClearOps::clearAll();

  // Transition swapchain and depth to attachment layouts
  transitionSwapchainToAttachment(cmd, imageIndex);
  transitionDepthToAttachment(cmd);
}

void VulkanRenderingSession::endFrame(vk::CommandBuffer cmd, uint32_t imageIndex) {
  endActivePass();

  // Transition swapchain to present layout
  transitionSwapchainToPresent(cmd, imageIndex);
  

}

RenderTargetInfo VulkanRenderingSession::ensureRendering(vk::CommandBuffer cmd, uint32_t imageIndex)
{
  if (m_activePass.has_value()) {
    return m_activeInfo;
  }

  auto info = m_target->info(m_device, m_targets);
  m_target->begin(*this, cmd, imageIndex);
  m_activeInfo = info;
  applyDynamicState(cmd);
  m_activePass.emplace(cmd);
  return info;
}

void VulkanRenderingSession::requestSwapchainTarget() {
  endActivePass();
  m_target = std::make_unique<SwapchainWithDepthTarget>();
}

void VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs) {
  endActivePass();
  m_clearOps = clearNonColorBufs ? ClearOps::clearAll() : m_clearOps.withDepthStencilClear();
  m_target = std::make_unique<DeferredGBufferTarget>();
}

void VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd) {
  Assertion(dynamic_cast<DeferredGBufferTarget*>(m_target.get()) != nullptr,
    "endDeferredGeometry called when not in deferred gbuffer target");

  endActivePass();
  transitionGBufferToShaderRead(cmd);
  m_target = std::make_unique<SwapchainNoDepthTarget>();
}

void VulkanRenderingSession::endActivePass() {
  // End dynamic rendering if active. Boundaries always own this responsibility.
  m_activePass.reset();
}

void VulkanRenderingSession::requestClear() {
  m_clearOps = ClearOps::clearAll();
}

void VulkanRenderingSession::requestDepthClear() {
  m_clearOps = m_clearOps.withDepthStencilClear();
}

void VulkanRenderingSession::setClearColor(float r, float g, float b, float a) {
  m_clearColor[0] = r;
  m_clearColor[1] = g;
  m_clearColor[2] = b;
  m_clearColor[3] = a;
}

// ---- Target state implementations ----

RenderTargetInfo
VulkanRenderingSession::SwapchainWithDepthTarget::info(const VulkanDevice& device, const VulkanRenderTargets& targets) const {
  RenderTargetInfo out{};
  out.colorFormat = device.swapchainFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = targets.depthFormat();
  return out;
}

void VulkanRenderingSession::SwapchainWithDepthTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t imageIndex) {
  s.beginSwapchainRenderingInternal(cmd, imageIndex);
}

RenderTargetInfo
VulkanRenderingSession::DeferredGBufferTarget::info(const VulkanDevice& device, const VulkanRenderTargets& targets) const {
  RenderTargetInfo out{};
  out.colorFormat = targets.gbufferFormat();
  out.colorAttachmentCount = VulkanRenderTargets::kGBufferCount;
  out.depthFormat = targets.depthFormat();
  return out;
}

void VulkanRenderingSession::DeferredGBufferTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/) {
  s.beginGBufferRenderingInternal(cmd);
}

RenderTargetInfo
VulkanRenderingSession::SwapchainNoDepthTarget::info(const VulkanDevice& device, const VulkanRenderTargets&) const {
  RenderTargetInfo out{};
  out.colorFormat = device.swapchainFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined; // No depth
  return out;
}

void VulkanRenderingSession::SwapchainNoDepthTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t imageIndex) {
  s.beginSwapchainRenderingNoDepthInternal(cmd, imageIndex);
}

// ---- Internal rendering methods ----

void VulkanRenderingSession::beginSwapchainRenderingInternal(vk::CommandBuffer cmd, uint32_t imageIndex) {
  const auto extent = m_device.swapchainExtent();

  // Depth may have been transitioned to shader-read during deferred lighting.
  transitionDepthToAttachment(cmd);

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = m_device.swapchainImageView(imageIndex);
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

  vk::RenderingAttachmentInfo depthAttachment{};
  depthAttachment.imageView = m_targets.depthAttachmentView();
  depthAttachment.imageLayout = m_targets.depthAttachmentLayout();
  depthAttachment.loadOp = m_clearOps.depth;
  depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  depthAttachment.clearValue.depthStencil.depth = m_clearDepth;
  depthAttachment.clearValue.depthStencil.stencil = 0;
  


  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = &depthAttachment;
  vk::RenderingAttachmentInfo stencilAttachment{};
  if (m_targets.depthHasStencil()) {
    stencilAttachment.imageView = m_targets.depthAttachmentView();
    stencilAttachment.imageLayout = m_targets.depthAttachmentLayout();
    stencilAttachment.loadOp = m_clearOps.stencil;
    stencilAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    stencilAttachment.clearValue.depthStencil.depth = m_clearDepth;
    stencilAttachment.clearValue.depthStencil.stencil = 0;
    renderingInfo.pStencilAttachment = &stencilAttachment;
  }

  cmd.beginRendering(renderingInfo);

  // Clear ops are one-shot; revert to load after consumption
  m_clearOps = ClearOps::loadAll();
}

void VulkanRenderingSession::beginGBufferRenderingInternal(vk::CommandBuffer cmd) {
  const auto extent = m_device.swapchainExtent();

  // Transition G-buffer images to color attachment optimal
  transitionGBufferToAttachment(cmd);
  transitionDepthToAttachment(cmd);

  // Setup color attachments for G-buffer
  std::array<vk::RenderingAttachmentInfo, VulkanRenderTargets::kGBufferCount> colorAttachments{};
  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    colorAttachments[i].imageView = m_targets.gbufferView(i);
    colorAttachments[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachments[i].loadOp = m_clearOps.color;
    colorAttachments[i].storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachments[i].clearValue = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
  }

  vk::RenderingAttachmentInfo depthAttachment{};
  depthAttachment.imageView = m_targets.depthAttachmentView();
  depthAttachment.imageLayout = m_targets.depthAttachmentLayout();
  depthAttachment.loadOp = m_clearOps.depth;
  depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  depthAttachment.clearValue.depthStencil.depth = m_clearDepth;
  depthAttachment.clearValue.depthStencil.stencil = 0;

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = VulkanRenderTargets::kGBufferCount;
  renderingInfo.pColorAttachments = colorAttachments.data();
  renderingInfo.pDepthAttachment = &depthAttachment;
  vk::RenderingAttachmentInfo stencilAttachment{};
  if (m_targets.depthHasStencil()) {
    stencilAttachment.imageView = m_targets.depthAttachmentView();
    stencilAttachment.imageLayout = m_targets.depthAttachmentLayout();
    stencilAttachment.loadOp = m_clearOps.stencil;
    stencilAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    stencilAttachment.clearValue.depthStencil.depth = m_clearDepth;
    stencilAttachment.clearValue.depthStencil.stencil = 0;
    renderingInfo.pStencilAttachment = &stencilAttachment;
  }



  cmd.beginRendering(renderingInfo);

  // Clear flags are one-shot; reset after we consume them
  m_clearOps = ClearOps::loadAll();
}

void VulkanRenderingSession::beginSwapchainRenderingNoDepthInternal(vk::CommandBuffer cmd, uint32_t imageIndex) {
  const auto extent = m_device.swapchainExtent();

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = m_device.swapchainImageView(imageIndex);
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  // Respect clearOps.color: load existing content unless a clear was requested.
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);
  


  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = nullptr;  // No depth for deferred lighting

  cmd.beginRendering(renderingInfo);

  // Clear ops are one-shot; revert to load after consumption.
  m_clearOps = ClearOps::loadAll();
}

// ---- Layout transitions ----

void VulkanRenderingSession::transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex) {
  Assertion(imageIndex < m_swapchainLayouts.size(),
    "imageIndex %u out of bounds (swapchain has %zu images)", imageIndex, m_swapchainLayouts.size());

  vk::ImageMemoryBarrier2 toRender{};
  toRender.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
  toRender.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  toRender.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
  toRender.oldLayout = m_swapchainLayouts[imageIndex];
  toRender.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
  toRender.image = m_device.swapchainImage(imageIndex);
  toRender.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toRender.subresourceRange.levelCount = 1;
  toRender.subresourceRange.layerCount = 1;

  vk::DependencyInfo depInfo{};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &toRender;
  cmd.pipelineBarrier2(depInfo);

  m_swapchainLayouts[imageIndex] = vk::ImageLayout::eColorAttachmentOptimal;
}

void VulkanRenderingSession::transitionDepthToAttachment(vk::CommandBuffer cmd) {
  vk::ImageMemoryBarrier2 toDepth{};
  const auto oldLayout = m_targets.depthLayout();
  const auto newLayout = m_targets.depthAttachmentLayout();
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  toDepth.srcStageMask = src.stageMask;
  toDepth.srcAccessMask = src.accessMask;
  toDepth.dstStageMask = dst.stageMask;
  toDepth.dstAccessMask = dst.accessMask;
  toDepth.oldLayout = oldLayout;
  toDepth.newLayout = newLayout;
  toDepth.image = m_targets.depthImage();
  toDepth.subresourceRange.aspectMask = m_targets.depthAttachmentAspectMask();
  toDepth.subresourceRange.levelCount = 1;
  toDepth.subresourceRange.layerCount = 1;

  vk::DependencyInfo depInfo{};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &toDepth;
  cmd.pipelineBarrier2(depInfo);
  m_targets.setDepthLayout(newLayout);
}

void VulkanRenderingSession::transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex) {
  Assertion(imageIndex < m_swapchainLayouts.size(),
    "imageIndex %u out of bounds (swapchain has %zu images)", imageIndex, m_swapchainLayouts.size());

  vk::ImageMemoryBarrier2 toPresent{};
  toPresent.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  toPresent.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
  toPresent.dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
  toPresent.dstAccessMask = {};
  toPresent.oldLayout = m_swapchainLayouts[imageIndex];
  toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
  toPresent.image = m_device.swapchainImage(imageIndex);
  toPresent.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toPresent.subresourceRange.levelCount = 1;
  toPresent.subresourceRange.layerCount = 1;

  vk::DependencyInfo depInfo{};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &toPresent;
  cmd.pipelineBarrier2(depInfo);

  m_swapchainLayouts[imageIndex] = vk::ImageLayout::ePresentSrcKHR;
}

void VulkanRenderingSession::transitionGBufferToAttachment(vk::CommandBuffer cmd) {
  std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount> barriers{};
  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    const auto oldLayout = m_targets.gbufferLayout(i);
    const auto newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    const auto src = stageAccessForLayout(oldLayout);
    const auto dst = stageAccessForLayout(newLayout);
    barriers[i].srcStageMask = src.stageMask;
    barriers[i].srcAccessMask = src.accessMask;
    barriers[i].dstStageMask = dst.stageMask;
    barriers[i].dstAccessMask = dst.accessMask;
    barriers[i].oldLayout = oldLayout;
    barriers[i].newLayout = newLayout;
    barriers[i].image = m_targets.gbufferImage(i);
    barriers[i].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barriers[i].subresourceRange.levelCount = 1;
    barriers[i].subresourceRange.layerCount = 1;
  }

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = VulkanRenderTargets::kGBufferCount;
  dep.pImageMemoryBarriers = barriers.data();
  cmd.pipelineBarrier2(dep);

  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    m_targets.setGBufferLayout(i, vk::ImageLayout::eColorAttachmentOptimal);
  }
}

void VulkanRenderingSession::transitionGBufferToShaderRead(vk::CommandBuffer cmd) {
  std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount + 1> barriers{};

  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    const auto oldLayout = m_targets.gbufferLayout(i);
    const auto newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    const auto src = stageAccessForLayout(oldLayout);
    const auto dst = stageAccessForLayout(newLayout);
    barriers[i].srcStageMask = src.stageMask;
    barriers[i].srcAccessMask = src.accessMask;
    barriers[i].dstStageMask = dst.stageMask;
    barriers[i].dstAccessMask = dst.accessMask;
    barriers[i].oldLayout = oldLayout;
    barriers[i].newLayout = newLayout;
    barriers[i].image = m_targets.gbufferImage(i);
    barriers[i].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barriers[i].subresourceRange.levelCount = 1;
    barriers[i].subresourceRange.layerCount = 1;
  }

  // Depth transition
  auto& bd = barriers[VulkanRenderTargets::kGBufferCount];
  const auto oldDepthLayout = m_targets.depthLayout();
  const auto newDepthLayout = m_targets.depthReadLayout();
  const auto srcDepth = stageAccessForLayout(oldDepthLayout);
  const auto dstDepth = stageAccessForLayout(newDepthLayout);
  bd.srcStageMask = srcDepth.stageMask;
  bd.srcAccessMask = srcDepth.accessMask;
  bd.dstStageMask = dstDepth.stageMask;
  bd.dstAccessMask = dstDepth.accessMask;
  bd.oldLayout = oldDepthLayout;
  bd.newLayout = newDepthLayout;
  bd.image = m_targets.depthImage();
  bd.subresourceRange.aspectMask = m_targets.depthAttachmentAspectMask();
  bd.subresourceRange.levelCount = 1;
  bd.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
  dep.pImageMemoryBarriers = barriers.data();
  cmd.pipelineBarrier2(dep);

  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    m_targets.setGBufferLayout(i, vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  m_targets.setDepthLayout(newDepthLayout);
}

// ---- Dynamic state ----

void VulkanRenderingSession::applyDynamicState(vk::CommandBuffer cmd) {
  const auto extent = m_device.swapchainExtent();
  const uint32_t attachmentCount = m_activeInfo.colorAttachmentCount;
  const bool hasDepthAttachment = (m_activeInfo.depthFormat != vk::Format::eUndefined);

  // Vulkan Y-flip: set y=height and height=-height to match OpenGL coordinate system
  vk::Viewport viewport;
  viewport.x = 0.f;
  viewport.y = static_cast<float>(extent.height);
  viewport.width = static_cast<float>(extent.width);
  viewport.height = -static_cast<float>(extent.height);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  cmd.setViewport(0, viewport);

  cmd.setCullMode(m_cullMode);
  cmd.setFrontFace(vk::FrontFace::eClockwise);  // CW compensates for negative viewport height Y-flip
  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  const bool depthTest = hasDepthAttachment && m_depthTest;
  const bool depthWrite = hasDepthAttachment && m_depthWrite;
  cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
  cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
  cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  if (m_device.supportsExtendedDynamicState3()) {
    const auto& caps = m_device.extDyn3Caps();
    if (caps.colorBlendEnable) {
      // Baseline: blending OFF. Draw paths must enable per-material.
      std::array<vk::Bool32, VulkanRenderTargets::kGBufferCount> blendEnables{};
      blendEnables.fill(VK_FALSE);
      cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(attachmentCount, blendEnables.data()));
    }
    if (caps.colorWriteMask) {
      vk::ColorComponentFlags mask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
      std::array<vk::ColorComponentFlags, VulkanRenderTargets::kGBufferCount> masks{};
      masks.fill(mask);
      cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(attachmentCount, masks.data()));
    }
    if (caps.polygonMode) {
      cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    }
    if (caps.rasterizationSamples) {
      cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    }
  }
}

} // namespace vulkan
} // namespace graphics
