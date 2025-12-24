#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout (location = 0) out vec4 fragOut0;

layout (binding = 1, std140) uniform genericData {
	vec4 color;
	int srgb;
	float intensity;
};

void main()
{
	vec4 outColor = (srgb == 1) ? vec4(srgb_to_linear(color.rgb), color.a) : color;
	fragOut0 = outColor * intensity;
}
