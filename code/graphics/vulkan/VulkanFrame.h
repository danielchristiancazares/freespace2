#pragma once

#include "VulkanRingBuffer.h"
#include "graphics/grinternal.h"

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanRenderer;
struct RecordingFrame;

struct BoundUniformBuffer {
  gr_buffer_handle handle{};
  size_t offset = 0;
  size_t size = 0;
};

struct DynamicUniformBinding {
  gr_buffer_handle bufferHandle{};
  uint32_t dynamicOffset = 0;
};

class VulkanFrame {
public:
  VulkanFrame(vk::Device device, uint32_t frameIndex, uint32_t queueFamilyIndex,
              const vk::PhysicalDeviceMemoryProperties &memoryProps, vk::DeviceSize uniformBufferSize,
              vk::DeviceSize uniformAlignment, vk::DeviceSize vertexBufferSize, vk::DeviceSize vertexAlignment,
              vk::DeviceSize stagingBufferSize, vk::DeviceSize stagingAlignment, vk::DescriptorSet globalSet,
              vk::DescriptorSet modelSet);

  void wait_for_gpu();
  void reset();

  uint32_t frameIndex() const { return m_frameIndex; }

  VulkanRingBuffer &uniformBuffer() { return m_uniformRing; }
  VulkanRingBuffer &vertexBuffer() { return m_vertexRing; }
  VulkanRingBuffer &stagingBuffer() { return m_stagingRing; }
  vk::Fence inflightFence() const { return m_inflightFence.get(); }

  vk::Semaphore imageAvailable() const { return m_imageAvailable.get(); }
  vk::Semaphore timelineSemaphore() const { return m_timelineSemaphore.get(); }

  uint64_t currentTimelineValue() const { return m_timelineValue; }
  uint64_t nextTimelineValue() const { return m_timelineValue + 1; }
  void advanceTimeline() { ++m_timelineValue; }

  vk::DescriptorSet globalDescriptorSet() const { return m_globalDescriptorSet; }
  vk::DescriptorSet modelDescriptorSet() const { return m_modelDescriptorSet; }
  DynamicUniformBinding modelUniformBinding{gr_buffer_handle::invalid(), 0};
  DynamicUniformBinding sceneUniformBinding{gr_buffer_handle::invalid(), 0};
  uint32_t modelTransformDynamicOffset = 0;
  size_t modelTransformSize = 0;
  BoundUniformBuffer nanovgData;
  BoundUniformBuffer decalGlobalsData;
  BoundUniformBuffer decalInfoData;

  void resetPerFrameBindings() {
    modelUniformBinding = {gr_buffer_handle::invalid(), 0};
    sceneUniformBinding = {gr_buffer_handle::invalid(), 0};
    modelTransformDynamicOffset = 0;
    modelTransformSize = 0;
    nanovgData = {};
    decalGlobalsData = {};
    decalInfoData = {};
  }

private:
  // Only the renderer/frame-flow tokens should be able to record commands.
  friend class VulkanRenderer;
  friend struct RecordingFrame;

  vk::CommandBuffer commandBuffer() const { return m_commandBuffer; }

  vk::Device m_device;
  uint32_t m_frameIndex = 0;

  vk::UniqueCommandPool m_commandPool;
  vk::CommandBuffer m_commandBuffer;
  vk::UniqueFence m_inflightFence;

  vk::UniqueSemaphore m_imageAvailable;
  vk::UniqueSemaphore m_timelineSemaphore;
  uint64_t m_timelineValue = 0;

  vk::DescriptorSet m_globalDescriptorSet{};
  vk::DescriptorSet m_modelDescriptorSet{};

  VulkanRingBuffer m_uniformRing;
  VulkanRingBuffer m_vertexRing;
  VulkanRingBuffer m_stagingRing;
};

} // namespace vulkan
} // namespace graphics
