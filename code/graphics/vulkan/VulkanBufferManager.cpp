#include "VulkanBufferManager.h"

#include "graphics/2d.h"

#include <cstring>
#include <stdexcept>

namespace graphics {
namespace vulkan {

VulkanBufferManager::VulkanBufferManager(vk::Device device,
	const vk::PhysicalDeviceMemoryProperties& memoryProps,
	vk::Queue transferQueue,
	uint32_t transferQueueIndex)
	: m_device(device)
	, m_memoryProperties(memoryProps)
	, m_transferQueue(transferQueue)
	, m_transferQueueIndex(transferQueueIndex)
{
}

uint32_t VulkanBufferManager::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const
{
	for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
		if ((typeFilter & (1 << i)) &&
			(m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
			return i;
		}
	}
	throw std::runtime_error("Failed to find suitable memory type.");
}

vk::BufferUsageFlags VulkanBufferManager::getVkUsageFlags(BufferType type) const
{
	switch (type) {
	case BufferType::Vertex:
		return vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst;
	case BufferType::Index:
		return vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
	case BufferType::Uniform:
		return vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst;
	default:
		UNREACHABLE("Unhandled buffer type!");
		return vk::BufferUsageFlags();
	}
}

vk::MemoryPropertyFlags VulkanBufferManager::getMemoryProperties(BufferUsageHint usage) const
{
	switch (usage) {
	case BufferUsageHint::Static:
		// For now, use host-visible for Static to allow updates
		// TODO: Use device-local + staging buffer for better performance
		return vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	case BufferUsageHint::Dynamic:
	case BufferUsageHint::Streaming:
	case BufferUsageHint::PersistentMapping:
		return vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
	default:
		UNREACHABLE("Unhandled usage hint!");
		return vk::MemoryPropertyFlags();
	}
}

gr_buffer_handle VulkanBufferManager::createBuffer(BufferType type, BufferUsageHint usage)
{
	VulkanBuffer buffer;
	buffer.type = type;
	buffer.usage = usage;
	buffer.size = 0;

	// Buffer will be created/resized on first update
	// Don't create buffer yet - wait for first updateBufferData call

	m_buffers.push_back(std::move(buffer));
	return gr_buffer_handle(static_cast<int>(m_buffers.size() - 1));
}

void VulkanBufferManager::deleteBuffer(gr_buffer_handle handle)
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return;
	}

	auto& buffer = m_buffers[handle.value()];
	
	// Unmap if mapped
	if (buffer.mapped) {
		m_device.unmapMemory(buffer.memory.get());
		buffer.mapped = nullptr;
	}

	// Buffers are automatically destroyed via UniqueBuffer/UniqueDeviceMemory
	// Mark as invalid by clearing
	buffer.buffer.reset();
	buffer.memory.reset();
	buffer.size = 0;
}

void VulkanBufferManager::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return;
	}

	if (size == 0) {
		return; // Can't create buffer with zero size
	}

	auto& buffer = m_buffers[handle.value()];

	// If size changed or buffer not created yet, recreate buffer
	if (size != buffer.size || buffer.size == 0) {
		// Unmap old buffer if mapped
		if (buffer.mapped) {
			m_device.unmapMemory(buffer.memory.get());
			buffer.mapped = nullptr;
		}

		// Destroy old buffer
		buffer.buffer.reset();
		buffer.memory.reset();

		// Create new buffer with correct size
		vk::BufferCreateInfo bufferInfo;
		bufferInfo.size = size;
		bufferInfo.usage = getVkUsageFlags(buffer.type);
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;

		buffer.buffer = m_device.createBufferUnique(bufferInfo);

		auto memRequirements = m_device.getBufferMemoryRequirements(buffer.buffer.get());
		vk::MemoryAllocateInfo allocInfo;
		allocInfo.allocationSize = memRequirements.size;
		allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, getMemoryProperties(buffer.usage));

		buffer.memory = m_device.allocateMemoryUnique(allocInfo);
		m_device.bindBufferMemory(buffer.buffer.get(), buffer.memory.get(), 0);

		// Remap if host-visible (all our usage hints use host-visible memory)
		buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);

		buffer.size = size;
	}

	// Upload data
	if (buffer.mapped) {
		// Host-visible: direct copy
		memcpy(static_cast<char*>(buffer.mapped), static_cast<const char*>(data), size);
		// Host-coherent memory doesn't need explicit flush
	} else {
		// Device-local: need staging buffer (TODO: implement staging buffer upload)
		// For now, this is an error - device-local buffers need staging
		// In practice, Static buffers should use staging, Dynamic/Streaming should be host-visible
		UNREACHABLE("Cannot update device-local buffer without staging buffer!");
	}
}

void VulkanBufferManager::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data)
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return;
	}

	auto& buffer = m_buffers[handle.value()];

	if (!buffer.mapped) {
		UNREACHABLE("Cannot update buffer offset without mapped memory!");
		return;
	}

	if (offset + size > buffer.size) {
		UNREACHABLE("Buffer update offset out of bounds!");
		return;
	}

	// Copy to mapped memory
	void* dest = static_cast<char*>(buffer.mapped) + offset;
	memcpy(static_cast<char*>(dest), static_cast<const char*>(data), size);
	// Host-coherent memory doesn't need explicit flush
}

void* VulkanBufferManager::mapBuffer(gr_buffer_handle handle)
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return nullptr;
	}

	auto& buffer = m_buffers[handle.value()];

	if (buffer.usage != BufferUsageHint::PersistentMapping) {
		return nullptr; // Only persistent mapping supports explicit mapping
	}

	return buffer.mapped;
}

void VulkanBufferManager::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size)
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return;
	}

	auto& buffer = m_buffers[handle.value()];

	if (!buffer.mapped) {
		return;
	}

	// For host-coherent memory, flush is a no-op
	// For non-coherent, we'd need vkFlushMappedMemoryRanges
	// Since we use HOST_COHERENT for host-visible buffers, this is a no-op
}

vk::Buffer VulkanBufferManager::getBuffer(gr_buffer_handle handle) const
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return vk::Buffer();
	}

	return m_buffers[handle.value()].buffer.get();
}

BufferType VulkanBufferManager::getBufferType(gr_buffer_handle handle) const
{
	if (!handle.isValid() || static_cast<size_t>(handle.value()) >= m_buffers.size()) {
		return BufferType::Vertex; // Default
	}

	return m_buffers[handle.value()].type;
}

void VulkanBufferManager::cleanup()
{
	// Unmap all mapped buffers
	for (auto& buffer : m_buffers) {
		if (buffer.mapped) {
			m_device.unmapMemory(buffer.memory.get());
			buffer.mapped = nullptr;
		}
	}

	m_buffers.clear();
}

} // namespace vulkan
} // namespace graphics
