#version 450
#extension GL_ARB_separate_shader_objects : enable

// Fullscreen pass vertex shader that also provides normalized UVs.
// Uses the same POSITION3 vertex layout as the deferred fullscreen triangle.
layout(location = 0) in vec3 vertPosition;

layout(location = 0) out vec2 fragTexCoord;

void main()
{
	gl_Position = vec4(vertPosition, 1.0);
	// Convert clip-space [-1, +1] -> UV [0, 1]. The fullscreen triangle vertices extend beyond [-1,1],
	// but fragments outside the viewport are clipped; UVs outside [0,1] are harmless with clamp sampling.
	fragTexCoord = vertPosition.xy * 0.5 + vec2(0.5);
}


