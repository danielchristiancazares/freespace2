#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec2 fragPixcoord;
layout(location = 2) in vec4 fragOffset[3];

layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2D edgesTex;
layout(binding = 3) uniform sampler2D areaTex;
layout(binding = 4) uniform sampler2D searchTex;

layout(binding = 1, std140) uniform genericData {
	vec2 smaa_rt_metrics; // {width, height}
	vec2 pad;
} u;

#define SMAA_RT_METRICS vec4(1.0 / u.smaa_rt_metrics, u.smaa_rt_metrics)
#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1

#include "SMAA.sdr"

void main()
{
	fragOut0 = SMAABlendingWeightCalculationPS(fragTexCoord, fragPixcoord, fragOffset, edgesTex, areaTex, searchTex, vec4(0.0));
}


