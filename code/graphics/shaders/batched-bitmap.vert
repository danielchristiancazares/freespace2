#version 450
#extension GL_ARB_separate_shader_objects : enable

// Vertex attributes (locations match VulkanPipelineManager mappings)
layout (location = 0) in vec4 vertPosition;
layout (location = 1) in vec4 vertColor;
layout (location = 2) in vec4 vertTexCoord;

// Outputs to fragment shader
layout (location = 0) out vec4 fragTexCoord;
layout (location = 1) out vec4 fragColor;

// Standard pipeline layout - binding 0: matrices
layout (binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

// Standard pipeline layout - binding 1: generic data
layout (binding = 1, std140) uniform genericData {
	vec4 color;
	float intensity;
};

void main()
{
	// Modulate vertex color with uniform color
	fragColor = vertColor * color;

	// Transform position (MVP)
	gl_Position = projMatrix * modelViewMatrix * vertPosition;

	// Pass texture coords (Z = array index)
	fragTexCoord = vertTexCoord;
}
