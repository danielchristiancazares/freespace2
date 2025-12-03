#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout (location = 0) in vec4 fragTexCoord;
layout (location = 1) in vec4 fragColor;

layout (location = 0) out vec4 fragOut0;

// Set 0: Uniform buffers (bindings match uniform_block_type enum)
// GenericData = 8
#ifdef GL_KHR_vulkan_glsl
#extension GL_KHR_vulkan_glsl : enable
#define LAYOUT_STD140_SET_BINDING(set_id, binding_id) layout(set = set_id, binding = binding_id, std140)
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(set = set_id, binding = binding_id)
#else
#define LAYOUT_STD140_SET_BINDING(set_id, binding_id) layout(std140, binding = binding_id)
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(binding = binding_id)
#endif

LAYOUT_STD140_SET_BINDING(0, 8) uniform genericData {
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

// Set 1: Material textures
// Using sampler2D for now (not sampler2DArray) until texture array support is added
LAYOUT_SET_BINDING(1, 0) uniform sampler2D baseMap;

void main()
{
	// Note: baseMapIndex is ignored for now - texture arrays not yet implemented
	vec4 baseColor = texture(baseMap, fragTexCoord.xy);
	if(alphaThreshold > baseColor.a) discard;
	baseColor.rgb = (srgb == 1) ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;
	vec4 blendColor = (srgb == 1) ? vec4(srgb_to_linear(fragColor.rgb), fragColor.a) : fragColor;
	fragOut0 = mix(mix(baseColor * blendColor, vec4(blendColor.rgb, baseColor.r * blendColor.a), float(alphaTexture)), blendColor, float(noTexturing)) * intensity;
}
