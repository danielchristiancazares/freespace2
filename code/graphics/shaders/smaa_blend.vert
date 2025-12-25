#version 450
#extension GL_ARB_separate_shader_objects : enable

// SMAA Blending Weight Calculation vertex shader (Vulkan).

layout(location = 0) in vec3 vertPosition;

layout(binding = 1, std140) uniform genericData {
	vec2 smaa_rt_metrics; // {width, height}
	vec2 pad;
} u;

#define SMAA_RT_METRICS vec4(1.0 / u.smaa_rt_metrics, u.smaa_rt_metrics)
#define SMAA_GLSL_4
#define SMAA_PRESET_HIGH
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0

#include "SMAA.sdr"

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec2 fragPixcoord;
layout(location = 2) out vec4 fragOffset[3];

void main()
{
	gl_Position = vec4(vertPosition, 1.0);
	vec2 uv = vertPosition.xy * 0.5 + vec2(0.5);
	fragTexCoord = uv;
	SMAABlendingWeightCalculationVS(uv, fragPixcoord, fragOffset);
}


