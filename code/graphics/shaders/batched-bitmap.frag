#version 450
#extension GL_ARB_separate_shader_objects : enable

#include "gamma.sdr"

// Inputs from vertex shader
layout (location = 0) in vec4 fragTexCoord;
layout (location = 1) in vec4 fragColor;

// Output
layout (location = 0) out vec4 fragOut0;

// Standard pipeline layout - binding 1: generic data
layout (binding = 1, std140) uniform genericData {
	vec4 color;
	float intensity;
};

// Standard pipeline layout - binding 2: texture sampler
layout (binding = 2) uniform sampler2DArray baseMap;

void main()
{
	// Perspective-correct texture coordinate
	float y = fragTexCoord.y / fragTexCoord.w;

	// Sample texture array (Z component = array index)
	vec4 baseColor = texture(baseMap, vec3(fragTexCoord.x, y, fragTexCoord.z));

	// Convert sRGB to linear for correct blending
	baseColor.rgb = srgb_to_linear(baseColor.rgb);
	vec4 blendColor = vec4(srgb_to_linear(fragColor.rgb), fragColor.a);

	// Final color: texture * vertex_color * uniform_color * intensity
	fragOut0 = baseColor * blendColor * intensity;
}
