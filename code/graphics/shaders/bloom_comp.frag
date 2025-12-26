#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2D bloomed;

layout(binding = 1, std140) uniform genericData {
	float bloom_intensity;
	int levels;
	float pad0;
	float pad1;
} u;

void main()
{
	vec3 color_out = vec3(0.0);
	float factor = 0.0;

	const int mipCount = max(u.levels, 0);
	for (int mipmap = 0; mipmap < mipCount; ++mipmap) {
		float scale = 1.0 / exp2(float(mipmap));
		factor += scale;
		color_out += textureLod(bloomed, fragTexCoord, float(mipmap)).rgb * scale;
	}

	if (factor > 0.0) {
		color_out /= factor;
	}
	color_out *= u.bloom_intensity;

	fragOut0 = vec4(color_out, 1.0);
}


