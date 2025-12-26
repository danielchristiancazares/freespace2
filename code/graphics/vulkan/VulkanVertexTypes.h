#pragma once

#include <cstddef>

namespace graphics {
namespace vulkan {

struct DefaultMaterialVertex {
  float position[4];
  float color[4];
  float texcoord[4];
};

static_assert(offsetof(DefaultMaterialVertex, position) == 0, "position offset must be 0");
static_assert(offsetof(DefaultMaterialVertex, color) == 16, "color offset must be 16");
static_assert(offsetof(DefaultMaterialVertex, texcoord) == 32, "texcoord offset must be 32");
static_assert(sizeof(DefaultMaterialVertex) == 48, "DefaultMaterialVertex size must be 48 bytes");

} // namespace vulkan
} // namespace graphics
