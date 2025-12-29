#pragma once

#include "VulkanRenderTargets.h"

#include <cstdint>

#define MODEL_SDR_FLAG_MODE_CPP
#include "def_files/data/effects/model_shader_flags.h"
#undef MODEL_SDR_FLAG_MODE_CPP

namespace graphics {
namespace vulkan {

// The model fragment shader comes in two output signatures:
// - Forward: 1 color attachment (location 0 only)
// - Deferred: G-buffer MRT (locations 0..4)
//
// Normalize MODEL_SDR_FLAG_DEFERRED based on the active render target contract so module selection and pipeline
// selection stay consistent.
inline uint32_t normalize_model_variant_flags_for_target(uint32_t variantFlags, uint32_t colorAttachmentCount) {
  const bool wantsDeferredOutputs = (colorAttachmentCount == VulkanRenderTargets::kGBufferCount);
  if (wantsDeferredOutputs) {
    return variantFlags | static_cast<uint32_t>(MODEL_SDR_FLAG_DEFERRED);
  }
  return variantFlags & ~static_cast<uint32_t>(MODEL_SDR_FLAG_DEFERRED);
}

} // namespace vulkan
} // namespace graphics
