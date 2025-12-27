#pragma once

#include <cstddef>
#include <functional>
#include <optional>

#include "globalincs/pstypes.h"

namespace graphics {
namespace vulkan {

class VulkanTextureBindings;
class VulkanTextureManager;

// Strong typedef for texture identity in the Vulkan backend.
//
// The value is a bmpman "base frame" handle (>= 0). Builtin fallback/default textures are
// not represented as fake/synthetic handles; they have explicit descriptor APIs on the
// Vulkan texture manager.
class TextureId {
public:
  // Boundary constructor: converts a validated base-frame integer into a TextureId.
  [[nodiscard]] static std::optional<TextureId> tryFromBaseFrame(int baseFrame) {
    if (baseFrame < 0) {
      return std::nullopt;
    }
    return TextureId(baseFrame);
  }

  int baseFrame() const { return m_baseFrame; }

  bool operator==(const TextureId &other) const { return m_baseFrame == other.m_baseFrame; }
  bool operator!=(const TextureId &other) const { return !(*this == other); }

private:
  // Internal constructor: only for code paths that already proved baseFrame >= 0 by construction
  // (e.g., container membership / validated inputs). Avoids reintroducing deep optionals.
  [[nodiscard]] static TextureId fromBaseFrameUnchecked(int baseFrame) {
    Assertion(baseFrame >= 0, "TextureId::fromBaseFrameUnchecked called with invalid base frame %d", baseFrame);
    return TextureId(baseFrame);
  }

  explicit TextureId(int baseFrame) : m_baseFrame(baseFrame) {}
  int m_baseFrame = 0;

  friend class VulkanTextureManager;
  friend class VulkanTextureBindings;
};

struct TextureIdHasher {
  size_t operator()(const TextureId &id) const noexcept { return std::hash<int>{}(id.baseFrame()); }
};

} // namespace vulkan
} // namespace graphics
