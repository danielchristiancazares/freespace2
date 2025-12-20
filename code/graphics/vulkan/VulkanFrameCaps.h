#pragma once

#include "VulkanFrame.h"

#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanRenderer;

struct FrameCtx {
  VulkanRenderer& renderer;
  VulkanFrame& frame;
  vk::CommandBuffer cmd;
};

struct ModelBoundFrame {
  FrameCtx ctx;
  vk::DescriptorSet modelSet;
  DynamicUniformBinding modelUbo;
};

inline ModelBoundFrame requireModelBound(FrameCtx ctx)
{
  Assertion(ctx.frame.modelUniformBinding.bufferHandle.isValid(),
    "ModelData UBO binding not set; call gr_bind_uniform_buffer(ModelData) before rendering models");
  Assertion(ctx.frame.modelDescriptorSet(), "Model descriptor set must be allocated");

  return ModelBoundFrame{ ctx, ctx.frame.modelDescriptorSet(), ctx.frame.modelUniformBinding };
}

struct NanoVGBoundFrame {
  FrameCtx ctx;
  BoundUniformBuffer nanovgUbo;
};

inline NanoVGBoundFrame requireNanoVGBound(FrameCtx ctx)
{
  Assertion(ctx.frame.nanovgData.handle.isValid(),
    "NanoVGData UBO binding not set; call gr_bind_uniform_buffer(NanoVGData) before rendering NanoVG");
  Assertion(ctx.frame.nanovgData.size > 0, "NanoVGData UBO binding must have non-zero size");

  return NanoVGBoundFrame{ ctx, ctx.frame.nanovgData };
}

} // namespace vulkan
} // namespace graphics

