#pragma once

#include "VulkanDevice.h"

#include "globalincs/pstypes.h"

#include <array>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanRenderTargets {
  public:
  // G-buffer attachments:
  // 0: Color
  // 1: Normal
  // 2: Position
  // 3: Specular
  // 4: Emissive
  static constexpr uint32_t kGBufferCount = 5;
  static constexpr uint32_t kGBufferEmissiveIndex = 4;

  explicit VulkanRenderTargets(VulkanDevice& device);

  void create(vk::Extent2D extent);
  void resize(vk::Extent2D newExtent);

  // Depth access
  vk::Format depthFormat() const { return m_depthFormat; }
  vk::ImageView depthAttachmentView() const { return m_depthImageView.get(); }
  vk::ImageView depthSampledView() const { return m_depthSampleView.get(); }
  vk::Image depthImage() const { return m_depthImage.get(); }
  vk::ImageLayout depthLayout() const { return m_depthLayout; }
  void setDepthLayout(vk::ImageLayout layout) { m_depthLayout = layout; }
  bool depthHasStencil() const;
  vk::ImageAspectFlags depthAttachmentAspectMask() const;
  vk::ImageLayout depthAttachmentLayout() const;
  vk::ImageLayout depthReadLayout() const;

  // G-buffer access
  vk::Format gbufferFormat() const { return m_gbufferFormat; }
  vk::Image gbufferImage(uint32_t index) const { return m_gbufferImages[index].get(); }
  vk::ImageView gbufferView(uint32_t index) const { return m_gbufferViews[index].get(); }
  vk::Sampler gbufferSampler() const { return m_gbufferSampler.get(); }
  vk::Sampler depthSampler() const { return m_depthSampler.get(); }
  vk::ImageLayout gbufferLayout(uint32_t index) const { return m_gbufferLayouts[index]; }
  void setGBufferLayout(uint32_t index, vk::ImageLayout layout) { m_gbufferLayouts[index] = layout; }

  // Scene color snapshot (swapchain-format) used for OpenGL-parity deferred begin:
  // pre-deferred swapchain content is captured and then copied into the emissive G-buffer.
  vk::Image sceneColorImage(uint32_t swapchainIndex) const { return m_sceneColorImages[swapchainIndex].get(); }
  vk::ImageView sceneColorView(uint32_t swapchainIndex) const { return m_sceneColorViews[swapchainIndex].get(); }
  vk::Sampler sceneColorSampler() const { return m_sceneColorSampler.get(); }
  vk::ImageLayout sceneColorLayout(uint32_t swapchainIndex) const { return m_sceneColorLayouts[swapchainIndex]; }
  void setSceneColorLayout(uint32_t swapchainIndex, vk::ImageLayout layout) { m_sceneColorLayouts[swapchainIndex] = layout; }

  private:
  void createDepthResources(vk::Extent2D extent);
  void createGBufferResources(vk::Extent2D extent);
  void createSceneColorResources(vk::Extent2D extent);
  vk::Format findDepthFormat() const;
  static bool formatHasStencil(vk::Format format);

  VulkanDevice& m_device;

  // Depth resources
  vk::UniqueImage m_depthImage;
  vk::UniqueDeviceMemory m_depthMemory;
  vk::UniqueImageView m_depthImageView;
  vk::UniqueImageView m_depthSampleView;
  vk::Format m_depthFormat = vk::Format::eUndefined;
  vk::ImageLayout m_depthLayout = vk::ImageLayout::eUndefined;
  vk::SampleCountFlagBits m_sampleCount = vk::SampleCountFlagBits::e1;

  // G-buffer resources
  std::array<vk::UniqueImage, kGBufferCount> m_gbufferImages;
  std::array<vk::UniqueDeviceMemory, kGBufferCount> m_gbufferMemories;
  std::array<vk::UniqueImageView, kGBufferCount> m_gbufferViews;
  std::array<vk::ImageLayout, kGBufferCount> m_gbufferLayouts{};
  vk::UniqueSampler m_gbufferSampler;
  vk::UniqueSampler m_depthSampler;
  vk::Format m_gbufferFormat = vk::Format::eR16G16B16A16Sfloat;

  // Captured pre-deferred scene color (one per swapchain image)
  SCP_vector<vk::UniqueImage> m_sceneColorImages;
  SCP_vector<vk::UniqueDeviceMemory> m_sceneColorMemories;
  SCP_vector<vk::UniqueImageView> m_sceneColorViews;
  SCP_vector<vk::ImageLayout> m_sceneColorLayouts;
  vk::UniqueSampler m_sceneColorSampler;
};

} // namespace vulkan
} // namespace graphics
