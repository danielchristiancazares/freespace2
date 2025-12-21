#pragma once

#include "VulkanConstants.h"
#include "VulkanFrame.h"
#include "VulkanModelTypes.h"

#include <vulkan/vulkan.hpp>
#include <array>
#include <unordered_map>
#include <cstddef>
#include <utility>
#include <vector>

namespace graphics {
namespace vulkan {

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

	enum class TextureState {
		Missing,
		Queued,
		Uploading,
		Resident,
		Failed,
		Retired // Marked for destruction, awaiting frame drainage
	};

		struct TextureBindingState {
			uint32_t arrayIndex = MODEL_OFFSET_ABSENT;
		};

	struct TextureRecord {
		VulkanTexture gpu;
		TextureState state = TextureState::Missing;
		uint32_t pendingFrameIndex = 0; // Frame that recorded the upload
		uint32_t lastUsedFrame = 0;
		TextureBindingState bindingState;
	};

	struct SamplerKey {
		vk::Filter filter = vk::Filter::eLinear;
		vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

		bool operator==(const SamplerKey& other) const
		{
			return filter == other.filter && address == other.address;
		}
	};

	// Flush pending uploads (only callable when no rendering is active).
	void flushPendingUploads(VulkanFrame& frame, vk::CommandBuffer cmd, uint32_t currentFrameIndex);
	void markUploadsCompleted(uint32_t completedFrameIndex);

	// Queue texture for async upload (public wrapper for ensureTextureResident)
	void queueTextureUpload(int bitmapHandle, VulkanFrame& frame, vk::CommandBuffer cmd, uint32_t currentFrameIndex, const SamplerKey& samplerKey);

	// Preload uploads immediately; returns true on success.
	bool preloadTexture(int bitmapHandle, bool isAABitmap);

	// Delete texture for a bitmap handle (base frame)
	void deleteTexture(int bitmapHandle);

	// Cleanup all resources
	void cleanup();

		// Descriptor binding management
		void onTextureResident(int textureHandle);
			void retireTexture(int textureHandle, uint64_t retireSerial);
			uint32_t getBindlessSlotIndex(int textureHandle);

		const std::vector<uint32_t>& getRetiredSlots() const { return m_retiredSlots; }
		void clearRetiredSlotsIfAllFramesUpdated(uint32_t completedFrameIndex);
		int getFallbackTextureHandle() const { return m_fallbackTextureHandle; }
		int getDefaultTextureHandle() const { return m_defaultTextureHandle; }
		void processPendingDestructions(uint64_t completedSerial);
		const std::vector<int>& getNewlyResidentTextures() const { return m_newlyResidentTextures; }
		void clearNewlyResidentTextures() { m_newlyResidentTextures.clear(); }

		// Direct access to textures for descriptor sync (non-const to allow marking dirty flags)
		std::unordered_map<int, TextureRecord>& allTextures() { return m_textures; }

		// Conservative serial used to defer destruction when evicting/deleting textures.
		// Updated by the renderer (typically "last submitted serial").
		void setSafeRetireSerial(uint64_t serial) { m_safeRetireSerial = serial; }

		// Current CPU frame counter (monotonic). Used for LRU bookkeeping.
		void setCurrentFrameIndex(uint32_t frameIndex) { m_currentFrameIndex = frameIndex; }

		// Get texture descriptor info without frame/cmd (for already-resident textures)
		vk::DescriptorImageInfo getTextureDescriptorInfo(int textureHandle, const SamplerKey& samplerKey);

	// Synthetic handle for fallback texture (won't collide with bmpman handles which are >= 0)
	static constexpr int kFallbackTextureHandle = -1000;
	// Synthetic handle for default white texture (won't collide with bmpman handles which are >= 0)
	static constexpr int kDefaultTextureHandle = -1001;

  private:
	vk::Device m_device;
	vk::PhysicalDeviceMemoryProperties m_memoryProperties;
	vk::Queue m_transferQueue;
	uint32_t m_transferQueueIndex;

	vk::UniqueSampler m_defaultSampler;

	std::unordered_map<int, TextureRecord> m_textures; // keyed by base frame
	std::unordered_map<size_t, vk::UniqueSampler> m_samplerCache;
	std::vector<int> m_pendingUploads; // base frame handles queued for upload

	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;
	void createDefaultSampler();

	vk::Sampler getOrCreateSampler(const SamplerKey& key);
	bool uploadImmediate(int baseFrame, bool isAABitmap);
	void createSolidTexture(int textureHandle, const uint8_t rgba[4]);
	void createFallbackTexture();
	void createDefaultTexture();
	TextureRecord* ensureTextureResident(int bitmapHandle,
		VulkanFrame& frame,
		vk::CommandBuffer cmd,
		uint32_t currentFrameIndex,
		const SamplerKey& samplerKey,
		bool& usedStaging,
		bool& uploadQueued);
	bool isUploadQueued(int baseFrame) const;

		// Slots that need fallback descriptor written (original slot indices)
		std::vector<uint32_t> m_retiredSlots;

		// Pool of bindless texture slots (excluding 0, reserved for fallback)
		std::vector<uint32_t> m_freeBindlessSlots;

		// Counter for tracking when all frames have processed retired slots
		uint32_t m_retiredSlotsFrameCounter = 0;

	// Fallback "black" texture for retired slots (initialized at startup)
	int m_fallbackTextureHandle = -1;
	// Default "white" texture for untextured draws (initialized at startup)
	int m_defaultTextureHandle = -1;
	
	// Textures that became Resident in markUploadsCompleted and need descriptor write
	std::vector<int> m_newlyResidentTextures;

		// Pending destructions: {textureHandle, retireSerial}
		std::vector<std::pair<int, uint64_t>> m_pendingDestructions;

		// Serial at/after which it is safe to destroy newly-retired resources.
		uint64_t m_safeRetireSerial = 0;

		uint32_t m_currentFrameIndex = 0;
	};

} // namespace vulkan
} // namespace graphics
