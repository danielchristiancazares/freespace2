#include "VulkanDescriptorLayouts.h"

#include <array>

namespace graphics {
namespace vulkan {

VulkanDescriptorLayouts::VulkanDescriptorLayouts(vk::Device device) : m_device(device)
{
	std::array<vk::DescriptorSetLayoutBinding, 3> globalBindings{};
	globalBindings[0].binding = 0;
	globalBindings[0].descriptorCount = 1;
	globalBindings[0].descriptorType = vk::DescriptorType::eCombinedImageSampler;
	globalBindings[0].stageFlags = vk::ShaderStageFlagBits::eFragment;

	globalBindings[1].binding = 1;
	globalBindings[1].descriptorCount = 1;
	globalBindings[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
	globalBindings[1].stageFlags = vk::ShaderStageFlagBits::eFragment;

	globalBindings[2].binding = 2;
	globalBindings[2].descriptorCount = 1;
	globalBindings[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
	globalBindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;

	vk::DescriptorSetLayoutCreateInfo globalLayoutInfo;
	globalLayoutInfo.bindingCount = static_cast<uint32_t>(globalBindings.size());
	globalLayoutInfo.pBindings = globalBindings.data();

	m_globalLayout = m_device.createDescriptorSetLayoutUnique(globalLayoutInfo);

	std::array<vk::DescriptorSetLayoutBinding, 3> perDrawBindings{};
	perDrawBindings[0].binding = 0;
	perDrawBindings[0].descriptorCount = 1;
	perDrawBindings[0].descriptorType = vk::DescriptorType::eUniformBuffer;
	perDrawBindings[0].stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

	perDrawBindings[1].binding = 1;
	perDrawBindings[1].descriptorCount = 1;
	perDrawBindings[1].descriptorType = vk::DescriptorType::eUniformBuffer;
	perDrawBindings[1].stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

	perDrawBindings[2].binding = 2;
	perDrawBindings[2].descriptorCount = 1;
	perDrawBindings[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
	perDrawBindings[2].stageFlags = vk::ShaderStageFlagBits::eFragment;

	vk::DescriptorSetLayoutCreateInfo perDrawLayoutInfo;
	perDrawLayoutInfo.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	perDrawLayoutInfo.bindingCount = static_cast<uint32_t>(perDrawBindings.size());
	perDrawLayoutInfo.pBindings = perDrawBindings.data();
	m_perDrawPushLayout = m_device.createDescriptorSetLayoutUnique(perDrawLayoutInfo);

	// Set order: set 0 = per-draw push descriptors, set 1 = global descriptors
	std::array<vk::DescriptorSetLayout, 2> setLayouts = {
		m_perDrawPushLayout.get(),
		m_globalLayout.get(),
	};
	vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
	pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	m_pipelineLayout = m_device.createPipelineLayoutUnique(pipelineLayoutInfo);

	std::array<vk::DescriptorPoolSize, 1> poolSizes{};
	poolSizes[0].type = vk::DescriptorType::eCombinedImageSampler;
	poolSizes[0].descriptorCount = 3;

	vk::DescriptorPoolCreateInfo poolInfo;
	poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	m_descriptorPool = m_device.createDescriptorPoolUnique(poolInfo);
}

vk::DescriptorSet VulkanDescriptorLayouts::allocateGlobalSet()
{
	vk::DescriptorSetAllocateInfo allocInfo;
	allocInfo.descriptorPool = m_descriptorPool.get();
	allocInfo.descriptorSetCount = 1;
	auto layout = m_globalLayout.get();
	allocInfo.pSetLayouts = &layout;

	return m_device.allocateDescriptorSets(allocInfo).front();
}

} // namespace vulkan
} // namespace graphics
