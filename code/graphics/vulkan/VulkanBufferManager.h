#pragma once

#include "graphics/2d.h"
#include "VulkanDeferredRelease.h"

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
	void resizeBuffer(gr_buffer_handle handle, size_t size);
	void* mapBuffer(gr_buffer_handle handle);
	void flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size);

	// Ensure buffer exists with at least minSize; returns the VkBuffer
	vk::Buffer ensureBuffer(gr_buffer_handle handle, vk::DeviceSize minSize);

	vk::Buffer getBuffer(gr_buffer_handle handle) const;
	BufferType getBufferType(gr_buffer_handle handle) const;

	void cleanup();

	// Serial at/after which it is safe to destroy newly-retired resources.
	// During frame recording this should be the serial of the upcoming submit; after submit it should match the last submitted serial.
	void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }
	void collect(uint64_t completedSerial) { m_deferredReleases.collect(completedSerial); }

  private:
	vk::Device m_device;
	vk::PhysicalDeviceMemoryProperties m_memoryProperties;
	vk::Queue m_transferQueue;
	uint32_t m_transferQueueIndex;

	std::vector<VulkanBuffer> m_buffers;
	DeferredReleaseQueue m_deferredReleases;
	uint64_t m_safeRetireSerial = 0;

	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
	vk::BufferUsageFlags getVkUsageFlags(BufferType type) const;
	vk::MemoryPropertyFlags getMemoryProperties(BufferUsageHint usage) const;
};

} // namespace vulkan
} // namespace graphics
