#version 450
#extension GL_ARB_separate_shader_objects : enable

layout (location = 0) in vec4 vertPosition;
layout (location = 1) in vec4 vertColor;
layout (location = 2) in vec4 vertTexCoord;

layout (location = 0) out vec4 fragTexCoord;
layout (location = 1) out vec4 fragColor;

#ifdef VULKAN
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(set = set_id, binding = binding_id)
#else
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(binding = binding_id)
#endif

layout(std140) LAYOUT_SET_BINDING(0, 6) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

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

void main()
{
	fragTexCoord = vertTexCoord;
	fragColor = vertColor * color;
	gl_Position = projMatrix * modelViewMatrix * vertPosition;

	if (clipEnabled) {
		gl_ClipDistance[0] = dot(clipEquation, modelMatrix * vertPosition);
	}
}
