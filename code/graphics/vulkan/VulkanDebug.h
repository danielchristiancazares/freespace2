#pragma once

#include "globalincs/pstypes.h"

namespace graphics {
namespace vulkan {

// Lightweight Vulkan logging helper that respects debug flags and flushes immediately.
void vkprintf(const char* format, ...);

} // namespace vulkan
} // namespace graphics

