#version 450
#extension GL_ARB_separate_shader_objects : enable

// Vertex attributes - no color, just position and texcoord
layout (location = 0) in vec4 vertPosition;
layout (location = 2) in vec4 vertTexCoord;

// Output to fragment shader
layout (location = 0) out vec2 fragTexCoord;

// Uniforms - same binding layout as default-material for compatibility
layout (binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

void main()
{
	fragTexCoord = vertTexCoord.xy;
	gl_Position = projMatrix * modelViewMatrix * vertPosition;
}
