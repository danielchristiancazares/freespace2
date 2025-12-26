#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2D tex;

layout(binding = 1, std140) uniform genericData {
	float texSize;
	int level;
	float pad0;
	float pad1;
} u;

// Gaussian blur: horizontal pass
void main()
{
	// Weights match the legacy shader (blur-f.sdr)
	float w[6];
	w[5] = 0.0402;
	w[4] = 0.0623;
	w[3] = 0.0877;
	w[2] = 0.1120;
	w[1] = 0.1297;
	w[0] = 0.1362;

	vec4 sum = textureLod(tex, fragTexCoord, float(u.level)) * w[0];
	for (int i = 1; i < 6; ++i) {
		const float o = float(i) * u.texSize;
		sum += textureLod(tex, vec2(clamp(fragTexCoord.x - o, 0.0, 1.0), fragTexCoord.y), float(u.level)) * w[i];
		sum += textureLod(tex, vec2(clamp(fragTexCoord.x + o, 0.0, 1.0), fragTexCoord.y), float(u.level)) * w[i];
	}

	fragOut0 = sum;
}


