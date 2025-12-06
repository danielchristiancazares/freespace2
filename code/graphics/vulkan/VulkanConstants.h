#pragma once

#include <cstdint>

namespace graphics {
namespace vulkan {

constexpr uint32_t kFramesInFlight = 3;
constexpr uint32_t kMaxBindlessTextures = 1024;
constexpr uint32_t kModelSetsPerPool = kFramesInFlight;

} // namespace vulkan
} // namespace graphics
