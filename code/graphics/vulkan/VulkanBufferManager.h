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
	void* mapBuffer(gr_buffer_handle handle);
	void flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size);

	vk::Buffer getBuffer(gr_buffer_handle handle) const;
	BufferType getBufferType(gr_buffer_handle handle) const;

	void cleanup();

  private:
	vk::Device m_device;
	vk::PhysicalDeviceMemoryProperties m_memoryProperties;
	vk::Queue m_transferQueue;
	uint32_t m_transferQueueIndex;

	std::vector<VulkanBuffer> m_buffers;

	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
	vk::BufferUsageFlags getVkUsageFlags(BufferType type) const;
	vk::MemoryPropertyFlags getMemoryProperties(BufferUsageHint usage) const;
};

} // namespace vulkan
} // namespace graphics
