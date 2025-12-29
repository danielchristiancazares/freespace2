#include "VulkanRenderer.h"

#include "VulkanFrameCaps.h"
#include "VulkanMovieManager.h"
#include "VulkanTextureBindings.h"
#include "graphics/util/uniform_structs.h"

#include "bmpman/bmpman.h"

#include <array>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace graphics {
namespace vulkan {

vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const {
  Assertion(m_bufferManager != nullptr, "getBuffer called before buffer manager initialization");
  return m_bufferManager->getBuffer(handle);
}

vk::Buffer VulkanRenderer::queryModelVertexHeapBuffer() const {
  Assertion(m_modelVertexHeapHandle.isValid(),
            "queryModelVertexHeapBuffer called without a valid model vertex heap handle");
  return getBuffer(m_modelVertexHeapHandle);
}

void VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle) {
  // Only store the handle - VkBuffer will be looked up lazily when needed.
  // At registration time, the buffer doesn't exist yet (VulkanBufferManager::createBuffer
  // defers actual VkBuffer creation until updateBufferData is called).
  m_modelVertexHeapHandle = handle;
}

gr_buffer_handle VulkanRenderer::createBuffer(BufferType type, BufferUsageHint usage) {
  Assertion(m_bufferManager != nullptr, "createBuffer called before buffer manager initialization");
  return m_bufferManager->createBuffer(type, usage);
}

void VulkanRenderer::deleteBuffer(gr_buffer_handle handle) {
  Assertion(m_bufferManager != nullptr, "deleteBuffer called before buffer manager initialization");
  m_bufferManager->deleteBuffer(handle);
}

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void *data) {
  Assertion(m_bufferManager != nullptr, "updateBufferData called before buffer manager initialization");
  m_bufferManager->updateBufferData(handle, size, data);
}

void VulkanRenderer::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void *data) {
  Assertion(m_bufferManager != nullptr, "updateBufferDataOffset called before buffer manager initialization");
  m_bufferManager->updateBufferDataOffset(handle, offset, size, data);
}

void *VulkanRenderer::mapBuffer(gr_buffer_handle handle) {
  Assertion(m_bufferManager != nullptr, "mapBuffer called before buffer manager initialization");
  return m_bufferManager->mapBuffer(handle);
}

void VulkanRenderer::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size) {
  Assertion(m_bufferManager != nullptr, "flushMappedBuffer called before buffer manager initialization");
  m_bufferManager->flushMappedBuffer(handle, offset, size);
}

void VulkanRenderer::resizeBuffer(gr_buffer_handle handle, size_t size) {
  Assertion(m_bufferManager != nullptr, "resizeBuffer called before buffer manager initialization");
  m_bufferManager->resizeBuffer(handle, size);
}

vk::DescriptorImageInfo VulkanRenderer::getTextureDescriptor(const FrameCtx &ctx, int bitmapHandle,
                                                             const VulkanTextureManager::SamplerKey &samplerKey) {
  Assertion(m_textureManager != nullptr, "getTextureDescriptor called before texture manager initialization");
  Assertion(&ctx.renderer == this,
            "getTextureDescriptor called with FrameCtx from a different VulkanRenderer instance");
  Assertion(bitmapHandle >= 0, "getTextureDescriptor called with invalid bitmapHandle %d", bitmapHandle);

  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  Assertion(baseFrame >= 0, "Invalid bitmapHandle %d in getTextureDescriptor", bitmapHandle);

  Assertion(m_textureBindings != nullptr, "getTextureDescriptor called before texture bindings initialization");
  const auto id = TextureId::tryFromBaseFrame(baseFrame);
  Assertion(id.has_value(), "Invalid base frame %d in getTextureDescriptor", baseFrame);

  return m_textureBindings->descriptor(*id, m_frameCounter, samplerKey);
}

bool VulkanRenderer::createBitmapRenderTarget(int handle, int *width, int *height, int *bpp, int *mm_lvl, int flags) {
  Assertion(m_textureManager != nullptr, "createBitmapRenderTarget called before texture manager initialization");
  if (!width || !height || !bpp || !mm_lvl) {
    return false;
  }
  if (handle < 0) {
    return false;
  }

  uint32_t w = static_cast<uint32_t>(*width);
  uint32_t h = static_cast<uint32_t>(*height);

  // Cubemap faces must be square. Mirror OpenGL behavior: clamp to max dimension.
  if ((flags & BMP_FLAG_CUBEMAP) && (w != h)) {
    const uint32_t mx = (w > h) ? w : h;
    w = mx;
    h = mx;
  }

  // Hard clamp to device limits (fail-fast clamping, no silent overflow).
  const auto &limits = m_vulkanDevice->properties().limits;
  const uint32_t maxDim = (flags & BMP_FLAG_CUBEMAP) ? limits.maxImageDimensionCube : limits.maxImageDimension2D;
  if (w > maxDim) {
    w = maxDim;
  }
  if (h > maxDim) {
    h = maxDim;
  }

  uint32_t mipLevels = 1;
  if (!m_textureManager->createRenderTarget(handle, w, h, flags, &mipLevels)) {
    return false;
  }

  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  // OpenGL parity: report 24bpp even though the underlying image is RGBA8.
  *bpp = 24;
  *mm_lvl = static_cast<int>(mipLevels);
  return true;
}

bool VulkanRenderer::setBitmapRenderTarget(const FrameCtx &ctx, int handle, int face) {
  Assertion(&ctx.renderer == this,
            "setBitmapRenderTarget called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "setBitmapRenderTarget called before rendering session initialization");
  Assertion(m_textureManager != nullptr, "setBitmapRenderTarget called before texture manager initialization");

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return false;
  }

  // bmpman updates gr_screen.rendering_to_texture *after* the graphics API callback returns, so this still reflects
  // the previous target at this point.
  const int prevTarget = gr_screen.rendering_to_texture;

  // Switching targets requires ending any active dynamic rendering scope.
  if (handle < 0) {
    requestMainTargetWithDepth();
  } else {
    if (!m_textureManager->hasRenderTarget(handle)) {
      return false;
    }
    m_renderingSession->requestBitmapTarget(handle, face);
  }

  // Leaving a bitmap render target: transition to shader-read and generate mipmaps if requested.
  // (Skip when switching faces on the same cubemap.)
  if (prevTarget >= 0 && prevTarget != handle) {
    if (m_textureManager->renderTargetMipLevels(prevTarget) > 1) {
      m_textureManager->generateRenderTargetMipmaps(cmd, prevTarget);
    } else {
      m_textureManager->transitionRenderTargetToShaderRead(cmd, prevTarget);
    }
  }

  return true;
}

vk::DescriptorImageInfo
VulkanRenderer::getDefaultTextureDescriptor(const VulkanTextureManager::SamplerKey &samplerKey) {
  Assertion(m_textureManager != nullptr, "getDefaultTextureDescriptor called before texture manager initialization");
  return m_textureManager->defaultBaseDescriptor(samplerKey);
}

uint32_t VulkanRenderer::getBindlessTextureIndex(const FrameCtx &ctx, int bitmapHandle) {
  if (bitmapHandle < 0) {
    return kBindlessTextureSlotFallback;
  }

  Assertion(&ctx.renderer == this,
            "getBindlessTextureIndex called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_textureBindings != nullptr, "getBindlessTextureIndex called before texture bindings initialization");
  Assertion(m_textureManager != nullptr, "getBindlessTextureIndex called before texture manager initialization");

  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return kBindlessTextureSlotFallback;
  }

  const auto id = TextureId::tryFromBaseFrame(baseFrame);
  if (!id.has_value()) {
    return kBindlessTextureSlotFallback;
  }

  return m_textureBindings->bindlessIndex(*id, m_frameCounter);
}

void VulkanRenderer::setModelUniformBinding(VulkanFrame &frame, gr_buffer_handle handle, size_t offset, size_t size) {
  const auto alignment = getMinUniformOffsetAlignment();
  Assertion(offset <= std::numeric_limits<uint32_t>::max(), "Model uniform offset %zu exceeds uint32_t range", offset);
  const auto dynOffset = static_cast<uint32_t>(offset);

  Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
  Assertion((dynOffset % alignment) == 0, "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
  Assertion(size >= sizeof(model_uniform_data), "Model uniform size %zu is smaller than sizeof(model_uniform_data) %zu",
            size, sizeof(model_uniform_data));

  Assertion(frame.modelDescriptorSet(), "Model descriptor set must be allocated before binding uniform buffer");
  Assertion(handle.isValid(), "Invalid model uniform buffer handle");
  Assertion(m_bufferManager != nullptr, "setModelUniformBinding requires buffer manager");

  vk::Buffer vkBuffer =
      m_bufferManager->ensureBuffer(handle, static_cast<vk::DeviceSize>(offset + sizeof(model_uniform_data)));
  Assertion(vkBuffer, "Failed to resolve Vulkan buffer for handle %d", handle.value());

  if (frame.modelUniformBinding.bufferHandle != handle) {
    vk::DescriptorBufferInfo info{};
    info.buffer = vkBuffer;
    info.offset = 0;
    info.range = sizeof(model_uniform_data);

    vk::WriteDescriptorSet write{};
    write.dstSet = frame.modelDescriptorSet();
    write.dstBinding = 2;
    write.dstArrayElement = 0;
    write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;

    m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
  }

  frame.modelUniformBinding = DynamicUniformBinding{handle, dynOffset};
}

void VulkanRenderer::setSceneUniformBinding(VulkanFrame &frame, gr_buffer_handle handle, size_t offset, size_t size) {
  // For now, we just track the state in the frame.
  // In the future, this will update a descriptor set for the scene/view block (binding 6).
  // Currently, the engine binds this, but the shaders might not use it via a dedicated set yet.
  // We store it so it's available when we add the descriptor wiring.

  const auto alignment = getMinUniformOffsetAlignment();
  Assertion(offset <= std::numeric_limits<uint32_t>::max(), "Scene uniform offset %zu exceeds uint32_t range", offset);
  const auto dynOffset = static_cast<uint32_t>(offset);

  Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
  Assertion((dynOffset % alignment) == 0, "Scene uniform offset %u is not aligned to %zu", dynOffset, alignment);

  frame.sceneUniformBinding = DynamicUniformBinding{handle, dynOffset};
}

void VulkanRenderer::updateModelDescriptors(uint32_t frameIndex, vk::DescriptorSet set, vk::Buffer vertexHeapBuffer,
                                            vk::Buffer transformBuffer,
                                            const std::vector<std::pair<uint32_t, TextureId>> &textures) {
  std::vector<vk::WriteDescriptorSet> writes;
  writes.reserve(3);

  // Binding 0: Vertex heap SSBO
  Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
            "updateModelDescriptors called with null vertexHeapBuffer");

  vk::DescriptorBufferInfo heapInfo{};
  heapInfo.buffer = vertexHeapBuffer;
  heapInfo.offset = 0;
  heapInfo.range = VK_WHOLE_SIZE;

  vk::WriteDescriptorSet heapWrite{};
  heapWrite.dstSet = set;
  heapWrite.dstBinding = 0;
  heapWrite.dstArrayElement = 0;
  heapWrite.descriptorCount = 1;
  heapWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
  heapWrite.pBufferInfo = &heapInfo;
  writes.push_back(heapWrite);

  // Binding 3: Batched transforms (dynamic SSBO)
  Assertion(static_cast<VkBuffer>(transformBuffer) != VK_NULL_HANDLE,
            "updateModelDescriptors called with null transformBuffer");

  vk::DescriptorBufferInfo transformInfo{};
  transformInfo.buffer = transformBuffer;
  transformInfo.offset = 0;
  // Dynamic offsets are only valid when the descriptor range is not VK_WHOLE_SIZE.
  // This binding is indexed via per-draw dynamic offsets into the per-frame vertex ring.
  transformInfo.range = VERTEX_RING_SIZE;

  vk::WriteDescriptorSet transformWrite{};
  transformWrite.dstSet = set;
  transformWrite.dstBinding = 3;
  transformWrite.dstArrayElement = 0;
  transformWrite.descriptorCount = 1;
  transformWrite.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
  transformWrite.pBufferInfo = &transformInfo;
  writes.push_back(transformWrite);

  // Binding 1: Bindless textures
  // Correctness rule: every slot must always point at a valid descriptor (fallback until resident).
  VulkanTextureManager::SamplerKey samplerKey{};
  samplerKey.address = vk::SamplerAddressMode::eRepeat;
  samplerKey.filter = vk::Filter::eLinear;

  Assertion(m_textureManager != nullptr, "updateModelDescriptors called before texture manager initialization");
  const vk::DescriptorImageInfo fallbackInfo = m_textureManager->fallbackDescriptor(samplerKey);
  const vk::DescriptorImageInfo defaultBaseInfo = m_textureManager->defaultBaseDescriptor(samplerKey);
  const vk::DescriptorImageInfo defaultNormalInfo = m_textureManager->defaultNormalDescriptor(samplerKey);
  const vk::DescriptorImageInfo defaultSpecInfo = m_textureManager->defaultSpecDescriptor(samplerKey);

  std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> desiredInfos{};
  desiredInfos.fill(fallbackInfo);
  desiredInfos[kBindlessTextureSlotDefaultBase] = defaultBaseInfo;
  desiredInfos[kBindlessTextureSlotDefaultNormal] = defaultNormalInfo;
  desiredInfos[kBindlessTextureSlotDefaultSpec] = defaultSpecInfo;

  for (const auto &[arrayIndex, id] : textures) {
    Assertion(arrayIndex < kMaxBindlessTextures, "updateModelDescriptors: slot index %u out of range (max %u)",
              arrayIndex, kMaxBindlessTextures);
    auto info = m_textureManager->tryGetResidentDescriptor(id, samplerKey);
    Assertion(info.has_value(), "updateModelDescriptors requires resident TextureId baseFrame=%d", id.baseFrame());
    desiredInfos[arrayIndex] = *info;
  }

  auto sameInfo = [](const vk::DescriptorImageInfo &a, const vk::DescriptorImageInfo &b) {
    return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
  };

  Assertion(frameIndex < m_modelBindlessCache.size(),
            "updateModelDescriptors called with invalid frameIndex %u (cache size %zu)", frameIndex,
            m_modelBindlessCache.size());
  auto &cache = m_modelBindlessCache[frameIndex];

  if (!cache.initialized) {
    vk::WriteDescriptorSet texturesWrite{};
    texturesWrite.dstSet = set;
    texturesWrite.dstBinding = 1;
    texturesWrite.dstArrayElement = 0;
    texturesWrite.descriptorCount = kMaxBindlessTextures;
    texturesWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    texturesWrite.pImageInfo = desiredInfos.data();
    writes.push_back(texturesWrite);

    cache.infos = desiredInfos;
    cache.initialized = true;
  } else {
    uint32_t i = 0;
    while (i < kMaxBindlessTextures) {
      if (sameInfo(cache.infos[i], desiredInfos[i])) {
        ++i;
        continue;
      }

      const uint32_t start = i;
      while (i < kMaxBindlessTextures && !sameInfo(cache.infos[i], desiredInfos[i])) {
        cache.infos[i] = desiredInfos[i];
        ++i;
      }
      const uint32_t count = i - start;

      vk::WriteDescriptorSet texturesWrite{};
      texturesWrite.dstSet = set;
      texturesWrite.dstBinding = 1;
      texturesWrite.dstArrayElement = start;
      texturesWrite.descriptorCount = count;
      texturesWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
      texturesWrite.pImageInfo = desiredInfos.data() + start;
      writes.push_back(texturesWrite);
    }
  }

  m_vulkanDevice->device().updateDescriptorSets(writes, {});
}

void VulkanRenderer::beginModelDescriptorSync(VulkanFrame &frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer) {
  Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
            "beginModelDescriptorSync called with null vertexHeapBuffer");
  Assertion(m_bufferManager != nullptr, "beginModelDescriptorSync requires buffer manager");

  Assertion(frameIndex < kFramesInFlight, "Invalid frame index %u (must be 0..%u)", frameIndex, kFramesInFlight - 1);

  Assertion(frame.modelDescriptorSet(), "Model descriptor set must be allocated at frame construction");

  Assertion(m_textureManager != nullptr, "beginModelDescriptorSync requires texture manager");

  // Binding 0: Vertex heap SSBO; Binding 1: bindless textures; Binding 3: batched transform buffer (dynamic SSBO).
  // We batch the writes to avoid issuing one vkUpdateDescriptorSets call per texture.
  std::vector<std::pair<uint32_t, TextureId>> textures;
  textures.reserve(kMaxBindlessTextures);
  m_textureManager->appendResidentBindlessDescriptors(textures);

  updateModelDescriptors(frameIndex, frame.modelDescriptorSet(), vertexHeapBuffer, frame.vertexBuffer().buffer(),
                         textures);
}

int VulkanRenderer::preloadTexture(int bitmapHandle, bool isAABitmap) {
  if (m_textureManager && bitmapHandle >= 0) {
    return m_textureManager->preloadTexture(bitmapHandle, isAABitmap) ? 1 : 0;
  }
  return 0;
}

void VulkanRenderer::updateTexture(const FrameCtx &ctx, int bitmapHandle, int bpp, const ubyte *data, int width,
                                   int height) {
  if (!m_textureManager || !m_textureUploader) {
    return;
  }
  if (bitmapHandle < 0 || data == nullptr || width <= 0 || height <= 0) {
    return;
  }

  // Transfer operations are invalid inside dynamic rendering.
  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  UploadCtx uploadCtx{ctx.m_recording.ref(), cmd, m_frameCounter};
  (void)m_textureUploader->updateTexture(uploadCtx, bitmapHandle, bpp, data, width, height);
}

void VulkanRenderer::releaseBitmap(int bitmapHandle) {
  if (m_textureManager && bitmapHandle >= 0) {
    m_textureManager->releaseBitmap(bitmapHandle);
  }
}

MovieTextureHandle VulkanRenderer::createMovieTexture(uint32_t width, uint32_t height, MovieColorSpace colorspace,
                                                      MovieColorRange range) {
  if (!m_movieManager || !m_movieManager->isAvailable()) {
    return MovieTextureHandle::Invalid;
  }
  return m_movieManager->createMovieTexture(width, height, colorspace, range);
}

void VulkanRenderer::uploadMovieTexture(const FrameCtx &ctx, MovieTextureHandle handle, const ubyte *y, int yStride,
                                        const ubyte *u, int uStride, const ubyte *v, int vStride) {
  if (!m_movieManager || !m_movieManager->isAvailable() || !gr_is_valid(handle)) {
    return;
  }

  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  UploadCtx uploadCtx{ctx.m_recording.ref(), cmd, m_frameCounter};
  m_movieManager->uploadMovieFrame(uploadCtx, handle, y, yStride, u, uStride, v, vStride);
}

void VulkanRenderer::drawMovieTexture(const FrameCtx &ctx, MovieTextureHandle handle, float x1, float y1, float x2,
                                      float y2, float alpha) {
  if (!m_movieManager || !m_movieManager->isAvailable() || !gr_is_valid(handle)) {
    return;
  }

  auto renderCtx = ensureRenderingStarted(ctx);
  incrementPrimDraw();
  m_movieManager->drawMovieTexture(renderCtx, handle, x1, y1, x2, y2, alpha);
}

void VulkanRenderer::releaseMovieTexture(MovieTextureHandle handle) {
  if (!m_movieManager || !gr_is_valid(handle)) {
    return;
  }
  m_movieManager->releaseMovieTexture(handle);
}

} // namespace vulkan
} // namespace graphics
