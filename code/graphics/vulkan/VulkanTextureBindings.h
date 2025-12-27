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
    auto info = m_textures.tryGetResidentDescriptor(id, samplerKey);
    if (info.has_value()) {
      m_textures.markTextureUsed(id, currentFrameIndex);
      return *info;
    }

    m_textures.queueTextureUpload(id, currentFrameIndex, samplerKey);
    return m_textures.fallbackDescriptor(samplerKey);
  }

  // Returns a stable bindless slot index for this texture id.
  // - If the texture is not resident or does not have a slot yet, returns fallback.
  // - Slot assignment is upload-phase only; draw paths must not allocate/evict slots.
  // Also queues an upload for missing textures.
  uint32_t bindlessIndex(TextureId id, uint32_t currentFrameIndex) {
    m_textures.requestBindlessSlot(id);

    if (!m_textures.isResident(id)) {
      VulkanTextureManager::SamplerKey samplerKey{};
      samplerKey.address = vk::SamplerAddressMode::eRepeat;
      samplerKey.filter = vk::Filter::eLinear;
      m_textures.queueTextureUpload(id, currentFrameIndex, samplerKey);
      return kBindlessTextureSlotFallback;
    }

    const auto slotOpt = m_textures.tryGetBindlessSlot(id);
    if (!slotOpt.has_value()) {
      return kBindlessTextureSlotFallback;
    }

    m_textures.markTextureUsed(id, currentFrameIndex);
    return *slotOpt;
  }

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
