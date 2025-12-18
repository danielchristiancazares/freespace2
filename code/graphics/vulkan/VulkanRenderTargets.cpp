#include "VulkanRenderTargets.h"
#include "VulkanDebug.h"

#include <stdexcept>

namespace graphics {
namespace vulkan {

bool VulkanRenderTargets::formatHasStencil(vk::Format format)
{
  switch (format) {
  case vk::Format::eD32SfloatS8Uint:
  case vk::Format::eD24UnormS8Uint:
    return true;
  default:
    return false;
  }
}

bool VulkanRenderTargets::depthHasStencil() const
{
  return formatHasStencil(m_depthFormat);
}

vk::ImageAspectFlags VulkanRenderTargets::depthAttachmentAspectMask() const
{
  auto aspects = vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth);
  if (depthHasStencil()) {
    aspects |= vk::ImageAspectFlagBits::eStencil;
  }
  return aspects;
}

vk::ImageLayout VulkanRenderTargets::depthAttachmentLayout() const
{
  return depthHasStencil() ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eDepthAttachmentOptimal;
}

vk::ImageLayout VulkanRenderTargets::depthReadLayout() const
{
  return depthHasStencil() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal;
}

VulkanRenderTargets::VulkanRenderTargets(VulkanDevice& device)
  : m_device(device)
{
}

void VulkanRenderTargets::create(vk::Extent2D extent) {
  createDepthResources(extent);
  createGBufferResources(extent);
}

void VulkanRenderTargets::resize(vk::Extent2D newExtent) {
  // Reset tracked layouts since we're recreating resources
  m_depthLayout = vk::ImageLayout::eUndefined;
  m_gbufferLayouts.fill(vk::ImageLayout::eUndefined);
  create(newExtent);
}

void VulkanRenderTargets::createDepthResources(vk::Extent2D extent) {
  m_depthFormat = findDepthFormat();
  m_depthLayout = vk::ImageLayout::eUndefined;

  vk::ImageCreateInfo imageInfo;
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.format = m_depthFormat;
  imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = vk::SampleCountFlagBits::e1;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;

  m_depthImage = m_device.device().createImageUnique(imageInfo);

  vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(m_depthImage.get());
  vk::MemoryAllocateInfo allocInfo;
  allocInfo.allocationSize = memReqs.size;
  allocInfo.memoryTypeIndex = m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

  m_depthMemory = m_device.device().allocateMemoryUnique(allocInfo);
  m_device.device().bindImageMemory(m_depthImage.get(), m_depthMemory.get(), 0);

  vk::ImageViewCreateInfo viewInfo;
  viewInfo.image = m_depthImage.get();
  viewInfo.viewType = vk::ImageViewType::e2D;
  viewInfo.format = m_depthFormat;
  viewInfo.subresourceRange.aspectMask = depthAttachmentAspectMask();
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;

  m_depthImageView = m_device.device().createImageViewUnique(viewInfo);

  // Create sampled depth view for deferred lighting
  vk::ImageViewCreateInfo sampleViewInfo{};
  sampleViewInfo.image = m_depthImage.get();
  sampleViewInfo.viewType = vk::ImageViewType::e2D;
  sampleViewInfo.format = m_depthFormat;
  sampleViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
  sampleViewInfo.subresourceRange.levelCount = 1;
  sampleViewInfo.subresourceRange.layerCount = 1;
  m_depthSampleView = m_device.device().createImageViewUnique(sampleViewInfo);

  // Create depth sampler with nearest filtering (linear filtering often unsupported for depth)
  if (!m_depthSampler) {
    vk::SamplerCreateInfo depthSamplerInfo{};
    depthSamplerInfo.magFilter = vk::Filter::eNearest;
    depthSamplerInfo.minFilter = vk::Filter::eNearest;
    depthSamplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    depthSamplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    depthSamplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    depthSamplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    m_depthSampler = m_device.device().createSamplerUnique(depthSamplerInfo);
  }
}

vk::Format VulkanRenderTargets::findDepthFormat() const
{
  // Candidate depth formats in preference order (stencil variants first for
  // future stencil buffer support, pure depth last for maximum compatibility)
  const std::vector<vk::Format> candidates = {
    vk::Format::eD32SfloatS8Uint,
    vk::Format::eD24UnormS8Uint,
    vk::Format::eD32Sfloat,
  };

  // Required features: depth buffer is used both as attachment and sampled
  // for deferred lighting (see createDepthResources usage flags and m_depthSampleView)
  constexpr auto requiredFeatures = vk::FormatFeatureFlagBits::eDepthStencilAttachment |
                                    vk::FormatFeatureFlagBits::eSampledImage;

  for (auto format : candidates) {
    vk::FormatProperties props = m_device.physicalDevice().getFormatProperties(format);
    if ((props.optimalTilingFeatures & requiredFeatures) == requiredFeatures) {
      return format;
    }
  }

  // No suitable format found - this is a fundamental capability failure.
  // Do NOT silently return a fallback format that may not actually work;
  // that would cause undefined behavior during deferred lighting.
  throw std::runtime_error("No suitable depth format found with both attachment and sampling support");
}

void VulkanRenderTargets::createGBufferResources(vk::Extent2D extent) {
    m_gbufferLayouts.fill(vk::ImageLayout::eUndefined);
    for (uint32_t i = 0; i < kGBufferCount; ++i) {
    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = m_gbufferFormat;
    imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = m_sampleCount;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;

    m_gbufferImages[i] = m_device.device().createImageUnique(imageInfo);

    vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(m_gbufferImages[i].get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

    m_gbufferMemories[i] = m_device.device().allocateMemoryUnique(allocInfo);
    m_device.device().bindImageMemory(m_gbufferImages[i].get(), m_gbufferMemories[i].get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = m_gbufferImages[i].get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = m_gbufferFormat;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

        m_gbufferViews[i] = m_device.device().createImageViewUnique(viewInfo);
    }

  // Sampler for G-buffer textures in lighting pass
  vk::SamplerCreateInfo samplerInfo{};
  samplerInfo.magFilter = vk::Filter::eLinear;
  samplerInfo.minFilter = vk::Filter::eLinear;
  samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  m_gbufferSampler = m_device.device().createSamplerUnique(samplerInfo);
}

} // namespace vulkan
} // namespace graphics
