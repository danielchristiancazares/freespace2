#pragma once

#include "graphics/2d.h"

#include <vulkan/vulkan.hpp>
#include <vector>

namespace graphics {
namespace vulkan {

struct VulkanBuffer {
	vk::UniqueBuffer buffer;
	vk::UniqueDeviceMemory memory;
	BufferType type;
	BufferUsageHint usage;
	vk::DeviceSize size = 0;
	void* mapped = nullptr; // For host-visible buffers
	bool isPersistentMapped = false;
};

struct RetiredBuffer {
	vk::UniqueBuffer buffer;
	vk::UniqueDeviceMemory memory;
	uint32_t retiredAtFrame;
};

class VulkanBufferManager {
  public:
	VulkanBufferManager(vk::Device device,
		const vk::PhysicalDeviceMemoryProperties& memoryProps,
		vk::Queue transferQueue,
		uint32_t transferQueueIndex);

	gr_buffer_handle createBuffer(BufferType type, BufferUsageHint usage);
	void deleteBuffer(gr_buffer_handle handle);
	void updateBufferData(gr_buffer_handle handle, size_t size, const void* data);
	void updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data);
	void resizeBuffer(gr_buffer_handle handle, size_t size);
	void* mapBuffer(gr_buffer_handle handle);
	void flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size);

	vk::Buffer getBuffer(gr_buffer_handle handle) const;
	BufferType getBufferType(gr_buffer_handle handle) const;

	void cleanup();

	// Called each frame to process deferred deletions
	void onFrameEnd();

  private:
	vk::Device m_device;
	vk::PhysicalDeviceMemoryProperties m_memoryProperties;
	vk::Queue m_transferQueue;
	uint32_t m_transferQueueIndex;

	std::vector<VulkanBuffer> m_buffers;
	std::vector<RetiredBuffer> m_retiredBuffers;
	uint32_t m_currentFrame = 0;
	static constexpr uint32_t FRAMES_BEFORE_DELETE = 3;

	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
	vk::BufferUsageFlags getVkUsageFlags(BufferType type) const;
	vk::MemoryPropertyFlags getMemoryProperties(BufferUsageHint usage) const;
};

} // namespace vulkan
} // namespace graphics
