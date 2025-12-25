#pragma once

#include "VulkanFrame.h"
#include "VulkanFrameFlow.h"

#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanRenderer;

struct FrameCtx {
  VulkanRenderer& renderer;

  FrameCtx(VulkanRenderer& inRenderer, graphics::vulkan::RecordingFrame& inRecording)
      : renderer(inRenderer), m_recording(inRecording)
  {
  }

  VulkanFrame& frame() const { return m_recording.ref(); }

  private:
  graphics::vulkan::RecordingFrame& m_recording;

  friend class VulkanRenderer;
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

struct DecalBoundFrame {
  FrameCtx ctx;
  BoundUniformBuffer globalsUbo;
  BoundUniformBuffer infoUbo;
};

inline DecalBoundFrame requireDecalBound(FrameCtx ctx)
{
  Assertion(ctx.frame().decalGlobalsData.handle.isValid(),
    "DecalGlobals UBO binding not set; call gr_bind_uniform_buffer(DecalGlobals) before rendering decals");
  Assertion(ctx.frame().decalGlobalsData.size > 0, "DecalGlobals UBO binding must have non-zero size");
  Assertion(ctx.frame().decalInfoData.handle.isValid(),
    "DecalInfo UBO binding not set; call gr_bind_uniform_buffer(DecalInfo) before rendering decals");
  Assertion(ctx.frame().decalInfoData.size > 0, "DecalInfo UBO binding must have non-zero size");

  return DecalBoundFrame{ ctx, ctx.frame().decalGlobalsData, ctx.frame().decalInfoData };
}

} // namespace vulkan
} // namespace graphics
