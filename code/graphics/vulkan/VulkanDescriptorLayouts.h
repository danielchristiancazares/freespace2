#pragma once

#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanDescriptorLayouts {
  public:
	explicit VulkanDescriptorLayouts(vk::Device device);

	vk::DescriptorSetLayout globalSetLayout() const { return m_globalLayout.get(); }
	vk::DescriptorSetLayout perDrawPushLayout() const { return m_perDrawPushLayout.get(); }
	vk::PipelineLayout pipelineLayout() const { return m_pipelineLayout.get(); }

	vk::DescriptorSet allocateGlobalSet();

  private:
	vk::Device m_device;
	vk::UniqueDescriptorPool m_descriptorPool;
	vk::UniqueDescriptorSetLayout m_globalLayout;
	vk::UniqueDescriptorSetLayout m_perDrawPushLayout;
	vk::UniquePipelineLayout m_pipelineLayout;
};

} // namespace vulkan
} // namespace graphics
