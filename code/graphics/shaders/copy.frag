#version 450
#extension GL_ARB_separate_shader_objects : enable

// Standard per-draw push-descriptor layout uses binding=2 for a combined image sampler.
layout(binding = 2) uniform sampler2D SceneColor;

layout(location = 0) out vec4 fragOut0;

void main()
{
	// Pixel-perfect copy: map fragment coordinates directly to texels.
	ivec2 coord = ivec2(gl_FragCoord.xy);
	fragOut0 = texelFetch(SceneColor, coord, 0);
}

