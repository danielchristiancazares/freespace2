#include "VulkanFrame.h"
#include "VulkanDebug.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace graphics {
namespace vulkan {

VulkanFrame::VulkanFrame(vk::Device device, uint32_t frameIndex, uint32_t queueFamilyIndex,
                         const vk::PhysicalDeviceMemoryProperties &memoryProps, vk::DeviceSize uniformBufferSize,
                         vk::DeviceSize uniformAlignment, vk::DeviceSize vertexBufferSize,
                         vk::DeviceSize vertexAlignment, vk::DeviceSize stagingBufferSize,
                         vk::DeviceSize stagingAlignment, vk::DescriptorSet globalSet, vk::DescriptorSet modelSet)
    : m_device(device), m_frameIndex(frameIndex),
      m_uniformRing(device, memoryProps, uniformBufferSize, uniformAlignment, vk::BufferUsageFlagBits::eUniformBuffer),
      m_vertexRing(device, memoryProps, vertexBufferSize, vertexAlignment,
                   vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eStorageBuffer),
      m_stagingRing(device, memoryProps, stagingBufferSize, stagingAlignment == 0 ? 1 : stagingAlignment,
                    vk::BufferUsageFlagBits::eTransferSrc),
      m_globalDescriptorSet(globalSet), m_modelDescriptorSet(modelSet) {
  Assertion(m_globalDescriptorSet, "VulkanFrame requires a valid global descriptor set");
  Assertion(m_modelDescriptorSet, "VulkanFrame requires a valid model descriptor set");

  vk::CommandPoolCreateInfo poolInfo;
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
  poolInfo.queueFamilyIndex = queueFamilyIndex;
  m_commandPool = m_device.createCommandPoolUnique(poolInfo);

  vk::CommandBufferAllocateInfo cmdAlloc;
  cmdAlloc.commandPool = m_commandPool.get();
  cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
  cmdAlloc.commandBufferCount = 1;
  m_commandBuffer = m_device.allocateCommandBuffers(cmdAlloc).front();

  vk::FenceCreateInfo fenceInfo;
  fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled; // allow first frame without waiting
  m_inflightFence = m_device.createFenceUnique(fenceInfo);

  vk::SemaphoreTypeCreateInfo timelineType;
  timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
  timelineType.initialValue = m_timelineValue;

  vk::SemaphoreCreateInfo semaphoreInfo;
  semaphoreInfo.pNext = &timelineType;
  m_timelineSemaphore = m_device.createSemaphoreUnique(semaphoreInfo);

  vk::SemaphoreCreateInfo binaryInfo;
  m_imageAvailable = m_device.createSemaphoreUnique(binaryInfo);
}

void VulkanFrame::wait_for_gpu() {
  auto fence = m_inflightFence.get();
  if (!fence) {
    return;
  }

  auto result = m_device.waitForFences(1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
  if (result != vk::Result::eSuccess) {
    Assertion(false, "Fence wait failed for Vulkan frame");
    throw std::runtime_error("Fence wait failed for Vulkan frame");
  }
}

void VulkanFrame::reset() {
#ifdef VULKAN_HPP_NO_EXCEPTIONS
  const auto poolResult = m_device.resetCommandPool(m_commandPool.get());
  if (poolResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to reset command pool for Vulkan frame");
  }
#else
  // Throws vk::SystemError on failure when exceptions are enabled.
  m_device.resetCommandPool(m_commandPool.get());
#endif
  m_uniformRing.reset();
  m_vertexRing.reset();
  m_stagingRing.reset();
}

} // namespace vulkan
} // namespace graphics
