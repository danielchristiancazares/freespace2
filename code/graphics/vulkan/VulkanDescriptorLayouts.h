#pragma once

#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanDescriptorLayouts {
  public:
	explicit VulkanDescriptorLayouts(vk::Device device);

	// Validate device limits before creating layouts - hard assert on failure
	static void validateDeviceLimits(const vk::PhysicalDeviceLimits& limits);

	vk::DescriptorSetLayout globalSetLayout() const { return m_globalLayout.get(); }
	vk::DescriptorSetLayout perDrawPushLayout() const { return m_perDrawPushLayout.get(); }
	vk::PipelineLayout pipelineLayout() const { return m_pipelineLayout.get(); }

	vk::DescriptorSetLayout modelSetLayout() const { return m_modelSetLayout.get(); }
	vk::PipelineLayout modelPipelineLayout() const { return m_modelPipelineLayout.get(); }
	vk::DescriptorPool modelDescriptorPool() const { return m_modelDescriptorPool.get(); }

	vk::PipelineLayout deferredPipelineLayout() const { return m_deferredPipelineLayout.get(); }

	vk::DescriptorSet allocateGlobalSet();
	vk::DescriptorSet allocateModelDescriptorSet();

  private:
	void createModelLayouts();
	void createDeferredLayouts();

	vk::Device m_device;
	vk::UniqueDescriptorPool m_descriptorPool;
	vk::UniqueDescriptorSetLayout m_globalLayout;
	vk::UniqueDescriptorSetLayout m_perDrawPushLayout;
	vk::UniquePipelineLayout m_pipelineLayout;

	vk::UniqueDescriptorSetLayout m_modelSetLayout;
	vk::UniquePipelineLayout m_modelPipelineLayout;
	vk::UniqueDescriptorPool m_modelDescriptorPool;

	vk::UniqueDescriptorSetLayout m_deferredPushLayout;
	vk::UniquePipelineLayout m_deferredPipelineLayout;
};

} // namespace vulkan
} // namespace graphics
