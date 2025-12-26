#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

#include "VulkanRenderTargetInfo.h"

namespace graphics {
namespace vulkan {

class VulkanFrame;
class VulkanRenderer;

// Upload-phase context: only constructible by VulkanRenderer. Use this to make "upload-only" APIs uncallable from draw
// paths.
struct UploadCtx {
  VulkanFrame &frame;
  vk::CommandBuffer cmd;
  uint32_t currentFrameIndex = 0;

  UploadCtx(const UploadCtx &) = delete;
  UploadCtx &operator=(const UploadCtx &) = delete;
  UploadCtx(UploadCtx &&) = default;
  UploadCtx &operator=(UploadCtx &&) = default;

private:
  UploadCtx(VulkanFrame &inFrame, vk::CommandBuffer inCmd, uint32_t inCurrentFrameIndex)
      : frame(inFrame), cmd(inCmd), currentFrameIndex(inCurrentFrameIndex) {}

  friend class VulkanRenderer;
};

// Rendering-phase context: only constructible by VulkanRenderer. Use this to make "draw-only" APIs uncallable
// without proof that dynamic rendering is active.
struct RenderCtx {
  vk::CommandBuffer cmd;
  RenderTargetInfo targetInfo;

  RenderCtx(const RenderCtx &) = delete;
  RenderCtx &operator=(const RenderCtx &) = delete;
  RenderCtx(RenderCtx &&) = default;
  RenderCtx &operator=(RenderCtx &&) = default;

private:
  RenderCtx(vk::CommandBuffer inCmd, const RenderTargetInfo &inTargetInfo) : cmd(inCmd), targetInfo(inTargetInfo) {}
  friend class VulkanRenderer;
};

// Deferred lighting typestate tokens. These encode call order (begin -> end -> finish) without enums.
struct DeferredGeometryCtx {
  uint32_t frameIndex = 0;

  DeferredGeometryCtx(const DeferredGeometryCtx &) = delete;
  DeferredGeometryCtx &operator=(const DeferredGeometryCtx &) = delete;
  DeferredGeometryCtx(DeferredGeometryCtx &&) = default;
  DeferredGeometryCtx &operator=(DeferredGeometryCtx &&) = default;

private:
  explicit DeferredGeometryCtx(uint32_t inFrameIndex) : frameIndex(inFrameIndex) {}
  friend class VulkanRenderer;
};

struct DeferredLightingCtx {
  uint32_t frameIndex = 0;

  DeferredLightingCtx(const DeferredLightingCtx &) = delete;
  DeferredLightingCtx &operator=(const DeferredLightingCtx &) = delete;
  DeferredLightingCtx(DeferredLightingCtx &&) = default;
  DeferredLightingCtx &operator=(DeferredLightingCtx &&) = default;

private:
  explicit DeferredLightingCtx(uint32_t inFrameIndex) : frameIndex(inFrameIndex) {}
  friend class VulkanRenderer;
};

} // namespace vulkan
} // namespace graphics
