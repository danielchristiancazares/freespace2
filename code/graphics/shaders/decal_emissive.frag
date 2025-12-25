#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec4 fragOut0;

#include "decal_common.glsl"

void main()
{
	vec4 diffuse_out;
	vec4 emissive_out;
	vec3 normal_out;

	ivec2 coord = ivec2(gl_FragCoord.xy);
	computeDecalOutputs(coord, diffuse_out, emissive_out, normal_out);

	fragOut0 = emissive_out;
}



