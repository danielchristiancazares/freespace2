#pragma once

#include <cstdint>

namespace graphics {
namespace vulkan {

// Vertex attribute presence bits for model vertex pulling.
// Must match the bit definitions in code/graphics/shaders/model.vert.
constexpr uint32_t MODEL_ATTRIB_POS = 1u << 0;
constexpr uint32_t MODEL_ATTRIB_NORMAL = 1u << 1;
constexpr uint32_t MODEL_ATTRIB_TEXCOORD = 1u << 2;
constexpr uint32_t MODEL_ATTRIB_TANGENT = 1u << 3;
constexpr uint32_t MODEL_ATTRIB_BONEINDICES = 1u << 4;
constexpr uint32_t MODEL_ATTRIB_BONEWEIGHTS = 1u << 5;
constexpr uint32_t MODEL_ATTRIB_MODEL_ID = 1u << 6;

// Push constant block for model rendering with vertex pulling and bindless textures.
// Layout must exactly match the GLSL declaration in model.vert and model.frag.
// 16 fields x 4 bytes = 64 bytes total.
struct ModelPushConstants {
  // Vertex heap addressing
  uint32_t vertexOffset; // Byte offset into vertex heap buffer for this draw
  uint32_t stride;       // Byte stride between vertices

  // Vertex attribute presence mask (MODEL_ATTRIB_* bits)
  uint32_t vertexAttribMask;

  // Vertex layout offsets (byte offsets within a vertex; ignored if not present in vertexAttribMask)
  uint32_t posOffset;         // Position (vec3)
  uint32_t normalOffset;      // Normal (vec3)
  uint32_t texCoordOffset;    // Texture coordinate (vec2)
  uint32_t tangentOffset;     // Tangent (vec4)
  uint32_t modelIdOffset;     // Model id (float; used for batched transforms)
  uint32_t boneIndicesOffset; // Bone indices (ivec4)
  uint32_t boneWeightsOffset; // Bone weights (vec4)

  // Material texture indices (into bindless texture array). Always valid.
  uint32_t baseMapIndex;
  uint32_t glowMapIndex;
  uint32_t normalMapIndex;
  uint32_t specMapIndex;

  // Instancing (reserved for future use)
  uint32_t matrixIndex;

  // Shader variant flags
  uint32_t flags;
};
static_assert(sizeof(ModelPushConstants) == 64, "ModelPushConstants must be 64 bytes to match GLSL layout");

} // namespace vulkan
} // namespace graphics
