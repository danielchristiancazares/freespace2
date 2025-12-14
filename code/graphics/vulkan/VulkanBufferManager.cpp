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
		return vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
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
	m_buffers.push_back(std::move(buffer));
	return gr_buffer_handle(static_cast<int>(m_buffers.size() - 1));
}

void VulkanBufferManager::deleteBuffer(gr_buffer_handle handle)
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in deleteBuffer", handle.value());

	auto& buffer = m_buffers[handle.value()];

	if (buffer.mapped) {
		m_device.unmapMemory(buffer.memory.get());
		buffer.mapped = nullptr;
	}

	if (buffer.buffer) {
		RetiredBuffer retired;
		retired.buffer = std::move(buffer.buffer);
		retired.memory = std::move(buffer.memory);
		retired.retiredAtFrame = m_currentFrame;
		m_retiredBuffers.push_back(std::move(retired));
	}

	buffer.size = 0;
}

void VulkanBufferManager::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in updateBufferData", handle.value());
	Assertion(size > 0, "Buffer size must be > 0 in updateBufferData");

	auto& buffer = m_buffers[handle.value()];

	if (size != buffer.size || buffer.size == 0) {
		if (buffer.mapped) {
			m_device.unmapMemory(buffer.memory.get());
			buffer.mapped = nullptr;
		}

		if (buffer.buffer) {
			RetiredBuffer retired;
			retired.buffer = std::move(buffer.buffer);
			retired.memory = std::move(buffer.memory);
			retired.retiredAtFrame = m_currentFrame;
			m_retiredBuffers.push_back(std::move(retired));
		}

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
		buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);
		buffer.size = size;
	}

	if (buffer.mapped) {
		memcpy(static_cast<char*>(buffer.mapped), static_cast<const char*>(data), size);
	} else {
		UNREACHABLE("Cannot update device-local buffer without staging buffer!");
	}
}

void VulkanBufferManager::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data)
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in updateBufferDataOffset", handle.value());

	auto& buffer = m_buffers[handle.value()];

	if (!buffer.mapped) {
		UNREACHABLE("Cannot update buffer offset without mapped memory!");
		return;
	}

	if (offset + size > buffer.size) {
		UNREACHABLE("Buffer update offset out of bounds!");
		return;
	}

	if (data == nullptr) {
		return;
	}

	void* dest = static_cast<char*>(buffer.mapped) + offset;
	memcpy(static_cast<char*>(dest), static_cast<const char*>(data), size);
}

void* VulkanBufferManager::mapBuffer(gr_buffer_handle handle)
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in mapBuffer", handle.value());

	auto& buffer = m_buffers[handle.value()];

	Assertion(buffer.usage == BufferUsageHint::PersistentMapping,
	          "mapBuffer called on non-persistent buffer");

	return buffer.mapped;
}

void VulkanBufferManager::flushMappedBuffer(gr_buffer_handle handle, size_t /*offset*/, size_t /*size*/)
{
	// No-op for HOST_COHERENT memory
	(void)handle;
}

vk::Buffer VulkanBufferManager::getBuffer(gr_buffer_handle handle) const
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in getBuffer", handle.value());

	return m_buffers[handle.value()].buffer.get();
}

BufferType VulkanBufferManager::getBufferType(gr_buffer_handle handle) const
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in getBufferType", handle.value());

	return m_buffers[handle.value()].type;
}

void VulkanBufferManager::resizeBuffer(gr_buffer_handle handle, size_t size)
{
	Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
	          "Invalid buffer handle %d in resizeBuffer", handle.value());
	Assertion(size > 0, "Buffer size must be > 0 in resizeBuffer");

	auto& buffer = m_buffers[handle.value()];

	if (size == buffer.size) {
		return;
	}

	if (buffer.buffer) {
		if (buffer.mapped) {
			m_device.unmapMemory(buffer.memory.get());
			buffer.mapped = nullptr;
		}

		RetiredBuffer retired;
		retired.buffer = std::move(buffer.buffer);
		retired.memory = std::move(buffer.memory);
		retired.retiredAtFrame = m_currentFrame;
		m_retiredBuffers.push_back(std::move(retired));
	}

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
	buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);
	buffer.size = size;
}

void VulkanBufferManager::onFrameEnd()
{
	++m_currentFrame;

	auto it = m_retiredBuffers.begin();
	while (it != m_retiredBuffers.end()) {
		if (m_currentFrame - it->retiredAtFrame >= FRAMES_BEFORE_DELETE) {
			it = m_retiredBuffers.erase(it);
		} else {
			++it;
		}
	}
}

void VulkanBufferManager::cleanup()
{
	for (auto& buffer : m_buffers) {
		if (buffer.mapped) {
			m_device.unmapMemory(buffer.memory.get());
			buffer.mapped = nullptr;
		}
	}
	m_buffers.clear();
	m_retiredBuffers.clear();
}

} // namespace vulkan
} // namespace graphics
