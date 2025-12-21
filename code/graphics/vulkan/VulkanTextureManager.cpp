#include "VulkanTextureManager.h"

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

  m_freeBindlessSlots.reserve(kMaxBindlessTextures - 1);
  for (uint32_t slot = kMaxBindlessTextures; slot-- > 1;) {
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
  samplerInfo.maxLod = 0.0f;

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

  // Store record
  TextureRecord record{};
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
  record.state = TextureState::Resident;
  m_textures[textureHandle] = std::move(record);
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
  samplerInfo.maxLod = 0.0f;

  auto sampler = m_device.createSamplerUnique(samplerInfo);
  vk::Sampler handle = sampler.get();
  m_samplerCache.emplace(hash, std::move(sampler));
  return handle;
}

bool VulkanTextureManager::uploadImmediate(int baseFrame, bool isAABitmap)
{
  int numFrames = 1;
  bm_get_base_frame(baseFrame, &numFrames);
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

  // Store record
  TextureRecord record{};
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
  record.state = TextureState::Resident;
  record.lastUsedFrame = 0;

  m_textures[baseFrame] = std::move(record);
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

VulkanTextureManager::TextureRecord* VulkanTextureManager::ensureTextureResident(int bitmapHandle,
  uint32_t currentFrameIndex,
  const SamplerKey& samplerKey,
  bool& uploadQueued)
{
  uploadQueued = false;

  int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return nullptr;
  }

  auto it = m_textures.find(baseFrame);
  if (it == m_textures.end()) {
    it = m_textures.emplace(baseFrame, TextureRecord{}).first;
  }
  auto& record = it->second;

  // If a previous upload for this record is queued, just update usage/sampler.
  if (record.state == TextureState::Queued) {
    record.lastUsedFrame = currentFrameIndex;
    record.gpu.sampler = getOrCreateSampler(samplerKey);
    return &record;
  }

  if (record.state == TextureState::Resident) {
    record.lastUsedFrame = currentFrameIndex;
    record.gpu.sampler = getOrCreateSampler(samplerKey);
    return &record;
  }

  if (record.state == TextureState::Retired) {
    // This record still owns GPU resources pending safe destruction. Do not reuse it for new uploads.
    return &record;
  }

  if (record.state == TextureState::Failed) {
    return &record;
  }

  // Missing: queue an upload to be flushed before rendering begins.
  record.lastUsedFrame = currentFrameIndex;
  record.gpu.sampler = getOrCreateSampler(samplerKey);

  if (!isUploadQueued(baseFrame)) {
    m_pendingUploads.push_back(baseFrame);
  }
  record.state = TextureState::Queued;
  uploadQueued = true;
  return &record;
}

void VulkanTextureManager::flushPendingUploads(VulkanFrame& frame, vk::CommandBuffer cmd, uint32_t currentFrameIndex)
{
  if (m_pendingUploads.empty()) {
    return;
  }

  vk::DeviceSize stagingBudget = frame.stagingBuffer().size();
  vk::DeviceSize stagingUsed = 0;
  std::vector<int> remaining;
  remaining.reserve(m_pendingUploads.size());

  for (int baseFrame : m_pendingUploads) {
    auto it = m_textures.find(baseFrame);
    if (it == m_textures.end()) {
      continue;
    }
    auto& record = it->second;
    if (record.state != TextureState::Queued) {
      continue;
    }

    int numFrames = 1;
    bm_get_base_frame(baseFrame, &numFrames);
    const bool isArray = bm_is_texture_array(baseFrame);
    const uint32_t layers = isArray ? static_cast<uint32_t>(numFrames) : 1u;

    ushort flags = 0;
    int w = 0, h = 0;
    bm_get_info(baseFrame, &w, &h, &flags, nullptr, nullptr);

    auto* bmp0 = bm_lock(baseFrame, 32, flags);
    if (!bmp0) {
      record.state = TextureState::Failed;
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
      record.state = TextureState::Failed;
      continue;
    }

    // Estimate upload size for budget check
    size_t totalUploadSize = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      totalUploadSize += compressed ? calculateCompressedSize(width, height, format)
                                    : singleChannel ? static_cast<size_t>(width) * height
                                                    : static_cast<size_t>(width) * height * 4;
    }

    // Textures that can never fit in the staging buffer are marked as Failed
    if (totalUploadSize > stagingBudget) {
      record.state = TextureState::Failed;
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
        record.state = TextureState::Failed;
        stagingFailed = true;
        break;
      }

      size_t layerSize = compressed ? calculateCompressedSize(width, height, format)
                                    : singleChannel ? static_cast<size_t>(width) * height
                                                    : static_cast<size_t>(width) * height * 4;

      auto allocOpt = frame.stagingBuffer().try_allocate(static_cast<vk::DeviceSize>(layerSize));
      if (!allocOpt) {
        // Staging buffer exhausted - defer to next frame
        record.state = TextureState::Queued;
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

    if (stagingFailed || record.state == TextureState::Failed || record.state == TextureState::Missing) {
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
    if (!record.gpu.sampler) {
      record.gpu.sampler = m_defaultSampler.get();
    }

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

    record.state = TextureState::Resident;
    record.gpu.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    record.lastUsedFrame = currentFrameIndex;
      onTextureResident(baseFrame);
    }

  m_pendingUploads.swap(remaining);
}

bool VulkanTextureManager::isUploadQueued(int baseFrame) const
{
  return std::find(m_pendingUploads.begin(), m_pendingUploads.end(), baseFrame) != m_pendingUploads.end();
}

bool VulkanTextureManager::preloadTexture(int bitmapHandle, bool /*isAABitmap*/)
{
  bool result = uploadImmediate(bitmapHandle, false);
  return result;
}

void VulkanTextureManager::deleteTexture(int bitmapHandle)
{
  int base = bm_get_base_frame(bitmapHandle, nullptr);
  // Never delete the synthetic default/fallback textures.
  if (base == kFallbackTextureHandle || base == kDefaultTextureHandle) {
    return;
  }

  auto it = m_textures.find(base);
  if (it == m_textures.end()) {
    return;
  }

  // Be conservative: if called during a frame, ensure we wait at least one more submit.
  retireTexture(base, m_safeRetireSerial + 1);
}

void VulkanTextureManager::cleanup()
{
  m_deferredReleases.clear();
  m_textures.clear();
  m_samplerCache.clear();
  m_defaultSampler.reset();
  m_pendingUploads.clear();
}

void VulkanTextureManager::onTextureResident(int textureHandle)
{
  auto it = m_textures.find(textureHandle);
  Assertion(it != m_textures.end(), "onTextureResident called for unknown texture handle %d", textureHandle);

  auto& record = it->second;
  if (textureHandle == kFallbackTextureHandle) {
    record.bindingState.arrayIndex = 0;
    return;
  }

  // Stable assignment: once a texture owns a slot, keep it.
  if (record.bindingState.arrayIndex != MODEL_OFFSET_ABSENT) {
    return;
  }

  if (m_freeBindlessSlots.empty()) {
    // No slots available - evict least recently used texture to free a slot
    uint32_t oldestFrame = UINT32_MAX;
    int oldestHandle = -1;
    uint32_t oldestSlot = MODEL_OFFSET_ABSENT;
    
    for (auto& [handle, other] : m_textures) {
      if (handle == kFallbackTextureHandle || handle == kDefaultTextureHandle) {
        continue; // Never evict fallback texture
      }
      if (other.state == TextureState::Resident && other.bindingState.arrayIndex != MODEL_OFFSET_ABSENT) {
        if (other.lastUsedFrame < oldestFrame) {
          oldestFrame = other.lastUsedFrame;
          oldestHandle = handle;
          oldestSlot = other.bindingState.arrayIndex;
        }
      }
    }
    
    if (oldestHandle >= 0 && oldestSlot != MODEL_OFFSET_ABSENT) {
      // Evict the oldest texture and reuse its slot.
      retireTexture(oldestHandle, m_safeRetireSerial);
    } else {
      // No slots available and nothing to evict; leave as absent so model shader will skip sampling.
      return;
    }
  }

  uint32_t assignedSlot = m_freeBindlessSlots.back();
  record.bindingState.arrayIndex = assignedSlot;
  m_freeBindlessSlots.pop_back();
}

uint32_t VulkanTextureManager::getBindlessSlotIndex(int textureHandle)
{
  auto it = m_textures.find(textureHandle);
  if (it == m_textures.end()) {
    it = m_textures.emplace(textureHandle, TextureRecord{}).first;
  }

  auto& record = it->second;
  if (record.state == TextureState::Missing) {
    if (!isUploadQueued(textureHandle)) {
      m_pendingUploads.push_back(textureHandle);
    }
    record.state = TextureState::Queued;
  }

  if (record.state != TextureState::Resident) {
    return MODEL_OFFSET_ABSENT;
  }

  record.lastUsedFrame = m_currentFrameIndex;

  if (record.bindingState.arrayIndex == MODEL_OFFSET_ABSENT) {
    onTextureResident(textureHandle);
  }

  return record.bindingState.arrayIndex;
}

void VulkanTextureManager::retireTexture(int textureHandle, uint64_t retireSerial)
{
  auto it = m_textures.find(textureHandle);
  Assertion(it != m_textures.end(), "retireTexture called for unknown texture handle %d", textureHandle);

  auto& record = it->second;

  const uint32_t slot = record.bindingState.arrayIndex;

  if (record.state == TextureState::Retired) {
    return;
  }

  // Free the slot for reuse. The old VkImage/VkImageView stay alive until collect(retireSerial).
  if (slot != MODEL_OFFSET_ABSENT && slot != 0) {
    m_freeBindlessSlots.push_back(slot);
  }

  record.bindingState.arrayIndex = MODEL_OFFSET_ABSENT;
  record.state = TextureState::Retired;

  m_deferredReleases.enqueue(retireSerial, [this, handle = textureHandle]() mutable {
    auto texIt = m_textures.find(handle);
    if (texIt != m_textures.end()) {
      // VkImageView/VkImage destroyed automatically via unique handles.
      m_textures.erase(texIt);
    }
  });
}

void VulkanTextureManager::collect(uint64_t completedSerial)
{
  m_deferredReleases.collect(completedSerial);
}

vk::DescriptorImageInfo VulkanTextureManager::getTextureDescriptorInfo(int textureHandle,
  const SamplerKey& samplerKey)
{
  vk::DescriptorImageInfo info{};

  // textureHandle is already the base frame key from m_textures.
  // Do not call bm_get_base_frame here - bmpman may have released this handle,
  // but VulkanTextureManager owns the GPU texture independently.
  auto it = m_textures.find(textureHandle);
  if (it == m_textures.end() || it->second.state != TextureState::Resident) {
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
  auto it = m_textures.find(baseFrame);
  if (it == m_textures.end()) {
    it = m_textures.emplace(baseFrame, TextureRecord{}).first;
  }
  auto& record = it->second;

  // Avoid resurrecting retired records; they will be erased once safe.
  if (record.state == TextureState::Retired || record.state == TextureState::Failed) {
    return;
  }

  record.lastUsedFrame = currentFrameIndex;
  record.gpu.sampler = getOrCreateSampler(samplerKey);

  if (record.state == TextureState::Missing) {
    if (!isUploadQueued(baseFrame)) {
      m_pendingUploads.push_back(baseFrame);
    }
    record.state = TextureState::Queued;
  }
}

} // namespace vulkan
} // namespace graphics
