#include "VulkanRenderingSession.h"
#include "VulkanDebug.h"
#include "VulkanSync2Helpers.h"
#include "VulkanTextureManager.h"
#include "osapi/outwnd.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

namespace graphics {
namespace vulkan {

VulkanRenderingSession::VulkanRenderingSession(VulkanDevice &device, VulkanRenderTargets &targets,
                                               VulkanTextureManager &textures)
    : m_device(device), m_targets(targets), m_textures(textures) {
  m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
  m_swapchainGeneration = m_device.swapchainGeneration();
  setTarget<SwapchainWithDepthTarget>();
  m_clearOps = ClearOps::clearAll();
  m_gbufferLoadOps.fill(vk::AttachmentLoadOp::eClear);
}

void VulkanRenderingSession::beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex) {
  const auto swapchainGeneration = m_device.swapchainGeneration();
  if (m_swapchainLayouts.size() != m_device.swapchainImageCount() || m_swapchainGeneration != swapchainGeneration) {
    m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
    m_swapchainGeneration = swapchainGeneration;
  }

  setTarget<SwapchainWithDepthTarget>();
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

RenderTargetInfo VulkanRenderingSession::ensureRendering(vk::CommandBuffer cmd, uint32_t imageIndex) {
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

void VulkanRenderingSession::requestSwapchainTarget() { setTarget<SwapchainWithDepthTarget>(); }

void VulkanRenderingSession::requestSwapchainNoDepthTarget() { setTarget<SwapchainNoDepthTarget>(); }

void VulkanRenderingSession::requestSceneHdrTarget() { setTarget<SceneHdrWithDepthTarget>(); }

void VulkanRenderingSession::requestSceneHdrNoDepthTarget() { setTarget<SceneHdrNoDepthTarget>(); }

void VulkanRenderingSession::requestPostLdrTarget() { setTarget<PostLdrTarget>(); }

void VulkanRenderingSession::requestPostLuminanceTarget() { setTarget<PostLuminanceTarget>(); }

void VulkanRenderingSession::requestSmaaEdgesTarget() { setTarget<SmaaEdgesTarget>(); }

void VulkanRenderingSession::requestSmaaBlendTarget() { setTarget<SmaaBlendTarget>(); }

void VulkanRenderingSession::requestSmaaOutputTarget() { setTarget<SmaaOutputTarget>(); }

void VulkanRenderingSession::requestBloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel) {
  setTarget<BloomMipTarget>(pingPongIndex, mipLevel);
}

bool VulkanRenderingSession::targetIsSceneHdr() const { return m_target && m_target->isSceneHdr(); }

bool VulkanRenderingSession::targetIsSwapchain() const { return m_target && m_target->isSwapchain(); }

const char *VulkanRenderingSession::debugTargetName() const { return m_target ? m_target->debugName() : "none"; }

void VulkanRenderingSession::requestBitmapTarget(int bitmapHandle, int face) {
  const auto fmt = m_textures.renderTargetFormat(bitmapHandle);
  setTarget<BitmapTarget>(bitmapHandle, face, fmt);
}

void VulkanRenderingSession::useMainDepthAttachment() {
  endActivePass();
  m_depthAttachment = DepthAttachmentKind::Main;
}

void VulkanRenderingSession::useCockpitDepthAttachment() {
  endActivePass();
  m_depthAttachment = DepthAttachmentKind::Cockpit;
}

void VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs, bool preserveEmissive) {
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
  setTarget<DeferredGBufferTarget>();
}

void VulkanRenderingSession::requestDeferredGBufferTarget() { setTarget<DeferredGBufferTarget>(); }

void VulkanRenderingSession::requestGBufferEmissiveTarget() { setTarget<GBufferEmissiveTarget>(); }

void VulkanRenderingSession::requestGBufferAttachmentTarget(uint32_t gbufferIndex) {
  setTarget<GBufferAttachmentTarget>(gbufferIndex);
}

void VulkanRenderingSession::captureSwapchainColorToSceneCopy(vk::CommandBuffer cmd, uint32_t imageIndex) {
  if ((m_device.swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
    return;
  }

  endActivePass();

  transitionSwapchainToLayout(cmd, imageIndex, vk::ImageLayout::eTransferSrcOptimal, __func__);
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

  cmd.copyImage(m_device.swapchainImage(imageIndex), vk::ImageLayout::eTransferSrcOptimal,
                m_targets.sceneColorImage(imageIndex), vk::ImageLayout::eTransferDstOptimal, 1, &region);

  transitionSceneCopyToLayout(cmd, imageIndex, vk::ImageLayout::eShaderReadOnlyOptimal);
  transitionSwapchainToLayout(cmd, imageIndex, vk::ImageLayout::eColorAttachmentOptimal, __func__);
}

void VulkanRenderingSession::captureSwapchainColorToRenderTarget(vk::CommandBuffer cmd, uint32_t imageIndex,
                                                                 int renderTargetHandle) {
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

  endActivePass();

  transitionSwapchainToLayout(cmd, imageIndex, vk::ImageLayout::eTransferSrcOptimal, __func__);
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

  cmd.copyImage(m_device.swapchainImage(imageIndex), vk::ImageLayout::eTransferSrcOptimal,
                m_textures.renderTargetImage(renderTargetHandle), vk::ImageLayout::eTransferDstOptimal, 1, &region);

  m_textures.transitionRenderTargetToShaderRead(cmd, renderTargetHandle);
  transitionSwapchainToLayout(cmd, imageIndex, vk::ImageLayout::eColorAttachmentOptimal, __func__);
}

void VulkanRenderingSession::transitionSceneHdrToShaderRead(vk::CommandBuffer cmd) {
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionMainDepthToShaderRead(vk::CommandBuffer cmd) {
  // Use the same read layout helper as deferred (depth+stencil read-only when applicable).
  const auto oldLayout = m_targets.depthLayout();
  const auto newLayout = m_targets.depthReadLayout();
  transitionImageLayout(cmd, m_targets.depthImage(), oldLayout, newLayout, m_targets.depthAttachmentAspectMask(), 1, 1);
  m_targets.setDepthLayout(newLayout);
}

void VulkanRenderingSession::transitionCockpitDepthToShaderRead(vk::CommandBuffer cmd) {
  transitionCockpitDepthToLayout(cmd, m_targets.depthReadLayout());
}

void VulkanRenderingSession::transitionPostLdrToShaderRead(vk::CommandBuffer cmd) {
  transitionPostLdrToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionPostLuminanceToShaderRead(vk::CommandBuffer cmd) {
  transitionPostLuminanceToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionSmaaEdgesToShaderRead(vk::CommandBuffer cmd) {
  transitionSmaaEdgesToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionSmaaBlendToShaderRead(vk::CommandBuffer cmd) {
  transitionSmaaBlendToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionSmaaOutputToShaderRead(vk::CommandBuffer cmd) {
  transitionSmaaOutputToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::transitionBloomToShaderRead(vk::CommandBuffer cmd, uint32_t pingPongIndex) {
  transitionBloomToLayout(cmd, pingPongIndex, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderingSession::copySceneHdrToEffect(vk::CommandBuffer cmd) {
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

  cmd.copyImage(m_targets.sceneHdrImage(), vk::ImageLayout::eTransferSrcOptimal, m_targets.sceneEffectImage(),
                vk::ImageLayout::eTransferDstOptimal, 1, &region);

  // Effect snapshot is sampled by distortion/effects; keep it shader-readable.
  transitionSceneEffectToLayout(cmd, vk::ImageLayout::eShaderReadOnlyOptimal);

  // Resume scene rendering: scene HDR back to attachment layout.
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
}

void VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd) {
  Assertion(m_target && m_target->isDeferredGBuffer(),
            "endDeferredGeometry called when not in deferred gbuffer target");

  endActivePass();
  transitionGBufferToShaderRead(cmd);
  setTarget<SwapchainNoDepthTarget>();
}

void VulkanRenderingSession::endActivePass() {
  // End dynamic rendering if active. Boundaries always own this responsibility.
  m_activePass.reset();
}

void VulkanRenderingSession::requestClear() { m_clearOps = ClearOps::clearAll(); }

void VulkanRenderingSession::requestDepthClear() { m_clearOps = m_clearOps.withDepthStencilClear(); }

void VulkanRenderingSession::setClearColor(float r, float g, float b, float a) {
  m_clearColor[0] = r;
  m_clearColor[1] = g;
  m_clearColor[2] = b;
  m_clearColor[3] = a;
}

// ---- Target state implementations ----

RenderTargetInfo VulkanRenderingSession::SwapchainWithDepthTarget::info(const VulkanDevice &device,
                                                                        const VulkanRenderTargets &targets) const {
  RenderTargetInfo out{};
  out.colorFormat = device.swapchainFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = targets.depthFormat();
  return out;
}

void VulkanRenderingSession::SwapchainWithDepthTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                             uint32_t imageIndex) {
  s.beginSwapchainRenderingInternal(cmd, imageIndex);
}

RenderTargetInfo VulkanRenderingSession::SceneHdrWithDepthTarget::info(const VulkanDevice & /*device*/,
                                                                       const VulkanRenderTargets &targets) const {
  RenderTargetInfo out{};
  out.colorFormat = targets.sceneHdrFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = targets.depthFormat();
  return out;
}

void VulkanRenderingSession::SceneHdrWithDepthTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                            uint32_t /*imageIndex*/) {
  s.beginSceneHdrRenderingInternal(cmd);
}

RenderTargetInfo VulkanRenderingSession::SceneHdrNoDepthTarget::info(const VulkanDevice & /*device*/,
                                                                     const VulkanRenderTargets &targets) const {
  RenderTargetInfo out{};
  out.colorFormat = targets.sceneHdrFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SceneHdrNoDepthTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                          uint32_t /*imageIndex*/) {
  s.beginSceneHdrRenderingNoDepthInternal(cmd);
}

RenderTargetInfo VulkanRenderingSession::PostLdrTarget::info(const VulkanDevice & /*device*/,
                                                             const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::PostLdrTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                  uint32_t /*imageIndex*/) {
  s.transitionPostLdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.postLdrView());
}

RenderTargetInfo VulkanRenderingSession::PostLuminanceTarget::info(const VulkanDevice & /*device*/,
                                                                   const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::PostLuminanceTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                        uint32_t /*imageIndex*/) {
  s.transitionPostLuminanceToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.postLuminanceView());
}

RenderTargetInfo VulkanRenderingSession::SmaaEdgesTarget::info(const VulkanDevice & /*device*/,
                                                               const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SmaaEdgesTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                    uint32_t /*imageIndex*/) {
  s.transitionSmaaEdgesToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.smaaEdgesView());
}

RenderTargetInfo VulkanRenderingSession::SmaaBlendTarget::info(const VulkanDevice & /*device*/,
                                                               const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SmaaBlendTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                    uint32_t /*imageIndex*/) {
  s.transitionSmaaBlendToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.smaaBlendView());
}

RenderTargetInfo VulkanRenderingSession::SmaaOutputTarget::info(const VulkanDevice & /*device*/,
                                                                const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eB8G8R8A8Unorm;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::SmaaOutputTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                     uint32_t /*imageIndex*/) {
  s.transitionSmaaOutputToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, s.m_device.swapchainExtent(), s.m_targets.smaaOutputView());
}

VulkanRenderingSession::BloomMipTarget::BloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel)
    : m_index(pingPongIndex), m_mip(mipLevel) {}

RenderTargetInfo VulkanRenderingSession::BloomMipTarget::info(const VulkanDevice & /*device*/,
                                                              const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = vk::Format::eR16G16B16A16Sfloat;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::BloomMipTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                   uint32_t /*imageIndex*/) {
  Assertion(m_index < VulkanRenderTargets::kBloomPingPongCount, "BloomMipTarget::begin pingPongIndex out of range (%u)",
            m_index);
  Assertion(m_mip < VulkanRenderTargets::kBloomMipLevels, "BloomMipTarget::begin mipLevel out of range (%u)", m_mip);

  // Half-res base extent, then downscale per mip.
  const auto full = s.m_device.swapchainExtent();
  const vk::Extent2D base{std::max(1u, full.width >> 1), std::max(1u, full.height >> 1)};
  const vk::Extent2D ex{std::max(1u, base.width >> m_mip), std::max(1u, base.height >> m_mip)};

  s.transitionBloomToLayout(cmd, m_index, vk::ImageLayout::eColorAttachmentOptimal);
  s.beginOffscreenColorRenderingInternal(cmd, ex, s.m_targets.bloomMipView(m_index, m_mip));
}

RenderTargetInfo VulkanRenderingSession::DeferredGBufferTarget::info(const VulkanDevice &device,
                                                                     const VulkanRenderTargets &targets) const {
  RenderTargetInfo out{};
  out.colorFormat = targets.gbufferFormat();
  out.colorAttachmentCount = VulkanRenderTargets::kGBufferCount;
  out.depthFormat = targets.depthFormat();
  return out;
}

void VulkanRenderingSession::DeferredGBufferTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                          uint32_t /*imageIndex*/) {
  s.beginGBufferRenderingInternal(cmd);
}

VulkanRenderingSession::GBufferAttachmentTarget::GBufferAttachmentTarget(uint32_t gbufferIndex)
    : m_index(gbufferIndex) {}

RenderTargetInfo VulkanRenderingSession::GBufferAttachmentTarget::info(const VulkanDevice & /*device*/,
                                                                       const VulkanRenderTargets &targets) const {
  RenderTargetInfo out{};
  out.colorFormat = targets.gbufferFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::GBufferAttachmentTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                            uint32_t /*imageIndex*/) {
  Assertion(m_index < VulkanRenderTargets::kGBufferCount, "GBufferAttachmentTarget index out of range (%u)", m_index);

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

RenderTargetInfo VulkanRenderingSession::SwapchainNoDepthTarget::info(const VulkanDevice &device,
                                                                      const VulkanRenderTargets &) const {
  RenderTargetInfo out{};
  out.colorFormat = device.swapchainFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined; // No depth
  return out;
}

void VulkanRenderingSession::SwapchainNoDepthTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                           uint32_t imageIndex) {
  s.beginSwapchainRenderingNoDepthInternal(cmd, imageIndex);
}

RenderTargetInfo VulkanRenderingSession::GBufferEmissiveTarget::info(const VulkanDevice & /*device*/,
                                                                     const VulkanRenderTargets &targets) const {
  RenderTargetInfo out{};
  out.colorFormat = targets.gbufferFormat();
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::GBufferEmissiveTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                          uint32_t /*imageIndex*/) {
  s.beginGBufferEmissiveRenderingInternal(cmd);
}

VulkanRenderingSession::BitmapTarget::BitmapTarget(int handle, int face, vk::Format format)
    : m_handle(handle), m_face(face), m_format(format) {}

RenderTargetInfo VulkanRenderingSession::BitmapTarget::info(const VulkanDevice & /*device*/,
                                                            const VulkanRenderTargets & /*targets*/) const {
  RenderTargetInfo out{};
  out.colorFormat = m_format;
  out.colorAttachmentCount = 1;
  out.depthFormat = vk::Format::eUndefined;
  return out;
}

void VulkanRenderingSession::BitmapTarget::begin(VulkanRenderingSession &s, vk::CommandBuffer cmd,
                                                 uint32_t /*imageIndex*/) {
  s.beginBitmapRenderingInternal(cmd, m_handle, m_face);
}

// ---- Internal rendering methods ----

void VulkanRenderingSession::beginSwapchainRenderingInternal(vk::CommandBuffer cmd, uint32_t imageIndex) {
  const auto extent = m_device.swapchainExtent();

  // Switching back to swapchain mid-frame requires re-establishing attachment layout.
  transitionSwapchainToAttachment(cmd, imageIndex);

  // Depth may have been transitioned to shader-read during deferred lighting.
  const vk::ImageView depthView = (m_depthAttachment == DepthAttachmentKind::Main)
                                      ? m_targets.depthAttachmentView()
                                      : m_targets.cockpitDepthAttachmentView();
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

void VulkanRenderingSession::beginSceneHdrRenderingInternal(vk::CommandBuffer cmd) {
  const auto extent = m_device.swapchainExtent();

  // Ensure scene HDR is writable and depth is attached.
  transitionSceneHdrToLayout(cmd, vk::ImageLayout::eColorAttachmentOptimal);
  const vk::ImageView depthView = (m_depthAttachment == DepthAttachmentKind::Main)
                                      ? m_targets.depthAttachmentView()
                                      : m_targets.cockpitDepthAttachmentView();
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

void VulkanRenderingSession::beginSceneHdrRenderingNoDepthInternal(vk::CommandBuffer cmd) {
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

void VulkanRenderingSession::beginOffscreenColorRenderingInternal(vk::CommandBuffer cmd, vk::Extent2D extent,
                                                                  vk::ImageView colorView) {
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
  const vk::ImageView depthView = (m_depthAttachment == DepthAttachmentKind::Main)
                                      ? m_targets.depthAttachmentView()
                                      : m_targets.cockpitDepthAttachmentView();
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

void VulkanRenderingSession::beginGBufferEmissiveRenderingInternal(vk::CommandBuffer cmd) {
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
  renderingInfo.pDepthAttachment = nullptr; // No depth for deferred lighting

  cmd.beginRendering(renderingInfo);

  // Clear ops are one-shot; revert to load after consumption.
  m_clearOps = ClearOps::loadAll();
}

void VulkanRenderingSession::beginBitmapRenderingInternal(vk::CommandBuffer cmd, int bitmapHandle, int face) {
  Assertion(bitmapHandle >= 0, "beginBitmapRenderingInternal called with invalid bitmap handle %d", bitmapHandle);
  Assertion(m_textures.hasRenderTarget(bitmapHandle),
            "beginBitmapRenderingInternal called for non-render-target bitmap %d", bitmapHandle);

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

void VulkanRenderingSession::transitionSwapchainToLayout(vk::CommandBuffer cmd, uint32_t imageIndex,
                                                         vk::ImageLayout newLayout, const char *debugName) {
  const char *name = (debugName != nullptr) ? debugName : "transitionSwapchainToLayout";
  Assertion(imageIndex < m_swapchainLayouts.size(), "%s: imageIndex %u out of bounds (swapchain has %zu images)", name,
            imageIndex, m_swapchainLayouts.size());

  const auto oldLayout = m_swapchainLayouts[imageIndex];
  transitionImageLayout(cmd, m_device.swapchainImage(imageIndex), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor,
                        1, 1);
  m_swapchainLayouts[imageIndex] = newLayout;
}

void VulkanRenderingSession::transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex) {
  transitionSwapchainToLayout(cmd, imageIndex, vk::ImageLayout::eColorAttachmentOptimal, __func__);
}

void VulkanRenderingSession::transitionDepthToAttachment(vk::CommandBuffer cmd) {
  const auto oldLayout = m_targets.depthLayout();
  const auto newLayout = m_targets.depthAttachmentLayout();
  transitionImageLayout(cmd, m_targets.depthImage(), oldLayout, newLayout, m_targets.depthAttachmentAspectMask(), 1, 1);
  m_targets.setDepthLayout(newLayout);
}

void VulkanRenderingSession::transitionCockpitDepthToAttachment(vk::CommandBuffer cmd) {
  transitionCockpitDepthToLayout(cmd, m_targets.depthAttachmentLayout());
}

void VulkanRenderingSession::transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex) {
  transitionSwapchainToLayout(cmd, imageIndex, vk::ImageLayout::ePresentSrcKHR, __func__);
}

void VulkanRenderingSession::transitionGBufferToAttachment(vk::CommandBuffer cmd) {
  std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount> barriers{};
  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    barriers[i] =
        makeImageLayoutBarrier(m_targets.gbufferImage(i), m_targets.gbufferLayout(i),
                               vk::ImageLayout::eColorAttachmentOptimal, vk::ImageAspectFlagBits::eColor, 1, 1);
  }

  submitImageBarriers(cmd, barriers.data(), static_cast<uint32_t>(barriers.size()));

  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    m_targets.setGBufferLayout(i, vk::ImageLayout::eColorAttachmentOptimal);
  }
}

void VulkanRenderingSession::transitionGBufferToShaderRead(vk::CommandBuffer cmd) {
  std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount + 1> barriers{};

  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    barriers[i] =
        makeImageLayoutBarrier(m_targets.gbufferImage(i), m_targets.gbufferLayout(i),
                               vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor, 1, 1);
  }

  // Depth transition
  const auto oldDepthLayout = m_targets.depthLayout();
  const auto newDepthLayout = m_targets.depthReadLayout();
  barriers[VulkanRenderTargets::kGBufferCount] = makeImageLayoutBarrier(
      m_targets.depthImage(), oldDepthLayout, newDepthLayout, m_targets.depthAttachmentAspectMask(), 1, 1);

  submitImageBarriers(cmd, barriers.data(), static_cast<uint32_t>(barriers.size()));

  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    m_targets.setGBufferLayout(i, vk::ImageLayout::eShaderReadOnlyOptimal);
  }
  m_targets.setDepthLayout(newDepthLayout);
}

void VulkanRenderingSession::transitionSceneCopyToLayout(vk::CommandBuffer cmd, uint32_t imageIndex,
                                                         vk::ImageLayout newLayout) {
  Assertion(imageIndex < m_device.swapchainImageCount(),
            "transitionSceneCopyToLayout: imageIndex %u out of bounds (swapchain has %u images)", imageIndex,
            m_device.swapchainImageCount());

  const auto oldLayout = m_targets.sceneColorLayout(imageIndex);
  transitionImageLayout(cmd, m_targets.sceneColorImage(imageIndex), oldLayout, newLayout,
                        vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setSceneColorLayout(imageIndex, newLayout);
}

void VulkanRenderingSession::transitionSceneHdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.sceneHdrLayout();
  transitionImageLayout(cmd, m_targets.sceneHdrImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setSceneHdrLayout(newLayout);
}

void VulkanRenderingSession::transitionSceneEffectToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.sceneEffectLayout();
  transitionImageLayout(cmd, m_targets.sceneEffectImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setSceneEffectLayout(newLayout);
}

void VulkanRenderingSession::transitionCockpitDepthToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.cockpitDepthLayout();
  transitionImageLayout(cmd, m_targets.cockpitDepthImage(), oldLayout, newLayout, m_targets.depthAttachmentAspectMask(),
                        1, 1);
  m_targets.setCockpitDepthLayout(newLayout);
}

void VulkanRenderingSession::transitionPostLdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.postLdrLayout();
  transitionImageLayout(cmd, m_targets.postLdrImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setPostLdrLayout(newLayout);
}

void VulkanRenderingSession::transitionPostLuminanceToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.postLuminanceLayout();
  transitionImageLayout(cmd, m_targets.postLuminanceImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1,
                        1);
  m_targets.setPostLuminanceLayout(newLayout);
}

void VulkanRenderingSession::transitionSmaaEdgesToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.smaaEdgesLayout();
  transitionImageLayout(cmd, m_targets.smaaEdgesImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setSmaaEdgesLayout(newLayout);
}

void VulkanRenderingSession::transitionSmaaBlendToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.smaaBlendLayout();
  transitionImageLayout(cmd, m_targets.smaaBlendImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setSmaaBlendLayout(newLayout);
}

void VulkanRenderingSession::transitionSmaaOutputToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout) {
  const auto oldLayout = m_targets.smaaOutputLayout();
  transitionImageLayout(cmd, m_targets.smaaOutputImage(), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor, 1, 1);
  m_targets.setSmaaOutputLayout(newLayout);
}

void VulkanRenderingSession::transitionBloomToLayout(vk::CommandBuffer cmd, uint32_t pingPongIndex,
                                                     vk::ImageLayout newLayout) {
  Assertion(pingPongIndex < VulkanRenderTargets::kBloomPingPongCount,
            "transitionBloomToLayout pingPongIndex out of range (%u)", pingPongIndex);

  const auto oldLayout = m_targets.bloomLayout(pingPongIndex);
  transitionImageLayout(cmd, m_targets.bloomImage(pingPongIndex), oldLayout, newLayout, vk::ImageAspectFlagBits::eColor,
                        0, VulkanRenderTargets::kBloomMipLevels, 0, 1);
  m_targets.setBloomLayout(pingPongIndex, newLayout);
}

// ---- Dynamic state ----

void VulkanRenderingSession::applyDynamicState(vk::CommandBuffer cmd) {
  const uint32_t attachmentCount = m_activeInfo.colorAttachmentCount;
  const bool hasDepthAttachment = (m_activeInfo.depthFormat != vk::Format::eUndefined);

  cmd.setCullMode(m_cullMode);
  cmd.setFrontFace(vk::FrontFace::eClockwise); // CW compensates for negative viewport height Y-flip
  cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
  const bool depthTest = hasDepthAttachment && m_depthTest;
  const bool depthWrite = hasDepthAttachment && m_depthWrite;
  cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
  cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
  cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  if (m_device.supportsExtendedDynamicState3()) {
    const auto &caps = m_device.extDyn3Caps();
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
