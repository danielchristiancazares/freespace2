#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

// Scene depth and cockpit depth masks (OpenGL parity).
layout(binding = 2) uniform sampler2D scene;
layout(binding = 3) uniform sampler2D cockpit;

layout(binding = 1, std140) uniform genericData {
	vec2 sun_pos;
	float density;
	float weight;

	float falloff;
	float intensity;
	float cp_intensity;
	int samplenum;
} u;

void main()
{
	// Fast path: avoid doing any work if the effect is effectively off.
	if (u.intensity <= 0.0001 && u.cp_intensity <= 0.0001) {
		fragOut0 = vec4(0.0);
		return;
	}

	// Cockpit depth acts as a mask: pixels with cockpit geometry should bloom with cp_intensity.
	float mask = texture(cockpit, fragTexCoord).r;
	if (mask < 1.0) {
		fragOut0 = vec4(u.cp_intensity);
		return;
	}

	// Clamp to avoid pathological GPU stalls if a mod sets a very large sample count.
	const int samples = clamp(u.samplenum, 1, 128);
	vec2 stepv = fragTexCoord - u.sun_pos;
	stepv *= (1.0 / float(samples)) * u.density;

	vec2 pos = fragTexCoord;
	float decay = 1.0;
	float sum = 0.0;
	for (int i = 0; i < samples; ++i) {
		pos -= stepv;
		float s = texture(scene, pos).r;
		if (s == 1.0) {
			sum += decay * u.weight;
		}
		decay *= u.falloff;
	}

	float outv = sum * u.intensity;
	fragOut0 = vec4(outv, outv, outv, 1.0);
}


