#version 460 core
#extension GL_EXT_nonuniform_qualifier : require

#include "gamma.sdr"

// The Vulkan backend uploads textures as 2D arrays (layer 0 for non-array textures).
layout(set = 0, binding = 1) uniform sampler2DArray textures[];

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in vec4 vTangent;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPosition;
layout(location = 3) out vec4 outSpecular;
layout(location = 4) out vec4 outEmissive;

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
	// Base color
	// Slot 0 is fallback; slots 1..3 are well-known defaults, so indices are always valid.
	vec4 baseColor = texture(textures[nonuniformEXT(pcs.baseMapIndex)], vec3(vTexCoord, 0.0));
	baseColor.rgb = srgb_to_linear(baseColor.rgb);

	// Normal mapping: normal map is tangent-space; convert to view-space using the per-vertex TBN.
	vec3 N = vNormal;
	float nLenSq = dot(N, N);
	N = (nLenSq > 0.0) ? (N * inversesqrt(nLenSq)) : vec3(0.0, 0.0, 1.0);

	vec3 T = vTangent.xyz;
	float tLenSq = dot(T, T);
	T = (tLenSq > 0.0) ? (T * inversesqrt(tLenSq)) : vec3(1.0, 0.0, 0.0);
	T = T - N * dot(T, N); // Gram-Schmidt orthonormalization
	float orthoLenSq = dot(T, T);
	T = (orthoLenSq > 0.0) ? (T * inversesqrt(orthoLenSq)) : vec3(1.0, 0.0, 0.0);

	float handedness = (vTangent.w < 0.0) ? -1.0 : 1.0;
	vec3 B = cross(N, T) * handedness;
	mat3 TBN = mat3(T, B, N);

	// Normal maps are typically stored as DXT5nm (X in A, Y in G).
	vec4 nSample = texture(textures[nonuniformEXT(pcs.normalMapIndex)], vec3(vTexCoord, 0.0));
	vec2 nXY = nSample.ag * 2.0 - 1.0;
	vec3 nmap = vec3(nXY, clamp(sqrt(max(0.0, 1.0 - dot(nXY, nXY))), 0.0001, 1.0));
	vec3 shadingNormal = TBN * nmap;
	float sLenSq = dot(shadingNormal, shadingNormal);
	shadingNormal = (sLenSq > 0.0) ? (shadingNormal * inversesqrt(sLenSq)) : N;

	// Specular
	vec4 specSample = texture(textures[nonuniformEXT(pcs.specMapIndex)], vec3(vTexCoord, 0.0));

	// Emissive/glow
	vec4 emissive = texture(textures[nonuniformEXT(pcs.glowMapIndex)], vec3(vTexCoord, 0.0));
	emissive.rgb = srgb_to_linear(emissive.rgb);

	outColor = baseColor;
	outNormal = vec4(shadingNormal, 1.0);
	outPosition = vec4(vPosition, 1.0);
	outSpecular = specSample;
	outEmissive = emissive;
}
