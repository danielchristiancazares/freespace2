#pragma once

#include "VulkanDevice.h"

#include <array>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanRenderTargets {
  public:
	static constexpr uint32_t kGBufferCount = 3;

	explicit VulkanRenderTargets(VulkanDevice& device);

	void create(vk::Extent2D extent);
	void resize(vk::Extent2D newExtent);

	// Depth access
	vk::Format depthFormat() const { return m_depthFormat; }
	vk::ImageView depthAttachmentView() const { return m_depthImageView.get(); }
	vk::ImageView depthSampledView() const { return m_depthSampleView.get(); }
	vk::Image depthImage() const { return m_depthImage.get(); }
	bool isDepthInitialized() const { return m_depthInitialized; }
	void markDepthInitialized() { m_depthInitialized = true; }

	// G-buffer access
	vk::Format gbufferFormat() const { return m_gbufferFormat; }
	vk::Image gbufferImage(uint32_t index) const { return m_gbufferImages[index].get(); }
	vk::ImageView gbufferView(uint32_t index) const { return m_gbufferViews[index].get(); }
	vk::Sampler gbufferSampler() const { return m_gbufferSampler.get(); }

  private:
	void createDepthResources(vk::Extent2D extent);
	void createGBufferResources(vk::Extent2D extent);
	vk::Format findDepthFormat() const;

	VulkanDevice& m_device;

	// Depth resources
	vk::UniqueImage m_depthImage;
	vk::UniqueDeviceMemory m_depthMemory;
	vk::UniqueImageView m_depthImageView;
	vk::UniqueImageView m_depthSampleView;
	vk::Format m_depthFormat = vk::Format::eUndefined;
	bool m_depthInitialized = false;
	vk::SampleCountFlagBits m_sampleCount = vk::SampleCountFlagBits::e1;

	// G-buffer resources
	std::array<vk::UniqueImage, kGBufferCount> m_gbufferImages;
	std::array<vk::UniqueDeviceMemory, kGBufferCount> m_gbufferMemories;
	std::array<vk::UniqueImageView, kGBufferCount> m_gbufferViews;
	vk::UniqueSampler m_gbufferSampler;
	vk::Format m_gbufferFormat = vk::Format::eR16G16B16A16Sfloat;
};

} // namespace vulkan
} // namespace graphics
