#include "VulkanDescriptorLayouts.h"
#include "VulkanConstants.h"
#include "VulkanModelTypes.h"

#include "globalincs/pstypes.h"

#include <array>

namespace graphics {
namespace vulkan {

void VulkanDescriptorLayouts::validateDeviceLimits(const vk::PhysicalDeviceLimits& limits)
{
	// Hard assert - no silent clamping
	// maxDescriptorSetSampledImages is total across all set layouts in pipeline
	Assertion(limits.maxDescriptorSetSampledImages >= kMaxBindlessTextures,
	          "Device maxDescriptorSetSampledImages (%u) < required %u. "
	          "Vulkan model rendering not supported on this device.",
	          limits.maxDescriptorSetSampledImages, kMaxBindlessTextures);

	// Also validate storage buffer limits
	Assertion(limits.maxDescriptorSetStorageBuffers >= kModelSetsPerPool,
	          "Device maxDescriptorSetStorageBuffers (%u) < required %u",
	          limits.maxDescriptorSetStorageBuffers, kModelSetsPerPool);
}

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
	perDrawLayoutInfo.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptor;
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

	createModelLayouts();
}

void VulkanDescriptorLayouts::createModelLayouts()
{
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};

	// Binding 0: Storage buffer (vertex heap)
	bindings[0].binding = 0;
	bindings[0].descriptorType = vk::DescriptorType::eStorageBuffer;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = vk::ShaderStageFlagBits::eVertex;

	// Binding 1: Texture array (bindless)
	bindings[1].binding = 1;
	bindings[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
	bindings[1].descriptorCount = kMaxBindlessTextures;
	bindings[1].stageFlags = vk::ShaderStageFlagBits::eFragment;

	// Binding 2: ModelData dynamic UBO
	bindings[2].binding = 2;
	bindings[2].descriptorType = vk::DescriptorType::eUniformBufferDynamic;
	bindings[2].descriptorCount = 1;
	bindings[2].stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;

	// Binding flags: ONLY PARTIALLY_BOUND on binding 1
	// PARTIALLY_BOUND: unwritten descriptors OK if never dynamically used
	// Shader MUST gate all texture reads: if (index != ABSENT) { ... }
	std::array<vk::DescriptorBindingFlags, 3> bindingFlags{};
	bindingFlags[0] = {};
	bindingFlags[1] = vk::DescriptorBindingFlagBits::ePartiallyBound;
	bindingFlags[2] = {};

	vk::DescriptorSetLayoutBindingFlagsCreateInfo flagsInfo;
	flagsInfo.bindingCount = static_cast<uint32_t>(bindingFlags.size());
	flagsInfo.pBindingFlags = bindingFlags.data();

	vk::DescriptorSetLayoutCreateInfo layoutInfo;
	layoutInfo.pNext = &flagsInfo;
	layoutInfo.flags = {}; // NO eUpdateAfterBindPool
	layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
	layoutInfo.pBindings = bindings.data();

	m_modelSetLayout = m_device.createDescriptorSetLayoutUnique(layoutInfo);

	// Push constant range
	vk::PushConstantRange pushRange;
	pushRange.stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment;
	pushRange.offset = 0;
	pushRange.size = sizeof(ModelPushConstants);

	// Static assert: push constants within spec minimum (256 bytes for Vulkan 1.4)
	static_assert(sizeof(ModelPushConstants) <= 256,
	              "ModelPushConstants exceeds guaranteed minimum push constant size");
	static_assert(sizeof(ModelPushConstants) % 4 == 0,
	              "ModelPushConstants size must be multiple of 4");

	vk::PipelineLayoutCreateInfo pipelineLayoutInfo;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_modelSetLayout.get();
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;

	m_modelPipelineLayout = m_device.createPipelineLayoutUnique(pipelineLayoutInfo);

	// Descriptor pool - sizes derived from kFramesInFlight, not magic numbers
	std::array<vk::DescriptorPoolSize, 3> poolSizes{};
	poolSizes[0].type = vk::DescriptorType::eStorageBuffer;
	poolSizes[0].descriptorCount = kModelSetsPerPool * 1; // 1 SSBO per set
	poolSizes[1].type = vk::DescriptorType::eCombinedImageSampler;
	poolSizes[1].descriptorCount = kModelSetsPerPool * kMaxBindlessTextures;
	poolSizes[2].type = vk::DescriptorType::eUniformBufferDynamic;
	poolSizes[2].descriptorCount = kModelSetsPerPool * 1; // 1 dynamic UBO per set

	vk::DescriptorPoolCreateInfo poolInfo;
	// eFreeDescriptorSet not strictly needed for fixed ring, but harmless
	poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	poolInfo.maxSets = kModelSetsPerPool; // Exactly what we need, no slack
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();

	m_modelDescriptorPool = m_device.createDescriptorPoolUnique(poolInfo);
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

vk::DescriptorSet VulkanDescriptorLayouts::allocateModelDescriptorSet()
{
	vk::DescriptorSetAllocateInfo allocInfo;
	allocInfo.descriptorPool = m_modelDescriptorPool.get();
	allocInfo.descriptorSetCount = 1;
	auto layout = m_modelSetLayout.get();
	allocInfo.pSetLayouts = &layout;

	return m_device.allocateDescriptorSets(allocInfo).front();
}

} // namespace vulkan
} // namespace graphics
