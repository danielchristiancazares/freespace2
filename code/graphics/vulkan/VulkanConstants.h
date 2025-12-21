#pragma once

#include <cstdint>

namespace graphics {
namespace vulkan {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxBindlessTextures = 1024;

// Bindless model texture slots:
// - Slot 0 is always valid fallback (black) so bindless sampling never touches destroyed images.
// - Slots 1..3 are well-known defaults so shaders never need "absent texture" sentinel routing.
constexpr uint32_t kBindlessTextureSlotFallback = 0;
constexpr uint32_t kBindlessTextureSlotDefaultBase = 1;
constexpr uint32_t kBindlessTextureSlotDefaultNormal = 2;
constexpr uint32_t kBindlessTextureSlotDefaultSpec = 3;
constexpr uint32_t kBindlessFirstDynamicTextureSlot = 4;
static_assert(kBindlessFirstDynamicTextureSlot < kMaxBindlessTextures,
	"Bindless default slots must fit within the bindless array");

} // namespace vulkan
} // namespace graphics
