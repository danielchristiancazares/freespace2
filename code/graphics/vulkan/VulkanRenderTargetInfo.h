#pragma once

#include <cstdint>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

// Render target "contract" for pipeline compatibility.
// This is the minimal set of properties the pipeline cache keys on.
struct RenderTargetInfo {
  vk::Format colorFormat = vk::Format::eUndefined;
  uint32_t colorAttachmentCount = 1;
  vk::Format depthFormat = vk::Format::eUndefined; // eUndefined => no depth attachment
};

} // namespace vulkan
} // namespace graphics
