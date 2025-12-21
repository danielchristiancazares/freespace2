#pragma once

#include <cstdint>

namespace graphics {
namespace vulkan {

// Sentinel value indicating an absent vertex attribute offset.
// Must match OFFSET_ABSENT in the model.vert shader.
constexpr uint32_t MODEL_OFFSET_ABSENT = 0xFFFFFFFFu;

// Push constant block for model rendering with vertex pulling and bindless textures.
// Layout must exactly match the GLSL declaration in model.vert and model.frag.
// 14 fields x 4 bytes = 56 bytes total.
struct ModelPushConstants {
	// Vertex heap addressing
	uint32_t vertexOffset;      // Byte offset into vertex heap buffer for this draw
	uint32_t stride;            // Byte stride between vertices

	// Vertex layout offsets (byte offsets within a vertex, or MODEL_OFFSET_ABSENT)
	uint32_t posOffset;         // Position (vec3)
	uint32_t normalOffset;      // Normal (vec3)
	uint32_t texCoordOffset;    // Texture coordinate (vec2)
	uint32_t tangentOffset;     // Tangent (vec4)
	uint32_t boneIndicesOffset; // Bone indices (ivec4), or MODEL_OFFSET_ABSENT
	uint32_t boneWeightsOffset; // Bone weights (vec4), or MODEL_OFFSET_ABSENT

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
static_assert(sizeof(ModelPushConstants) == 56, "ModelPushConstants must be 56 bytes to match GLSL layout");

} // namespace vulkan
} // namespace graphics
