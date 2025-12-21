#pragma once

#include "VulkanFrame.h"
#include "VulkanFrameFlow.h"

#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanRenderer;

struct FrameCtx {
  VulkanRenderer& renderer;
  graphics::vulkan::RecordingFrame& recording;

  VulkanFrame& frame() const { return recording.ref(); }
  vk::CommandBuffer cmd() const { return recording.cmd(); }
  uint32_t imageIndex() const { return recording.imageIndex; }
};

struct ModelBoundFrame {
  FrameCtx ctx;
  vk::DescriptorSet modelSet;
  DynamicUniformBinding modelUbo;
  uint32_t transformDynamicOffset = 0;
  size_t transformSize = 0;
};

inline ModelBoundFrame requireModelBound(FrameCtx ctx)
{
  Assertion(ctx.frame().modelUniformBinding.bufferHandle.isValid(),
    "ModelData UBO binding not set; call gr_bind_uniform_buffer(ModelData) before rendering models");
  Assertion(ctx.frame().modelDescriptorSet(), "Model descriptor set must be allocated");

  return ModelBoundFrame{ ctx,
    ctx.frame().modelDescriptorSet(),
    ctx.frame().modelUniformBinding,
    ctx.frame().modelTransformDynamicOffset,
    ctx.frame().modelTransformSize };
}

struct NanoVGBoundFrame {
  FrameCtx ctx;
  BoundUniformBuffer nanovgUbo;
};

inline NanoVGBoundFrame requireNanoVGBound(FrameCtx ctx)
{
  Assertion(ctx.frame().nanovgData.handle.isValid(),
    "NanoVGData UBO binding not set; call gr_bind_uniform_buffer(NanoVGData) before rendering NanoVG");
  Assertion(ctx.frame().nanovgData.size > 0, "NanoVGData UBO binding must have non-zero size");

  return NanoVGBoundFrame{ ctx, ctx.frame().nanovgData };
}

} // namespace vulkan
} // namespace graphics

