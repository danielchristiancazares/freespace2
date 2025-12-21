#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanFrame;
class VulkanRenderer;

// Upload-phase context: only constructible by VulkanRenderer. Use this to make "upload-only" APIs uncallable from draw paths.
struct UploadCtx {
	VulkanFrame& frame;
	vk::CommandBuffer cmd;
	uint32_t currentFrameIndex = 0;

  private:
	UploadCtx(VulkanFrame& inFrame, vk::CommandBuffer inCmd, uint32_t inCurrentFrameIndex)
	    : frame(inFrame), cmd(inCmd), currentFrameIndex(inCurrentFrameIndex)
	{
	}

	friend class VulkanRenderer;
};

} // namespace vulkan
} // namespace graphics

