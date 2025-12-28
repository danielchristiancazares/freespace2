#version 460 core
#extension GL_EXT_nonuniform_qualifier : require

#include "gamma.sdr"

// The Vulkan backend uploads textures as 2D arrays (layer 0 for non-array textures).
layout(set = 0, binding = 1) uniform sampler2DArray textures[];

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in vec4 vTangent;

// Forward / single-attachment path: only write location 0 so validation doesn't warn about unused MRT outputs.
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ModelPushConstants
{
	uint vertexOffset;        // unused in fragment stage
	uint stride;              // unused in fragment stage
	uint vertexAttribMask;    // unused in fragment stage (layout must match vertex stage)
	uint posOffset;
	uint normalOffset;
	uint texCoordOffset;
	uint tangentOffset;
	uint modelIdOffset;
	uint boneIndicesOffset;
	uint boneWeightsOffset;
	uint baseMapIndex;
	uint glowMapIndex;
	uint normalMapIndex;
	uint specMapIndex;
	uint matrixIndex;
	uint flags;
} pcs;

void main()
{
	// Base color only (lighting is handled by deferred or other passes; this matches the current Vulkan model.frag outColor).
	vec4 baseColor = texture(textures[nonuniformEXT(pcs.baseMapIndex)], vec3(vTexCoord, 0.0));
	baseColor.rgb = srgb_to_linear(baseColor.rgb);
	outColor = baseColor;
}



