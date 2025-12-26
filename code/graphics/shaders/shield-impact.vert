#version 450
#extension GL_ARB_separate_shader_objects : enable

// Shield mesh vertex attributes (match VulkanPipelineManager location mapping)
layout (location = 0) in vec4 vertPosition;
layout (location = 3) in vec3 vertNormal;

layout (location = 0) out vec4 fragImpactUV;
layout (location = 1) out float fragNormOffset;

layout (binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

layout (binding = 1, std140) uniform genericData {
	mat4 shieldModelViewMatrix;
	mat4 shieldProjMatrix;

	vec3 hitNormal;
	int srgb;

	vec4 color;

	int shieldMapIndex;
};

void main()
{
	gl_Position = projMatrix * modelViewMatrix * vertPosition;

	// Match legacy shield-impact-v.sdr behavior: dot in the shield mesh's local space.
	fragNormOffset = dot(hitNormal, vertNormal);

	fragImpactUV = shieldProjMatrix * shieldModelViewMatrix * vertPosition;
	fragImpactUV += 1.0f;
	fragImpactUV *= 0.5f;
}


