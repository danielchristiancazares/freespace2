#pragma once

#include "VulkanDebug.h"
#include "VulkanModelTypes.h"
#include "VulkanPhaseContexts.h"
#include "VulkanTextureId.h"
#include "VulkanTextureManager.h"

#include "bmpman/bmpman.h"

namespace graphics {
namespace vulkan {

// Draw-path API: no command buffer access; may only return already-valid descriptors/indices and queue uploads.
class VulkanTextureBindings {
public:
  explicit VulkanTextureBindings(VulkanTextureManager &textures) : m_textures(textures) {}

  // Returns a valid descriptor (falls back if not resident) and queues an upload if needed.
  vk::DescriptorImageInfo descriptor(TextureId id, uint32_t currentFrameIndex,
                                     const VulkanTextureManager::SamplerKey &samplerKey) {
    auto info = m_textures.getTextureDescriptorInfo(id.baseFrame(), samplerKey);
    if (info.imageView) {
      m_textures.markTextureUsedBaseFrame(id.baseFrame(), currentFrameIndex);
    } else {
      m_textures.queueTextureUploadBaseFrame(id.baseFrame(), currentFrameIndex, samplerKey);
      info = m_textures.fallbackDescriptor(samplerKey);
    }

    Assertion(info.imageView, "TextureBindings::descriptor must return a valid imageView");
    return info;
  }

  // Returns a stable bindless slot index for this texture id.
  // - If the texture is not resident yet, the slot's descriptor points at fallback until the upload completes.
  // - If no slot can be assigned due to pressure, returns slot 0 (fallback) for this frame and retries at frame start.
  // Also queues an upload for missing textures.
  uint32_t bindlessIndex(TextureId id) { return m_textures.getBindlessSlotIndex(id.baseFrame()); }

private:
  VulkanTextureManager &m_textures;
};

// Upload-phase API: records GPU work. Must only be called while no rendering is active.
class VulkanTextureUploader {
public:
  explicit VulkanTextureUploader(VulkanTextureManager &textures) : m_textures(textures) {}

  void flushPendingUploads(const UploadCtx &ctx) { m_textures.flushPendingUploads(ctx); }

  bool updateTexture(const UploadCtx &ctx, int bitmapHandle, int bpp, const ubyte *data, int width, int height) {
    return m_textures.updateTexture(ctx, bitmapHandle, bpp, data, width, height);
  }

private:
  VulkanTextureManager &m_textures;
};

} // namespace vulkan
} // namespace graphics
