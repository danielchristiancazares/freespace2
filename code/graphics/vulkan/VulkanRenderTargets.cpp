#include "VulkanRenderTargets.h"
#include "VulkanDebug.h"

namespace graphics {
namespace vulkan {

VulkanRenderTargets::VulkanRenderTargets(VulkanDevice& device)
	: m_device(device)
{
}

void VulkanRenderTargets::create(vk::Extent2D extent) {
	createDepthResources(extent);
	createGBufferResources(extent);
}

void VulkanRenderTargets::resize(vk::Extent2D newExtent) {
	// Reset initialization flag since we're recreating resources
	m_depthInitialized = false;
	create(newExtent);
}

void VulkanRenderTargets::createDepthResources(vk::Extent2D extent) {
	m_depthFormat = findDepthFormat();

	vk::ImageCreateInfo imageInfo;
	imageInfo.imageType = vk::ImageType::e2D;
	imageInfo.format = m_depthFormat;
	imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = vk::SampleCountFlagBits::e1;
	imageInfo.tiling = vk::ImageTiling::eOptimal;
	imageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
	imageInfo.initialLayout = vk::ImageLayout::eUndefined;

	m_depthImage = m_device.device().createImageUnique(imageInfo);

	vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(m_depthImage.get());
	vk::MemoryAllocateInfo allocInfo;
	allocInfo.allocationSize = memReqs.size;
	allocInfo.memoryTypeIndex = m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

	m_depthMemory = m_device.device().allocateMemoryUnique(allocInfo);
	m_device.device().bindImageMemory(m_depthImage.get(), m_depthMemory.get(), 0);

	vk::ImageViewCreateInfo viewInfo;
	viewInfo.image = m_depthImage.get();
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format = m_depthFormat;
	viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.layerCount = 1;

	m_depthImageView = m_device.device().createImageViewUnique(viewInfo);

	// Create sampled depth view for deferred lighting
	vk::ImageViewCreateInfo sampleViewInfo{};
	sampleViewInfo.image = m_depthImage.get();
	sampleViewInfo.viewType = vk::ImageViewType::e2D;
	sampleViewInfo.format = m_depthFormat;
	sampleViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
	sampleViewInfo.subresourceRange.levelCount = 1;
	sampleViewInfo.subresourceRange.layerCount = 1;
	m_depthSampleView = m_device.device().createImageViewUnique(sampleViewInfo);
}

vk::Format VulkanRenderTargets::findDepthFormat() const
{
	const std::vector<vk::Format> candidates = {
		vk::Format::eD32SfloatS8Uint,
		vk::Format::eD24UnormS8Uint,
		vk::Format::eD32Sfloat,
	};

	for (auto format : candidates) {
		vk::FormatProperties props = m_device.physicalDevice().getFormatProperties(format);
		if (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
			return format;
		}
	}

	return vk::Format::eD32Sfloat; // Fallback
}

void VulkanRenderTargets::createGBufferResources(vk::Extent2D extent) {
	for (uint32_t i = 0; i < kGBufferCount; ++i) {
		vk::ImageCreateInfo imageInfo{};
		imageInfo.imageType = vk::ImageType::e2D;
		imageInfo.format = m_gbufferFormat;
		imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = m_sampleCount;
		imageInfo.tiling = vk::ImageTiling::eOptimal;
		imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
		imageInfo.initialLayout = vk::ImageLayout::eUndefined;

		m_gbufferImages[i] = m_device.device().createImageUnique(imageInfo);

		vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(m_gbufferImages[i].get());
		vk::MemoryAllocateInfo allocInfo{};
		allocInfo.allocationSize = memReqs.size;
		allocInfo.memoryTypeIndex = m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

		m_gbufferMemories[i] = m_device.device().allocateMemoryUnique(allocInfo);
		m_device.device().bindImageMemory(m_gbufferImages[i].get(), m_gbufferMemories[i].get(), 0);

		vk::ImageViewCreateInfo viewInfo{};
		viewInfo.image = m_gbufferImages[i].get();
		viewInfo.viewType = vk::ImageViewType::e2D;
		viewInfo.format = m_gbufferFormat;
		viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;

		m_gbufferViews[i] = m_device.device().createImageViewUnique(viewInfo);
	}

	// Sampler for G-buffer textures in lighting pass
	vk::SamplerCreateInfo samplerInfo{};
	samplerInfo.magFilter = vk::Filter::eLinear;
	samplerInfo.minFilter = vk::Filter::eLinear;
	samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	m_gbufferSampler = m_device.device().createSamplerUnique(samplerInfo);

	vkprintf("Created G-buffer: %u attachments, format %d, %ux%u\n",
		kGBufferCount,
		static_cast<int>(m_gbufferFormat),
		extent.width,
		extent.height);
}

} // namespace vulkan
} // namespace graphics
