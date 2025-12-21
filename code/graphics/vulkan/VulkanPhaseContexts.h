#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

#include "VulkanRenderTargetInfo.h"

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

	// Rendering-phase context: only constructible by VulkanRenderer. Use this to make "draw-only" APIs uncallable
	// without proof that dynamic rendering is active.
	struct RenderCtx {
		vk::CommandBuffer cmd;
		RenderTargetInfo targetInfo;

	private:
		RenderCtx(vk::CommandBuffer inCmd, const RenderTargetInfo& inTargetInfo) : cmd(inCmd), targetInfo(inTargetInfo) {}
		friend class VulkanRenderer;
	};

} // namespace vulkan
} // namespace graphics
