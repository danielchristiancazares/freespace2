#version 450
#extension GL_ARB_separate_shader_objects : enable

// Position-only vertex input
layout (location = 0) in vec4 vertPosition;

// Uniforms - same binding layout as other shaders for compatibility
layout (binding = 0, std140) uniform matrixData {
	mat4 modelViewMatrix;
	mat4 projMatrix;
};

void main()
{
	gl_Position = projMatrix * modelViewMatrix * vertPosition;
}
