#include "VulkanRenderTargets.h"
#include "VulkanDebug.h"

#include <stdexcept>

namespace graphics {
namespace vulkan {

bool VulkanRenderTargets::formatHasStencil(vk::Format format) {
  switch (format) {
  case vk::Format::eD32SfloatS8Uint:
  case vk::Format::eD24UnormS8Uint:
    return true;
  default:
    return false;
  }
}

bool VulkanRenderTargets::depthHasStencil() const { return formatHasStencil(m_depthFormat); }

vk::ImageAspectFlags VulkanRenderTargets::depthAttachmentAspectMask() const {
  auto aspects = vk::ImageAspectFlags(vk::ImageAspectFlagBits::eDepth);
  if (depthHasStencil()) {
    aspects |= vk::ImageAspectFlagBits::eStencil;
  }
  return aspects;
}

vk::ImageLayout VulkanRenderTargets::depthAttachmentLayout() const {
  return depthHasStencil() ? vk::ImageLayout::eDepthStencilAttachmentOptimal : vk::ImageLayout::eDepthAttachmentOptimal;
}

vk::ImageLayout VulkanRenderTargets::depthReadLayout() const {
  return depthHasStencil() ? vk::ImageLayout::eDepthStencilReadOnlyOptimal : vk::ImageLayout::eShaderReadOnlyOptimal;
}

VulkanRenderTargets::VulkanRenderTargets(VulkanDevice &device) : m_device(device) {}

void VulkanRenderTargets::create(vk::Extent2D extent) {
  createDepthResources(extent);
  createGBufferResources(extent);
  createSceneColorResources(extent);
  createScenePostProcessResources(extent);
  createPostProcessResources(extent);
}

void VulkanRenderTargets::resize(vk::Extent2D newExtent) {
  // Reset tracked layouts since we're recreating resources
  m_depthLayout = vk::ImageLayout::eUndefined;
  m_gbufferLayouts.fill(vk::ImageLayout::eUndefined);
  m_sceneHdrLayout = vk::ImageLayout::eUndefined;
  m_sceneEffectLayout = vk::ImageLayout::eUndefined;
  m_cockpitDepthLayout = vk::ImageLayout::eUndefined;
  m_postLdrLayout = vk::ImageLayout::eUndefined;
  m_postLuminanceLayout = vk::ImageLayout::eUndefined;
  m_smaaEdgesLayout = vk::ImageLayout::eUndefined;
  m_smaaBlendLayout = vk::ImageLayout::eUndefined;
  m_smaaOutputLayout = vk::ImageLayout::eUndefined;
  m_bloomLayouts.fill(vk::ImageLayout::eUndefined);
  create(newExtent);
}

void VulkanRenderTargets::createDepthResources(vk::Extent2D extent) {
  m_depthFormat = findDepthFormat();
  m_depthLayout = vk::ImageLayout::eUndefined;
  m_cockpitDepthLayout = vk::ImageLayout::eUndefined;

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

  // Cockpit depth (separate attachment, same format)
  {
    vk::ImageCreateInfo cinfo;
    cinfo.imageType = vk::ImageType::e2D;
    cinfo.format = m_depthFormat;
    cinfo.extent = vk::Extent3D(extent.width, extent.height, 1);
    cinfo.mipLevels = 1;
    cinfo.arrayLayers = 1;
    cinfo.samples = vk::SampleCountFlagBits::e1;
    cinfo.tiling = vk::ImageTiling::eOptimal;
    cinfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled;
    cinfo.initialLayout = vk::ImageLayout::eUndefined;

    m_cockpitDepthImage = m_device.device().createImageUnique(cinfo);

    vk::MemoryRequirements cockpitMemReqs = m_device.device().getImageMemoryRequirements(m_cockpitDepthImage.get());
    vk::MemoryAllocateInfo cockpitAllocInfo;
    cockpitAllocInfo.allocationSize = cockpitMemReqs.size;
    cockpitAllocInfo.memoryTypeIndex =
        m_device.findMemoryType(cockpitMemReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_cockpitDepthMemory = m_device.device().allocateMemoryUnique(cockpitAllocInfo);
    m_device.device().bindImageMemory(m_cockpitDepthImage.get(), m_cockpitDepthMemory.get(), 0);

    vk::ImageViewCreateInfo cockpitViewInfo;
    cockpitViewInfo.image = m_cockpitDepthImage.get();
    cockpitViewInfo.viewType = vk::ImageViewType::e2D;
    cockpitViewInfo.format = m_depthFormat;
    cockpitViewInfo.subresourceRange.aspectMask = depthAttachmentAspectMask();
    cockpitViewInfo.subresourceRange.levelCount = 1;
    cockpitViewInfo.subresourceRange.layerCount = 1;
    m_cockpitDepthImageView = m_device.device().createImageViewUnique(cockpitViewInfo);

    vk::ImageViewCreateInfo cockpitSampleViewInfo{};
    cockpitSampleViewInfo.image = m_cockpitDepthImage.get();
    cockpitSampleViewInfo.viewType = vk::ImageViewType::e2D;
    cockpitSampleViewInfo.format = m_depthFormat;
    cockpitSampleViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    cockpitSampleViewInfo.subresourceRange.levelCount = 1;
    cockpitSampleViewInfo.subresourceRange.layerCount = 1;
    m_cockpitDepthSampleView = m_device.device().createImageViewUnique(cockpitSampleViewInfo);
  }
}

vk::Format VulkanRenderTargets::findDepthFormat() const {
  // Candidate depth formats in preference order (stencil variants first for
  // future stencil buffer support, pure depth last for maximum compatibility)
  const std::vector<vk::Format> candidates = {
      vk::Format::eD32SfloatS8Uint,
      vk::Format::eD24UnormS8Uint,
      vk::Format::eD32Sfloat,
  };

  // Required features: depth buffer is used both as attachment and sampled
  // for deferred lighting (see createDepthResources usage flags and m_depthSampleView)
  constexpr auto requiredFeatures =
      vk::FormatFeatureFlagBits::eDepthStencilAttachment | vk::FormatFeatureFlagBits::eSampledImage;

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
    allocInfo.memoryTypeIndex =
        m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

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

  vk::SamplerCreateInfo samplerInfo{};
  samplerInfo.magFilter = vk::Filter::eLinear;
  samplerInfo.minFilter = vk::Filter::eLinear;
  samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
  samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
  m_gbufferSampler = m_device.device().createSamplerUnique(samplerInfo);
}

void VulkanRenderTargets::createSceneColorResources(vk::Extent2D extent) {
  const uint32_t imageCount = m_device.swapchainImageCount();
  m_sceneColorImages.clear();
  m_sceneColorMemories.clear();
  m_sceneColorViews.clear();
  m_sceneColorLayouts.clear();

  m_sceneColorImages.reserve(imageCount);
  m_sceneColorMemories.reserve(imageCount);
  m_sceneColorViews.reserve(imageCount);
  m_sceneColorLayouts.reserve(imageCount);

  const vk::Format format = m_device.swapchainFormat();

  // Captured scene color is sampled in a fullscreen copy shader, so ensure the format supports sampling.
  {
    const vk::FormatProperties props = m_device.physicalDevice().getFormatProperties(format);
    const bool sampledOk =
        (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImage) != vk::FormatFeatureFlags{};
    Assertion(sampledOk, "Swapchain format %u does not support sampled image usage (optimalTilingFeatures=0x%x)",
              static_cast<uint32_t>(format), static_cast<uint32_t>(props.optimalTilingFeatures));
  }

  vk::ImageCreateInfo imageInfo{};
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.format = format;
  imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = vk::SampleCountFlagBits::e1;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;

  for (uint32_t i = 0; i < imageCount; ++i) {
    auto image = m_device.device().createImageUnique(imageInfo);

    vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(image.get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex =
        m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);

    auto memory = m_device.device().allocateMemoryUnique(allocInfo);
    m_device.device().bindImageMemory(image.get(), memory.get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = image.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;

    auto view = m_device.device().createImageViewUnique(viewInfo);

    m_sceneColorImages.push_back(std::move(image));
    m_sceneColorMemories.push_back(std::move(memory));
    m_sceneColorViews.push_back(std::move(view));
    m_sceneColorLayouts.push_back(vk::ImageLayout::eUndefined);
  }

  if (!m_sceneColorSampler) {
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    m_sceneColorSampler = m_device.device().createSamplerUnique(samplerInfo);
  }
}

void VulkanRenderTargets::createScenePostProcessResources(vk::Extent2D extent) {
  // Scene HDR target (float) + effect snapshot (float).
  // These are sampled by fullscreen post-process passes and may also be copied via transfer ops.
  const vk::Format format = m_sceneHdrFormat;

  vk::ImageCreateInfo imageInfo{};
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.format = format;
  imageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = vk::SampleCountFlagBits::e1;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;

  // Scene HDR: render target + sampled + transfer src (for effect snapshot).
  imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                    vk::ImageUsageFlagBits::eTransferSrc;

  m_sceneHdrImage = m_device.device().createImageUnique(imageInfo);
  {
    vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(m_sceneHdrImage.get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex =
        m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_sceneHdrMemory = m_device.device().allocateMemoryUnique(allocInfo);
    m_device.device().bindImageMemory(m_sceneHdrImage.get(), m_sceneHdrMemory.get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = m_sceneHdrImage.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    m_sceneHdrView = m_device.device().createImageViewUnique(viewInfo);
  }

  // Effect snapshot: sampled + transfer dst, and allow rendering into it for future parity.
  imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                    vk::ImageUsageFlagBits::eTransferDst;
  m_sceneEffectImage = m_device.device().createImageUnique(imageInfo);
  {
    vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(m_sceneEffectImage.get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex =
        m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_sceneEffectMemory = m_device.device().allocateMemoryUnique(allocInfo);
    m_device.device().bindImageMemory(m_sceneEffectImage.get(), m_sceneEffectMemory.get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = m_sceneEffectImage.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    m_sceneEffectView = m_device.device().createImageViewUnique(viewInfo);
  }

  // Samplers (shared config; separate handles for future specialization).
  if (!m_sceneHdrSampler) {
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    m_sceneHdrSampler = m_device.device().createSamplerUnique(samplerInfo);
  }
  if (!m_sceneEffectSampler) {
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    m_sceneEffectSampler = m_device.device().createSamplerUnique(samplerInfo);
  }

  m_sceneHdrLayout = vk::ImageLayout::eUndefined;
  m_sceneEffectLayout = vk::ImageLayout::eUndefined;
}

void VulkanRenderTargets::createPostProcessResources(vk::Extent2D extent) {
  // Post-processing intermediate targets:
  // - LDR color (RGBA8 UNORM)
  // - FXAA luminance (RGBA8 UNORM)
  // - SMAA edges/blend/output (RGBA8 UNORM)
  // - Bloom ping-pong (RGBA16F, half-res, mip chain)

  const vk::Format ldrFormat = vk::Format::eB8G8R8A8Unorm;

  auto createColorImage = [&](vk::Format format, vk::Extent2D ex, uint32_t mipLevels, vk::ImageUsageFlags usage,
                              vk::UniqueImage &outImg, vk::UniqueDeviceMemory &outMem, vk::UniqueImageView &outView) {
    vk::ImageCreateInfo info{};
    info.imageType = vk::ImageType::e2D;
    info.format = format;
    info.extent = vk::Extent3D(ex.width, ex.height, 1);
    info.mipLevels = mipLevels;
    info.arrayLayers = 1;
    info.samples = vk::SampleCountFlagBits::e1;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = usage;
    info.initialLayout = vk::ImageLayout::eUndefined;

    outImg = m_device.device().createImageUnique(info);

    vk::MemoryRequirements memReqs = m_device.device().getImageMemoryRequirements(outImg.get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex =
        m_device.findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    outMem = m_device.device().allocateMemoryUnique(allocInfo);
    m_device.device().bindImageMemory(outImg.get(), outMem.get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = outImg.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    outView = m_device.device().createImageViewUnique(viewInfo);
  };

  const auto fullUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;

  createColorImage(ldrFormat, extent, 1, fullUsage, m_postLdrImage, m_postLdrMemory, m_postLdrView);
  createColorImage(ldrFormat, extent, 1, fullUsage, m_postLuminanceImage, m_postLuminanceMemory, m_postLuminanceView);
  createColorImage(ldrFormat, extent, 1, fullUsage, m_smaaEdgesImage, m_smaaEdgesMemory, m_smaaEdgesView);
  createColorImage(ldrFormat, extent, 1, fullUsage, m_smaaBlendImage, m_smaaBlendMemory, m_smaaBlendView);
  createColorImage(ldrFormat, extent, 1, fullUsage, m_smaaOutputImage, m_smaaOutputMemory, m_smaaOutputView);

  // Linear sampler for post textures
  if (!m_postLinearSampler) {
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    m_postLinearSampler = m_device.device().createSamplerUnique(samplerInfo);
  }

  // Bloom ping-pong: half-res, RGBA16F, mip chain, also used for blit-based mip generation
  const uint32_t bw = std::max(1u, extent.width >> 1);
  const uint32_t bh = std::max(1u, extent.height >> 1);
  const vk::Extent2D bloomExtent{bw, bh};

  const vk::Format bloomFormat = vk::Format::eR16G16B16A16Sfloat;
  const auto bloomUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                          vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;

  for (uint32_t i = 0; i < kBloomPingPongCount; ++i) {
    vk::UniqueImageView fullView;
    createColorImage(bloomFormat, bloomExtent, kBloomMipLevels, bloomUsage, m_bloomImages[i], m_bloomMemories[i],
                     fullView);
    m_bloomViews[i] = std::move(fullView);

    // Per-mip render views (single mip)
    for (uint32_t mip = 0; mip < kBloomMipLevels; ++mip) {
      vk::ImageViewCreateInfo viewInfo{};
      viewInfo.image = m_bloomImages[i].get();
      viewInfo.viewType = vk::ImageViewType::e2D;
      viewInfo.format = bloomFormat;
      viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      viewInfo.subresourceRange.baseMipLevel = mip;
      viewInfo.subresourceRange.levelCount = 1;
      viewInfo.subresourceRange.baseArrayLayer = 0;
      viewInfo.subresourceRange.layerCount = 1;
      m_bloomMipViews[i][mip] = m_device.device().createImageViewUnique(viewInfo);
    }
  }

  m_postLdrLayout = vk::ImageLayout::eUndefined;
  m_postLuminanceLayout = vk::ImageLayout::eUndefined;
  m_smaaEdgesLayout = vk::ImageLayout::eUndefined;
  m_smaaBlendLayout = vk::ImageLayout::eUndefined;
  m_smaaOutputLayout = vk::ImageLayout::eUndefined;
  m_bloomLayouts.fill(vk::ImageLayout::eUndefined);
}

} // namespace vulkan
} // namespace graphics
