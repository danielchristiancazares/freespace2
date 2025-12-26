#version 450
#extension GL_ARB_separate_shader_objects : enable

// Decal box vertex
layout(location = 0) in vec4 vertPosition;
// Instance transform (mat4 occupies locations 8..11 per VulkanPipelineManager mapping)
layout(location = 8) in mat4 vertModelMatrix;

// Flat outputs: per-instance constants
layout(location = 0) flat out mat4 invModelMatrix;
layout(location = 4) flat out vec3 decalDirection;
layout(location = 5) flat out float normal_angle_cutoff;
layout(location = 6) flat out float angle_fade_start;
layout(location = 7) flat out float alpha_scale;

layout(binding = 0, std140) uniform decalGlobalData {
	mat4 viewMatrix;
	mat4 projMatrix;
	mat4 invViewMatrix;
	mat4 invProjMatrix;

	vec2 viewportSize;
	float pad0;
	float pad1;
} uGlobals;

void main()
{
	normal_angle_cutoff = vertModelMatrix[0][3];
	angle_fade_start = vertModelMatrix[1][3];
	alpha_scale = vertModelMatrix[2][3];

	mat4 modelMatrix = vertModelMatrix;
	modelMatrix[0][3] = 0.0;
	modelMatrix[1][3] = 0.0;
	modelMatrix[2][3] = 0.0;

	invModelMatrix = inverse(modelMatrix);
	decalDirection = mat3(uGlobals.viewMatrix) * modelMatrix[2].xyz;
	gl_Position = uGlobals.projMatrix * uGlobals.viewMatrix * modelMatrix * vertPosition;
}



