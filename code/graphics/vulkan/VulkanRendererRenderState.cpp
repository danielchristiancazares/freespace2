#include "VulkanFrameCaps.h"
#include "VulkanRenderer.h"

namespace graphics {
namespace vulkan {

RenderCtx VulkanRenderer::ensureRenderingStarted(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this,
            "ensureRenderingStarted called with FrameCtx from a different VulkanRenderer instance");
  return ensureRenderingStartedRecording(ctx.m_recording);
}

void VulkanRenderer::applySetupFrameDynamicState(const FrameCtx &ctx, const vk::Viewport &viewport,
                                                 const vk::Rect2D &scissor, float lineWidth) {
  Assertion(&ctx.renderer == this,
            "applySetupFrameDynamicState called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  Assertion(cmd, "applySetupFrameDynamicState called with null command buffer");

  cmd.setViewport(0, 1, &viewport);
  cmd.setScissor(0, 1, &scissor);
  cmd.setLineWidth(lineWidth);
}

void VulkanRenderer::pushDebugGroup(const FrameCtx &ctx, const char *name) {
  Assertion(name != nullptr, "pushDebugGroup called with null name");
  Assertion(&ctx.renderer == this, "pushDebugGroup called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  vk::DebugUtilsLabelEXT label{};
  label.pLabelName = name;
  label.color[0] = 1.0f;
  label.color[1] = 1.0f;
  label.color[2] = 1.0f;
  label.color[3] = 1.0f;

  cmd.beginDebugUtilsLabelEXT(label);
}

void VulkanRenderer::popDebugGroup(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this, "popDebugGroup called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  cmd.endDebugUtilsLabelEXT();
}

RenderCtx VulkanRenderer::ensureRenderingStartedRecording(graphics::vulkan::RecordingFrame &rec) {
  auto info = m_renderingSession->ensureRendering(rec.cmd(), rec.imageIndex);
  return RenderCtx{rec.cmd(), info};
}

void VulkanRenderer::setPendingRenderTargetSwapchain() { m_renderingSession->requestSwapchainTarget(); }

void VulkanRenderer::requestMainTargetWithDepth() {
  Assertion(m_renderingSession != nullptr, "requestMainTargetWithDepth called before rendering session initialization");
  if (m_sceneTexture.has_value()) {
    m_renderingSession->requestSceneHdrTarget();
  } else {
    m_renderingSession->requestSwapchainTarget();
  }
}

void VulkanRenderer::beginDecalPass(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this, "beginDecalPass called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "beginDecalPass called before rendering session initialization");

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  Assertion(cmd, "beginDecalPass called with null command buffer");

  // Decals sample depth; transitions are invalid inside dynamic rendering.
  m_renderingSession->suspendRendering();
  m_renderingSession->transitionMainDepthToShaderRead(cmd);
}

void VulkanRenderer::setViewport(const FrameCtx &ctx, const vk::Viewport &viewport) {
  Assertion(&ctx.renderer == this, "setViewport called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  cmd.setViewport(0, 1, &viewport);
}

void VulkanRenderer::setScissor(const FrameCtx &ctx, const vk::Rect2D &scissor) {
  Assertion(&ctx.renderer == this, "setScissor called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  cmd.setScissor(0, 1, &scissor);
}

void VulkanRenderer::setClearColor(int r, int g, int b) {
  m_renderingSession->setClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

int VulkanRenderer::setCullMode(int cull) {
  switch (cull) {
  case 0:
    m_renderingSession->setCullMode(vk::CullModeFlagBits::eNone);
    break;
  case 1:
    m_renderingSession->setCullMode(vk::CullModeFlagBits::eBack);
    break;
  case 2:
    m_renderingSession->setCullMode(vk::CullModeFlagBits::eFront);
    break;
  default:
    return 0;
  }
  return 1;
}

int VulkanRenderer::setZbufferMode(int mode) {
  switch (mode) {
  case 0: // ZBUFFER_TYPE_NONE
    m_renderingSession->setDepthTest(false);
    m_renderingSession->setDepthWrite(false);
    m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_NONE;
    break;
  case 1: // ZBUFFER_TYPE_READ
    m_renderingSession->setDepthTest(true);
    m_renderingSession->setDepthWrite(false);
    m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_READ;
    break;
  case 2: // ZBUFFER_TYPE_WRITE
    m_renderingSession->setDepthTest(false);
    m_renderingSession->setDepthWrite(true);
    m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_WRITE;
    break;
  case 3: // ZBUFFER_TYPE_FULL
    m_renderingSession->setDepthTest(true);
    m_renderingSession->setDepthWrite(true);
    m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;
    break;
  default:
    return 0;
  }
  return 1;
}

int VulkanRenderer::getZbufferMode() const { return static_cast<int>(m_zbufferMode); }

void VulkanRenderer::requestClear() { m_renderingSession->requestClear(); }

void VulkanRenderer::zbufferClear(int mode) {
  if (mode) {
    // Enable zbuffering + clear
    gr_zbuffering = 1;
    gr_zbuffering_mode = GR_ZBUFF_FULL;
    gr_global_zbuffering = 1;
    m_renderingSession->setDepthTest(true);
    m_renderingSession->setDepthWrite(true);
    m_renderingSession->requestDepthClear();
  } else {
    // Disable zbuffering
    gr_zbuffering = 0;
    gr_zbuffering_mode = GR_ZBUFF_NONE;
    gr_global_zbuffering = 0;
    m_renderingSession->setDepthTest(false);
  }
}

} // namespace vulkan
} // namespace graphics
