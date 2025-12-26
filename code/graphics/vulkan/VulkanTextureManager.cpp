#include "VulkanTextureManager.h"
#include "VulkanPhaseContexts.h"

#include "bmpman/bm_internal.h"
#include "bmpman/bmpman.h"
#include "osapi/outwnd.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace graphics {
namespace vulkan {
namespace {

vk::Format selectFormat(const bitmap &bmp) {
  if (bmp.flags & BMP_TEX_DXT1) {
    return vk::Format::eBc1RgbaUnormBlock;
  }
  if (bmp.flags & BMP_TEX_DXT3) {
    return vk::Format::eBc2UnormBlock;
  }
  if (bmp.flags & BMP_TEX_DXT5) {
    return vk::Format::eBc3UnormBlock;
  }
  if (bmp.flags & BMP_TEX_BC7) {
    return vk::Format::eBc7UnormBlock;
  }
  // 8bpp: AABITMAP (font/text alpha) or grayscale/palettized - all treated as single-channel.
  // Upload path memcpy's 1 byte/pixel, so format must match.
  if ((bmp.flags & BMP_AABITMAP) || bmp.bpp == 8) {
    return vk::Format::eR8Unorm;
  }
  // 16bpp and 24bpp get expanded to 4 bytes in upload path.
  // 32bpp is already 4 bytes.
  // bmpman stores pixels as BGRA in memory.
  return vk::Format::eB8G8R8A8Unorm;
}

bool isCompressed(const bitmap &bmp) { return (bmp.flags & BMP_TEX_COMP) != 0; }

uint32_t bytesPerPixel(const bitmap &bmp) {
  switch (bmp.bpp) {
  case 8:
    return 1;
  case 16:
    return 2;
  case 24:
    return 3;
  case 32:
  default:
    return 4;
  }
}

inline bool isDynamicBindlessSlot(uint32_t slot) {
  return slot >= kBindlessFirstDynamicTextureSlot && slot < kMaxBindlessTextures;
}

struct StageAccess {
  vk::PipelineStageFlags2 stageMask{};
  vk::AccessFlags2 accessMask{};
};

StageAccess stageAccessForLayout(vk::ImageLayout layout) {
  StageAccess out{};
  switch (layout) {
  case vk::ImageLayout::eUndefined:
    out.stageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
    out.accessMask = {};
    break;
  case vk::ImageLayout::eColorAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    out.accessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    break;
  case vk::ImageLayout::eShaderReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::eTransferSrcOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferRead;
    break;
  case vk::ImageLayout::eTransferDstOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferWrite;
    break;
  default:
    out.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    out.accessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    break;
  }
  return out;
}

uint32_t mipLevelsForExtent(uint32_t w, uint32_t h) {
  uint32_t levels = 1;
  uint32_t size = (w > h) ? w : h;
  while (size > 1) {
    size >>= 1;
    ++levels;
  }
  return levels;
}

} // namespace

VulkanTextureManager::VulkanTextureManager(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memoryProps,
                                           vk::Queue transferQueue, uint32_t transferQueueIndex)
    : m_device(device), m_memoryProperties(memoryProps), m_transferQueue(transferQueue),
      m_transferQueueIndex(transferQueueIndex) {
  createDefaultSampler();

  createFallbackTexture();
  createDefaultTexture();
  createDefaultNormalTexture();
  createDefaultSpecTexture();

  m_freeBindlessSlots.reserve(kMaxBindlessTextures - kBindlessFirstDynamicTextureSlot);
  for (uint32_t slot = kMaxBindlessTextures; slot-- > kBindlessFirstDynamicTextureSlot;) {
    m_freeBindlessSlots.push_back(slot);
  }
}

uint32_t VulkanTextureManager::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const {
  for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) && (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  throw std::runtime_error("Failed to find suitable memory type.");
}

void VulkanTextureManager::createDefaultSampler() {
  vk::SamplerCreateInfo samplerInfo;
  samplerInfo.magFilter = vk::Filter::eLinear;
  samplerInfo.minFilter = vk::Filter::eLinear;
  samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
  samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
  samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = vk::CompareOp::eAlways;
  samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.minLod = 0.0f;
  // Allow sampling all mip levels present in the bound image view.
  // (maxLod=0 would clamp sampling to base mip even when mipmaps exist.)
  samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

  m_defaultSampler = m_device.createSamplerUnique(samplerInfo);
}

VulkanTexture VulkanTextureManager::createSolidTexture(const uint8_t rgba[4]) {
  // Create a 1x1 RGBA texture for use as a stable descriptor target.
  constexpr uint32_t width = 1;
  constexpr uint32_t height = 1;
  constexpr vk::Format format = vk::Format::eR8G8B8A8Unorm;

  // Create staging buffer
  vk::BufferCreateInfo bufInfo;
  bufInfo.size = 4;
  bufInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
  bufInfo.sharingMode = vk::SharingMode::eExclusive;
  auto stagingBuf = m_device.createBufferUnique(bufInfo);

  auto reqs = m_device.getBufferMemoryRequirements(stagingBuf.get());
  vk::MemoryAllocateInfo allocInfo;
  allocInfo.allocationSize = reqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(reqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible |
                                                                      vk::MemoryPropertyFlagBits::eHostCoherent);
  auto stagingMem = m_device.allocateMemoryUnique(allocInfo);
  m_device.bindBufferMemory(stagingBuf.get(), stagingMem.get(), 0);

  // Copy pixel data to staging
  void *mapped = m_device.mapMemory(stagingMem.get(), 0, 4);
  std::memcpy(mapped, rgba, 4);
  m_device.unmapMemory(stagingMem.get());

  // Create image
  vk::ImageCreateInfo imageInfo;
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.format = format;
  imageInfo.extent = vk::Extent3D(width, height, 1);
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.samples = vk::SampleCountFlagBits::e1;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;

  auto image = m_device.createImageUnique(imageInfo);
  auto imgReqs = m_device.getImageMemoryRequirements(image.get());
  vk::MemoryAllocateInfo imgAllocInfo;
  imgAllocInfo.allocationSize = imgReqs.size;
  imgAllocInfo.memoryTypeIndex = findMemoryType(imgReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
  auto imageMem = m_device.allocateMemoryUnique(imgAllocInfo);
  m_device.bindImageMemory(image.get(), imageMem.get(), 0);

  // Command pool + buffer for upload
  vk::CommandPoolCreateInfo poolInfo;
  poolInfo.queueFamilyIndex = m_transferQueueIndex;
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
  auto uploadPool = m_device.createCommandPoolUnique(poolInfo);

  vk::CommandBufferAllocateInfo cmdAlloc;
  cmdAlloc.commandPool = uploadPool.get();
  cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
  cmdAlloc.commandBufferCount = 1;
  auto cmdBuf = m_device.allocateCommandBuffers(cmdAlloc).front();

  vk::CommandBufferBeginInfo beginInfo;
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmdBuf.begin(beginInfo);

  // Transition to transfer dst
  vk::ImageMemoryBarrier2 toTransfer{};
  toTransfer.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
  toTransfer.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toTransfer.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toTransfer.oldLayout = vk::ImageLayout::eUndefined;
  toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
  toTransfer.image = image.get();
  toTransfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toTransfer.subresourceRange.levelCount = 1;
  toTransfer.subresourceRange.layerCount = 1;

  vk::DependencyInfo depToTransfer{};
  depToTransfer.imageMemoryBarrierCount = 1;
  depToTransfer.pImageMemoryBarriers = &toTransfer;
  cmdBuf.pipelineBarrier2(depToTransfer);

  // Copy buffer to image
  vk::BufferImageCopy region{};
  region.bufferOffset = 0;
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = vk::Extent3D(width, height, 1);
  cmdBuf.copyBufferToImage(stagingBuf.get(), image.get(), vk::ImageLayout::eTransferDstOptimal, 1, &region);

  // Transition to shader read
  vk::ImageMemoryBarrier2 toShader{};
  toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toShader.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
  toShader.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
  toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  toShader.image = image.get();
  toShader.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toShader.subresourceRange.levelCount = 1;
  toShader.subresourceRange.layerCount = 1;

  vk::DependencyInfo depToShader{};
  depToShader.imageMemoryBarrierCount = 1;
  depToShader.pImageMemoryBarriers = &toShader;
  cmdBuf.pipelineBarrier2(depToShader);

  cmdBuf.end();

  // Submit and wait
  vk::SubmitInfo submitInfo;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuf;
  m_transferQueue.submit(submitInfo, nullptr);
  m_transferQueue.waitIdle();

  // Create view
  vk::ImageViewCreateInfo viewInfo;
  viewInfo.image = image.get();
  viewInfo.viewType = vk::ImageViewType::e2DArray;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = 1;
  auto view = m_device.createImageViewUnique(viewInfo);

  VulkanTexture tex{};
  tex.image = std::move(image);
  tex.memory = std::move(imageMem);
  tex.imageView = std::move(view);
  tex.sampler = m_defaultSampler.get();
  tex.width = width;
  tex.height = height;
  tex.layers = 1;
  tex.mipLevels = 1;
  tex.format = format;
  tex.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return tex;
}

vk::Sampler VulkanTextureManager::getOrCreateSampler(const SamplerKey &key) const {
  const size_t hash = (static_cast<size_t>(key.filter) << 4) ^ static_cast<size_t>(key.address);
  auto it = m_samplerCache.find(hash);
  if (it != m_samplerCache.end()) {
    return it->second.get();
  }

  vk::SamplerCreateInfo samplerInfo;
  samplerInfo.magFilter = key.filter;
  samplerInfo.minFilter = key.filter;
  samplerInfo.addressModeU = key.address;
  samplerInfo.addressModeV = key.address;
  samplerInfo.addressModeW = key.address;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = vk::CompareOp::eAlways;
  samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

  auto sampler = m_device.createSamplerUnique(samplerInfo);
  vk::Sampler handle = sampler.get();
  m_samplerCache.emplace(hash, std::move(sampler));
  return handle;
}

bool VulkanTextureManager::uploadImmediate(int baseFrame, bool isAABitmap) {
  int numFrames = 1;
  const int resolvedBase = bm_get_base_frame(baseFrame, &numFrames);
  if (resolvedBase < 0) {
    return false;
  }
  baseFrame = resolvedBase;

  const bool isArray = bm_is_texture_array(baseFrame);
  const uint32_t layers = isArray ? static_cast<uint32_t>(numFrames) : 1u;

  // Lock first frame to determine format/size
  ushort flags = 0;
  int w = 0, h = 0;
  bm_get_info(baseFrame, &w, &h, &flags, nullptr, nullptr);
  auto *bmp = bm_lock(baseFrame, 32, flags);
  if (!bmp) {
    return false;
  }
  const bool compressed = isCompressed(*bmp);
  const vk::Format format = selectFormat(*bmp);
  const bool singleChannel = format == vk::Format::eR8Unorm;
  const uint32_t width = bmp->w;
  const uint32_t height = bmp->h;
  bm_unlock(baseFrame);

  // Validate frames
  if (isArray) {
    for (int i = 0; i < numFrames; ++i) {
      ushort f = 0;
      int fw = 0, fh = 0;
      bm_get_info(baseFrame + i, &fw, &fh, &f, nullptr, nullptr);
      if (static_cast<uint32_t>(fw) != width || static_cast<uint32_t>(fh) != height ||
          (f & BMP_TEX_COMP) != (flags & BMP_TEX_COMP)) {
        return false;
      }
    }
  }

  const auto layout = buildImmediateUploadLayout(width, height, format, layers);
  const size_t layerSize = layout.layerSize;
  const size_t totalSize = layout.totalSize;

  // Create staging buffer
  vk::BufferCreateInfo bufInfo;
  bufInfo.size = static_cast<vk::DeviceSize>(totalSize);
  bufInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
  bufInfo.sharingMode = vk::SharingMode::eExclusive;
  auto stagingBuf = m_device.createBufferUnique(bufInfo);

  auto reqs = m_device.getBufferMemoryRequirements(stagingBuf.get());
  vk::MemoryAllocateInfo allocInfo;
  allocInfo.allocationSize = reqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(reqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible |
                                                                      vk::MemoryPropertyFlagBits::eHostCoherent);
  auto stagingMem = m_device.allocateMemoryUnique(allocInfo);
  m_device.bindBufferMemory(stagingBuf.get(), stagingMem.get(), 0);

  // Map and copy
  void *mapped = m_device.mapMemory(stagingMem.get(), 0, static_cast<vk::DeviceSize>(totalSize));
  for (uint32_t layer = 0; layer < layers; ++layer) {
    const size_t offset = layout.layerOffsets[layer];
    const int frameHandle = isArray ? baseFrame + static_cast<int>(layer) : baseFrame;
    auto *frameBmp = bm_lock(frameHandle, 32, flags);
    if (!frameBmp) {
      m_device.unmapMemory(stagingMem.get());
      return false;
    }
    if (!compressed && !singleChannel && frameBmp->bpp == 24) {
      // Expand to RGBA
      auto *dst = static_cast<uint8_t *>(mapped) + offset;
      auto *src = reinterpret_cast<uint8_t *>(frameBmp->data);
      for (uint32_t i = 0; i < width * height; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
      }
    } else if (!compressed && !singleChannel && frameBmp->bpp == 16) {
      // 16bpp textures in bmpman use A1R5G5B5 packing (see Gr_t_* masks in code/graphics/2d.cpp).
      // Expand to BGRA8 to match eB8G8R8A8Unorm.
      auto *src = reinterpret_cast<uint16_t *>(frameBmp->data);
      auto *dst = static_cast<uint8_t *>(mapped) + offset;
      for (uint32_t i = 0; i < width * height; ++i) {
        const uint16_t pixel = src[i];
        const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
        const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
        const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
        const uint8_t a = (pixel & 0x8000) ? 255u : 0u;

        dst[i * 4 + 0] = b;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = r;
        dst[i * 4 + 3] = a;
      }
    } else if (!compressed && singleChannel) {
      std::memcpy(static_cast<uint8_t *>(mapped) + offset, reinterpret_cast<uint8_t *>(frameBmp->data), layerSize);
    } else {
      // 32bpp or compressed - use actual data size
      size_t actualSize = compressed ? layerSize : static_cast<size_t>(width) * height * bytesPerPixel(*frameBmp);
      std::memcpy(static_cast<uint8_t *>(mapped) + offset, reinterpret_cast<uint8_t *>(frameBmp->data), actualSize);
    }
    bm_unlock(frameHandle);
  }
  m_device.unmapMemory(stagingMem.get());

  // Create image
  vk::ImageCreateInfo imageInfo;
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.format = format;
  imageInfo.extent = vk::Extent3D(width, height, 1);
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = layers;
  imageInfo.samples = vk::SampleCountFlagBits::e1;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;

  auto image = m_device.createImageUnique(imageInfo);
  auto imgReqs = m_device.getImageMemoryRequirements(image.get());
  vk::MemoryAllocateInfo imgAllocInfo;
  imgAllocInfo.allocationSize = imgReqs.size;
  imgAllocInfo.memoryTypeIndex = findMemoryType(imgReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
  auto imageMem = m_device.allocateMemoryUnique(imgAllocInfo);
  m_device.bindImageMemory(image.get(), imageMem.get(), 0);

  // Command pool + buffer
  vk::CommandPoolCreateInfo poolInfo;
  poolInfo.queueFamilyIndex = m_transferQueueIndex;
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
  auto uploadPool = m_device.createCommandPoolUnique(poolInfo);

  vk::CommandBufferAllocateInfo cmdAlloc;
  cmdAlloc.commandPool = uploadPool.get();
  cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
  cmdAlloc.commandBufferCount = 1;
  auto cmdBuf = m_device.allocateCommandBuffers(cmdAlloc).front();

  vk::CommandBufferBeginInfo beginInfo;
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmdBuf.begin(beginInfo);

  vk::ImageMemoryBarrier2 toTransfer{};
  toTransfer.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
  toTransfer.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toTransfer.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toTransfer.oldLayout = vk::ImageLayout::eUndefined;
  toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
  toTransfer.image = image.get();
  toTransfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toTransfer.subresourceRange.levelCount = 1;
  toTransfer.subresourceRange.layerCount = layers;

  vk::DependencyInfo depToTransfer{};
  depToTransfer.imageMemoryBarrierCount = 1;
  depToTransfer.pImageMemoryBarriers = &toTransfer;
  cmdBuf.pipelineBarrier2(depToTransfer);

  std::vector<vk::BufferImageCopy> copies;
  copies.reserve(layers);
  for (uint32_t layer = 0; layer < layers; ++layer) {
    vk::BufferImageCopy region{};
    region.bufferOffset = static_cast<vk::DeviceSize>(layout.layerOffsets[layer]);
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = vk::Extent3D(width, height, 1);
    region.imageOffset = vk::Offset3D(0, 0, 0);
    copies.push_back(region);
  }
  cmdBuf.copyBufferToImage(stagingBuf.get(), image.get(), vk::ImageLayout::eTransferDstOptimal,
                           static_cast<uint32_t>(copies.size()), copies.data());

  // Transition to shader read so descriptor layout matches actual layout
  vk::ImageMemoryBarrier2 toShader{};
  toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toShader.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toShader.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
  toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  toShader.image = image.get();
  toShader.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toShader.subresourceRange.levelCount = 1;
  toShader.subresourceRange.layerCount = layers;
  vk::DependencyInfo depToShader{};
  depToShader.imageMemoryBarrierCount = 1;
  depToShader.pImageMemoryBarriers = &toShader;
  cmdBuf.pipelineBarrier2(depToShader);

  cmdBuf.end();

  vk::SubmitInfo submitInfo;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuf;
  m_transferQueue.submit(submitInfo, nullptr);
  m_transferQueue.waitIdle();

  // Create view
  vk::ImageViewCreateInfo viewInfo;
  viewInfo.image = image.get();
  viewInfo.viewType = vk::ImageViewType::e2DArray;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.layerCount = layers;
  auto view = m_device.createImageViewUnique(viewInfo);

  // Store as resident texture
  ResidentTexture record{};
  record.gpu.image = std::move(image);
  record.gpu.memory = std::move(imageMem);
  record.gpu.imageView = std::move(view);
  record.gpu.sampler = m_defaultSampler.get();
  record.gpu.width = width;
  record.gpu.height = height;
  record.gpu.layers = layers;
  record.gpu.mipLevels = 1;
  record.gpu.format = format;
  // Image already transitioned to shader read layout in the upload command buffer.
  record.gpu.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  m_residentTextures[baseFrame] = std::move(record);
  onTextureResident(baseFrame);
  return true;
}

void VulkanTextureManager::createFallbackTexture() {
  // Create a 1x1 black texture for use when retired textures are sampled.
  // This prevents accessing destroyed VkImage/VkImageView resources.
  const uint8_t black[4] = {0, 0, 0, 255}; // RGBA black
  m_builtins.fallback = createSolidTexture(black);
}

void VulkanTextureManager::createDefaultTexture() {
  // Create a 1x1 white texture for untextured draws that still require a sampler binding.
  const uint8_t white[4] = {255, 255, 255, 255}; // RGBA white
  m_builtins.defaultBase = createSolidTexture(white);
}

void VulkanTextureManager::createDefaultNormalTexture() {
  // Flat tangent-space normal: (0.5, 0.5, 1.0) in [0,1] -> (0,0,1) after remap.
  const uint8_t flatNormal[4] = {128, 128, 255, 255};
  m_builtins.defaultNormal = createSolidTexture(flatNormal);
}

void VulkanTextureManager::createDefaultSpecTexture() {
  // Default dielectric F0 (~0.04). Alpha is currently unused by the deferred lighting stage.
  const uint8_t dielectricF0[4] = {10, 10, 10, 0};
  m_builtins.defaultSpec = createSolidTexture(dielectricF0);
}

vk::DescriptorImageInfo VulkanTextureManager::fallbackDescriptor(const SamplerKey &samplerKey) const {
  vk::DescriptorImageInfo info{};
  info.imageView = m_builtins.fallback.imageView.get();
  Assertion(info.imageView, "Fallback texture must be initialized");
  info.sampler = getOrCreateSampler(samplerKey);
  info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return info;
}

vk::DescriptorImageInfo VulkanTextureManager::defaultBaseDescriptor(const SamplerKey &samplerKey) const {
  vk::DescriptorImageInfo info{};
  info.imageView = m_builtins.defaultBase.imageView.get();
  Assertion(info.imageView, "Default base texture must be initialized");
  info.sampler = getOrCreateSampler(samplerKey);
  info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return info;
}

vk::DescriptorImageInfo VulkanTextureManager::defaultNormalDescriptor(const SamplerKey &samplerKey) const {
  vk::DescriptorImageInfo info{};
  info.imageView = m_builtins.defaultNormal.imageView.get();
  Assertion(info.imageView, "Default normal texture must be initialized");
  info.sampler = getOrCreateSampler(samplerKey);
  info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return info;
}

vk::DescriptorImageInfo VulkanTextureManager::defaultSpecDescriptor(const SamplerKey &samplerKey) const {
  vk::DescriptorImageInfo info{};
  info.imageView = m_builtins.defaultSpec.imageView.get();
  Assertion(info.imageView, "Default spec texture must be initialized");
  info.sampler = getOrCreateSampler(samplerKey);
  info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return info;
}

void VulkanTextureManager::flushPendingUploads(const UploadCtx &ctx) {
  VulkanFrame &frame = ctx.frame;
  vk::CommandBuffer cmd = ctx.cmd;
  const uint32_t currentFrameIndex = ctx.currentFrameIndex;

  processPendingRetirements();

  // Resolve any pending bindless slot requests at the frame-start safe point (before descriptor sync).
  retryPendingBindlessSlots();

  if (m_pendingUploads.empty()) {
    return;
  }

  vk::DeviceSize stagingBudget = frame.stagingBuffer().size();
  vk::DeviceSize stagingUsed = 0;
  std::vector<int> remaining;
  remaining.reserve(m_pendingUploads.size());

  auto markUnavailable = [&](int baseFrame, UnavailableReason reason) {
    m_unavailableTextures.insert_or_assign(baseFrame, UnavailableTexture{reason});
    m_pendingBindlessSlots.erase(baseFrame);

    // Drop any slot mapping: an unavailable texture should not consume bindless slots.
    auto slotIt = m_bindlessSlots.find(baseFrame);
    if (slotIt != m_bindlessSlots.end()) {
      const uint32_t slot = slotIt->second;
      m_bindlessSlots.erase(slotIt);
      if (isDynamicBindlessSlot(slot)) {
        m_freeBindlessSlots.push_back(slot);
      }
    }
  };

  for (int baseFrame : m_pendingUploads) {
    // State as location:
    // - If already resident, nothing to do.
    // - If permanently unavailable, do not retry.
    if (m_residentTextures.find(baseFrame) != m_residentTextures.end()) {
      continue;
    }
    if (m_unavailableTextures.find(baseFrame) != m_unavailableTextures.end()) {
      continue;
    }

    int numFrames = 1;
    const int resolvedBase = bm_get_base_frame(baseFrame, &numFrames);
    if (resolvedBase < 0) {
      markUnavailable(baseFrame, UnavailableReason::InvalidHandle);
      continue;
    }
    const bool isArray = bm_is_texture_array(baseFrame);
    const uint32_t layers = isArray ? static_cast<uint32_t>(numFrames) : 1u;

    ushort flags = 0;
    int w = 0, h = 0;
    bm_get_info(baseFrame, &w, &h, &flags, nullptr, nullptr);

    auto *bmp0 = bm_lock(baseFrame, 32, flags);
    if (!bmp0) {
      markUnavailable(baseFrame, UnavailableReason::BmpLockFailed);
      continue;
    }
    const bool compressed = isCompressed(*bmp0);
    const vk::Format format = selectFormat(*bmp0);
    const bool singleChannel = format == vk::Format::eR8Unorm;
    const uint32_t width = bmp0->w;
    const uint32_t height = bmp0->h;
    bm_unlock(baseFrame);

    bool validArray = true;
    if (isArray) {
      for (int i = 0; i < numFrames; ++i) {
        ushort f = 0;
        int fw = 0, fh = 0;
        bm_get_info(baseFrame + i, &fw, &fh, &f, nullptr, nullptr);
        if (static_cast<uint32_t>(fw) != width || static_cast<uint32_t>(fh) != height ||
            ((f & BMP_TEX_COMP) != (flags & BMP_TEX_COMP))) {
          validArray = false;
          break;
        }
      }
    }
    if (!validArray) {
      markUnavailable(baseFrame, UnavailableReason::InvalidArray);
      continue;
    }

    // Estimate upload size for budget check
    size_t totalUploadSize = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      totalUploadSize += compressed      ? calculateCompressedSize(width, height, format)
                         : singleChannel ? static_cast<size_t>(width) * height
                                         : static_cast<size_t>(width) * height * 4;
    }

    // Textures that can never fit in the staging buffer are unavailable under the current algorithm.
    if (totalUploadSize > stagingBudget) {
      markUnavailable(baseFrame, UnavailableReason::TooLargeForStaging);
      continue;
    }

    if (stagingUsed + totalUploadSize > stagingBudget) {
      remaining.push_back(baseFrame);
      continue; // defer to next frame
    }

    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(layers);
    bool stagingFailed = false;

    for (uint32_t layer = 0; layer < layers; ++layer) {
      const int frameHandle = isArray ? baseFrame + static_cast<int>(layer) : baseFrame;
      auto *frameBmp = bm_lock(frameHandle, 32, flags);
      if (!frameBmp) {
        markUnavailable(baseFrame, UnavailableReason::BmpLockFailed);
        stagingFailed = true;
        break;
      }

      size_t layerSize = compressed      ? calculateCompressedSize(width, height, format)
                         : singleChannel ? static_cast<size_t>(width) * height
                                         : static_cast<size_t>(width) * height * 4;

      auto allocOpt = frame.stagingBuffer().try_allocate(static_cast<vk::DeviceSize>(layerSize));
      if (!allocOpt) {
        // Staging buffer exhausted - defer to next frame.
        bm_unlock(frameHandle);
        remaining.push_back(baseFrame);
        stagingFailed = true;
        break;
      }
      auto &alloc = *allocOpt;

      if (!compressed && !singleChannel && frameBmp->bpp == 24) {
        auto *src = reinterpret_cast<uint8_t *>(frameBmp->data);
        auto *dst = static_cast<uint8_t *>(alloc.mapped);
        for (uint32_t i = 0; i < width * height; ++i) {
          dst[i * 4 + 0] = src[i * 3 + 0];
          dst[i * 4 + 1] = src[i * 3 + 1];
          dst[i * 4 + 2] = src[i * 3 + 2];
          dst[i * 4 + 3] = 255;
        }
      } else if (!compressed && !singleChannel && frameBmp->bpp == 16) {
        // 16bpp textures in bmpman use A1R5G5B5 packing (see Gr_t_* masks in code/graphics/2d.cpp).
        // Expand to BGRA8 to match eB8G8R8A8Unorm.
        auto *src = reinterpret_cast<uint16_t *>(frameBmp->data);
        auto *dst = static_cast<uint8_t *>(alloc.mapped);
        for (uint32_t i = 0; i < width * height; ++i) {
          const uint16_t pixel = src[i];
          const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
          const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
          const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
          const uint8_t a = (pixel & 0x8000) ? 255u : 0u;

          dst[i * 4 + 0] = b;
          dst[i * 4 + 1] = g;
          dst[i * 4 + 2] = r;
          dst[i * 4 + 3] = a;
        }
      } else if (!compressed && singleChannel) {
        std::memcpy(alloc.mapped, reinterpret_cast<uint8_t *>(frameBmp->data), layerSize);
      } else {
        // 32bpp or compressed - use actual data size
        size_t actualSize = compressed ? layerSize : static_cast<size_t>(width) * height * bytesPerPixel(*frameBmp);
        std::memcpy(alloc.mapped, reinterpret_cast<uint8_t *>(frameBmp->data), actualSize);
      }

      vk::BufferImageCopy region{};
      region.bufferOffset = alloc.offset;
      region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = layer;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = vk::Extent3D(width, height, 1);
      region.imageOffset = vk::Offset3D(0, 0, 0);

      regions.push_back(region);
      bm_unlock(frameHandle);
    }

    if (stagingFailed) {
      continue;
    }

    // Create image resources now that staging succeeded
    vk::ImageCreateInfo imageInfo;
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = format;
    imageInfo.extent = vk::Extent3D(width, height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layers;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;

    ResidentTexture record{};

    record.gpu.image = m_device.createImageUnique(imageInfo);
    auto imgReqs = m_device.getImageMemoryRequirements(record.gpu.image.get());
    vk::MemoryAllocateInfo imgAlloc;
    imgAlloc.allocationSize = imgReqs.size;
    imgAlloc.memoryTypeIndex = findMemoryType(imgReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    record.gpu.memory = m_device.allocateMemoryUnique(imgAlloc);
    m_device.bindImageMemory(record.gpu.image.get(), record.gpu.memory.get(), 0);

    record.gpu.width = width;
    record.gpu.height = height;
    record.gpu.layers = layers;
    record.gpu.mipLevels = 1;
    record.gpu.format = format;
    record.gpu.sampler = m_defaultSampler.get();

    // Transition to transfer dst
    vk::ImageMemoryBarrier2 toTransfer{};
    toTransfer.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
    toTransfer.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toTransfer.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    toTransfer.oldLayout = vk::ImageLayout::eUndefined;
    toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
    toTransfer.image = record.gpu.image.get();
    toTransfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = layers;
    vk::DependencyInfo depToTransfer{};
    depToTransfer.imageMemoryBarrierCount = 1;
    depToTransfer.pImageMemoryBarriers = &toTransfer;
    cmd.pipelineBarrier2(depToTransfer);

    stagingUsed += static_cast<vk::DeviceSize>(totalUploadSize);
    cmd.copyBufferToImage(frame.stagingBuffer().buffer(), record.gpu.image.get(), vk::ImageLayout::eTransferDstOptimal,
                          static_cast<uint32_t>(regions.size()), regions.data());

    // Barrier to shader read
    vk::ImageMemoryBarrier2 toShader{};
    toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    toShader.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    toShader.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    toShader.image = record.gpu.image.get();
    toShader.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    toShader.subresourceRange.levelCount = 1;
    toShader.subresourceRange.layerCount = layers;
    vk::DependencyInfo depToShader{};
    depToShader.imageMemoryBarrierCount = 1;
    depToShader.pImageMemoryBarriers = &toShader;
    cmd.pipelineBarrier2(depToShader);

    // Create view
    vk::ImageViewCreateInfo viewInfo;
    viewInfo.image = record.gpu.image.get();
    viewInfo.viewType = vk::ImageViewType::e2DArray;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = layers;
    record.gpu.imageView = m_device.createImageViewUnique(viewInfo);

    record.gpu.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    record.lastUsedFrame = currentFrameIndex;

    m_residentTextures.emplace(baseFrame, std::move(record));
    onTextureResident(baseFrame);
  }

  m_pendingUploads.swap(remaining);
}

bool VulkanTextureManager::updateTexture(const UploadCtx &ctx, int bitmapHandle, int bpp, const ubyte *data, int width,
                                         int height) {
  if (bitmapHandle < 0 || data == nullptr || width <= 0 || height <= 0) {
    return false;
  }

  int numFrames = 1;
  const int baseFrame = bm_get_base_frame(bitmapHandle, &numFrames);
  if (baseFrame < 0) {
    return false;
  }

  // Multi-layer texture arrays require a layer index for updates which the gr_update_texture() API doesn't provide.
  // Note: bm_is_texture_array() returns true for single-frame textures too; only reject actual multi-layer arrays.
  const bool isArray = bm_is_texture_array(baseFrame);
  const uint32_t layers = isArray ? static_cast<uint32_t>(numFrames) : 1u;
  if (layers != 1u) {
    return false;
  }

  // Permanently unavailable textures do not retry.
  if (m_unavailableTextures.find(baseFrame) != m_unavailableTextures.end()) {
    return false;
  }

  VulkanFrame &frame = ctx.frame;
  vk::CommandBuffer cmd = ctx.cmd;
  const uint32_t currentFrameIndex = ctx.currentFrameIndex;

  const uint32_t w = static_cast<uint32_t>(width);
  const uint32_t h = static_cast<uint32_t>(height);

  // Ensure a resident texture exists for this handle. Dynamic updates rely on an existing VkImage.
  auto it = m_residentTextures.find(baseFrame);
  if (it == m_residentTextures.end()) {
    // Don't overwrite bmpman render targets.
    if (hasRenderTarget(baseFrame)) {
      return false;
    }

    ushort flags = 0;
    bm_get_info(baseFrame, nullptr, nullptr, &flags, nullptr, nullptr);

    auto *bmp0 = bm_lock(baseFrame, 32, flags);
    if (!bmp0) {
      return false;
    }

    const vk::Format format = selectFormat(*bmp0);
    const uint32_t bw = static_cast<uint32_t>(bmp0->w);
    const uint32_t bh = static_cast<uint32_t>(bmp0->h);
    bm_unlock(baseFrame);

    if (isBlockCompressedFormat(format)) {
      return false;
    }

    if (bw != w || bh != h) {
      return false;
    }

    vk::ImageCreateInfo imageInfo;
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = format;
    imageInfo.extent = vk::Extent3D(w, h, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;

    ResidentTexture record{};
    record.gpu.image = m_device.createImageUnique(imageInfo);
    auto imgReqs = m_device.getImageMemoryRequirements(record.gpu.image.get());
    vk::MemoryAllocateInfo imgAlloc;
    imgAlloc.allocationSize = imgReqs.size;
    imgAlloc.memoryTypeIndex = findMemoryType(imgReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    record.gpu.memory = m_device.allocateMemoryUnique(imgAlloc);
    m_device.bindImageMemory(record.gpu.image.get(), record.gpu.memory.get(), 0);

    vk::ImageViewCreateInfo viewInfo;
    viewInfo.image = record.gpu.image.get();
    viewInfo.viewType = vk::ImageViewType::e2DArray;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    record.gpu.imageView = m_device.createImageViewUnique(viewInfo);

    record.gpu.sampler = m_defaultSampler.get();
    record.gpu.width = w;
    record.gpu.height = h;
    record.gpu.layers = 1;
    record.gpu.mipLevels = 1;
    record.gpu.format = format;
    record.gpu.currentLayout = vk::ImageLayout::eUndefined;

    record.lastUsedFrame = currentFrameIndex;
    record.lastUsedSerial = m_safeRetireSerial;

    it = m_residentTextures.emplace(baseFrame, std::move(record)).first;
    onTextureResident(baseFrame);
  }

  auto &record = it->second;
  auto &tex = record.gpu;

  if (tex.width != w || tex.height != h || tex.layers != 1) {
    return false;
  }

  // Streaming updates are expected to be uncompressed (raw pixels or masks).
  if (isBlockCompressedFormat(tex.format)) {
    return false;
  }

  // Determine source bytes-per-pixel.
  // - bpp != 8: bpp matches the source pixel format (in bits-per-pixel).
  // - bpp == 8: this is a mask-update mode. Most callers pass 1 byte/pixel mask data, but user textures (e.g. APNG)
  //   may pass full-color source while requesting an 8bpp upload/conversion.
  uint32_t srcBytesPerPixel = 0;
  if (bpp == 8) {
    const auto *entry = bm_get_entry(bitmapHandle);
    if (!entry) {
      return false;
    }
    // bm.bpp is mutable (set by bm_lock) and can change under our feet. true_bpp is stable and describes the bitmap's
    // declared pixel format (or bm_create's bpp for BM_TYPE_USER).
    //
    // For non-user bitmaps, treat bpp==8 as "caller provided a 1 byte/pixel mask" since streaming paths lock source
    // frames as BMP_AABITMAP and pass the resulting 8-bit buffer.
    if (entry->type == BM_TYPE_USER || entry->type == BM_TYPE_3D) {
      srcBytesPerPixel = std::max<uint32_t>(1u, static_cast<uint32_t>(entry->bm.true_bpp) >> 3);
    } else {
      srcBytesPerPixel = 1u;
    }
  } else {
    srcBytesPerPixel = std::max<uint32_t>(1u, static_cast<uint32_t>(bpp) >> 3);
  }

  const size_t uploadSize = calculateLayerSize(w, h, tex.format);
  auto allocOpt = frame.stagingBuffer().try_allocate(static_cast<vk::DeviceSize>(uploadSize));
  if (!allocOpt) {
    return false;
  }
  auto &alloc = *allocOpt;

  auto *dst = static_cast<uint8_t *>(alloc.mapped);
  const auto *src = reinterpret_cast<const uint8_t *>(data);

  auto computeMask = [&](const uint8_t *px) -> uint8_t {
    if (srcBytesPerPixel <= 1) {
      return px[0];
    }

    uint32_t lum = 0;
    const uint32_t rgbCount = std::min<uint32_t>(3u, srcBytesPerPixel);
    for (uint32_t k = 0; k < rgbCount; ++k) {
      lum += px[k];
    }
    lum /= rgbCount;

    if (srcBytesPerPixel >= 4) {
      const uint32_t a = px[3];
      lum = (lum * a + 127u) / 255u;
    }

    return static_cast<uint8_t>(lum);
  };

  const bool maskUpdate = (bpp == 8);
  if (tex.format == vk::Format::eR8Unorm) {
    if (maskUpdate && srcBytesPerPixel == 1) {
      std::memcpy(dst, src, uploadSize);
    } else {
      // Convert to a single-channel mask (luma, optionally alpha-modulated if source has alpha).
      const size_t pixelCount = static_cast<size_t>(w) * h;
      for (size_t i = 0; i < pixelCount; ++i) {
        dst[i] = computeMask(src + i * srcBytesPerPixel);
      }
    }
  } else if (tex.format == vk::Format::eB8G8R8A8Unorm) {
    if (maskUpdate) {
      // Expand to BGRA8 with the mask in the red channel (OpenGL parity for GL_RED uploads).
      const size_t pixelCount = static_cast<size_t>(w) * h;
      for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t mask = computeMask(src + i * srcBytesPerPixel);
        dst[i * 4 + 0] = 0;    // B
        dst[i * 4 + 1] = 0;    // G
        dst[i * 4 + 2] = mask; // R
        dst[i * 4 + 3] = 255;  // A
      }
    } else if (srcBytesPerPixel == 4) {
      std::memcpy(dst, src, uploadSize);
    } else if (srcBytesPerPixel == 3) {
      // Expand to BGRA8.
      for (uint32_t i = 0; i < w * h; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
      }
    } else if (srcBytesPerPixel == 2) {
      // 16bpp textures in bmpman use A1R5G5B5 packing (see Gr_t_* masks in code/graphics/2d.cpp).
      // Expand to BGRA8 to match eB8G8R8A8Unorm.
      auto *src16 = reinterpret_cast<const uint16_t *>(src);
      for (uint32_t i = 0; i < w * h; ++i) {
        const uint16_t pixel = src16[i];
        const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
        const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
        const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
        const uint8_t a = (pixel & 0x8000) ? 255u : 0u;

        dst[i * 4 + 0] = b;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = r;
        dst[i * 4 + 3] = a;
      }
    } else if (srcBytesPerPixel == 1) {
      // Treat as a mask; place it in red to match alphaTexture sampling (.r).
      for (uint32_t i = 0; i < w * h; ++i) {
        const uint8_t mask = src[i];
        dst[i * 4 + 0] = 0;
        dst[i * 4 + 1] = 0;
        dst[i * 4 + 2] = mask;
        dst[i * 4 + 3] = 255;
      }
    } else {
      return false;
    }
  } else {
    // Unexpected format for dynamic updates.
    return false;
  }

  // Transition to transfer dst.
  vk::ImageMemoryBarrier2 toTransfer{};
  const auto srcAccess = stageAccessForLayout(tex.currentLayout);
  toTransfer.srcStageMask = srcAccess.stageMask;
  toTransfer.srcAccessMask = srcAccess.accessMask;
  toTransfer.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toTransfer.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toTransfer.oldLayout = tex.currentLayout;
  toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
  toTransfer.image = tex.image.get();
  toTransfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toTransfer.subresourceRange.baseMipLevel = 0;
  toTransfer.subresourceRange.levelCount = 1;
  toTransfer.subresourceRange.baseArrayLayer = 0;
  toTransfer.subresourceRange.layerCount = 1;

  vk::DependencyInfo depToTransfer{};
  depToTransfer.imageMemoryBarrierCount = 1;
  depToTransfer.pImageMemoryBarriers = &toTransfer;
  cmd.pipelineBarrier2(depToTransfer);

  // Copy staging -> image.
  vk::BufferImageCopy region{};
  region.bufferOffset = alloc.offset;
  region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = vk::Extent3D(w, h, 1);
  region.imageOffset = vk::Offset3D(0, 0, 0);
  cmd.copyBufferToImage(frame.stagingBuffer().buffer(), tex.image.get(), vk::ImageLayout::eTransferDstOptimal, 1,
                        &region);

  // Barrier back to shader read.
  vk::ImageMemoryBarrier2 toShader{};
  toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toShader.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
  toShader.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
  toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  toShader.image = tex.image.get();
  toShader.subresourceRange = toTransfer.subresourceRange;

  vk::DependencyInfo depToShader{};
  depToShader.imageMemoryBarrierCount = 1;
  depToShader.pImageMemoryBarriers = &toShader;
  cmd.pipelineBarrier2(depToShader);

  tex.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  record.lastUsedFrame = currentFrameIndex;
  record.lastUsedSerial = m_safeRetireSerial;

  return true;
}

bool VulkanTextureManager::isUploadQueued(int baseFrame) const {
  return std::find(m_pendingUploads.begin(), m_pendingUploads.end(), baseFrame) != m_pendingUploads.end();
}

void VulkanTextureManager::processPendingRetirements() {
  if (m_pendingRetirements.empty()) {
    return;
  }

  for (const int handle : m_pendingRetirements) {
    // Any pending "slot request" becomes irrelevant once we're deleting the texture.
    m_pendingBindlessSlots.erase(handle);

    // If the texture is resident, retire it (this also releases any bindless slot mapping).
    if (m_residentTextures.find(handle) != m_residentTextures.end()) {
      retireTexture(handle, m_safeRetireSerial);
      continue;
    }

    // Non-resident: drop any bindless slot assignment so the slot can be reused safely at frame start.
    auto slotIt = m_bindlessSlots.find(handle);
    if (slotIt != m_bindlessSlots.end()) {
      const uint32_t slot = slotIt->second;
      m_bindlessSlots.erase(slotIt);
      if (isDynamicBindlessSlot(slot)) {
        m_freeBindlessSlots.push_back(slot);
      }
    }
  }

  m_pendingRetirements.clear();
}

void VulkanTextureManager::retryPendingBindlessSlots() {
  for (auto it = m_pendingBindlessSlots.begin(); it != m_pendingBindlessSlots.end();) {
    const int handle = *it;
    if (m_unavailableTextures.find(handle) != m_unavailableTextures.end()) {
      it = m_pendingBindlessSlots.erase(it);
      continue;
    }

    if (tryAssignBindlessSlot(handle, /*allowResidentEvict=*/true)) {
      it = m_pendingBindlessSlots.erase(it);
      continue;
    }

    ++it;
  }
}

bool VulkanTextureManager::tryAssignBindlessSlot(int textureHandle, bool allowResidentEvict) {
  if (m_bindlessSlots.find(textureHandle) != m_bindlessSlots.end()) {
    return true;
  }

  auto reclaimNonResidentSlot = [&]() -> bool {
    for (auto it = m_bindlessSlots.begin(); it != m_bindlessSlots.end(); ++it) {
      const int handle = it->first;
      if (m_residentTextures.find(handle) != m_residentTextures.end()) {
        continue;
      }

      const uint32_t slot = it->second;
      m_pendingBindlessSlots.erase(handle);
      m_bindlessSlots.erase(it);
      if (isDynamicBindlessSlot(slot)) {
        m_freeBindlessSlots.push_back(slot);
      }
      return true;
    }
    return false;
  };

  if (m_freeBindlessSlots.empty()) {
    // Rendering-path safe reclaim:
    // if we can free a slot from a non-resident mapping, the descriptor for that slot is fallback this frame.
    if (!reclaimNonResidentSlot() && !allowResidentEvict) {
      return false;
    }
  }

  if (m_freeBindlessSlots.empty() && allowResidentEvict) {
    // Upload-phase eviction: free a slot by retiring a safe-to-evict resident texture.
    // Safety rule: only evict textures whose last use has completed on the GPU.
    uint32_t oldestFrame = UINT32_MAX;
    int oldestHandle = -1;

    for (const auto &[handle, slot] : m_bindlessSlots) {
      (void)slot;
      // Render targets are long-lived GPU resources (cockpit displays, monitors, envmaps).
      // Treat their bindless slot mapping as pinned: evicting them causes visible flicker.
      if (m_renderTargets.find(handle) != m_renderTargets.end()) {
        continue;
      }

      auto residentIt = m_residentTextures.find(handle);
      if (residentIt == m_residentTextures.end()) {
        continue;
      }

      const auto &other = residentIt->second;
      if (other.lastUsedSerial <= m_completedSerial) {
        if (other.lastUsedFrame < oldestFrame) {
          oldestFrame = other.lastUsedFrame;
          oldestHandle = handle;
        }
      }
    }

    if (oldestHandle >= 0) {
      retireTexture(oldestHandle, m_safeRetireSerial);
    }
  }

  if (m_freeBindlessSlots.empty()) {
    return false;
  }

  uint32_t assignedSlot = m_freeBindlessSlots.back();
  m_freeBindlessSlots.pop_back();
  m_bindlessSlots.emplace(textureHandle, assignedSlot);
  return true;
}

void VulkanTextureManager::appendResidentBindlessDescriptors(std::vector<std::pair<uint32_t, int>> &out) const {
  for (const auto &[handle, slot] : m_bindlessSlots) {
    if (m_residentTextures.find(handle) == m_residentTextures.end()) {
      continue;
    }
    out.emplace_back(slot, handle);
  }
}

bool VulkanTextureManager::hasBindlessSlot(int baseFrameHandle) const {
  return m_bindlessSlots.find(baseFrameHandle) != m_bindlessSlots.end();
}

bool VulkanTextureManager::preloadTexture(int bitmapHandle, bool isAABitmap) {
  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    // Preloading is best-effort. An invalid handle is not a VRAM-budget failure; keep preloading other textures.
    return true;
  }

  if (m_residentTextures.find(baseFrame) != m_residentTextures.end()) {
    return true;
  }

  try {
    if (uploadImmediate(baseFrame, isAABitmap)) {
      return true;
    }
  } catch (const vk::SystemError &err) {
    const auto result = static_cast<vk::Result>(err.code().value());
    if (result == vk::Result::eErrorOutOfDeviceMemory || result == vk::Result::eErrorOutOfHostMemory) {
      // This is the only failure mode bmpman understands during page-in: stop preloading.
      return false;
    }
    mprintf(("VulkanTextureManager: preloadTexture(%d) failed with VkResult %d; continuing preload.\n", baseFrame,
             static_cast<int>(result)));
  } catch (const std::exception &err) {
    mprintf(("VulkanTextureManager: preloadTexture(%d) failed (%s); continuing preload.\n", baseFrame, err.what()));
  }

  // Anything that isn't an out-of-memory condition should not abort bmpman preloading.
  // Mark it unavailable so later draw calls don't keep queuing uploads for a texture that will never become resident.
  m_unavailableTextures.insert_or_assign(baseFrame, UnavailableTexture{UnavailableReason::BmpLockFailed});
  m_pendingBindlessSlots.erase(baseFrame);
  auto newEnd = std::remove(m_pendingUploads.begin(), m_pendingUploads.end(), baseFrame);
  m_pendingUploads.erase(newEnd, m_pendingUploads.end());
  return true;
}

void VulkanTextureManager::deleteTexture(int bitmapHandle) {
  int base = bm_get_base_frame(bitmapHandle, nullptr);
  if (base < 0) {
    return;
  }

  // Remove from any queued/unavailable state immediately.
  m_unavailableTextures.erase(base);
  m_pendingBindlessSlots.erase(base);
  auto newEnd = std::remove(m_pendingUploads.begin(), m_pendingUploads.end(), base);
  m_pendingUploads.erase(newEnd, m_pendingUploads.end());

  // Defer slot reuse + resource retirement to the upload-phase flush (frame-start safe point).
  m_pendingRetirements.insert(base);
}

void VulkanTextureManager::releaseBitmap(int bitmapHandle) {
  int base = bm_get_base_frame(bitmapHandle, nullptr);
  if (base < 0) {
    return;
  }

  // Hard lifecycle boundary: bmpman may reuse this handle immediately after release.
  // Drop all cache state for this handle now; GPU lifetime safety is handled via deferred release.
  m_unavailableTextures.erase(base);
  m_pendingBindlessSlots.erase(base);
  m_pendingRetirements.erase(base);
  auto newEnd = std::remove(m_pendingUploads.begin(), m_pendingUploads.end(), base);
  m_pendingUploads.erase(newEnd, m_pendingUploads.end());

  // If the texture is resident, retire it immediately (releasing any bindless slot mapping).
  auto it = m_residentTextures.find(base);
  if (it != m_residentTextures.end()) {
    const uint64_t retireSerial = std::max(m_safeRetireSerial, it->second.lastUsedSerial);
    retireTexture(base, retireSerial);
    return;
  }

  // If we somehow have a render-target record without a resident texture, still defer its view destruction.
  if (auto rt = tryTakeRenderTargetRecord(base); rt.has_value()) {
    const uint64_t retireSerial = m_safeRetireSerial;
    m_deferredReleases.enqueue(retireSerial, [rt = std::move(rt)]() mutable { (void)rt; });
  }

  // Non-resident: drop any bindless slot assignment so the slot can be reused.
  auto slotIt = m_bindlessSlots.find(base);
  if (slotIt != m_bindlessSlots.end()) {
    const uint32_t slot = slotIt->second;
    m_bindlessSlots.erase(slotIt);
    if (isDynamicBindlessSlot(slot)) {
      m_freeBindlessSlots.push_back(slot);
    }
  }
}

void VulkanTextureManager::cleanup() {
  m_deferredReleases.clear();
  m_builtins.reset();
  m_residentTextures.clear();
  m_unavailableTextures.clear();
  m_renderTargets.clear();
  m_bindlessSlots.clear();
  m_pendingBindlessSlots.clear();
  m_pendingRetirements.clear();
  m_samplerCache.clear();
  m_defaultSampler.reset();
  m_pendingUploads.clear();
}

void VulkanTextureManager::onTextureResident(int textureHandle) {
  Assertion(m_residentTextures.find(textureHandle) != m_residentTextures.end(),
            "onTextureResident called for unknown texture handle %d", textureHandle);
  (void)textureHandle;
}

uint32_t VulkanTextureManager::getBindlessSlotIndex(int textureHandle) {
  // Domain-real unavailability: bind fallback slot 0 rather than "absent".
  if (m_unavailableTextures.find(textureHandle) != m_unavailableTextures.end()) {
    return 0;
  }

  if (!tryAssignBindlessSlot(textureHandle, /*allowResidentEvict=*/false)) {
    // Slot pressure: keep returning fallback this frame and retry at the next upload flush.
    m_pendingBindlessSlots.insert(textureHandle);
    if (m_residentTextures.find(textureHandle) == m_residentTextures.end() && !isUploadQueued(textureHandle)) {
      m_pendingUploads.push_back(textureHandle);
    }
    return 0;
  }

  auto slotIt = m_bindlessSlots.find(textureHandle);
  Assertion(slotIt != m_bindlessSlots.end(), "Assigned bindless slot missing for texture handle %d", textureHandle);
  const uint32_t slot = slotIt->second;

  auto residentIt = m_residentTextures.find(textureHandle);
  if (residentIt == m_residentTextures.end()) {
    if (!isUploadQueued(textureHandle)) {
      m_pendingUploads.push_back(textureHandle);
    }
    return slot; // slot points at fallback until the upload completes
  }

  auto &record = residentIt->second;
  record.lastUsedFrame = m_currentFrameIndex;
  record.lastUsedSerial = m_safeRetireSerial;

  return slot;
}

void VulkanTextureManager::markTextureUsedBaseFrame(int baseFrame, uint32_t currentFrameIndex) {
  auto it = m_residentTextures.find(baseFrame);
  if (it == m_residentTextures.end()) {
    return;
  }

  auto &record = it->second;
  record.lastUsedFrame = currentFrameIndex;
  record.lastUsedSerial = m_safeRetireSerial;
}

void VulkanTextureManager::retireTexture(int textureHandle, uint64_t retireSerial) {
  m_pendingBindlessSlots.erase(textureHandle);

  auto slotIt = m_bindlessSlots.find(textureHandle);
  if (slotIt != m_bindlessSlots.end()) {
    const uint32_t slot = slotIt->second;
    m_bindlessSlots.erase(slotIt);
    if (isDynamicBindlessSlot(slot)) {
      m_freeBindlessSlots.push_back(slot);
    }
  }

  auto it = m_residentTextures.find(textureHandle);
  Assertion(it != m_residentTextures.end(), "retireTexture called for unknown texture handle %d", textureHandle);

  ResidentTexture record = std::move(it->second);
  m_residentTextures.erase(it); // Drop cache state immediately; in-flight GPU users are protected by deferred release.

  VulkanTexture gpu = std::move(record.gpu);
  auto rt = tryTakeRenderTargetRecord(textureHandle);
  m_deferredReleases.enqueue(retireSerial, [gpu = std::move(gpu), rt = std::move(rt)]() mutable {
    // Capture moved resources in the deferred-release closure to guarantee they are destroyed only after retireSerial.
    (void)gpu;
    (void)rt;
  });
}

void VulkanTextureManager::collect(uint64_t completedSerial) {
  m_completedSerial = std::max(m_completedSerial, completedSerial);
  m_deferredReleases.collect(completedSerial);
}

vk::DescriptorImageInfo VulkanTextureManager::getTextureDescriptorInfo(int textureHandle,
                                                                       const SamplerKey &samplerKey) {
  vk::DescriptorImageInfo info{};

  // textureHandle is already the base frame key for resident textures.
  // Do not call bm_get_base_frame here - bmpman may have released this handle,
  // but VulkanTextureManager owns the GPU texture independently.
  auto it = m_residentTextures.find(textureHandle);
  if (it == m_residentTextures.end()) {
    return info;
  }

  auto &record = it->second;
  info.imageView = record.gpu.imageView.get();
  info.sampler = getOrCreateSampler(samplerKey);
  info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return info;
}

void VulkanTextureManager::queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex,
                                              const SamplerKey &samplerKey) {
  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return;
  }
  queueTextureUploadBaseFrame(baseFrame, currentFrameIndex, samplerKey);
}

void VulkanTextureManager::queueTextureUploadBaseFrame(int baseFrame, uint32_t currentFrameIndex,
                                                       const SamplerKey &samplerKey) {
  (void)currentFrameIndex;

  // If already resident, there's nothing to queue.
  if (m_residentTextures.find(baseFrame) != m_residentTextures.end()) {
    return;
  }

  // Permanently unavailable textures do not retry.
  if (m_unavailableTextures.find(baseFrame) != m_unavailableTextures.end()) {
    return;
  }

  // Warm the sampler cache so descriptor requests don't allocate later.
  (void)getOrCreateSampler(samplerKey);

  if (!isUploadQueued(baseFrame)) {
    m_pendingUploads.push_back(baseFrame);
  }
}

std::optional<VulkanTextureManager::RenderTargetRecord>
VulkanTextureManager::tryTakeRenderTargetRecord(int baseFrameHandle) {
  auto it = m_renderTargets.find(baseFrameHandle);
  if (it == m_renderTargets.end()) {
    return std::nullopt;
  }
  RenderTargetRecord rec = std::move(it->second);
  m_renderTargets.erase(it);
  return rec;
}

bool VulkanTextureManager::createRenderTarget(int baseFrameHandle, uint32_t width, uint32_t height, int flags,
                                              uint32_t *outMipLevels) {
  if (baseFrameHandle < 0 || width == 0 || height == 0 || outMipLevels == nullptr) {
    return false;
  }

  // Render targets are created explicitly. bmpman handles are reused after release, so we must be
  // robust to the case where stale GPU state still exists for this handle.
  m_unavailableTextures.erase(baseFrameHandle);
  m_pendingBindlessSlots.erase(baseFrameHandle);
  m_pendingRetirements.erase(baseFrameHandle);
  auto newEnd = std::remove(m_pendingUploads.begin(), m_pendingUploads.end(), baseFrameHandle);
  m_pendingUploads.erase(newEnd, m_pendingUploads.end());

  if (auto it = m_residentTextures.find(baseFrameHandle); it != m_residentTextures.end()) {
    const uint64_t retireSerial = std::max(m_safeRetireSerial, it->second.lastUsedSerial);
    mprintf(("VulkanTextureManager: Recreating texture handle %d as render target (retireSerial=%llu)\n",
             baseFrameHandle, static_cast<unsigned long long>(retireSerial)));
    retireTexture(baseFrameHandle, retireSerial);
  }

  const bool isCubemap = (flags & BMP_FLAG_CUBEMAP) != 0;
  const bool wantsMips = (flags & BMP_FLAG_RENDER_TARGET_MIPMAP) != 0;

  const uint32_t layers = isCubemap ? 6u : 1u;
  const uint32_t mipLevels = wantsMips ? mipLevelsForExtent(width, height) : 1u;
  *outMipLevels = mipLevels;

  // Match the engine's common uncompressed texture format (BGRA8).
  constexpr vk::Format format = vk::Format::eB8G8R8A8Unorm;

  vk::ImageCreateInfo imageInfo{};
  imageInfo.flags = isCubemap ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags{};
  imageInfo.imageType = vk::ImageType::e2D;
  imageInfo.format = format;
  imageInfo.extent = vk::Extent3D(width, height, 1);
  imageInfo.mipLevels = mipLevels;
  imageInfo.arrayLayers = layers;
  imageInfo.samples = vk::SampleCountFlagBits::e1;
  imageInfo.tiling = vk::ImageTiling::eOptimal;
  imageInfo.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                    vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
  imageInfo.sharingMode = vk::SharingMode::eExclusive;
  imageInfo.initialLayout = vk::ImageLayout::eUndefined;

  ResidentTexture record{};
  record.gpu.image = m_device.createImageUnique(imageInfo);
  auto imgReqs = m_device.getImageMemoryRequirements(record.gpu.image.get());
  vk::MemoryAllocateInfo allocInfo{};
  allocInfo.allocationSize = imgReqs.size;
  allocInfo.memoryTypeIndex = findMemoryType(imgReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
  record.gpu.memory = m_device.allocateMemoryUnique(allocInfo);
  m_device.bindImageMemory(record.gpu.image.get(), record.gpu.memory.get(), 0);

  record.gpu.width = width;
  record.gpu.height = height;
  record.gpu.layers = layers;
  record.gpu.mipLevels = mipLevels;
  record.gpu.format = format;
  record.gpu.sampler = m_defaultSampler.get();

  // Sample view: treat everything as a 2D array in the standard (non-model) shader path.
  vk::ImageViewCreateInfo viewInfo{};
  viewInfo.image = record.gpu.image.get();
  viewInfo.viewType = vk::ImageViewType::e2DArray;
  viewInfo.format = format;
  viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = mipLevels;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = layers;
  record.gpu.imageView = m_device.createImageViewUnique(viewInfo);

  RenderTargetRecord rt{};
  rt.extent = vk::Extent2D(width, height);
  rt.format = format;
  rt.mipLevels = mipLevels;
  rt.layers = layers;
  rt.isCubemap = isCubemap;

  // Attachment views: one per face (cubemap) or just face 0 (2D target).
  const uint32_t faceCount = isCubemap ? 6u : 1u;
  for (uint32_t face = 0; face < faceCount; ++face) {
    vk::ImageViewCreateInfo faceViewInfo{};
    faceViewInfo.image = record.gpu.image.get();
    faceViewInfo.viewType = vk::ImageViewType::e2D;
    faceViewInfo.format = format;
    faceViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    faceViewInfo.subresourceRange.baseMipLevel = 0;
    faceViewInfo.subresourceRange.levelCount = 1;
    faceViewInfo.subresourceRange.baseArrayLayer = face;
    faceViewInfo.subresourceRange.layerCount = 1;
    rt.faceViews[face] = m_device.createImageViewUnique(faceViewInfo);
  }

  // Initialize the image contents to black (alpha=1) and transition to shader-read.
  vk::CommandPoolCreateInfo poolInfo{};
  poolInfo.queueFamilyIndex = m_transferQueueIndex;
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
  auto pool = m_device.createCommandPoolUnique(poolInfo);

  vk::CommandBufferAllocateInfo cmdAlloc{};
  cmdAlloc.commandPool = pool.get();
  cmdAlloc.level = vk::CommandBufferLevel::ePrimary;
  cmdAlloc.commandBufferCount = 1;
  auto cmd = m_device.allocateCommandBuffers(cmdAlloc).front();

  vk::CommandBufferBeginInfo beginInfo{};
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmd.begin(beginInfo);

  vk::ImageMemoryBarrier2 toClear{};
  toClear.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
  toClear.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toClear.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toClear.oldLayout = vk::ImageLayout::eUndefined;
  toClear.newLayout = vk::ImageLayout::eTransferDstOptimal;
  toClear.image = record.gpu.image.get();
  toClear.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toClear.subresourceRange.baseMipLevel = 0;
  toClear.subresourceRange.levelCount = mipLevels;
  toClear.subresourceRange.baseArrayLayer = 0;
  toClear.subresourceRange.layerCount = layers;

  vk::DependencyInfo depToClear{};
  depToClear.imageMemoryBarrierCount = 1;
  depToClear.pImageMemoryBarriers = &toClear;
  cmd.pipelineBarrier2(depToClear);

  vk::ClearColorValue clearValue(std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
  vk::ImageSubresourceRange clearRange{};
  clearRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  clearRange.baseMipLevel = 0;
  clearRange.levelCount = mipLevels;
  clearRange.baseArrayLayer = 0;
  clearRange.layerCount = layers;
  cmd.clearColorImage(record.gpu.image.get(), vk::ImageLayout::eTransferDstOptimal, &clearValue, 1, &clearRange);

  vk::ImageMemoryBarrier2 toShader{};
  toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  toShader.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
  toShader.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
  toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
  toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  toShader.image = record.gpu.image.get();
  toShader.subresourceRange = clearRange;

  vk::DependencyInfo depToShader{};
  depToShader.imageMemoryBarrierCount = 1;
  depToShader.pImageMemoryBarriers = &toShader;
  cmd.pipelineBarrier2(depToShader);

  cmd.end();

  vk::SubmitInfo submitInfo{};
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmd;
  m_transferQueue.submit(submitInfo, nullptr);
  m_transferQueue.waitIdle();

  record.gpu.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  record.lastUsedFrame = m_currentFrameIndex;

  m_renderTargets.emplace(baseFrameHandle, std::move(rt));
  m_residentTextures.emplace(baseFrameHandle, std::move(record));
  onTextureResident(baseFrameHandle);

  // Render targets are frequently sampled via the model bindless set (e.g. cockpit displays).
  // Request a bindless slot at the next upload flush so the descriptor sync can populate it.
  m_pendingBindlessSlots.insert(baseFrameHandle);

  return true;
}

bool VulkanTextureManager::hasRenderTarget(int baseFrameHandle) const {
  return m_renderTargets.find(baseFrameHandle) != m_renderTargets.end();
}

vk::Extent2D VulkanTextureManager::renderTargetExtent(int baseFrameHandle) const {
  auto it = m_renderTargets.find(baseFrameHandle);
  Assertion(it != m_renderTargets.end(), "renderTargetExtent called for unknown render target handle %d",
            baseFrameHandle);
  return it->second.extent;
}

vk::Format VulkanTextureManager::renderTargetFormat(int baseFrameHandle) const {
  auto it = m_renderTargets.find(baseFrameHandle);
  if (it == m_renderTargets.end()) {
    return vk::Format::eUndefined;
  }
  return it->second.format;
}

uint32_t VulkanTextureManager::renderTargetMipLevels(int baseFrameHandle) const {
  auto it = m_renderTargets.find(baseFrameHandle);
  if (it == m_renderTargets.end()) {
    return 1;
  }
  return it->second.mipLevels;
}

vk::Image VulkanTextureManager::renderTargetImage(int baseFrameHandle) const {
  auto it = m_residentTextures.find(baseFrameHandle);
  Assertion(it != m_residentTextures.end(), "renderTargetImage called for unknown texture handle %d", baseFrameHandle);
  return it->second.gpu.image.get();
}

vk::ImageView VulkanTextureManager::renderTargetAttachmentView(int baseFrameHandle, int face) const {
  auto it = m_renderTargets.find(baseFrameHandle);
  Assertion(it != m_renderTargets.end(), "renderTargetAttachmentView called for unknown render target handle %d",
            baseFrameHandle);

  const auto &rt = it->second;
  const int clampedFace = (face < 0) ? 0 : face;
  if (!rt.isCubemap) {
    Assertion(clampedFace == 0, "Non-cubemap render target %d requested invalid face %d", baseFrameHandle, clampedFace);
    return rt.faceViews[0].get();
  }
  Assertion(clampedFace >= 0 && clampedFace < 6, "Cubemap render target %d requested invalid face %d", baseFrameHandle,
            clampedFace);
  return rt.faceViews[static_cast<size_t>(clampedFace)].get();
}

void VulkanTextureManager::transitionRenderTargetToAttachment(vk::CommandBuffer cmd, int baseFrameHandle) {
  auto it = m_residentTextures.find(baseFrameHandle);
  Assertion(it != m_residentTextures.end(), "transitionRenderTargetToAttachment called for unknown texture handle %d",
            baseFrameHandle);
  auto &tex = it->second.gpu;

  const auto newLayout = vk::ImageLayout::eColorAttachmentOptimal;
  if (tex.currentLayout == newLayout) {
    return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(tex.currentLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = tex.currentLayout;
  barrier.newLayout = newLayout;
  barrier.image = tex.image.get();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = tex.mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = tex.layers;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  tex.currentLayout = newLayout;
}

void VulkanTextureManager::transitionRenderTargetToTransferDst(vk::CommandBuffer cmd, int baseFrameHandle) {
  auto it = m_residentTextures.find(baseFrameHandle);
  Assertion(it != m_residentTextures.end(), "transitionRenderTargetToTransferDst called for unknown texture handle %d",
            baseFrameHandle);
  auto &tex = it->second.gpu;

  const auto newLayout = vk::ImageLayout::eTransferDstOptimal;
  if (tex.currentLayout == newLayout) {
    return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(tex.currentLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = tex.currentLayout;
  barrier.newLayout = newLayout;
  barrier.image = tex.image.get();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = tex.mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = tex.layers;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  tex.currentLayout = newLayout;
}

void VulkanTextureManager::transitionRenderTargetToShaderRead(vk::CommandBuffer cmd, int baseFrameHandle) {
  auto it = m_residentTextures.find(baseFrameHandle);
  Assertion(it != m_residentTextures.end(), "transitionRenderTargetToShaderRead called for unknown texture handle %d",
            baseFrameHandle);
  auto &tex = it->second.gpu;

  const auto newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  if (tex.currentLayout == newLayout) {
    return;
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(tex.currentLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = tex.currentLayout;
  barrier.newLayout = newLayout;
  barrier.image = tex.image.get();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = tex.mipLevels;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = tex.layers;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  tex.currentLayout = newLayout;
}

void VulkanTextureManager::generateRenderTargetMipmaps(vk::CommandBuffer cmd, int baseFrameHandle) {
  auto it = m_residentTextures.find(baseFrameHandle);
  Assertion(it != m_residentTextures.end(), "generateRenderTargetMipmaps called for unknown texture handle %d",
            baseFrameHandle);
  auto &tex = it->second.gpu;

  if (tex.mipLevels <= 1) {
    transitionRenderTargetToShaderRead(cmd, baseFrameHandle);
    return;
  }

  // Transition the entire image to transfer src, then iteratively blit down the mip chain.
  {
    const auto newLayout = vk::ImageLayout::eTransferSrcOptimal;
    vk::ImageMemoryBarrier2 barrier{};
    const auto src = stageAccessForLayout(tex.currentLayout);
    const auto dst = stageAccessForLayout(newLayout);
    barrier.srcStageMask = src.stageMask;
    barrier.srcAccessMask = src.accessMask;
    barrier.dstStageMask = dst.stageMask;
    barrier.dstAccessMask = dst.accessMask;
    barrier.oldLayout = tex.currentLayout;
    barrier.newLayout = newLayout;
    barrier.image = tex.image.get();
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = tex.mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = tex.layers;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmd.pipelineBarrier2(dep);

    tex.currentLayout = newLayout;
  }

  int32_t mipW = static_cast<int32_t>(tex.width);
  int32_t mipH = static_cast<int32_t>(tex.height);

  for (uint32_t level = 1; level < tex.mipLevels; ++level) {
    const int32_t nextW = (mipW > 1) ? (mipW / 2) : 1;
    const int32_t nextH = (mipH > 1) ? (mipH / 2) : 1;

    // Make dst mip writable.
    vk::ImageMemoryBarrier2 toDst{};
    toDst.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toDst.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
    toDst.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toDst.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    toDst.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    toDst.newLayout = vk::ImageLayout::eTransferDstOptimal;
    toDst.image = tex.image.get();
    toDst.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    toDst.subresourceRange.baseMipLevel = level;
    toDst.subresourceRange.levelCount = 1;
    toDst.subresourceRange.baseArrayLayer = 0;
    toDst.subresourceRange.layerCount = tex.layers;

    vk::DependencyInfo depToDst{};
    depToDst.imageMemoryBarrierCount = 1;
    depToDst.pImageMemoryBarriers = &toDst;
    cmd.pipelineBarrier2(depToDst);

    vk::ImageBlit blit{};
    blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.srcSubresource.mipLevel = level - 1;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = tex.layers;
    blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
    blit.srcOffsets[1] = vk::Offset3D(mipW, mipH, 1);

    blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.dstSubresource.mipLevel = level;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = tex.layers;
    blit.dstOffsets[0] = vk::Offset3D(0, 0, 0);
    blit.dstOffsets[1] = vk::Offset3D(nextW, nextH, 1);

    cmd.blitImage(tex.image.get(), vk::ImageLayout::eTransferSrcOptimal, tex.image.get(),
                  vk::ImageLayout::eTransferDstOptimal, 1, &blit, vk::Filter::eLinear);

    // Promote dst mip to transfer src for the next iteration.
    vk::ImageMemoryBarrier2 toSrc{};
    toSrc.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toSrc.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    toSrc.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    toSrc.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    toSrc.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    toSrc.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    toSrc.image = tex.image.get();
    toSrc.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    toSrc.subresourceRange.baseMipLevel = level;
    toSrc.subresourceRange.levelCount = 1;
    toSrc.subresourceRange.baseArrayLayer = 0;
    toSrc.subresourceRange.layerCount = tex.layers;

    vk::DependencyInfo depToSrc{};
    depToSrc.imageMemoryBarrierCount = 1;
    depToSrc.pImageMemoryBarriers = &toSrc;
    cmd.pipelineBarrier2(depToSrc);

    mipW = nextW;
    mipH = nextH;
  }

  // Transition the whole image to shader-read for sampling.
  transitionRenderTargetToShaderRead(cmd, baseFrameHandle);
}

} // namespace vulkan
} // namespace graphics
