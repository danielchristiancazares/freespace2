#include "VulkanFrame.h"

#include <array>
#include <limits>
#include <stdexcept>

namespace graphics {
namespace vulkan {

VulkanFrame::VulkanFrame(vk::Device device,
	uint32_t queueFamilyIndex,
	const vk::PhysicalDeviceMemoryProperties& memoryProps,
	vk::DeviceSize uniformBufferSize,
	vk::DeviceSize uniformAlignment,
	vk::DeviceSize vertexBufferSize,
	vk::DeviceSize vertexAlignment)
	: m_device(device),
	  m_uniformRing(device, memoryProps, uniformBufferSize, uniformAlignment,
	                vk::BufferUsageFlagBits::eUniformBuffer),
	  m_vertexRing(device, memoryProps, vertexBufferSize, vertexAlignment,
	               vk::BufferUsageFlagBits::eVertexBuffer)
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
	if (m_timelineValue == 0) {
		return;
	}

	vk::SemaphoreWaitInfo waitInfo;
	waitInfo.semaphoreCount = 1;
	auto semaphore = m_timelineSemaphore.get();
	waitInfo.pSemaphores = &semaphore;
	waitInfo.pValues = &m_timelineValue;

	auto result = m_device.waitSemaphores(waitInfo, std::numeric_limits<uint64_t>::max());
	if (result != vk::Result::eSuccess && result != vk::Result::eTimeout) {
		throw std::runtime_error("Timeline semaphore wait failed");
	}
}

void VulkanFrame::reset()
{
	m_device.resetCommandPool(m_commandPool.get());
	m_uniformRing.reset();
	m_vertexRing.reset();
}

} // namespace vulkan
} // namespace graphics
