#pragma once

#include <cstdint>

namespace graphics {
namespace vulkan {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxBindlessTextures = 1024;
constexpr uint32_t kModelSetsPerPool = 4096 * kFramesInFlight;

} // namespace vulkan
} // namespace graphics
