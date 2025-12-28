#pragma once

#include "VulkanConstants.h"
#include "VulkanDeferredRelease.h"
#include "VulkanFrame.h"
#include "VulkanModelTypes.h"
#include "VulkanTextureId.h"

#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

class PendingUploadQueueTest;
class SlotGatingTest;

namespace graphics {
namespace vulkan {

class VulkanTextureUploader;
struct UploadCtx;

// Helper for block-compressed images (BC1/BC3/BC7). Public for test coverage.
inline size_t calculateCompressedSize(uint32_t w, uint32_t h, vk::Format format) {
  const size_t blockSize = (format == vk::Format::eBc1RgbaUnormBlock) ? 8 : 16;
  const size_t blocksWide = (w + 3) / 4;
  const size_t blocksHigh = (h + 3) / 4;
  return blocksWide * blocksHigh * blockSize;
}

inline bool isBlockCompressedFormat(vk::Format format) {
  switch (format) {
  case vk::Format::eBc1RgbaUnormBlock:
  case vk::Format::eBc2UnormBlock:
  case vk::Format::eBc3UnormBlock:
  case vk::Format::eBc7UnormBlock:
    return true;
  default:
    return false;
  }
}

inline size_t calculateLayerSize(uint32_t w, uint32_t h, vk::Format format) {
  if (isBlockCompressedFormat(format)) {
    return calculateCompressedSize(w, h, format);
  }
  if (format == vk::Format::eR8Unorm) {
    return static_cast<size_t>(w) * h;
  }
  // Non-compressed uploads are expanded to 4 bytes/pixel in the upload path.
  return static_cast<size_t>(w) * h * 4;
}

inline size_t alignUp(size_t value, size_t alignment) { return (value + (alignment - 1)) & ~(alignment - 1); }

struct ImmediateUploadLayout {
  size_t layerSize = 0;
  size_t totalSize = 0;
  std::vector<size_t> layerOffsets;
};

inline ImmediateUploadLayout buildImmediateUploadLayout(uint32_t w, uint32_t h, vk::Format format, uint32_t layers) {
  ImmediateUploadLayout layout;
  layout.layerSize = calculateLayerSize(w, h, format);
  layout.layerOffsets.reserve(layers);

  constexpr size_t kCopyOffsetAlignment = 4;
  size_t offset = 0;
  for (uint32_t layer = 0; layer < layers; ++layer) {
    offset = alignUp(offset, kCopyOffsetAlignment);
    layout.layerOffsets.push_back(offset);
    offset += layout.layerSize;
  }
  layout.totalSize = alignUp(offset, kCopyOffsetAlignment);
  return layout;
}

struct VulkanTexture {
  vk::UniqueImage image;
  vk::UniqueDeviceMemory memory;
  vk::UniqueImageView imageView;
  vk::Sampler sampler; // Borrowed from sampler cache
  vk::ImageLayout currentLayout = vk::ImageLayout::eUndefined;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t layers = 1;
  uint32_t mipLevels = 1;
  vk::Format format = vk::Format::eUndefined;
};

class VulkanTextureManager {
public:
  VulkanTextureManager(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memoryProps,
                       vk::Queue transferQueue, uint32_t transferQueueIndex);

  struct SamplerKey {
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

    bool operator==(const SamplerKey &other) const { return filter == other.filter && address == other.address; }
  };

  // Queue texture for upload (CPU-side only; does not record GPU work).
  void queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex, const SamplerKey &samplerKey);
  // Variant for callers that already have a base-frame handle.
  void queueTextureUploadBaseFrame(int baseFrame, uint32_t currentFrameIndex, const SamplerKey &samplerKey);
  // Variant for callers that already have a validated TextureId.
  void queueTextureUpload(TextureId id, uint32_t currentFrameIndex, const SamplerKey &samplerKey);

  // Preload uploads immediately. Return value follows bmpman gf_preload semantics:
  // - false: abort further preloading (out of memory)
  // - true: continue preloading (success or recoverable/unavailable texture)
  bool preloadTexture(int bitmapHandle, bool isAABitmap);

  // Delete texture for a bitmap handle (base frame)
  void deleteTexture(int bitmapHandle);

  // Called by bmpman when a bitmap handle is being released (slot will become BM_TYPE_NONE).
  // This must drop any GPU mapping immediately so handle reuse cannot collide.
  void releaseBitmap(int bitmapHandle);

  // Cleanup all resources
  void cleanup();

  // Bindless slot ownership (draw-path safe: request is CPU-only; lookup is read-only).
  void requestBindlessSlot(TextureId id);
  [[nodiscard]] std::optional<uint32_t> tryGetBindlessSlot(TextureId id) const;
  [[nodiscard]] bool hasBindlessSlot(TextureId id) const { return tryGetBindlessSlot(id).has_value(); }

  // Residency / usage tracking.
  [[nodiscard]] bool isResident(TextureId id) const;
  void markTextureUsed(TextureId id, uint32_t currentFrameIndex);
  // Debug: record HUD textures that were drawn while non-resident (for -vk_hud_debug).
  void markHudTextureMissing(TextureId id);

  // Get descriptor for a resident texture. Absence is represented as std::nullopt (no fake inhabitant).
  [[nodiscard]] std::optional<vk::DescriptorImageInfo> tryGetResidentDescriptor(TextureId id,
                                                                                const SamplerKey &samplerKey) const;

  void collect(uint64_t completedSerial);

  // Builtin texture descriptors (always valid).
  // These replace the old synthetic-handle approach (negative handle sentinels).
  vk::DescriptorImageInfo fallbackDescriptor(const SamplerKey &samplerKey) const;
  vk::DescriptorImageInfo defaultBaseDescriptor(const SamplerKey &samplerKey) const;
  vk::DescriptorImageInfo defaultNormalDescriptor(const SamplerKey &samplerKey) const;
  vk::DescriptorImageInfo defaultSpecDescriptor(const SamplerKey &samplerKey) const;

  // Populate (slot, TextureId) pairs for bindless descriptor updates.
  void appendResidentBindlessDescriptors(std::vector<std::pair<uint32_t, TextureId>> &out) const;

  // Serial at/after which it is safe to destroy newly-retired resources.
  // During frame recording this should be the serial of the upcoming submit; after submit it should match the last
  // submitted serial.
  void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }

  // Current CPU frame counter (monotonic). Used for LRU bookkeeping.
  void setCurrentFrameIndex(uint32_t frameIndex) { m_currentFrameIndex = frameIndex; }

  // ------------------------------------------------------------------------
  // Bitmap render targets (bmpman RTT)
  // ------------------------------------------------------------------------
  // Creates a GPU-backed bitmap render target for the given bmpman base-frame handle.
  // The image is cleared to black and transitioned to shader-read on creation.
  bool createRenderTarget(int baseFrameHandle, uint32_t width, uint32_t height, int flags, uint32_t *outMipLevels);
  bool hasRenderTarget(int baseFrameHandle) const;
  vk::Extent2D renderTargetExtent(int baseFrameHandle) const;
  vk::Format renderTargetFormat(int baseFrameHandle) const;
  uint32_t renderTargetMipLevels(int baseFrameHandle) const;
  vk::Image renderTargetImage(int baseFrameHandle) const;
  vk::ImageView renderTargetAttachmentView(int baseFrameHandle, int face) const;

  // Layout transitions and mip generation for render targets. These record GPU work into the provided cmd buffer.
  void transitionRenderTargetToAttachment(vk::CommandBuffer cmd, int baseFrameHandle);
  void transitionRenderTargetToTransferDst(vk::CommandBuffer cmd, int baseFrameHandle);
  void transitionRenderTargetToShaderRead(vk::CommandBuffer cmd, int baseFrameHandle);
  void generateRenderTargetMipmaps(vk::CommandBuffer cmd, int baseFrameHandle);

private:
  friend class VulkanTextureUploader;
  friend class ::PendingUploadQueueTest;
  friend class ::SlotGatingTest;

  struct BuiltinTextures {
    VulkanTexture fallback;
    VulkanTexture defaultBase;
    VulkanTexture defaultNormal;
    VulkanTexture defaultSpec;

    void reset() noexcept {
      fallback = {};
      defaultBase = {};
      defaultNormal = {};
      defaultSpec = {};
    }
  };

  // Flush pending uploads (upload phase only; records GPU work).
  void flushPendingUploads(const UploadCtx &ctx);
  // Update the contents of an existing bitmap texture (upload phase only; records GPU work).
  bool updateTexture(const UploadCtx &ctx, int bitmapHandle, int bpp, const ubyte *data, int width, int height);

  struct SamplerKeyHash {
    size_t operator()(const SamplerKey &key) const noexcept {
      const size_t filter = static_cast<size_t>(key.filter);
      const size_t address = static_cast<size_t>(key.address);
      return (filter << 4) ^ address;
    }
  };

  struct UsageTracking {
    uint32_t lastUsedFrame = 0;
    uint64_t lastUsedSerial = 0; // Serial of most recent submission that may reference this texture
  };

  struct BitmapTexture {
    VulkanTexture gpu;
    UsageTracking usage;
  };

  void processPendingRetirements();
  bool shouldLogHudDebug(int baseFrame) const;
  bool logHudDebugOnce(int baseFrame, uint32_t flag);

  struct RenderTargetRecord {
    vk::Extent2D extent{};
    vk::Format format = vk::Format::eUndefined;
    uint32_t mipLevels = 1;
    uint32_t layers = 1;
    bool isCubemap = false;
    // Attachment views for rendering:
    // - 2D target: faceViews[0]
    // - Cubemap: faceViews[0..5]
    std::array<vk::UniqueImageView, 6> faceViews{};
  };

  struct RenderTargetTexture {
    VulkanTexture gpu;
    RenderTargetRecord rt;
    UsageTracking usage;
  };

  class PendingUploadQueue {
  public:
    // Returns true if newly enqueued; false if already present (idempotent).
    bool enqueue(TextureId id);
    // Returns true if the id was present and removed.
    bool erase(TextureId id);

    bool empty() const { return m_fifo.empty(); }
    std::deque<TextureId> takeAll();

  private:
    std::deque<TextureId> m_fifo;
    std::unordered_set<TextureId, TextureIdHasher> m_membership;
  };

  void assignBindlessSlots(const UploadCtx &ctx);
  [[nodiscard]] std::optional<TextureId> findEvictionCandidate() const;
  [[nodiscard]] std::optional<uint32_t> acquireFreeSlotOrEvict(const UploadCtx &ctx);
  void retireTexture(TextureId id, uint64_t retireSerial);

  vk::Device m_device;
  vk::PhysicalDeviceMemoryProperties m_memoryProperties;
  vk::Queue m_transferQueue;
  uint32_t m_transferQueueIndex;

  vk::UniqueSampler m_defaultSampler;

  // State as location:
  // - presence in m_bitmaps  => resident sampled bitmap texture
  // - presence in m_targets  => resident bmpman render target
  // - presence in m_pendingUploads => queued for upload
  // - presence in m_bindlessSlots  => has a bindless slot assigned (dynamic slots only)
  // - presence in m_permanentlyRejected => outside supported upload domain (do not retry automatically)
  std::unordered_map<TextureId, BitmapTexture, TextureIdHasher> m_bitmaps;
  std::unordered_map<TextureId, RenderTargetTexture, TextureIdHasher> m_targets;
  std::unordered_set<TextureId, TextureIdHasher> m_permanentlyRejected;
  std::unordered_map<TextureId, uint32_t, TextureIdHasher> m_bindlessSlots;
  std::unordered_set<TextureId, TextureIdHasher> m_bindlessRequested;
  std::unordered_set<TextureId, TextureIdHasher>
      m_pendingRetirements; // textures to retire at the next upload-phase flush (slot reuse safe point)
  mutable std::unordered_map<SamplerKey, vk::UniqueSampler, SamplerKeyHash> m_samplerCache;
  PendingUploadQueue m_pendingUploads;
  std::unordered_set<int> m_hudDebugMissing;
  std::unordered_map<int, uint32_t> m_hudDebugLogFlags;

  uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
  void createDefaultSampler();

  vk::Sampler getOrCreateSampler(const SamplerKey &key) const;
  bool uploadImmediate(TextureId id, bool isAABitmap);
  VulkanTexture createSolidTexture(const uint8_t rgba[4]);
  [[nodiscard]] VulkanTexture &renderTargetGpuOrAssert(int baseFrameHandle, const char *caller);
  void transitionRenderTargetToLayout(vk::CommandBuffer cmd, int baseFrameHandle, vk::ImageLayout newLayout,
                                      const char *caller);
  void createFallbackTexture();
  void createDefaultTexture();
  void createDefaultNormalTexture();
  void createDefaultSpecTexture();

  // Pool of bindless texture slots (excluding reserved default slots; see VulkanConstants.h)
  std::vector<uint32_t> m_freeBindlessSlots;

  // Builtin textures (fallback + defaults). Always valid while the texture manager is alive.
  BuiltinTextures m_builtins;

  DeferredReleaseQueue m_deferredReleases;

  // Serial at/after which it is safe to destroy newly-retired resources.
  uint64_t m_safeRetireSerial = 0;

  uint32_t m_currentFrameIndex = 0;
  uint64_t m_completedSerial = 0;
};

} // namespace vulkan
} // namespace graphics
