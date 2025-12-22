#version 450
#extension GL_ARB_separate_shader_objects : enable

// Fullscreen pass vertex shader.
// Uses the same POSITION3 vertex layout as the deferred fullscreen triangle.
layout(location = 0) in vec3 vertPosition;

void main()
{
	gl_Position = vec4(vertPosition, 1.0);
}

