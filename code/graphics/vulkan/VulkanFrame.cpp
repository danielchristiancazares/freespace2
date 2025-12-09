#include "VulkanFrame.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace graphics {
namespace vulkan {

VulkanFrame::VulkanFrame(vk::Device device,
	uint32_t queueFamilyIndex,
	const vk::PhysicalDeviceMemoryProperties& memoryProps,
	vk::DeviceSize uniformBufferSize,
	vk::DeviceSize uniformAlignment,
	vk::DeviceSize vertexBufferSize,
	vk::DeviceSize vertexAlignment,
	vk::DeviceSize stagingBufferSize,
	vk::DeviceSize stagingAlignment)
	: m_device(device),
	  m_uniformRing(device, memoryProps, uniformBufferSize, uniformAlignment,
	                vk::BufferUsageFlagBits::eUniformBuffer),
	  m_vertexRing(device, memoryProps, vertexBufferSize, vertexAlignment,
	               vk::BufferUsageFlagBits::eVertexBuffer),
	  m_stagingRing(device, memoryProps, stagingBufferSize,
	                stagingAlignment == 0 ? 1 : stagingAlignment,
	                vk::BufferUsageFlagBits::eTransferSrc)
{
	vk::CommandPoolCreateInfo poolInfo;
	poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	m_commandPool = m_device.createCommandPoolUnique(poolInfo);

	vk::CommandBufferAllocateInfo cmdAlloc;
	cmdAlloc.commandPool = m_commandPool.get();
	cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
	cmdAlloc.commandBufferCount = 1;
	m_commandBuffer = m_device.allocateCommandBuffers(cmdAlloc).front();

	vk::FenceCreateInfo fenceInfo;
	fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled; // allow first frame without waiting
	m_inflightFence = m_device.createFenceUnique(fenceInfo);

	vk::SemaphoreTypeCreateInfo timelineType;
	timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
	timelineType.initialValue = m_timelineValue;

	vk::SemaphoreCreateInfo semaphoreInfo;
	semaphoreInfo.pNext = &timelineType;
	m_timelineSemaphore = m_device.createSemaphoreUnique(semaphoreInfo);

	vk::SemaphoreCreateInfo binaryInfo;
	m_imageAvailable = m_device.createSemaphoreUnique(binaryInfo);
	m_renderFinished = m_device.createSemaphoreUnique(binaryInfo);
}

void VulkanFrame::wait_for_gpu()
{
	auto fence = m_inflightFence.get();
	if (!fence) {
		return;
	}

	auto result = m_device.waitForFences(1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	if (result != vk::Result::eSuccess) {
		throw std::runtime_error("Fence wait failed for Vulkan frame");
	}
	auto resetResult = m_device.resetFences(1, &fence);
	if (resetResult != vk::Result::eSuccess) {
		throw std::runtime_error("Failed to reset fence for Vulkan frame");
	}
}

void VulkanFrame::reset()
{
	using ResetReturn = decltype(m_device.resetCommandPool(m_commandPool.get()));
	constexpr bool returnsVoid = std::is_same_v<ResetReturn, void>;

	auto reset_pool = [](auto& dev, VkCommandPool pool, std::true_type) {
		dev.resetCommandPool(pool);          // returns void
		return vk::Result::eSuccess;
	};
	auto reset_pool_result = [](auto& dev, VkCommandPool pool, std::false_type) {
		return dev.resetCommandPool(pool);   // returns vk::Result
	};

	vk::Result poolResult = reset_pool(m_device, m_commandPool.get(), std::integral_constant<bool, returnsVoid>{});
	if (poolResult != vk::Result::eSuccess) {
		throw std::runtime_error("Failed to reset command pool for Vulkan frame");
	}
	m_uniformRing.reset();
	m_vertexRing.reset();
	m_stagingRing.reset();
}

} // namespace vulkan
} // namespace graphics
