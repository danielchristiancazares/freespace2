#include "VulkanRingBuffer.h"

#include <stdexcept>

namespace graphics {
namespace vulkan {

VulkanRingBuffer::VulkanRingBuffer(vk::Device device,
	const vk::PhysicalDeviceMemoryProperties& memoryProps,
	vk::DeviceSize size,
	vk::DeviceSize alignment,
	vk::BufferUsageFlags usage)
	: m_device(device), m_size(size), m_alignment(alignment == 0 ? 1 : alignment)
{
	vk::BufferCreateInfo bufferInfo;
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = vk::SharingMode::eExclusive;

	m_buffer = m_device.createBufferUnique(bufferInfo);

	auto requirements = m_device.getBufferMemoryRequirements(m_buffer.get());
	vk::MemoryAllocateInfo allocInfo;
	allocInfo.allocationSize = requirements.size;
	allocInfo.memoryTypeIndex = findMemoryType(
		requirements.memoryTypeBits,
		vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
		memoryProps);

	m_memory = m_device.allocateMemoryUnique(allocInfo);
	m_device.bindBufferMemory(m_buffer.get(), m_memory.get(), 0);

	m_mapped = m_device.mapMemory(m_memory.get(), 0, size);
}

VulkanRingBuffer::Allocation VulkanRingBuffer::allocate(vk::DeviceSize requestSize,
	vk::DeviceSize alignmentOverride)
{
	const vk::DeviceSize align = alignmentOverride ? alignmentOverride : m_alignment;
	vk::DeviceSize alignedOffset = (m_offset + align - 1) & ~(align - 1);

	if (alignedOffset + requestSize > m_size) {
		// Ring buffer overflow; wrap to start.
		alignedOffset = 0;
		if (requestSize > m_size) {
			throw std::runtime_error("Allocation size exceeds ring buffer capacity");
		}
	}

	Allocation result;
	result.offset = alignedOffset;
	result.mapped = static_cast<uint8_t*>(m_mapped) + alignedOffset;

	m_offset = alignedOffset + requestSize;
	return result;
}

void VulkanRingBuffer::reset()
{
	m_offset = 0;
}

uint32_t VulkanRingBuffer::findMemoryType(uint32_t typeFilter,
	vk::MemoryPropertyFlags properties,
	const vk::PhysicalDeviceMemoryProperties& memoryProps)
{
	for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
		if ((typeFilter & (1 << i)) && (memoryProps.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}

	throw std::runtime_error("Failed to find suitable memory type for ring buffer.");
}

} // namespace vulkan
} // namespace graphics
