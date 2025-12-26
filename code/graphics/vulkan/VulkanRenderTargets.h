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

  explicit VulkanRenderTargets(VulkanDevice &device);

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
  void setSceneColorLayout(uint32_t swapchainIndex, vk::ImageLayout layout) {
    m_sceneColorLayouts[swapchainIndex] = layout;
  }

  // Scene HDR color (float) used for scene_texture_begin/end + post-processing.
  vk::Format sceneHdrFormat() const { return m_sceneHdrFormat; }
  vk::Image sceneHdrImage() const { return m_sceneHdrImage.get(); }
  vk::ImageView sceneHdrView() const { return m_sceneHdrView.get(); }
  vk::Sampler sceneHdrSampler() const { return m_sceneHdrSampler.get(); }
  vk::ImageLayout sceneHdrLayout() const { return m_sceneHdrLayout; }
  void setSceneHdrLayout(vk::ImageLayout layout) { m_sceneHdrLayout = layout; }

  // Effect texture snapshot (float) captured mid-scene for distortion/effects.
  vk::Image sceneEffectImage() const { return m_sceneEffectImage.get(); }
  vk::ImageView sceneEffectView() const { return m_sceneEffectView.get(); }
  vk::Sampler sceneEffectSampler() const { return m_sceneEffectSampler.get(); }
  vk::ImageLayout sceneEffectLayout() const { return m_sceneEffectLayout; }
  void setSceneEffectLayout(vk::ImageLayout layout) { m_sceneEffectLayout = layout; }

  // Cockpit depth snapshot (used by lightshafts and other depth-aware post effects).
  vk::Image cockpitDepthImage() const { return m_cockpitDepthImage.get(); }
  vk::ImageView cockpitDepthAttachmentView() const { return m_cockpitDepthImageView.get(); }
  vk::ImageView cockpitDepthSampledView() const { return m_cockpitDepthSampleView.get(); }
  vk::ImageLayout cockpitDepthLayout() const { return m_cockpitDepthLayout; }
  void setCockpitDepthLayout(vk::ImageLayout layout) { m_cockpitDepthLayout = layout; }

  // Post-process LDR target (RGBA8)
  vk::Image postLdrImage() const { return m_postLdrImage.get(); }
  vk::ImageView postLdrView() const { return m_postLdrView.get(); }
  vk::ImageLayout postLdrLayout() const { return m_postLdrLayout; }
  void setPostLdrLayout(vk::ImageLayout layout) { m_postLdrLayout = layout; }

  // FXAA luminance target (RGBA8)
  vk::Image postLuminanceImage() const { return m_postLuminanceImage.get(); }
  vk::ImageView postLuminanceView() const { return m_postLuminanceView.get(); }
  vk::ImageLayout postLuminanceLayout() const { return m_postLuminanceLayout; }
  void setPostLuminanceLayout(vk::ImageLayout layout) { m_postLuminanceLayout = layout; }

  // SMAA intermediate/output targets (RGBA8)
  vk::Image smaaEdgesImage() const { return m_smaaEdgesImage.get(); }
  vk::ImageView smaaEdgesView() const { return m_smaaEdgesView.get(); }
  vk::ImageLayout smaaEdgesLayout() const { return m_smaaEdgesLayout; }
  void setSmaaEdgesLayout(vk::ImageLayout layout) { m_smaaEdgesLayout = layout; }

  vk::Image smaaBlendImage() const { return m_smaaBlendImage.get(); }
  vk::ImageView smaaBlendView() const { return m_smaaBlendView.get(); }
  vk::ImageLayout smaaBlendLayout() const { return m_smaaBlendLayout; }
  void setSmaaBlendLayout(vk::ImageLayout layout) { m_smaaBlendLayout = layout; }

  vk::Image smaaOutputImage() const { return m_smaaOutputImage.get(); }
  vk::ImageView smaaOutputView() const { return m_smaaOutputView.get(); }
  vk::ImageLayout smaaOutputLayout() const { return m_smaaOutputLayout; }
  void setSmaaOutputLayout(vk::ImageLayout layout) { m_smaaOutputLayout = layout; }

  // Bloom ping-pong targets (RGBA16F with mip chain)
  static constexpr uint32_t kBloomPingPongCount = 2;
  static constexpr uint32_t kBloomMipLevels = 4;
  vk::Image bloomImage(uint32_t index) const { return m_bloomImages[index].get(); }
  vk::ImageView bloomView(uint32_t index) const { return m_bloomViews[index].get(); } // full mip chain view
  vk::ImageView bloomMipView(uint32_t index, uint32_t mip) const { return m_bloomMipViews[index][mip].get(); }
  vk::ImageLayout bloomLayout(uint32_t index) const { return m_bloomLayouts[index]; }
  void setBloomLayout(uint32_t index, vk::ImageLayout layout) { m_bloomLayouts[index] = layout; }

  vk::Sampler postLinearSampler() const { return m_postLinearSampler.get(); }

private:
  void createDepthResources(vk::Extent2D extent);
  void createGBufferResources(vk::Extent2D extent);
  void createSceneColorResources(vk::Extent2D extent);
  void createScenePostProcessResources(vk::Extent2D extent);
  void createPostProcessResources(vk::Extent2D extent);
  vk::Format findDepthFormat() const;
  static bool formatHasStencil(vk::Format format);

  VulkanDevice &m_device;

  // Depth resources
  vk::UniqueImage m_depthImage;
  vk::UniqueDeviceMemory m_depthMemory;
  vk::UniqueImageView m_depthImageView;
  vk::UniqueImageView m_depthSampleView;
  vk::Format m_depthFormat = vk::Format::eUndefined;
  vk::ImageLayout m_depthLayout = vk::ImageLayout::eUndefined;
  vk::SampleCountFlagBits m_sampleCount = vk::SampleCountFlagBits::e1;

  // Cockpit depth resources (separate attachment)
  vk::UniqueImage m_cockpitDepthImage;
  vk::UniqueDeviceMemory m_cockpitDepthMemory;
  vk::UniqueImageView m_cockpitDepthImageView;
  vk::UniqueImageView m_cockpitDepthSampleView;
  vk::ImageLayout m_cockpitDepthLayout = vk::ImageLayout::eUndefined;

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

  // Scene HDR color (one per frame; not swapchain-indexed)
  vk::UniqueImage m_sceneHdrImage;
  vk::UniqueDeviceMemory m_sceneHdrMemory;
  vk::UniqueImageView m_sceneHdrView;
  vk::UniqueSampler m_sceneHdrSampler;
  vk::ImageLayout m_sceneHdrLayout = vk::ImageLayout::eUndefined;
  // Matches G-buffer format for consistency and to avoid precision loss.
  vk::Format m_sceneHdrFormat = vk::Format::eR16G16B16A16Sfloat;

  // Scene effect snapshot (float; same format as scene HDR)
  vk::UniqueImage m_sceneEffectImage;
  vk::UniqueDeviceMemory m_sceneEffectMemory;
  vk::UniqueImageView m_sceneEffectView;
  vk::UniqueSampler m_sceneEffectSampler;
  vk::ImageLayout m_sceneEffectLayout = vk::ImageLayout::eUndefined;

  // Post-processing intermediates
  vk::UniqueImage m_postLdrImage;
  vk::UniqueDeviceMemory m_postLdrMemory;
  vk::UniqueImageView m_postLdrView;
  vk::ImageLayout m_postLdrLayout = vk::ImageLayout::eUndefined;

  vk::UniqueImage m_postLuminanceImage;
  vk::UniqueDeviceMemory m_postLuminanceMemory;
  vk::UniqueImageView m_postLuminanceView;
  vk::ImageLayout m_postLuminanceLayout = vk::ImageLayout::eUndefined;

  vk::UniqueImage m_smaaEdgesImage;
  vk::UniqueDeviceMemory m_smaaEdgesMemory;
  vk::UniqueImageView m_smaaEdgesView;
  vk::ImageLayout m_smaaEdgesLayout = vk::ImageLayout::eUndefined;

  vk::UniqueImage m_smaaBlendImage;
  vk::UniqueDeviceMemory m_smaaBlendMemory;
  vk::UniqueImageView m_smaaBlendView;
  vk::ImageLayout m_smaaBlendLayout = vk::ImageLayout::eUndefined;

  vk::UniqueImage m_smaaOutputImage;
  vk::UniqueDeviceMemory m_smaaOutputMemory;
  vk::UniqueImageView m_smaaOutputView;
  vk::ImageLayout m_smaaOutputLayout = vk::ImageLayout::eUndefined;

  vk::UniqueSampler m_postLinearSampler;

  // Bloom ping-pong (half-res + mips)
  std::array<vk::UniqueImage, kBloomPingPongCount> m_bloomImages;
  std::array<vk::UniqueDeviceMemory, kBloomPingPongCount> m_bloomMemories;
  std::array<vk::UniqueImageView, kBloomPingPongCount> m_bloomViews;
  std::array<std::array<vk::UniqueImageView, kBloomMipLevels>, kBloomPingPongCount> m_bloomMipViews;
  std::array<vk::ImageLayout, kBloomPingPongCount> m_bloomLayouts{};
};

} // namespace vulkan
} // namespace graphics
