#include "VulkanBufferManager.h"

#include "graphics/2d.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace graphics {
namespace vulkan {

VulkanBufferManager::VulkanBufferManager(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memoryProps,
                                         vk::Queue transferQueue, uint32_t transferQueueIndex)
    : m_device(device), m_memoryProperties(memoryProps), m_transferQueue(transferQueue),
      m_transferQueueIndex(transferQueueIndex) {
  // Used for synchronous staging uploads to device-local buffers (Static usage hint).
  vk::CommandPoolCreateInfo poolInfo{};
  poolInfo.queueFamilyIndex = transferQueueIndex;
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  m_transferCommandPool = m_device.createCommandPoolUnique(poolInfo);
  Assertion(m_transferCommandPool, "Failed to create Vulkan transfer command pool");
}

uint32_t VulkanBufferManager::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const {
  for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) && (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  throw std::runtime_error("Failed to find suitable memory type.");
}

vk::BufferUsageFlags VulkanBufferManager::getVkUsageFlags(BufferType type) const {
  switch (type) {
  case BufferType::Vertex:
    return vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
           vk::BufferUsageFlagBits::eTransferDst;
  case BufferType::Index:
    return vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst;
  case BufferType::Uniform:
    return vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eTransferDst;
  default:
    UNREACHABLE("Unhandled buffer type!");
    return vk::BufferUsageFlags();
  }
}

vk::MemoryPropertyFlags VulkanBufferManager::getMemoryProperties(BufferUsageHint usage) const {
  switch (usage) {
  case BufferUsageHint::Static:
    // Prefer device-local memory; updates are handled via staging uploads when needed.
    return vk::MemoryPropertyFlagBits::eDeviceLocal;
  case BufferUsageHint::Dynamic:
  case BufferUsageHint::Streaming:
  case BufferUsageHint::PersistentMapping:
    return vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
  default:
    UNREACHABLE("Unhandled usage hint!");
    return vk::MemoryPropertyFlags();
  }
}

void VulkanBufferManager::uploadToDeviceLocal(const VulkanBuffer &buffer, vk::DeviceSize dstOffset, vk::DeviceSize size,
                                              const void *data) {
  Assertion(buffer.buffer, "uploadToDeviceLocal called with null destination buffer");
  Assertion(data != nullptr, "uploadToDeviceLocal called with null data");
  Assertion(m_transferCommandPool, "uploadToDeviceLocal requires a valid transfer command pool");
  Assertion(size > 0, "uploadToDeviceLocal requires size > 0");
  Assertion(dstOffset + size <= buffer.size, "uploadToDeviceLocal range exceeds destination buffer size");

  // Create a transient staging buffer for this upload.
  vk::BufferCreateInfo stagingInfo{};
  stagingInfo.size = size;
  stagingInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
  stagingInfo.sharingMode = vk::SharingMode::eExclusive;

  auto stagingBuffer = m_device.createBufferUnique(stagingInfo);
  auto stagingReqs = m_device.getBufferMemoryRequirements(stagingBuffer.get());

  vk::MemoryAllocateInfo stagingAlloc{};
  stagingAlloc.allocationSize = stagingReqs.size;
  stagingAlloc.memoryTypeIndex = findMemoryType(
      stagingReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

  auto stagingMemory = m_device.allocateMemoryUnique(stagingAlloc);
  m_device.bindBufferMemory(stagingBuffer.get(), stagingMemory.get(), 0);

  void *mapped = m_device.mapMemory(stagingMemory.get(), 0, size);
  std::memcpy(mapped, data, static_cast<size_t>(size));
  m_device.unmapMemory(stagingMemory.get());

  // Record copy into a one-time command buffer and wait for completion.
  vk::CommandBufferAllocateInfo allocInfo{};
  allocInfo.commandPool = m_transferCommandPool.get();
  allocInfo.level = vk::CommandBufferLevel::ePrimary;
  allocInfo.commandBufferCount = 1;
  const auto cmdBuffers = m_device.allocateCommandBuffers(allocInfo);
  Assertion(!cmdBuffers.empty(), "Failed to allocate transfer command buffer");
  vk::CommandBuffer cmd = cmdBuffers[0];

  vk::CommandBufferBeginInfo beginInfo{};
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmd.begin(beginInfo);

  vk::BufferCopy copy{};
  copy.srcOffset = 0;
  copy.dstOffset = dstOffset;
  copy.size = size;
  cmd.copyBuffer(stagingBuffer.get(), buffer.buffer.get(), 1, &copy);

  // Make transfer writes visible to later reads in subsequent submissions.
  vk::PipelineStageFlags2 dstStage = vk::PipelineStageFlagBits2::eAllCommands;
  vk::AccessFlags2 dstAccess = vk::AccessFlagBits2::eMemoryRead;
  switch (buffer.type) {
  case BufferType::Vertex:
    dstStage = vk::PipelineStageFlagBits2::eVertexInput | vk::PipelineStageFlagBits2::eVertexShader;
    dstAccess = vk::AccessFlagBits2::eVertexAttributeRead | vk::AccessFlagBits2::eShaderRead;
    break;
  case BufferType::Index:
    dstStage = vk::PipelineStageFlagBits2::eVertexInput;
    dstAccess = vk::AccessFlagBits2::eIndexRead;
    break;
  case BufferType::Uniform:
    dstStage = vk::PipelineStageFlagBits2::eVertexShader | vk::PipelineStageFlagBits2::eFragmentShader;
    dstAccess = vk::AccessFlagBits2::eUniformRead;
    break;
  default:
    break;
  }

  vk::BufferMemoryBarrier2 barrier{};
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  barrier.dstStageMask = dstStage;
  barrier.dstAccessMask = dstAccess;
  barrier.buffer = buffer.buffer.get();
  barrier.offset = dstOffset;
  barrier.size = size;

  vk::DependencyInfo depInfo{};
  depInfo.bufferMemoryBarrierCount = 1;
  depInfo.pBufferMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(depInfo);

  cmd.end();

  vk::FenceCreateInfo fenceInfo{};
  auto fence = m_device.createFenceUnique(fenceInfo);

  vk::SubmitInfo submit{};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  m_transferQueue.submit(submit, fence.get());

  const vk::Fence fenceHandle = fence.get();
  const auto waitResult = m_device.waitForFences(1, &fenceHandle, VK_TRUE, std::numeric_limits<uint64_t>::max());
  Assertion(waitResult == vk::Result::eSuccess, "Failed waiting for buffer upload fence");

  // Safe after fence wait.
  m_device.resetCommandPool(m_transferCommandPool.get());
}

gr_buffer_handle VulkanBufferManager::createBuffer(BufferType type, BufferUsageHint usage) {
  VulkanBuffer buffer;
  buffer.type = type;
  buffer.usage = usage;
  buffer.size = 0;

  // Buffer will be created/resized on first update
  // Don't create buffer yet - wait for first updateBufferData call

  m_buffers.push_back(std::move(buffer));
  return gr_buffer_handle(static_cast<int>(m_buffers.size() - 1));
}

void VulkanBufferManager::deleteBuffer(gr_buffer_handle handle) {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in deleteBuffer", handle.value());

  auto &buffer = m_buffers[handle.value()];

  // Unmap if mapped
  if (buffer.mapped) {
    m_device.unmapMemory(buffer.memory.get());
    buffer.mapped = nullptr;
  }

  // Retire buffer for deferred deletion (GPU may still be using it).
  if (buffer.buffer) {
    // Be conservative: if deleted during a frame, ensure we wait for at least the next submit to complete.
    const uint64_t retireSerial = m_safeRetireSerial + 1;
    m_deferredReleases.enqueue(retireSerial,
                               [buf = std::move(buffer.buffer), mem = std::move(buffer.memory)]() mutable {});
  }

  // Mark slot as invalid
  buffer.size = 0;
}

void VulkanBufferManager::updateBufferData(gr_buffer_handle handle, size_t size, const void *data) {
  Assertion(size > 0, "Buffer size must be > 0 in updateBufferData");
  auto &buffer = m_buffers[handle.value()];

  // Match OpenGL semantics:
  // - gr_update_buffer_data() maps to glBufferData(), which recreates storage (orphaning) for non-persistent buffers.
  // - The engine relies on this for Dynamic/Streaming buffers to avoid overwriting GPU-in-flight data with
  //   multiple frames in flight.
  if (buffer.usage == BufferUsageHint::Dynamic || buffer.usage == BufferUsageHint::Streaming) {
    // Always recreate storage (even if size is unchanged).
    resizeBuffer(handle, size);
  } else {
    ensureBuffer(handle, static_cast<vk::DeviceSize>(size));
  }

  // refresh reference after potential resize
  auto &updatedBuffer = m_buffers[handle.value()];

  if (data == nullptr) {
    // Allocation-only (used by persistent mapping). Caller will write later via mapBuffer().
    return;
  }

  // Upload data
  if (updatedBuffer.mapped) {
    // Host-visible: direct copy
    memcpy(static_cast<char *>(updatedBuffer.mapped), static_cast<const char *>(data), size);
    // Host-coherent memory doesn't need explicit flush
  } else {
    // Device-local: stage and copy.
    uploadToDeviceLocal(updatedBuffer, 0, static_cast<vk::DeviceSize>(size), data);
  }
}

void VulkanBufferManager::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size,
                                                 const void *data) {
  // OpenGL allows 0-byte glBufferSubData calls; the engine may issue these in edge cases
  // (e.g., building an empty uniform buffer when nothing is visible). Treat as a no-op.
  if (size == 0) {
    return;
  }
  const vk::DeviceSize required = static_cast<vk::DeviceSize>(offset + size);
  ensureBuffer(handle, required);

  auto &buffer = m_buffers[handle.value()];

  if (data == nullptr) {
    // Null data pointer - skip copy (caller may initialize buffer separately)
    return;
  }

  if (!buffer.mapped) {
    uploadToDeviceLocal(buffer, static_cast<vk::DeviceSize>(offset), static_cast<vk::DeviceSize>(size), data);
    return;
  }

  // Copy to mapped memory
  void *dest = static_cast<char *>(buffer.mapped) + offset;
  memcpy(static_cast<char *>(dest), static_cast<const char *>(data), size);
  // Host-coherent memory doesn't need explicit flush
}

void *VulkanBufferManager::mapBuffer(gr_buffer_handle handle) {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in mapBuffer", handle.value());

  auto &buffer = m_buffers[handle.value()];

  Assertion(buffer.usage == BufferUsageHint::PersistentMapping, "mapBuffer called on non-persistent buffer");

  return buffer.mapped;
}

void VulkanBufferManager::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size) {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in flushMappedBuffer", handle.value());

  auto &buffer = m_buffers[handle.value()];

  Assertion(buffer.mapped != nullptr, "flushMappedBuffer called on unmapped buffer");
  (void)offset;
  (void)size;

  // For host-coherent memory, flush is a no-op
  // For non-coherent, we'd need vkFlushMappedMemoryRanges
  // Since we use HOST_COHERENT for host-visible buffers, this is a no-op
}

vk::Buffer VulkanBufferManager::getBuffer(gr_buffer_handle handle) const {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in getBuffer", handle.value());

  return m_buffers[handle.value()].buffer.get();
}

BufferType VulkanBufferManager::getBufferType(gr_buffer_handle handle) const {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in getBufferType", handle.value());

  return m_buffers[handle.value()].type;
}

void VulkanBufferManager::resizeBuffer(gr_buffer_handle handle, size_t size) {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in resizeBuffer", handle.value());
  Assertion(size > 0, "Buffer size must be > 0 in resizeBuffer");

  auto &buffer = m_buffers[handle.value()];

  // OpenGL treats "resize to same size" as an orphaning hint (new storage) which is relied upon by
  // `gr_reset_immediate_buffer()` to avoid CPU overwriting data still in use by the GPU across frames.
  // Mirror that behavior for host-visible dynamic/streaming buffers. For other usages, a same-size resize
  // is a no-op.
  if (size == buffer.size) {
    if (buffer.usage != BufferUsageHint::Dynamic && buffer.usage != BufferUsageHint::Streaming) {
      return;
    }
  }

  // Retire the old buffer if it exists
  if (buffer.buffer) {
    // Unmap if mapped
    if (buffer.mapped) {
      m_device.unmapMemory(buffer.memory.get());
      buffer.mapped = nullptr;
    }

    const uint64_t retireSerial = m_safeRetireSerial + 1;
    m_deferredReleases.enqueue(retireSerial,
                               [oldBuf = std::move(buffer.buffer), oldMem = std::move(buffer.memory)]() mutable {});
  }

  // Create new buffer with the new size
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

  buffer.mapped = nullptr;
  if (getMemoryProperties(buffer.usage) & vk::MemoryPropertyFlagBits::eHostVisible) {
    buffer.mapped = m_device.mapMemory(buffer.memory.get(), 0, VK_WHOLE_SIZE);
  }

  buffer.size = size;
}

vk::Buffer VulkanBufferManager::ensureBuffer(gr_buffer_handle handle, vk::DeviceSize minSize) {
  Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
            "Invalid buffer handle %d in ensureBuffer", handle.value());
  Assertion(minSize > 0, "ensureBuffer requires minSize > 0");

  auto &buffer = m_buffers[handle.value()];

  if (!buffer.buffer || buffer.size < minSize) {
    resizeBuffer(handle, static_cast<size_t>(minSize));
  }

  return buffer.buffer.get();
}

void VulkanBufferManager::cleanup() {
  // Unmap all mapped buffers
  for (auto &buffer : m_buffers) {
    if (buffer.mapped) {
      m_device.unmapMemory(buffer.memory.get());
      buffer.mapped = nullptr;
    }
  }

  m_deferredReleases.clear();
  m_buffers.clear();
}

} // namespace vulkan
} // namespace graphics
