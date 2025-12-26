#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2D tex;
layout(binding = 3) uniform sampler2D depth_tex; // reserved for custom effects; not used by the built-in chain

layout(binding = 1, std140) uniform genericData {
	float timer;
	float noise_amount;
	float saturation;
	float brightness;

	float contrast;
	float film_grain;
	float tv_stripes;
	float cutoff;

	vec3 tint;
	float dither;

	vec3 custom_effect_vec3_a;
	float custom_effect_float_a;

	vec3 custom_effect_vec3_b;
	float custom_effect_float_b;
} u;

float luminance(vec3 color)
{
	// Matches legacy `post-f.sdr` coefficients (note: B term is 0.184 there).
	return dot(color, vec3(0.299, 0.587, 0.184));
}

void main()
{
	// Distort noise (runtime gated by noise_amount)
	vec2 distort = vec2(0.0);
	if (u.noise_amount > 0.0) {
		float distort_factor = u.timer * sin(fragTexCoord.x * fragTexCoord.y * 100.0 + u.timer);
		distort_factor = mod(distort_factor, 8.0) * mod(distort_factor, 4.0);
		distort = vec2(mod(distort_factor, u.noise_amount), mod(distort_factor, u.noise_amount + 0.002));
	}

	vec4 color_in = texture(tex, fragTexCoord + distort);
	vec4 color_out = color_in;

	// Saturation: saturation=1 => identity
	vec4 color_grayscale = color_in;
	color_grayscale.rgb = vec3(luminance(color_in.rgb));
	color_out = mix(color_in, color_grayscale, 1.0 - u.saturation);

	// Brightness: brightness=1 => identity
	color_out.rgb *= vec3(u.brightness);

	// Contrast: contrast=1 => identity
	color_out.rgb += vec3(0.5 - 0.5 * u.contrast);

	// Film grain
	if (u.film_grain > 0.0) {
		float x = fragTexCoord.x * fragTexCoord.y * u.timer * 1000.0;
		x = mod(x, 13.0) * mod(x, 123.0);
		float dx = mod(x, 0.01);
		vec3 result = color_out.rgb + color_out.rgb * clamp(0.1 + dx * 100.0, 0.0, 1.0);
		color_out.rgb = mix(color_out.rgb, result, u.film_grain);
	}

	// TV stripes
	if (u.tv_stripes > 0.0) {
		vec2 sc;
		sc.x = sin(fragTexCoord.y * 2048.0);
		sc.y = cos(fragTexCoord.y * 2048.0);
		vec3 stripes = color_out.rgb + color_out.rgb * vec3(sc.x, sc.y, sc.x) * 0.8;
		color_out.rgb = mix(color_out.rgb, stripes, u.tv_stripes);
	}

	// Cutoff
	if (u.cutoff > 0.0) {
		vec4 color_greyscale = color_in;
		color_greyscale.rgb = vec3(luminance(color_in.rgb));
		vec4 normalized_col;
		float col_length = length(color_out.rgb);
		if (col_length > 1.0) {
			normalized_col = (color_out / col_length);
		} else {
			normalized_col = color_out;
		}

		vec3 unit_grey = vec3(0.5773);
		float sat = dot(normalized_col.rgb, unit_grey);
		color_out = mix(color_greyscale, color_out, sat * u.cutoff);
	}

	// Dither (runtime gated)
	if (u.dither > 0.0) {
		float downsampling_factor = 4.0;
		float bias = 0.5;
		color_out.rgb = floor(color_out.rgb * downsampling_factor + bias) / downsampling_factor;
	}

	// Tint: tint defaults to (0,0,0)
	color_out.rgb += u.tint;

	color_out.a = 1.0;
	fragOut0 = color_out;
}


