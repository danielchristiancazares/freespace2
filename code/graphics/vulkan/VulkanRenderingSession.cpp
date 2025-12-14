#include "VulkanRenderingSession.h"
#include "VulkanDebug.h"

namespace graphics {
namespace vulkan {

VulkanRenderingSession::VulkanRenderingSession(VulkanDevice& device,
	VulkanRenderTargets& targets,
	VulkanDescriptorLayouts& descriptorLayouts)
	: m_device(device)
	, m_targets(targets)
	, m_descriptorLayouts(descriptorLayouts)
{
}

void VulkanRenderingSession::resetFrameState() {
	m_deferredActive = false;
	m_deferredGeometryDone = false;
	m_activeMode = RenderMode::Swapchain;
	m_pendingMode = RenderMode::Swapchain;
	m_renderPassActive = false;
}

void VulkanRenderingSession::beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex, vk::DescriptorSet globalDescriptorSet) {
	m_currentImageIndex = imageIndex;
	resetFrameState();

	// Transition swapchain and depth to attachment layouts
	transitionSwapchainToAttachment(cmd, imageIndex);
	transitionDepthToAttachment(cmd);
}

void VulkanRenderingSession::endFrame(vk::CommandBuffer cmd, uint32_t imageIndex) {
	if (m_renderPassActive) {
		cmd.endRendering();
		m_renderPassActive = false;
	}

	// Transition swapchain to present layout
	transitionSwapchainToPresent(cmd, imageIndex);
}

void VulkanRenderingSession::transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex) {
	vk::ImageMemoryBarrier2 toRender{};
	toRender.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
	toRender.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	toRender.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
	toRender.oldLayout = vk::ImageLayout::eUndefined;
	toRender.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
	toRender.image = m_device.swapchainImage(imageIndex);
	toRender.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	toRender.subresourceRange.levelCount = 1;
	toRender.subresourceRange.layerCount = 1;

	vk::DependencyInfo depInfo{};
	depInfo.imageMemoryBarrierCount = 1;
	depInfo.pImageMemoryBarriers = &toRender;
	cmd.pipelineBarrier2(depInfo);
}

void VulkanRenderingSession::transitionDepthToAttachment(vk::CommandBuffer cmd) {
	vk::ImageMemoryBarrier2 toDepth{};
	toDepth.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
	toDepth.dstStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
	toDepth.dstAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
		vk::AccessFlagBits2::eDepthStencilAttachmentRead;
	toDepth.oldLayout = m_targets.isDepthInitialized() ? vk::ImageLayout::eDepthAttachmentOptimal : vk::ImageLayout::eUndefined;
	toDepth.newLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	toDepth.image = m_targets.depthImage();
	toDepth.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	toDepth.subresourceRange.levelCount = 1;
	toDepth.subresourceRange.layerCount = 1;

	vk::DependencyInfo depInfo{};
	depInfo.imageMemoryBarrierCount = 1;
	depInfo.pImageMemoryBarriers = &toDepth;
	cmd.pipelineBarrier2(depInfo);

	m_targets.markDepthInitialized();
}

void VulkanRenderingSession::transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex) {
	vk::ImageMemoryBarrier2 toPresent{};
	toPresent.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
	toPresent.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
	toPresent.dstStageMask = vk::PipelineStageFlagBits2::eBottomOfPipe;
	toPresent.dstAccessMask = {};
	toPresent.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
	toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
	toPresent.image = m_device.swapchainImage(imageIndex);
	toPresent.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
	toPresent.subresourceRange.levelCount = 1;
	toPresent.subresourceRange.layerCount = 1;

	vk::DependencyInfo depInfo{};
	depInfo.imageMemoryBarrierCount = 1;
	depInfo.pImageMemoryBarriers = &toPresent;
	cmd.pipelineBarrier2(depInfo);
}

void VulkanRenderingSession::transitionGBufferToAttachment(vk::CommandBuffer cmd) {
	std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount> barriers{};
	for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
		barriers[i].srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
		barriers[i].dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
		barriers[i].dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
		barriers[i].oldLayout = vk::ImageLayout::eUndefined;
		barriers[i].newLayout = vk::ImageLayout::eColorAttachmentOptimal;
		barriers[i].image = m_targets.gbufferImage(i);
		barriers[i].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		barriers[i].subresourceRange.levelCount = 1;
		barriers[i].subresourceRange.layerCount = 1;
	}

	vk::DependencyInfo dep{};
	dep.imageMemoryBarrierCount = VulkanRenderTargets::kGBufferCount;
	dep.pImageMemoryBarriers = barriers.data();
	cmd.pipelineBarrier2(dep);
}

void VulkanRenderingSession::transitionGBufferToShaderRead(vk::CommandBuffer cmd) {
	std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount + 1> barriers{};

	for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
		barriers[i].srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
		barriers[i].srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
		barriers[i].dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
		barriers[i].dstAccessMask = vk::AccessFlagBits2::eShaderRead;
		barriers[i].oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
		barriers[i].newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		barriers[i].image = m_targets.gbufferImage(i);
		barriers[i].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		barriers[i].subresourceRange.levelCount = 1;
		barriers[i].subresourceRange.layerCount = 1;
	}

	// Depth transition
	auto& bd = barriers[VulkanRenderTargets::kGBufferCount];
	bd.srcStageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests;
	bd.srcAccessMask = vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
	bd.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
	bd.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
	bd.oldLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	bd.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	bd.image = m_targets.depthImage();
	bd.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	bd.subresourceRange.levelCount = 1;
	bd.subresourceRange.layerCount = 1;

	vk::DependencyInfo dep{};
	dep.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
	dep.pImageMemoryBarriers = barriers.data();
	cmd.pipelineBarrier2(dep);
}

void VulkanRenderingSession::beginSwapchainRendering(vk::CommandBuffer cmd, uint32_t imageIndex) {
	const auto extent = m_device.swapchainExtent();

	vk::RenderingAttachmentInfo colorAttachment{};
	colorAttachment.imageView = m_device.swapchainImageView(imageIndex);
	colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	colorAttachment.loadOp = m_shouldClearColor ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	colorAttachment.clearValue = vk::ClearColorValue(m_clearColor);

	vk::RenderingAttachmentInfo depthAttachment{};
	depthAttachment.imageView = m_targets.depthAttachmentView();
	depthAttachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	depthAttachment.loadOp = m_shouldClearDepth ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	depthAttachment.clearValue.depthStencil.depth = m_clearDepth;
	depthAttachment.clearValue.depthStencil.stencil = 0;

	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = &depthAttachment;

	cmd.beginRendering(renderingInfo);

	// Clear flags are one-shot; reset after we consume them
	m_shouldClearColor = false;
	m_shouldClearDepth = false;
}

void VulkanRenderingSession::beginSwapchainRenderingNoDepthInternal(vk::CommandBuffer cmd, uint32_t imageIndex) {
	const auto extent = m_device.swapchainExtent();

	vk::RenderingAttachmentInfo colorAttachment{};
	colorAttachment.imageView = m_device.swapchainImageView(imageIndex);
	colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	// Don't clear - ambient light will overwrite with blend-off, then subsequent lights add
	colorAttachment.loadOp = vk::AttachmentLoadOp::eDontCare;
	colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;

	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachments = &colorAttachment;
	renderingInfo.pDepthAttachment = nullptr;  // No depth for deferred lighting

	cmd.beginRendering(renderingInfo);
}

void VulkanRenderingSession::beginSwapchainRenderingNoDepth(vk::CommandBuffer cmd, uint32_t imageIndex) {
	// End any active render pass
	if (m_renderPassActive) {
		cmd.endRendering();
		m_renderPassActive = false;
	}

	beginSwapchainRenderingNoDepthInternal(cmd, imageIndex);
	m_renderPassActive = true;
	m_activeMode = RenderMode::Swapchain;
}

void VulkanRenderingSession::beginGBufferRendering(vk::CommandBuffer cmd) {
	const auto extent = m_device.swapchainExtent();

	// Transition G-buffer images to color attachment optimal
	transitionGBufferToAttachment(cmd);

	// Setup color attachments for G-buffer
	std::array<vk::RenderingAttachmentInfo, VulkanRenderTargets::kGBufferCount> colorAttachments{};
	for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
		colorAttachments[i].imageView = m_targets.gbufferView(i);
		colorAttachments[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		colorAttachments[i].loadOp = m_shouldClearColor ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
		colorAttachments[i].storeOp = vk::AttachmentStoreOp::eStore;
		colorAttachments[i].clearValue = vk::ClearColorValue(std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
	}

	vk::RenderingAttachmentInfo depthAttachment{};
	depthAttachment.imageView = m_targets.depthAttachmentView();
	depthAttachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
	depthAttachment.loadOp = m_shouldClearDepth ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
	depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	depthAttachment.clearValue.depthStencil.depth = m_clearDepth;
	depthAttachment.clearValue.depthStencil.stencil = 0;

	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
	renderingInfo.layerCount = 1;
	renderingInfo.colorAttachmentCount = VulkanRenderTargets::kGBufferCount;
	renderingInfo.pColorAttachments = colorAttachments.data();
	renderingInfo.pDepthAttachment = &depthAttachment;

	cmd.beginRendering(renderingInfo);

	// Clear flags are one-shot; reset after we consume them
	m_shouldClearColor = false;
	m_shouldClearDepth = false;
}

	void VulkanRenderingSession::applyDynamicState(vk::CommandBuffer cmd, vk::DescriptorSet globalDescriptorSet) {
		const auto extent = m_device.swapchainExtent();
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
		cmd.setDepthTestEnable(m_depthTest ? VK_TRUE : VK_FALSE);
	cmd.setDepthWriteEnable(m_depthWrite ? VK_TRUE : VK_FALSE);
	cmd.setDepthCompareOp(m_depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
	cmd.setStencilTestEnable(VK_FALSE);
	if (m_device.supportsExtendedDynamicState3()) {
		const auto& caps = m_device.extDyn3Caps();
		const uint32_t attachmentCount = currentColorAttachmentCount();
		if (caps.colorBlendEnable) {
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

	// Bind global descriptor set
	cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics, m_descriptorLayouts.pipelineLayout(), 1, 1, &globalDescriptorSet, 0, nullptr);
}

void VulkanRenderingSession::ensureRenderingActive(vk::CommandBuffer cmd, uint32_t imageIndex) {
	// Check if we need to switch render target modes
	if (m_renderPassActive && m_activeMode == m_pendingMode) {
		return;
	}

	// End current rendering if switching modes
	if (m_renderPassActive) {
		cmd.endRendering();
		m_renderPassActive = false;
	}

	// Begin rendering to the appropriate target
	switch (m_pendingMode) {
	case RenderMode::Swapchain:
		beginSwapchainRendering(cmd, imageIndex);
		break;
	case RenderMode::DeferredGBuffer:
		beginGBufferRendering(cmd);
		break;
	}

	m_activeMode = m_pendingMode;
	m_renderPassActive = true;
}

void VulkanRenderingSession::endRendering(vk::CommandBuffer cmd) {
	if (m_renderPassActive) {
		cmd.endRendering();
		m_renderPassActive = false;
	}
}

VkFormat VulkanRenderingSession::currentColorFormat() const {
	return (m_pendingMode == RenderMode::DeferredGBuffer)
		? static_cast<VkFormat>(m_targets.gbufferFormat())
		: static_cast<VkFormat>(m_device.swapchainFormat());
}

uint32_t VulkanRenderingSession::currentColorAttachmentCount() const {
	return (m_pendingMode == RenderMode::DeferredGBuffer)
		? VulkanRenderTargets::kGBufferCount
		: 1;
}

void VulkanRenderingSession::beginDeferredPass(bool clearNonColorBufs) {
	m_deferredActive = true;
	m_deferredGeometryDone = false;
	m_deferredClearNonColorBufs = clearNonColorBufs;
	m_pendingMode = RenderMode::DeferredGBuffer;
	m_shouldClearColor = true;
	m_shouldClearDepth = true;
}

void VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd) {
	if (!m_deferredActive || m_deferredGeometryDone) {
		return;
	}

	// End G-buffer rendering
	if (m_renderPassActive) {
		cmd.endRendering();
		m_renderPassActive = false;
	}

	// Transition G-buffer + depth to shader-read layout
	transitionGBufferToShaderRead(cmd);

	m_deferredGeometryDone = true;
}

void VulkanRenderingSession::requestClear() {
	m_shouldClearColor = true;
	m_shouldClearDepth = true;
}

void VulkanRenderingSession::setClearColor(float r, float g, float b, float a) {
	m_clearColor[0] = r;
	m_clearColor[1] = g;
	m_clearColor[2] = b;
	m_clearColor[3] = a;
}

} // namespace vulkan
} // namespace graphics
