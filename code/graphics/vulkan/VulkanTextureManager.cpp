#include "VulkanTextureManager.h"
#include "VulkanPhaseContexts.h"

#include "bmpman/bmpman.h"
#include "bmpman/bm_internal.h"

#include <stdexcept>
#include <cstring>
#include <vector>
#include <algorithm>

namespace graphics {
namespace vulkan {
namespace {

vk::Format selectFormat(const bitmap& bmp)
{
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

bool isCompressed(const bitmap& bmp)
{
  return (bmp.flags & BMP_TEX_COMP) != 0;
}

uint32_t bytesPerPixel(const bitmap& bmp)
{
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

inline bool isBuiltinTextureHandle(int handle)
{
  return handle == VulkanTextureManager::kFallbackTextureHandle ||
    handle == VulkanTextureManager::kDefaultTextureHandle ||
    handle == VulkanTextureManager::kDefaultNormalTextureHandle ||
    handle == VulkanTextureManager::kDefaultSpecTextureHandle;
}

inline bool isDynamicBindlessSlot(uint32_t slot)
{
  return slot >= kBindlessFirstDynamicTextureSlot && slot < kMaxBindlessTextures;
}

} // namespace

VulkanTextureManager::VulkanTextureManager(vk::Device device,
    const vk::PhysicalDeviceMemoryProperties& memoryProps,
    vk::Queue transferQueue,
    uint32_t transferQueueIndex)
    : m_device(device)
    , m_memoryProperties(memoryProps)
    , m_transferQueue(transferQueue)
    , m_transferQueueIndex(transferQueueIndex)
{
  createDefaultSampler();

  createFallbackTexture();
  createDefaultTexture();
  createDefaultNormalTexture();
  createDefaultSpecTexture();

  // Fixed bindless slots for built-in textures.
  m_bindlessSlots.insert_or_assign(kFallbackTextureHandle, kBindlessTextureSlotFallback);
  m_bindlessSlots.insert_or_assign(kDefaultTextureHandle, kBindlessTextureSlotDefaultBase);
  m_bindlessSlots.insert_or_assign(kDefaultNormalTextureHandle, kBindlessTextureSlotDefaultNormal);
  m_bindlessSlots.insert_or_assign(kDefaultSpecTextureHandle, kBindlessTextureSlotDefaultSpec);

  m_freeBindlessSlots.reserve(kMaxBindlessTextures - kBindlessFirstDynamicTextureSlot);
  for (uint32_t slot = kMaxBindlessTextures; slot-- > kBindlessFirstDynamicTextureSlot;) {
    m_freeBindlessSlots.push_back(slot);
  }
}

uint32_t VulkanTextureManager::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const
{
  for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
    if ((typeFilter & (1 << i)) &&
      (m_memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  throw std::runtime_error("Failed to find suitable memory type.");
}

void VulkanTextureManager::createDefaultSampler()
{
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

void VulkanTextureManager::createSolidTexture(int textureHandle, const uint8_t rgba[4])
{
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
  allocInfo.memoryTypeIndex = findMemoryType(reqs.memoryTypeBits,
    vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  auto stagingMem = m_device.allocateMemoryUnique(allocInfo);
  m_device.bindBufferMemory(stagingBuf.get(), stagingMem.get(), 0);

  // Copy pixel data to staging
  void* mapped = m_device.mapMemory(stagingMem.get(), 0, 4);
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

  // Store as resident texture
  ResidentTexture record{};
  record.gpu.image = std::move(image);
  record.gpu.memory = std::move(imageMem);
  record.gpu.imageView = std::move(view);
  record.gpu.sampler = m_defaultSampler.get();
  record.gpu.width = width;
  record.gpu.height = height;
  record.gpu.layers = 1;
  record.gpu.mipLevels = 1;
  record.gpu.format = format;
  record.gpu.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  m_residentTextures[textureHandle] = std::move(record);
  onTextureResident(textureHandle);
}

vk::Sampler VulkanTextureManager::getOrCreateSampler(const SamplerKey& key)
{
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

bool VulkanTextureManager::uploadImmediate(int baseFrame, bool isAABitmap)
{
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
  auto* bmp = bm_lock(baseFrame, 32, flags);
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
      if (static_cast<uint32_t>(fw) != width ||
        static_cast<uint32_t>(fh) != height ||
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
  allocInfo.memoryTypeIndex =
    findMemoryType(reqs.memoryTypeBits,
                   vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
  auto stagingMem = m_device.allocateMemoryUnique(allocInfo);
  m_device.bindBufferMemory(stagingBuf.get(), stagingMem.get(), 0);

  // Map and copy
  void* mapped = m_device.mapMemory(stagingMem.get(), 0, static_cast<vk::DeviceSize>(totalSize));
  for (uint32_t layer = 0; layer < layers; ++layer) {
    const size_t offset = layout.layerOffsets[layer];
    const int frameHandle = isArray ? baseFrame + static_cast<int>(layer) : baseFrame;
    auto* frameBmp = bm_lock(frameHandle, 32, flags);
    if (!frameBmp) {
      m_device.unmapMemory(stagingMem.get());
      return false;
    }
    if (!compressed && !singleChannel && frameBmp->bpp == 24) {
      // Expand to RGBA
      auto* dst = static_cast<uint8_t*>(mapped) + offset;
      auto* src = reinterpret_cast<uint8_t*>(frameBmp->data);
      for (uint32_t i = 0; i < width * height; ++i) {
        dst[i * 4 + 0] = src[i * 3 + 0];
        dst[i * 4 + 1] = src[i * 3 + 1];
        dst[i * 4 + 2] = src[i * 3 + 2];
        dst[i * 4 + 3] = 255;
      }
    } else if (!compressed && !singleChannel && frameBmp->bpp == 16) {
      // 16bpp (5-6-5 RGB) - expand to BGRA to match eB8G8R8A8Unorm format
      auto* src = reinterpret_cast<uint16_t*>(frameBmp->data);
      auto* dst = static_cast<uint8_t*>(mapped) + offset;
      for (uint32_t i = 0; i < width * height; ++i) {
        uint16_t pixel = src[i];
        dst[i * 4 + 0] = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);          // B
        dst[i * 4 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);   // G
        dst[i * 4 + 2] = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);  // R
        dst[i * 4 + 3] = 255;                                                       // A
      }
    } else if (!compressed && singleChannel) {
      std::memcpy(static_cast<uint8_t*>(mapped) + offset,
                  reinterpret_cast<uint8_t*>(frameBmp->data),
                  layerSize);
    } else {
      // 32bpp or compressed - use actual data size
      size_t actualSize = compressed ? layerSize
                                     : static_cast<size_t>(width) * height * bytesPerPixel(*frameBmp);
      std::memcpy(static_cast<uint8_t*>(mapped) + offset, reinterpret_cast<uint8_t*>(frameBmp->data), actualSize);
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

void VulkanTextureManager::createFallbackTexture()
{
  // Create a 1x1 black texture for use when retired textures are sampled.
  // This prevents accessing destroyed VkImage/VkImageView resources.
  const uint8_t black[4] = {0, 0, 0, 255}; // RGBA black
  createSolidTexture(kFallbackTextureHandle, black);

  // Set the handle so getFallbackTextureHandle() returns valid value
  m_fallbackTextureHandle = kFallbackTextureHandle;
}

void VulkanTextureManager::createDefaultTexture()
{
  // Create a 1x1 white texture for untextured draws that still require a sampler binding.
  const uint8_t white[4] = {255, 255, 255, 255}; // RGBA white
  createSolidTexture(kDefaultTextureHandle, white);

  m_defaultTextureHandle = kDefaultTextureHandle;
}

void VulkanTextureManager::createDefaultNormalTexture()
{
  // Flat tangent-space normal: (0.5, 0.5, 1.0) in [0,1] -> (0,0,1) after remap.
  const uint8_t flatNormal[4] = {128, 128, 255, 255};
  createSolidTexture(kDefaultNormalTextureHandle, flatNormal);
  m_defaultNormalTextureHandle = kDefaultNormalTextureHandle;
}

void VulkanTextureManager::createDefaultSpecTexture()
{
  // Default dielectric F0 (~0.04). Alpha is currently unused by the deferred lighting stage.
  const uint8_t dielectricF0[4] = {10, 10, 10, 0};
  createSolidTexture(kDefaultSpecTextureHandle, dielectricF0);
  m_defaultSpecTextureHandle = kDefaultSpecTextureHandle;
}

void VulkanTextureManager::flushPendingUploads(const UploadCtx& ctx)
{
  VulkanFrame& frame = ctx.frame;
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

    auto* bmp0 = bm_lock(baseFrame, 32, flags);
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
        if (static_cast<uint32_t>(fw) != width ||
          static_cast<uint32_t>(fh) != height ||
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
      totalUploadSize += compressed ? calculateCompressedSize(width, height, format)
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
      auto* frameBmp = bm_lock(frameHandle, 32, flags);
      if (!frameBmp) {
        markUnavailable(baseFrame, UnavailableReason::BmpLockFailed);
        stagingFailed = true;
        break;
      }

      size_t layerSize = compressed ? calculateCompressedSize(width, height, format)
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
      auto& alloc = *allocOpt;

      if (!compressed && !singleChannel && frameBmp->bpp == 24) {
        auto* src = reinterpret_cast<uint8_t*>(frameBmp->data);
        auto* dst = static_cast<uint8_t*>(alloc.mapped);
        for (uint32_t i = 0; i < width * height; ++i) {
          dst[i * 4 + 0] = src[i * 3 + 0];
          dst[i * 4 + 1] = src[i * 3 + 1];
          dst[i * 4 + 2] = src[i * 3 + 2];
          dst[i * 4 + 3] = 255;
        }
      } else if (!compressed && !singleChannel && frameBmp->bpp == 16) {
        // 16bpp (5-6-5 RGB) - expand to BGRA to match eB8G8R8A8Unorm format
        auto* src = reinterpret_cast<uint16_t*>(frameBmp->data);
        auto* dst = static_cast<uint8_t*>(alloc.mapped);
        for (uint32_t i = 0; i < width * height; ++i) {
          uint16_t pixel = src[i];
          dst[i * 4 + 0] = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);          // B
          dst[i * 4 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);   // G
          dst[i * 4 + 2] = static_cast<uint8_t>(((pixel >> 11) & 0x1F) * 255 / 31);  // R
          dst[i * 4 + 3] = 255;                                                       // A
        }
      } else if (!compressed && singleChannel) {
        std::memcpy(alloc.mapped, reinterpret_cast<uint8_t*>(frameBmp->data), layerSize);
      } else {
        // 32bpp or compressed - use actual data size
        size_t actualSize = compressed ? layerSize
                                       : static_cast<size_t>(width) * height * bytesPerPixel(*frameBmp);
        std::memcpy(alloc.mapped, reinterpret_cast<uint8_t*>(frameBmp->data), actualSize);
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
    cmd.copyBufferToImage(frame.stagingBuffer().buffer(),
      record.gpu.image.get(),
      vk::ImageLayout::eTransferDstOptimal,
      static_cast<uint32_t>(regions.size()),
      regions.data());

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

bool VulkanTextureManager::isUploadQueued(int baseFrame) const
{
  return std::find(m_pendingUploads.begin(), m_pendingUploads.end(), baseFrame) != m_pendingUploads.end();
}

void VulkanTextureManager::processPendingRetirements()
{
  if (m_pendingRetirements.empty()) {
    return;
  }

  for (const int handle : m_pendingRetirements) {
    if (isBuiltinTextureHandle(handle)) {
      continue;
    }

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

void VulkanTextureManager::retryPendingBindlessSlots()
{
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

bool VulkanTextureManager::tryAssignBindlessSlot(int textureHandle, bool allowResidentEvict)
{
  if (textureHandle == kFallbackTextureHandle) {
    return true;
  }

  if (m_bindlessSlots.find(textureHandle) != m_bindlessSlots.end()) {
    return true;
  }

  auto reclaimNonResidentSlot = [&]() -> bool {
    for (auto it = m_bindlessSlots.begin(); it != m_bindlessSlots.end(); ++it) {
      const int handle = it->first;
      if (isBuiltinTextureHandle(handle)) {
        continue;
      }
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

    for (const auto& [handle, slot] : m_bindlessSlots) {
      (void)slot;
      if (isBuiltinTextureHandle(handle)) {
        continue;
      }

      auto residentIt = m_residentTextures.find(handle);
      if (residentIt == m_residentTextures.end()) {
        continue;
      }

      const auto& other = residentIt->second;
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

void VulkanTextureManager::appendResidentBindlessDescriptors(std::vector<std::pair<uint32_t, int>>& out) const
{
  for (const auto& [handle, slot] : m_bindlessSlots) {
    if (m_residentTextures.find(handle) == m_residentTextures.end()) {
      continue;
    }
    out.emplace_back(slot, handle);
  }
}

bool VulkanTextureManager::preloadTexture(int bitmapHandle, bool /*isAABitmap*/)
{
  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return false;
  }

  return uploadImmediate(baseFrame, false);
}

void VulkanTextureManager::deleteTexture(int bitmapHandle)
{
  int base = bm_get_base_frame(bitmapHandle, nullptr);
  if (base < 0) {
    return;
  }
  // Never delete the synthetic default/fallback textures.
  if (isBuiltinTextureHandle(base)) {
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

void VulkanTextureManager::cleanup()
{
  m_deferredReleases.clear();
  m_residentTextures.clear();
  m_unavailableTextures.clear();
  m_bindlessSlots.clear();
  m_pendingBindlessSlots.clear();
  m_pendingRetirements.clear();
  m_samplerCache.clear();
  m_defaultSampler.reset();
  m_pendingUploads.clear();
}

void VulkanTextureManager::onTextureResident(int textureHandle)
{
  Assertion(m_residentTextures.find(textureHandle) != m_residentTextures.end(),
    "onTextureResident called for unknown texture handle %d",
    textureHandle);
  (void)textureHandle;
}

uint32_t VulkanTextureManager::getBindlessSlotIndex(int textureHandle)
{
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

  auto& record = residentIt->second;
  record.lastUsedFrame = m_currentFrameIndex;
  record.lastUsedSerial = m_safeRetireSerial;

  return slot;
}

void VulkanTextureManager::markTextureUsedBaseFrame(int baseFrame, uint32_t currentFrameIndex)
{
  auto it = m_residentTextures.find(baseFrame);
  if (it == m_residentTextures.end()) {
    return;
  }

  auto& record = it->second;
  record.lastUsedFrame = currentFrameIndex;
  record.lastUsedSerial = m_safeRetireSerial;
}

void VulkanTextureManager::retireTexture(int textureHandle, uint64_t retireSerial)
{
  // Never retire the synthetic default/fallback textures.
  if (isBuiltinTextureHandle(textureHandle)) {
    return;
  }

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
  m_deferredReleases.enqueue(retireSerial, [gpu = std::move(gpu)]() mutable {});
}

void VulkanTextureManager::collect(uint64_t completedSerial)
{
  m_completedSerial = std::max(m_completedSerial, completedSerial);
  m_deferredReleases.collect(completedSerial);
}

vk::DescriptorImageInfo VulkanTextureManager::getTextureDescriptorInfo(int textureHandle,
  const SamplerKey& samplerKey)
{
  vk::DescriptorImageInfo info{};

  // textureHandle is already the base frame key for resident textures.
  // Do not call bm_get_base_frame here - bmpman may have released this handle,
  // but VulkanTextureManager owns the GPU texture independently.
  auto it = m_residentTextures.find(textureHandle);
  if (it == m_residentTextures.end()) {
    return info;
  }

  auto& record = it->second;
  info.imageView = record.gpu.imageView.get();
  info.sampler = getOrCreateSampler(samplerKey);
  info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  return info;
}

void VulkanTextureManager::queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex, const SamplerKey& samplerKey)
{
  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return;
  }
  queueTextureUploadBaseFrame(baseFrame, currentFrameIndex, samplerKey);
}

void VulkanTextureManager::queueTextureUploadBaseFrame(int baseFrame, uint32_t currentFrameIndex, const SamplerKey& samplerKey)
{
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

} // namespace vulkan
} // namespace graphics
