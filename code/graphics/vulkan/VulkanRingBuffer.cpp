#include "VulkanRingBuffer.h"

#include <stdexcept>
#include <optional>

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
	auto result = try_allocate(requestSize, alignmentOverride);
	if (!result) {
		throw std::runtime_error("Allocation size exceeds remaining ring buffer capacity");
	}
	return *result;
}

std::optional<VulkanRingBuffer::Allocation> VulkanRingBuffer::try_allocate(vk::DeviceSize requestSize,
	vk::DeviceSize alignmentOverride)
{
	const vk::DeviceSize align = alignmentOverride ? alignmentOverride : m_alignment;
	vk::DeviceSize alignedOffset = ((m_offset + align - 1) / align) * align;

	// Do not wrap within a frame - this would overwrite in-flight GPU reads
	if (alignedOffset + requestSize > m_size) {
		return std::nullopt;
	}

	Allocation result;
	result.offset = alignedOffset;
	result.mapped = static_cast<uint8_t*>(m_mapped) + alignedOffset;

	m_offset = alignedOffset + requestSize;
	return result;
}

vk::DeviceSize VulkanRingBuffer::remaining() const
{
	const vk::DeviceSize alignedOffset = ((m_offset + m_alignment - 1) / m_alignment) * m_alignment;
	if (alignedOffset >= m_size) {
		return 0;
	}
	return m_size - alignedOffset;
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
		if ((typeFilter & (1u << i)) && (memoryProps.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}

	throw std::runtime_error("Failed to find suitable memory type for ring buffer.");
}

} // namespace vulkan
} // namespace graphics
