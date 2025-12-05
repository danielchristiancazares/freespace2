#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout (location = 0) in vec4 fragTexCoord;
layout (location = 1) in vec4 fragColor;

layout (location = 0) out vec4 fragOut0;

#ifdef VULKAN
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(set = set_id, binding = binding_id)
#else
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(binding = binding_id)
#endif

layout(std140) LAYOUT_SET_BINDING(0, 8) uniform genericData {
	mat4 modelMatrix;

	vec4 color;

	vec4 clipEquation;

	int baseMapIndex;
	int alphaTexture;
	int noTexturing;
	int srgb;

	float intensity;
	float alphaThreshold;
	bool clipEnabled;
};

LAYOUT_SET_BINDING(1, 0) uniform sampler2DArray baseMap;

void main()
{
	vec4 baseColor = texture(baseMap, vec3(fragTexCoord.xy, float(baseMapIndex)));
	if(alphaThreshold > baseColor.a) discard;
	baseColor.rgb = (srgb == 1) ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;
	vec4 blendColor = (srgb == 1) ? vec4(srgb_to_linear(fragColor.rgb), fragColor.a) : fragColor;
	fragOut0 = mix(mix(baseColor * blendColor, vec4(blendColor.rgb, baseColor.r * blendColor.a), float(alphaTexture)), blendColor, float(noTexturing)) * intensity;
}
