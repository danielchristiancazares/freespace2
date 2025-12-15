#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "lighting.glsl"

// Light volume / fullscreen quad vertex
// Location 0 matches VulkanPipelineManager vertex mapping (POSITION)
layout(location = 0) in vec4 vertPosition;

// Per-draw UBOs (set=0, push descriptors)
layout(binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

layout(binding = 1, std140) uniform lightData {
	vec3 diffuseLightColor;
	float coneAngle;

	vec3 lightDir;
	float coneInnerAngle;

	vec3 coneDir;
	uint dualCone;

	vec3 scale;
	float lightRadius;

	int lightType;
	uint enable_shadows;

	float sourceRadius;
};

void main()
{
	// Full-frame lights render as a fullscreen quad in clip space
	if (lightType == LT_DIRECTIONAL || lightType == LT_AMBIENT) {
		gl_Position = vec4(vertPosition.xyz, 1.0);
	} else {
		// Volume lights render scaled geometry in view space
		gl_Position = projMatrix * modelViewMatrix * vec4(vertPosition.xyz * scale, 1.0);
	}
}
