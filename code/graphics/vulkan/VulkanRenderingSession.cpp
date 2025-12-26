#include "VulkanRenderingSession.h"
#include "VulkanDebug.h"
#include "VulkanTextureManager.h"
#include "osapi/outwnd.h"
#include <fstream>
#include <chrono>
#include <atomic>
#include <string>
#include <sstream>

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
  case vk::ImageLayout::eTransferSrcOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferRead;
    break;
  case vk::ImageLayout::eTransferDstOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferWrite;
    break;
  case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::ePresentSrcKHR:
    // "Present" is external to the pipeline. For sync2 barriers that transition to/from present,
    // the destination stage/access should typically be NONE/0.
    out.stageMask = {};
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
    VulkanRenderTargets& targets,
    VulkanTextureManager& textures)
    : m_device(device)
    , m_targets(targets)
    , m_textures(textures)
{
  m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
  m_target = std::make_unique<SwapchainWithDepthTarget>();
  m_clearOps = ClearOps::clearAll();
  m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eClear);
}

void VulkanRenderingSession::beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex) {
  if (m_swapchainLayouts.size() != m_device.swapchainImageCount()) {
    m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
  }

  endActivePass();
  m_target = std::make_unique<SwapchainWithDepthTarget>();
  m_depthAttachment = DepthAttachmentKind::Main;

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

void VulkanRenderingSession::requestSwapchainNoDepthTarget()
{
  endActivePass();
  m_target = std::make_unique<SwapchainNoDepthTarget>();
}

void VulkanRenderingSession::requestSceneHdrTarget()
{
  endActivePass();
  m_target = std::make_unique<SceneHdrWithDepthTarget>();
}

void VulkanRenderingSession::requestSceneHdrNoDepthTarget()
{
  endActivePass();
  m_target = std::make_unique<SceneHdrNoDepthTarget>();
}

void VulkanRenderingSession::requestPostLdrTarget()
{
  endActivePass();
  m_target = std::make_unique<PostLdrTarget>();
}

void VulkanRenderingSession::requestPostLuminanceTarget()
{
  endActivePass();
  m_target = std::make_unique<PostLuminanceTarget>();
}

void VulkanRenderingSession::requestSmaaEdgesTarget()
{
  endActivePass();
  m_target = std::make_unique<SmaaEdgesTarget>();
}

void VulkanRenderingSession::requestSmaaBlendTarget()
{
  endActivePass();
  m_target = std::make_unique<SmaaBlendTarget>();
}

void VulkanRenderingSession::requestSmaaOutputTarget()
{
  endActivePass();
  m_target = std::make_unique<SmaaOutputTarget>();
}

void VulkanRenderingSession::requestBloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel)
{
  endActivePass();
  m_target = std::make_unique<BloomMipTarget>(pingPongIndex, mipLevel);
}

void VulkanRenderingSession::requestBitmapTarget(int bitmapHandle, int face)
{
  endActivePass();
  const auto fmt = m_textures.renderTargetFormat(bitmapHandle);
  m_target = std::make_unique<BitmapTarget>(bitmapHandle, face, fmt);
}

void VulkanRenderingSession::useMainDepthAttachment()
{
  endActivePass();
  m_depthAttachment = DepthAttachmentKind::Main;
}

void VulkanRenderingSession::useCockpitDepthAttachment()
{
  endActivePass();
  m_depthAttachment = DepthAttachmentKind::Cockpit;
}

void VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive) {
  endActivePass();
  // Vulkan mirrors OpenGL's deferred begin semantics:
  // - pre-deferred swapchain color is captured and copied into the emissive buffer (handled by VulkanRenderer)
  // - the remaining G-buffer attachments are cleared here by loadOp=CLEAR
  (void)clearNonColorBufs; // parameter retained for API parity; Vulkan always clears non-emissive G-buffer attachments.

  // Depth is shared across swapchain and deferred targets. Always clear it when entering deferred geometry so
  // pre-deferred draws (treated as emissive background) cannot occlude deferred geometry.
  m_clearOps = m_clearOps.withDepthStencilClear();

  m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eClear);
  if (preserveEmissive) {
    m_gbufferLoadOps[VulkanRenderTargets::kGBufferEmissiveIndex] = vk::AttachmentLoadOp::eLoad;
  }
  m_target = std::make_unique<DeferredGBufferTarget>();
}

void VulkanRenderingSession::requestDeferredGBufferTarget()
{
  endActivePass();
  m_target = std::make_unique<DeferredGBufferTarget>();
}

void VulkanRenderingSession::requestGBufferEmissiveTarget()
{
  endActivePass();
  m_target = std::make_unique<GBufferEmissiveTarget>();
}

void VulkanRenderingSession::requestGBufferAttachmentTarget(uint32_t gbufferIndex)
{
  endActivePass();
  m_target = std::make_unique<GBufferAttachmentTarget>(gbufferIndex);
}

void VulkanRenderingSession::captureSwapchainColorToSceneCopy(vk::CommandBuffer cmd, uint32_t imageIndex)
{
  if ((m_device.swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
    return;
  }

  Assertion(imageIndex < m_swapchainLayouts.size(),
    "captureSwapchainColorToSceneCopy: imageIndex %u out of bounds (swapchain has %zu images)",
    imageIndex, m_swapchainLayouts.size());

  endActivePass();

  auto transitionSwapchainTo = [&](vk::ImageLayout newLayout) {
    const auto oldLayout = m_swapchainLayouts[imageIndex];
    if (oldLayout == newLayout) {
      return;
    }

    vk::ImageMemoryBarrier2 barrier{};
    const auto src = stageAccessForLayout(oldLayout);
    const auto dst = stageAccessForLayout(newLayout);
    barrier.srcStageMask = src.stageMask;
    barrier.srcAccessMask = src.accessMask;
    barrier.dstStageMask = dst.stageMask;
    barrier.dstAccessMask = dst.accessMask;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.image = m_device.swapchainImage(imageIndex);
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmd.pipelineBarrier2(dep);

    m_swapchainLayouts[imageIndex] = newLayout;
  };

  transitionSwapchainTo(vk::ImageLayout::eTransferSrcOptimal);
  transitionSceneCopyToLayout(cmd, imageIndex, vk::ImageLayout::eTransferDstOptimal);

  vk::ImageCopy region{};
  region.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.dstSubresource.mipLevel = 0;
  region.dstSubresource.baseArrayLayer = 0;
  region.dstSubresource.layerCount = 1;
  region.extent = vk::Extent3D(m_device.swapchainExtent().width, m_device.swapchainExtent().height, 1);

  cmd.copyImage(
    m_device.swapchainImage(imageIndex),
    vk::ImageLayout::eTransferSrcOptimal,
    m_targets.sceneColorImage(imageIndex),
    vk::ImageLayout::eTransferDstOptimal,
    1,
    &region);

  transitionSceneCopyToLayout(cmd, imageIndex, vk::ImageLayout::eShaderReadOnlyOptimal);
  transitionSwapchainTo(vk::ImageLayout::eColorAttachmentOptimal);
}

void VulkanRenderingSession::captureSwapchainColorToRenderTarget(vk::CommandBuffer cmd,
  uint32_t imageIndex,
  int renderTargetHandle)
{
  if ((m_device.swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
	return;
  }

  if (!m_textures.hasRenderTarget(renderTargetHandle)) {
	return;
  }

  const auto swapExtent = m_device.swapchainExtent();
  const auto targetExtent = m_textures.renderTargetExtent(renderTargetHandle);
  if (swapExtent.width != targetExtent.width || swapExtent.height != targetExtent.height) {
	return;
  }

  Assertion(imageIndex < m_swapchainLayouts.size(),
	"captureSwapchainColorToRenderTarget: imageIndex %u out of bounds (swapchain has %zu images)",
	imageIndex, m_swapchainLayouts.size());

  endActivePass();

  auto transitionSwapchainTo = [&](vk::ImageLayout newLayout) {
	const auto oldLayout = m_swapchainLayouts[imageIndex];
	if (oldLayout == newLayout) {
	  return;
	}

	vk::ImageMemoryBarrier2 barrier{};
	const auto src = stageAccessForLayout(oldLayout);
	const auto dst = stageAccessForLayout(newLayout);
	barrier.srcStageMask = src.stageMask;
	barrier.srcAccessMask = src.accessMask;
	barrier.dstStageMask = dst.stageMask;
	barrier.dstAccessMask = dst.accessMask;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.image = m_device.swapchainImage(imageIndex);
	barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	vk::DependencyInfo dep{};
	dep.imageMemoryBarrierCount = 1;
	dep.pImageMemoryBarriers = &barrier;
	cmd.pipelineBarrier2(dep);

	m_swapchainLayouts[imageIndex] = newLayout;
  };

  transitionSwapchainTo(vk::ImageLayout::eTransferSrcOptimal);
  m_textures.transitionRenderTargetToTransferDst(cmd, renderTargetHandle);

  vk::ImageCopy region{};
  region.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.dstSubresource.mipLevel = 0;
  region.dstSubresource.baseArrayLayer = 0;
  region.dstSubresource.layerCount = 1;
  region.extent = vk::Extent3D(swapExtent.width, swapExtent.height, 1);

  cmd.copyImage(
	m_device.swapchainImage(imageIndex),
	vk::ImageLayout::eTransferSrcOptimal,
	m_textures.renderTargetImage(renderTargetHandle),
	vk::ImageLayout::eTransferDstOptimal,
	1,
	&region);

  m_textures.transitionRenderTargetToShaderRead(cmd, renderTargetHandle);
  transitionSwapchainTo(vk::ImageLayout::eColorAttachmentOptimal);
}

void VulkanRenderingSession::transitionSceneHdrToShaderRead(vk::CommandBuffer cmd)
{
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionMainDepthToShaderRead(vk::CommandBuffer cmd)
{
  // Use the same read layout helper as deferred (depth+stencil read-only when applicable).
  const auto oldLayout = m_targets.depthLayout();
  const auto newLayout = m_targets.depthReadLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.depthImage();
  barrier.subresourceRange.aspectMask = m_targets.depthAttachmentAspectMask();
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setDepthLayout(newLayout);
}

void VulkanRenderingSession::transitionCockpitDepthToShaderRead(vk::CommandBuffer cmd)
{
  transitionCockpitDepthToLayout(cmd, m_targets.depthReadLayout());
}

void VulkanRenderingSession::transitionPostLdrToShaderRead(vk::CommandBuffer cmd)
{
  transitionPostLdrToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionPostLuminanceToShaderRead(vk::CommandBuffer cmd)
{
  transitionPostLuminanceToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionSmaaEdgesToShaderRead(vk::CommandBuffer cmd)
{
  transitionSmaaEdgesToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionSmaaBlendToShaderRead(vk::CommandBuffer cmd)
{
  transitionSmaaBlendToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionSmaaOutputToShaderRead(vk::CommandBuffer cmd)
{
  transitionSmaaOutputToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionBloomToShaderRead(vk::CommandBuffer cmd, uint32_t pingPongIndex)
{
  transitionBloomToLayout(cmd, pingPongIndex, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::copySceneHdrToEffect(vk::CommandBuffer cmd)
{
  // Transfers are invalid inside dynamic rendering.
  endActivePass();

  // Transition images for copy.
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eTransferSrcOptimal);
  transitionSceneEffectToLayout(cmd, vk::ImageLayout::eTransferDstOptimal);

  vk::ImageCopy region{};
  region.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.srcSubresource.mipLevel = 0;
  region.srcSubresource.baseArrayLayer = 0;
  region.srcSubresource.layerCount = 1;
  region.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.dstSubresource.mipLevel = 0;
  region.dstSubresource.baseArrayLayer = 0;
  region.dstSubresource.layerCount = 1;
  region.extent = vk::Extent3D(m_device.swapchainExtent().width, m_device.swapchainExtent().height, 1);

  cmd.copyImage(
	m_targets.sceneHdrImage(),
	vk::ImageLayout::eTransferSrcOptimal,
	m_targets.sceneEffectImage(),
	vk::ImageLayout::eTransferDstOptimal,
	1,
	&region);

  // Effect snapshot is sampled by distortion/effects; keep it shader-readable.
  transitionSceneEffectToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);

  // Resume scene rendering: scene HDR back to attachment layout.
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
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
VulkanRenderingSession::SceneHdrWithDepthTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& targets) const
{
  RenderTargetInfo out{};
  out.colorFormat = targets.sceneHdrFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = targets.depthFormat();
  return out;
}

void VulkanRenderingSession::SceneHdrWithDepthTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.beginSceneHdrRenderingInternal(cmd);
}

RenderTargetInfo
VulkanRenderingSession::SceneHdrNoDepthTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& targets) const
{
  RenderTargetInfo out{};
  out.colorFormat = targets.sceneHdrFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SceneHdrNoDepthTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.beginSceneHdrRenderingNoDepthInternal(cmd);
}

RenderTargetInfo
VulkanRenderingSession::PostLdrTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::PostLdrTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.transitionPostLdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.postLdrView());
}

RenderTargetInfo
VulkanRenderingSession::PostLuminanceTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::PostLuminanceTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.transitionPostLuminanceToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.postLuminanceView());
}

RenderTargetInfo
VulkanRenderingSession::SmaaEdgesTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SmaaEdgesTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.transitionSmaaEdgesToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.smaaEdgesView());
}

RenderTargetInfo
VulkanRenderingSession::SmaaBlendTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SmaaBlendTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.transitionSmaaBlendToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.smaaBlendView());
}

RenderTargetInfo
VulkanRenderingSession::SmaaOutputTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SmaaOutputTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.transitionSmaaOutputToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.smaaOutputView());
}

VulkanRenderingSession::BloomMipTarget::BloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel) : m_index(pingPongIndex), m_mip(mipLevel)
{
}

RenderTargetInfo
VulkanRenderingSession::BloomMipTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eR16G16B16A16Sfloat;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::BloomMipTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  Assertion(m_index < VulkanRenderTargets::kBloomPingPongCount,
	"BloomMipTarget::begin pingPongIndex out of range (%u)", m_index);
  Assertion(m_mip < VulkanRenderTargets::kBloomMipLevels,
	"BloomMipTarget::begin mipLevel out of range (%u)", m_mip);

  // Half-res base extent, then downscale per mip.
  const auto full = s.m_device.swapchainExtent();
  const vk::Extent2D base{std::max(1u, full.width >> 1), std::max(1u, full.height >> 1)};
  const vk::Extent2D ex{std::max(1u, base.width >> m_mip), std::max(1u, base.height >> m_mip)};

  s.transitionBloomToLayout(cmd, m_index, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, ex, s.m_targets.bloomMipView(m_index, m_mip));
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

VulkanRenderingSession::GBufferAttachmentTarget::GBufferAttachmentTarget(uint32_t gbufferIndex) : m_index(gbufferIndex)
{
}

RenderTargetInfo
VulkanRenderingSession::GBufferAttachmentTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& targets) const
{
  RenderTargetInfo out{};
  out.colorFormat = targets.gbufferFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::GBufferAttachmentTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  Assertion(m_index < VulkanRenderTargets::kGBufferCount,
	"GBufferAttachmentTarget index out of range (%u)", m_index);

  const auto extent = s.m_device.swapchainExtent();

  // Ensure gbuffer images are in color-attachment layout for rendering.
  s.transitionGBufferToAttachment(cmd);

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = s.m_targets.gbufferView(m_index);
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  // Always LOAD for decals: we blend into existing gbuffer content.
  colorAttachment.loadOp = vk::AttachmentLoadOp::eLoad;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = nullptr;
  renderingInfo.pStencilAttachment = nullptr;

  cmd.beginRendering(renderingInfo);
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

RenderTargetInfo
VulkanRenderingSession::GBufferEmissiveTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& targets) const
{
  RenderTargetInfo out{};
  out.colorFormat = targets.gbufferFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::GBufferEmissiveTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.beginGBufferEmissiveRenderingInternal(cmd);
}

VulkanRenderingSession::BitmapTarget::BitmapTarget(int handle, int face, vk::Format format)
    : m_handle(handle), m_face(face), m_format(format)
{
}

RenderTargetInfo
VulkanRenderingSession::BitmapTarget::info(const VulkanDevice& /*device*/, const VulkanRenderTargets& /*targets*/) const
{
  RenderTargetInfo out{};
  out.colorFormat = m_format;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::BitmapTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t /*imageIndex*/)
{
  s.beginBitmapRenderingInternal(cmd, m_handle, m_face);
}

// ---- Internal rendering methods ----

void VulkanRenderingSession::beginSwapchainRenderingInternal(vk::CommandBuffer cmd, uint32_t imageIndex) {
  const auto extent = m_device.swapchainExtent();

  // Switching back to swapchain mid-frame requires re-establishing attachment layout.
  transitionSwapchainToAttachment(cmd, imageIndex);

  // Depth may have been transitioned to shader-read during deferred lighting.
  const vk::ImageView depthView =
	(m_depthAttachment == DepthAttachmentKind::Main) ? m_targets.depthAttachmentView() : m_targets.cockpitDepthAttachmentView();
  if (m_depthAttachment == DepthAttachmentKind::Main) {
	transitionDepthToAttachment(cmd);
  } else {
	transitionCockpitDepthToAttachment(cmd);
  }

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = m_device.swapchainImageView(imageIndex);
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

  vk::RenderingAttachmentInfo depthAttachment{};
  depthAttachment.imageView = depthView;
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
    stencilAttachment.imageView = depthView;
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

void VulkanRenderingSession::beginSceneHdrRenderingInternal(vk::CommandBuffer cmd)
{
  const auto extent = m_device.swapchainExtent();

  // Ensure scene HDR is writable and depth is attached.
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  const vk::ImageView depthView =
	(m_depthAttachment == DepthAttachmentKind::Main) ? m_targets.depthAttachmentView() : m_targets.cockpitDepthAttachmentView();
  if (m_depthAttachment == DepthAttachmentKind::Main) {
	transitionDepthToAttachment(cmd);
  } else {
	transitionCockpitDepthToAttachment(cmd);
  }

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = m_targets.sceneHdrView();
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

  vk::RenderingAttachmentInfo depthAttachment{};
  depthAttachment.imageView = depthView;
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
	stencilAttachment.imageView = depthView;
	stencilAttachment.imageLayout = m_targets.depthAttachmentLayout();
	stencilAttachment.loadOp = m_clearOps.stencil;
	stencilAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	stencilAttachment.clearValue.depthStencil.depth = m_clearDepth;
	stencilAttachment.clearValue.depthStencil.stencil = 0;
	renderingInfo.pStencilAttachment = &stencilAttachment;
  }

  cmd.beginRendering(renderingInfo);

  // Clear ops are one-shot; revert to load after consumption.
  m_clearOps = ClearOps::loadAll();
}

void VulkanRenderingSession::beginSceneHdrRenderingNoDepthInternal(vk::CommandBuffer cmd)
{
  const auto extent = m_device.swapchainExtent();

  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = m_targets.sceneHdrView();
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = nullptr;
  renderingInfo.pStencilAttachment = nullptr;

  cmd.beginRendering(renderingInfo);

  // Clear ops are one-shot; revert to load after consumption.
  m_clearOps = ClearOps::loadAll();
}

void VulkanRenderingSession::beginOffscreenColorRenderingInternal(vk::CommandBuffer cmd, vk::Extent2D extent, vk::ImageView colorView)
{
  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = colorView;
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = nullptr;
  renderingInfo.pStencilAttachment = nullptr;

  cmd.beginRendering(renderingInfo);

  // Clear ops are one-shot; revert to load after consumption.
  m_clearOps = ClearOps::loadAll();
}

void VulkanRenderingSession::beginGBufferRenderingInternal(vk::CommandBuffer cmd) {
  const auto extent = m_device.swapchainExtent();

  // Transition G-buffer images to color attachment optimal
  transitionGBufferToAttachment(cmd);
  const vk::ImageView depthView =
	(m_depthAttachment == DepthAttachmentKind::Main) ? m_targets.depthAttachmentView() : m_targets.cockpitDepthAttachmentView();
  if (m_depthAttachment == DepthAttachmentKind::Main) {
	transitionDepthToAttachment(cmd);
  } else {
	transitionCockpitDepthToAttachment(cmd);
  }

  // Setup color attachments for G-buffer
  std::array<vk::RenderingAttachmentInfo, VulkanRenderTargets::kGBufferCount> colorAttachments{};
  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    colorAttachments[i].imageView = m_targets.gbufferView(i);
    colorAttachments[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachments[i].loadOp = m_gbufferLoadOps[i];
    colorAttachments[i].storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachments[i].clearValue = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
  }

  vk::RenderingAttachmentInfo depthAttachment{};
  depthAttachment.imageView = depthView;
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
    stencilAttachment.imageView = depthView;
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
  // beginDeferredPass() sets G-buffer attachment load ops to CLEAR. If we suspend rendering mid-pass (e.g. for
  // transfers/texture updates), restarting dynamic rendering must preserve the existing contents.
  m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eLoad);
}

void VulkanRenderingSession::beginGBufferEmissiveRenderingInternal(vk::CommandBuffer cmd)
{
  const auto extent = m_device.swapchainExtent();

  // Transition G-buffer images to color attachment optimal (emissive uses the same layout).
  transitionGBufferToAttachment(cmd);

  vk::RenderingAttachmentInfo emissiveAttachment{};
  emissiveAttachment.imageView = m_targets.gbufferView(VulkanRenderTargets::kGBufferEmissiveIndex);
  emissiveAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  // The fullscreen copy shader overwrites the entire target, so the previous contents are irrelevant.
  emissiveAttachment.loadOp = vk::AttachmentLoadOp::eDontCare;
  emissiveAttachment.storeOp = vk::AttachmentStoreOp::eStore;

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &emissiveAttachment;
  renderingInfo.pDepthAttachment = nullptr;

  cmd.beginRendering(renderingInfo);
}

void VulkanRenderingSession::beginSwapchainRenderingNoDepthInternal(vk::CommandBuffer cmd, uint32_t imageIndex) {
  const auto extent = m_device.swapchainExtent();

  // Switching back to swapchain mid-frame requires re-establishing attachment layout.
  transitionSwapchainToAttachment(cmd, imageIndex);

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

void VulkanRenderingSession::beginBitmapRenderingInternal(vk::CommandBuffer cmd, int bitmapHandle, int face)
{
  Assertion(bitmapHandle >= 0, "beginBitmapRenderingInternal called with invalid bitmap handle %d", bitmapHandle);
  Assertion(m_textures.hasRenderTarget(bitmapHandle), "beginBitmapRenderingInternal called for non-render-target bitmap %d",
            bitmapHandle);

  // Normalize face for 2D render targets.
  const int clampedFace = (face < 0) ? 0 : face;

  const auto extent = m_textures.renderTargetExtent(bitmapHandle);

  // Transition the target to a writable attachment layout.
  m_textures.transitionRenderTargetToAttachment(cmd, bitmapHandle);

  vk::RenderingAttachmentInfo colorAttachment{};
  colorAttachment.imageView = m_textures.renderTargetAttachmentView(bitmapHandle, clampedFace);
  colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
  colorAttachment.loadOp = m_clearOps.color;
  colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
  colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

  vk::RenderingInfo renderingInfo{};
  renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &colorAttachment;
  renderingInfo.pDepthAttachment = nullptr;
  renderingInfo.pStencilAttachment = nullptr;

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

void VulkanRenderingSession::transitionCockpitDepthToAttachment(vk::CommandBuffer cmd)
{
  transitionCockpitDepthToLayout(cmd, m_targets.depthAttachmentLayout());
}

void VulkanRenderingSession::transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex) {
  Assertion(imageIndex < m_swapchainLayouts.size(),
    "imageIndex %u out of bounds (swapchain has %zu images)", imageIndex, m_swapchainLayouts.size());

  vk::ImageMemoryBarrier2 toPresent{};
  toPresent.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  toPresent.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
  // Present is external to the pipeline; this is a release barrier.
  toPresent.dstStageMask = {};
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

void VulkanRenderingSession::transitionSceneCopyToLayout(vk::CommandBuffer cmd, uint32_t imageIndex, vk::ImageLayout newLayout)
{
  Assertion(imageIndex < m_device.swapchainImageCount(),
    "transitionSceneCopyToLayout: imageIndex %u out of bounds (swapchain has %u images)",
    imageIndex, m_device.swapchainImageCount());

  const auto oldLayout = m_targets.sceneColorLayout(imageIndex);
  if (oldLayout == newLayout) {
    return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.sceneColorImage(imageIndex);
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSceneColorLayout(imageIndex, newLayout);
}

void VulkanRenderingSession::transitionSceneHdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.sceneHdrLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.sceneHdrImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSceneHdrLayout(newLayout);
}

void VulkanRenderingSession::transitionSceneEffectToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.sceneEffectLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.sceneEffectImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSceneEffectLayout(newLayout);
}

void VulkanRenderingSession::transitionCockpitDepthToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.cockpitDepthLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.cockpitDepthImage();
  barrier.subresourceRange.aspectMask = m_targets.depthAttachmentAspectMask();
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setCockpitDepthLayout(newLayout);
}

void VulkanRenderingSession::transitionPostLdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.postLdrLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.postLdrImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setPostLdrLayout(newLayout);
}

void VulkanRenderingSession::transitionPostLuminanceToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.postLuminanceLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.postLuminanceImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setPostLuminanceLayout(newLayout);
}

void VulkanRenderingSession::transitionSmaaEdgesToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.smaaEdgesLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.smaaEdgesImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSmaaEdgesLayout(newLayout);
}

void VulkanRenderingSession::transitionSmaaBlendToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.smaaBlendLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.smaaBlendImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSmaaBlendLayout(newLayout);
}

void VulkanRenderingSession::transitionSmaaOutputToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.smaaOutputLayout();
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.smaaOutputImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSmaaOutputLayout(newLayout);
}

void VulkanRenderingSession::transitionBloomToLayout(vk::CommandBuffer cmd, uint32_t pingPongIndex, vk::ImageLayout newLayout)
{
  Assertion(pingPongIndex < VulkanRenderTargets::kBloomPingPongCount,
	"transitionBloomToLayout pingPongIndex out of range (%u)", pingPongIndex);

  const auto oldLayout = m_targets.bloomLayout(pingPongIndex);
  if (oldLayout == newLayout) {
	return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.bloomImage(pingPongIndex);
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = VulkanRenderTargets::kBloomMipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setBloomLayout(pingPongIndex, newLayout);
}

// ---- Dynamic state ----

void VulkanRenderingSession::applyDynamicState(vk::CommandBuffer cmd) {
  const uint32_t attachmentCount = m_activeInfo.colorAttachmentCount;
  const bool hasDepthAttachment = (m_activeInfo.depthFormat != vk::Format::eUndefined);

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
