#pragma once

#include "VulkanConstants.h"
#include "VulkanDeferredRelease.h"
#include "VulkanFrame.h"
#include "VulkanModelTypes.h"

#include <vulkan/vulkan.hpp>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace graphics {
namespace vulkan {

class VulkanTextureUploader;
struct UploadCtx;

// Helper for block-compressed images (BC1/BC3/BC7). Public for test coverage.
inline size_t calculateCompressedSize(uint32_t w, uint32_t h, vk::Format format)
{
	const size_t blockSize = (format == vk::Format::eBc1RgbaUnormBlock) ? 8 : 16;
	const size_t blocksWide = (w + 3) / 4;
	const size_t blocksHigh = (h + 3) / 4;
	return blocksWide * blocksHigh * blockSize;
}

inline bool isBlockCompressedFormat(vk::Format format)
{
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

inline size_t calculateLayerSize(uint32_t w, uint32_t h, vk::Format format)
{
	if (isBlockCompressedFormat(format)) {
		return calculateCompressedSize(w, h, format);
	}
	if (format == vk::Format::eR8Unorm) {
		return static_cast<size_t>(w) * h;
	}
	// Non-compressed uploads are expanded to 4 bytes/pixel in the upload path.
	return static_cast<size_t>(w) * h * 4;
}

inline size_t alignUp(size_t value, size_t alignment)
{
	return (value + (alignment - 1)) & ~(alignment - 1);
}

struct ImmediateUploadLayout {
	size_t layerSize = 0;
	size_t totalSize = 0;
	std::vector<size_t> layerOffsets;
};

inline ImmediateUploadLayout buildImmediateUploadLayout(uint32_t w, uint32_t h, vk::Format format, uint32_t layers)
{
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
	VulkanTextureManager(vk::Device device,
		const vk::PhysicalDeviceMemoryProperties& memoryProps,
		vk::Queue transferQueue,
		uint32_t transferQueueIndex);

	enum class UnavailableReason {
		InvalidHandle,
		InvalidArray,
		BmpLockFailed,
		TooLargeForStaging,
		UnsupportedFormat,
	};

	struct ResidentTexture {
		VulkanTexture gpu;
		uint32_t lastUsedFrame = 0;
		uint64_t lastUsedSerial = 0; // Serial of most recent submission that may reference this texture
	};

	struct UnavailableTexture {
		UnavailableReason reason = UnavailableReason::InvalidHandle;
	};

	struct SamplerKey {
		vk::Filter filter = vk::Filter::eLinear;
		vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

		bool operator==(const SamplerKey& other) const
		{
			return filter == other.filter && address == other.address;
		}
	};

		// Queue texture for upload (CPU-side only; does not record GPU work).
		void queueTextureUpload(int bitmapHandle, uint32_t currentFrameIndex, const SamplerKey& samplerKey);
		// Variant for callers that already have a base-frame handle.
		void queueTextureUploadBaseFrame(int baseFrame, uint32_t currentFrameIndex, const SamplerKey& samplerKey);

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

		// Descriptor binding management
		// Returns a stable bindless slot index for this texture handle.
		// - If the handle is missing/unavailable/not-yet-resident, the slot's descriptor points at fallback.
		// - If no slot can be assigned safely, returns slot 0 (fallback).
		uint32_t getBindlessSlotIndex(int textureHandle);

		// Mark a texture as used by the upcoming submission (bindless or descriptor bind).
		void markTextureUsedBaseFrame(int baseFrame, uint32_t currentFrameIndex);

		void collect(uint64_t completedSerial);

		// Builtin texture descriptors (always valid).
		// These replace the old synthetic-handle approach (negative handle sentinels).
		vk::DescriptorImageInfo fallbackDescriptor(const SamplerKey& samplerKey) const;
		vk::DescriptorImageInfo defaultBaseDescriptor(const SamplerKey& samplerKey) const;
		vk::DescriptorImageInfo defaultNormalDescriptor(const SamplerKey& samplerKey) const;
		vk::DescriptorImageInfo defaultSpecDescriptor(const SamplerKey& samplerKey) const;

		// Populate (slot, baseFrameHandle) pairs for bindless descriptor updates.
		void appendResidentBindlessDescriptors(std::vector<std::pair<uint32_t, int>>& out) const;
		// Query slot assignment state (state-as-location: presence in m_bindlessSlots).
		bool hasBindlessSlot(int baseFrameHandle) const;

		// Serial at/after which it is safe to destroy newly-retired resources.
		// During frame recording this should be the serial of the upcoming submit; after submit it should match the last submitted serial.
		void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }

		// Current CPU frame counter (monotonic). Used for LRU bookkeeping.
		void setCurrentFrameIndex(uint32_t frameIndex) { m_currentFrameIndex = frameIndex; }

		// Get texture descriptor info without frame/cmd (for already-resident textures)
		vk::DescriptorImageInfo getTextureDescriptorInfo(int textureHandle, const SamplerKey& samplerKey);

		// ------------------------------------------------------------------------
		// Bitmap render targets (bmpman RTT)
		// ------------------------------------------------------------------------
		// Creates a GPU-backed bitmap render target for the given bmpman base-frame handle.
		// The image is cleared to black and transitioned to shader-read on creation.
		bool createRenderTarget(int baseFrameHandle, uint32_t width, uint32_t height, int flags, uint32_t* outMipLevels);
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

		struct BuiltinTextures {
			VulkanTexture fallback;
			VulkanTexture defaultBase;
			VulkanTexture defaultNormal;
			VulkanTexture defaultSpec;

			void reset() noexcept
			{
				fallback = {};
				defaultBase = {};
				defaultNormal = {};
				defaultSpec = {};
			}
		};

		// Flush pending uploads (upload phase only; records GPU work).
		void flushPendingUploads(const UploadCtx& ctx);
		// Update the contents of an existing bitmap texture (upload phase only; records GPU work).
		bool updateTexture(const UploadCtx& ctx, int bitmapHandle, int bpp, const ubyte* data, int width, int height);

		void processPendingRetirements();
		void retryPendingBindlessSlots();
		bool tryAssignBindlessSlot(int textureHandle, bool allowResidentEvict);
		void onTextureResident(int textureHandle);
		void retireTexture(int textureHandle, uint64_t retireSerial);

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

		std::optional<RenderTargetRecord> tryTakeRenderTargetRecord(int baseFrameHandle);

	vk::Device m_device;
	vk::PhysicalDeviceMemoryProperties m_memoryProperties;
	vk::Queue m_transferQueue;
	uint32_t m_transferQueueIndex;

	vk::UniqueSampler m_defaultSampler;

	// State as location:
	// - presence in m_residentTextures => resident
	// - presence in m_pendingUploads   => queued for upload
	// - presence in m_unavailable      => permanently unavailable (non-retriable under current algorithm)
	// - presence in m_bindlessSlots    => has a bindless slot assigned
	std::unordered_map<int, ResidentTexture> m_residentTextures; // keyed by bmpman base frame handle
	std::unordered_map<int, UnavailableTexture> m_unavailableTextures; // keyed by base frame
		std::unordered_map<int, RenderTargetRecord> m_renderTargets; // keyed by base frame (bmpman render targets)
		std::unordered_map<int, uint32_t> m_bindlessSlots; // keyed by base frame
		std::unordered_set<int> m_pendingBindlessSlots; // textures waiting for a bindless slot assignment (retry each frame start)
		std::unordered_set<int> m_pendingRetirements; // textures to retire at the next upload-phase flush (slot reuse safe point)
		mutable std::unordered_map<size_t, vk::UniqueSampler> m_samplerCache;
		std::vector<int> m_pendingUploads; // base frame handles queued for upload

	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
	void createDefaultSampler();

	vk::Sampler getOrCreateSampler(const SamplerKey& key) const;
	bool uploadImmediate(int baseFrame, bool isAABitmap);
		VulkanTexture createSolidTexture(const uint8_t rgba[4]);
		void createFallbackTexture();
		void createDefaultTexture();
		void createDefaultNormalTexture();
		void createDefaultSpecTexture();
		bool isUploadQueued(int baseFrame) const;

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
