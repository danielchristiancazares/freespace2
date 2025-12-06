#pragma once

#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

// Generic per-frame ring buffer that sub-allocates from a single
// host-visible buffer. Supports configurable usage flags (uniform, vertex, etc.)
// Alignment is enforced by the caller via the provided alignment parameter.
class VulkanRingBuffer {
  public:
	struct Allocation {
		vk::DeviceSize offset{0};
		void* mapped{nullptr};
	};

	VulkanRingBuffer() = default;
	VulkanRingBuffer(vk::Device device,
		const vk::PhysicalDeviceMemoryProperties& memoryProps,
		vk::DeviceSize size,
		vk::DeviceSize alignment,
		vk::BufferUsageFlags usage);

	VulkanRingBuffer(VulkanRingBuffer&&) noexcept = default;
	VulkanRingBuffer& operator=(VulkanRingBuffer&&) noexcept = default;

	VulkanRingBuffer(const VulkanRingBuffer&) = delete;
	VulkanRingBuffer& operator=(const VulkanRingBuffer&) = delete;

	Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
	void reset();

	vk::Buffer buffer() const { return m_buffer.get(); }
	vk::DeviceSize size() const { return m_size; }

  private:
	vk::Device m_device;
	vk::UniqueBuffer m_buffer;
	vk::UniqueDeviceMemory m_memory;
	void* m_mapped = nullptr;

	vk::DeviceSize m_size = 0;
	vk::DeviceSize m_alignment = 1;
	vk::DeviceSize m_offset = 0;

	static uint32_t findMemoryType(uint32_t typeFilter,
	                               vk::MemoryPropertyFlags properties,
	                               const vk::PhysicalDeviceMemoryProperties& memoryProps);
};

} // namespace vulkan
} // namespace graphics
