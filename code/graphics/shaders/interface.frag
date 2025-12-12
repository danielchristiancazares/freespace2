#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

layout (location = 0) in vec2 fragTexCoord;

layout (location = 0) out vec4 fragOut0;

layout (binding = 1, std140) uniform genericData {
	vec4 color;

	int baseMapIndex;
	int alphaTexture;
	int noTexturing;
	int srgb;

	float intensity;
	float alphaThreshold;
};

layout(binding = 2) uniform sampler2DArray baseMap;

void main()
{
	vec4 baseColor = texture(baseMap, vec3(fragTexCoord, float(baseMapIndex)));

	if (alphaThreshold > baseColor.a) discard;

	// Convert texture from sRGB if needed
	baseColor.rgb = (srgb == 1) ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;

	// Uniform color (no vertex color multiplication)
	vec4 blendColor = (srgb == 1) ? vec4(srgb_to_linear(color.rgb), color.a) : color;

	// Mix based on alpha texture mode and no-texturing mode
	fragOut0 = mix(
		mix(baseColor * blendColor, vec4(blendColor.rgb, baseColor.r * blendColor.a), float(alphaTexture)),
		blendColor,
		float(noTexturing)
	) * intensity;
}
