#pragma once

#include "globalincs/pstypes.h"

namespace graphics {
namespace vulkan {

// Lightweight Vulkan logging helper that respects debug flags and flushes immediately.
void vkprintf(const char *format, ...);

// Per-feature extended dynamic state 3 capability flags.
// Vulkan 1.3 promoted some dynamic state features to core, but EDS3 features
// are still extension-only and must be queried per-feature.
struct ExtendedDynamicState3Caps {
  bool colorBlendEnable = false;
  bool colorWriteMask = false;
  bool polygonMode = false;
  bool rasterizationSamples = false;
};

} // namespace vulkan
} // namespace graphics
