#pragma once

#include "VulkanRingBuffer.h"
#include "graphics/grinternal.h"

#include <vulkan/vulkan.hpp>
#include <cstdint>

namespace graphics {
namespace vulkan {

struct ModelUniformState {
	gr_buffer_handle bufferHandle{};          // Engine-level buffer handle currently bound to descriptor
	uint32_t dynamicOffset = UINT32_MAX;      // Dynamic offset for next draw; UINT32_MAX means "unset"
};

class VulkanFrame {
  public:
	VulkanFrame(vk::Device device,
		uint32_t queueFamilyIndex,
		const vk::PhysicalDeviceMemoryProperties& memoryProps,
		vk::DeviceSize uniformBufferSize,
		vk::DeviceSize uniformAlignment,
		vk::DeviceSize vertexBufferSize,
		vk::DeviceSize vertexAlignment,
		vk::DeviceSize stagingBufferSize,
		vk::DeviceSize stagingAlignment);

	void wait_for_gpu();
	void reset();

	vk::CommandBuffer commandBuffer() const { return m_commandBuffer; }
	VulkanRingBuffer& uniformBuffer() { return m_uniformRing; }
	VulkanRingBuffer& vertexBuffer() { return m_vertexRing; }
	VulkanRingBuffer& stagingBuffer() { return m_stagingRing; }
	vk::Fence inflightFence() const { return m_inflightFence.get(); }

	vk::Semaphore imageAvailable() const { return m_imageAvailable.get(); }
	vk::Semaphore renderFinished() const { return m_renderFinished.get(); }
	vk::Semaphore timelineSemaphore() const { return m_timelineSemaphore.get(); }

	uint64_t currentTimelineValue() const { return m_timelineValue; }
	uint64_t nextTimelineValue() const { return m_timelineValue + 1; }
	void advanceTimeline() { ++m_timelineValue; }

	vk::DescriptorSet modelDescriptorSet = nullptr;
	ModelUniformState modelUniformState;

  private:
	vk::Device m_device;

	vk::UniqueCommandPool m_commandPool;
	vk::CommandBuffer m_commandBuffer;
	vk::UniqueFence m_inflightFence;

	vk::UniqueSemaphore m_imageAvailable;
	vk::UniqueSemaphore m_renderFinished;
	vk::UniqueSemaphore m_timelineSemaphore;
	uint64_t m_timelineValue = 0;

	VulkanRingBuffer m_uniformRing;
	VulkanRingBuffer m_vertexRing;
	VulkanRingBuffer m_stagingRing;
};

} // namespace vulkan
} // namespace graphics
