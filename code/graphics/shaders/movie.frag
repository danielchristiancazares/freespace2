#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

// YCbCr conversion is performed by the sampler.
layout(binding = 0) uniform sampler2D movieTex;

layout(push_constant) uniform MoviePushConstants {
	layout(offset = 0) vec2 screenSize;
	layout(offset = 8) vec2 rectMin;
	layout(offset = 16) vec2 rectMax;
	layout(offset = 24) float alpha;
	layout(offset = 28) float _pad;
} pc;

void main()
{
	vec3 rgb = texture(movieTex, fragTexCoord).rgb;

	// Keep blending consistent with other sRGB sources.
	rgb = srgb_to_linear(rgb);

	outColor = vec4(rgb, pc.alpha);
}

