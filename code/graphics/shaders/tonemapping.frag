#version 450
#extension GL_ARB_separate_shader_objects : enable

// Tonemapping pass for Vulkan.
// Samples the HDR scene color and outputs LDR *linear* color. The swapchain uses an sRGB format,
// so the hardware will perform linear->sRGB conversion on store.

layout(binding = 2) uniform sampler2D SceneColor;

layout(binding = 1, std140) uniform genericData {
	float exposure;
	int tonemapper;
	float x0; // PPC intermediates
	float y0;

	float x1;
	float toe_B;
	float toe_lnA;
	float sh_B;

	float sh_lnA;
	float sh_offsetX;
	float sh_offsetY;
	float pad0;
} u;

layout(location = 0) out vec4 fragOut0;

vec3 Uncharted2ToneMapping(vec3 hdr_color)
{
	const float A = 0.15;
	const float B = 0.50;
	const float C = 0.10;
	const float D = 0.20;
	const float E = 0.02;
	const float F = 0.30;
	const float W = 11.2;

	hdr_color = ((hdr_color * (A * hdr_color + C * B) + D * E) / (hdr_color * (A * hdr_color + B) + D * F)) - E / F;
	const float white = ((W * (A * W + C * B) + D * E) / (W * (A * W + B) + D * F)) - E / F;
	hdr_color /= white;
	return hdr_color;
}

vec3 rtt_and_odt_fit(vec3 v)
{
	vec3 a = v * (v + 0.0245786f) - 0.000090537f;
	vec3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
	return a / b;
}

const mat3 aces_input = mat3(
	vec3(0.59719, 0.07600, 0.02840),
	vec3(0.35458, 0.90834, 0.13383),
	vec3(0.04823, 0.01566, 0.83777)
);

const mat3 aces_output = mat3(
	vec3( 1.60475, -0.10208, -0.00327),
	vec3(-0.53108,  1.10813, -0.07276),
	vec3(-0.07367, -0.00605,  1.07602)
);

vec3 aces(vec3 hdr_color)
{
	hdr_color = aces_input * hdr_color;
	hdr_color = rtt_and_odt_fit(hdr_color);
	hdr_color = aces_output * hdr_color;
	return hdr_color;
}

vec3 aces_approx(vec3 hdr_color)
{
	const float a = 2.51f;
	const float b = 0.03f;
	const float c = 2.43f;
	const float d = 0.59f;
	const float e = 0.14f;
	hdr_color *= 0.6f;
	return clamp((hdr_color * (a * hdr_color + b)) / (hdr_color * (c * hdr_color + d) + e), 0.0f, 1.0f);
}

vec3 OptimizedCineonToneMapping(vec3 color)
{
	// Optimized filmic operator by Jim Hejl and Richard Burgess-Dawson
	color = max(vec3(0.0), color - 0.004);
	color = (color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06);
	return color;
}

vec3 rhj_lumaToneMapping(vec3 hdr_color)
{
	const float luma = dot(hdr_color, vec3(0.2126, 0.7152, 0.0722));
	const float toneMappedLuma = luma / (1.0 + luma);
	// Avoid division by zero on black
	return (luma > 0.0) ? (hdr_color * (toneMappedLuma / luma)) : hdr_color;
}

vec3 reinhard_extended(vec3 hdr_color)
{
	const float max_white = 1.0;
	const vec3 numerator = hdr_color * (1.0f + (hdr_color / vec3(max_white * max_white)));
	return numerator / (1.0f + hdr_color);
}

vec3 linearToneMapping(vec3 hdr_color)
{
	return clamp(hdr_color, 0.0, 1.0);
}

float toe(float x) {
	return exp(u.toe_lnA + u.toe_B * log(x));
}

float linear_seg(float x) {
	// Slope is 1 by definition
	return u.y0 + (x - u.x0);
}

float shoulder(float x) {
	// Scale is -1 so reverse subtraction to save a mult
	float t = u.sh_offsetX - x;
	t = exp(u.sh_lnA + u.sh_B * log(t));
	t = u.sh_offsetY - t;
	return t;
}

float funswitch(float x) {
	if (x <= u.x0) {
		return toe(x);
	}
	if (x <= u.x1) {
		return linear_seg(x);
	}
	if (x < u.sh_offsetX) {
		return shoulder(x);
	}
	return u.sh_offsetY;
}

vec3 PPC_RGB(vec3 color)
{
	return vec3(
		funswitch(color.r),
		funswitch(color.g),
		funswitch(color.b)
	);
}

vec3 PPC(vec3 color)
{
	const float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
	const float luma_tone = funswitch(luma);
	return (luma > 0.0) ? (color * (luma_tone / luma)) : color;
}

void main()
{
	// Pixel-perfect sampling: map fragment coordinates directly to texels.
	ivec2 coord = ivec2(gl_FragCoord.xy);
	vec3 color = texelFetch(SceneColor, coord, 0).rgb;

	color *= u.exposure;

	switch (u.tonemapper) {
		case 0: color = linearToneMapping(color); break;
		case 1: color = Uncharted2ToneMapping(color); break;
		case 2: color = aces(color); break;
		case 3: color = aces_approx(color); break;
		case 4: color = OptimizedCineonToneMapping(color); break;
		case 5: color = rhj_lumaToneMapping(color); break;
		case 6: color = reinhard_extended(color); break;
		case 7: color = PPC(color); break;
		case 8: color = PPC_RGB(color); break;
		default: break;
	}

	fragOut0 = vec4(color, 1.0);
}


