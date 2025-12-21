#pragma once

#include <optional>

namespace graphics {
namespace vulkan {

// Strong typedef for texture identity in the Vulkan backend.
//
// The value is a bmpman "base frame" handle (or a synthetic handle such as the fallback/default
// textures). This keeps APIs explicit about what kind of texture identifier they expect.
class TextureId {
  public:
	// Boundary constructor: converts a validated base-frame integer into a TextureId.
	[[nodiscard]] static std::optional<TextureId> tryFromBaseFrame(int baseFrame)
	{
		if (baseFrame < 0) {
			return std::nullopt;
		}
		return TextureId(baseFrame);
	}

	int baseFrame() const { return m_baseFrame; }

	bool operator==(const TextureId& other) const { return m_baseFrame == other.m_baseFrame; }
	bool operator!=(const TextureId& other) const { return !(*this == other); }

  private:
	explicit TextureId(int baseFrame) : m_baseFrame(baseFrame) {}
	int m_baseFrame = 0;
};

} // namespace vulkan
} // namespace graphics
