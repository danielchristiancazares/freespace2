#pragma once

namespace graphics {
namespace vulkan {

// Strong typedef for texture identity in the Vulkan backend.
//
// The value is a bmpman "base frame" handle (or a synthetic handle such as the fallback/default
// textures). This keeps APIs explicit about what kind of texture identifier they expect.
struct TextureId {
	int value = -1;

	constexpr TextureId() = default;
	explicit constexpr TextureId(int baseFrame) : value(baseFrame) {}

	constexpr bool isValid() const { return value != -1; }
};

} // namespace vulkan
} // namespace graphics

