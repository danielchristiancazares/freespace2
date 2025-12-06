#version 460 core
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(location = 0) in vec3 vPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in vec4 vTangent;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outPosition;
layout(location = 3) out vec4 outSpecular;
layout(location = 4) out vec4 outEmissive;

const uint OFFSET_ABSENT = 0xFFFFFFFFu;

layout(push_constant) uniform ModelPushConstants
{
	uint vertexOffset;        // unused in fragment stage
	uint stride;              // unused in fragment stage
	uint posOffset;
	uint normalOffset;
	uint texCoordOffset;
	uint tangentOffset;
	uint boneIndicesOffset;
	uint boneWeightsOffset;
	uint baseMapIndex;
	uint glowMapIndex;
	uint normalMapIndex;
	uint specMapIndex;
	uint matrixIndex;
	uint flags;
} pcs;

bool hasTexture(uint index)
{
	return index != OFFSET_ABSENT;
}

vec4 sampleTexture(uint index, vec2 uv, vec4 fallback)
{
	if (!hasTexture(index)) {
		return fallback;
	}
	// nonuniformEXT ensures proper descriptor indexing semantics
	return texture(textures[nonuniformEXT(index)], uv);
}

void main()
{
	// Base color
	vec4 baseColor = sampleTexture(pcs.baseMapIndex, vTexCoord, vec4(1.0));

	// Normals: fallback to interpolated normal; if a normal map exists, sample and remap
	vec3 shadingNormal = normalize(vNormal);
	if (hasTexture(pcs.normalMapIndex)) {
		vec3 nmap = texture(textures[nonuniformEXT(pcs.normalMapIndex)], vTexCoord).xyz * 2.0 - 1.0;
		shadingNormal = normalize(nmap);
	}

	// Specular/roughness map placeholder
	vec4 specSample = sampleTexture(pcs.specMapIndex, vTexCoord, vec4(0.0));

	// Emissive/glow
	vec4 emissive = sampleTexture(pcs.glowMapIndex, vTexCoord, vec4(0.0));

	outColor = baseColor;
	outNormal = vec4(shadingNormal, 1.0);
	outPosition = vec4(vPosition, 1.0);
	outSpecular = specSample;
	outEmissive = emissive;
}
