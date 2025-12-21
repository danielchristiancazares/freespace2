#pragma once

#include "VulkanDebug.h"
#include "VulkanModelTypes.h"
#include "VulkanTextureId.h"
#include "VulkanTextureManager.h"

namespace graphics {
namespace vulkan {

// Draw-path API: no command buffer access; may only return already-valid descriptors/indices and queue uploads.
class VulkanTextureBindings {
public:
	explicit VulkanTextureBindings(VulkanTextureManager& textures) : m_textures(textures) {}

	// Returns a valid descriptor (falls back if not resident) and queues an upload if needed.
	vk::DescriptorImageInfo descriptor(TextureId id,
		uint32_t currentFrameIndex,
		const VulkanTextureManager::SamplerKey& samplerKey)
	{
		const int fallbackHandle = m_textures.getFallbackTextureHandle();
		Assertion(fallbackHandle != -1, "Fallback texture must be initialized");

		if (!id.isValid()) {
			return m_textures.getTextureDescriptorInfo(fallbackHandle, samplerKey);
		}

		auto info = m_textures.getTextureDescriptorInfo(id.value, samplerKey);
		if (info.imageView) {
			m_textures.markTextureUsedBaseFrame(id.value, currentFrameIndex);
		} else {
			m_textures.queueTextureUploadBaseFrame(id.value, currentFrameIndex, samplerKey);
			info = m_textures.getTextureDescriptorInfo(fallbackHandle, samplerKey);
		}

		Assertion(info.imageView, "TextureBindings::descriptor must return a valid imageView");
		return info;
	}

	// Returns the bindless slot index if the texture is resident; otherwise returns MODEL_OFFSET_ABSENT.
	// Also queues an upload for missing textures.
	uint32_t bindlessIndex(TextureId id)
	{
		if (!id.isValid()) {
			return MODEL_OFFSET_ABSENT;
		}
		return m_textures.getBindlessSlotIndex(id.value);
	}

private:
	VulkanTextureManager& m_textures;
};

// Upload-phase API: records GPU work. Must only be called while no rendering is active.
class VulkanTextureUploader {
public:
	explicit VulkanTextureUploader(VulkanTextureManager& textures) : m_textures(textures) {}

	void flushPendingUploads(VulkanFrame& frame, vk::CommandBuffer cmd, uint32_t currentFrameIndex)
	{
		m_textures.flushPendingUploads(frame, cmd, currentFrameIndex);
	}

private:
	VulkanTextureManager& m_textures;
};

} // namespace vulkan
} // namespace graphics
