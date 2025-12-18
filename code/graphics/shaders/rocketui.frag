#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in vec2 fragScreenPosition;

layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2DArray baseMap;

layout(binding = 1, std140) uniform genericData {
	mat4 projMatrix;

	vec2 offset;
	int textured;
	int baseMapIndex;

	float horizontalSwipeOffset;
};

void main()
{
	if (fragScreenPosition.x > horizontalSwipeOffset) {
		discard;
	}

	float distance = horizontalSwipeOffset - fragScreenPosition.x;

	vec4 color;
	if (textured != 0) {
		color = texture(baseMap, vec3(fragTexCoord, float(baseMapIndex))) * fragColor;
	} else {
		color = fragColor;
	}

	// Hard-coded for now but can be easily made configurable should that be needed at some point
	if (distance < 10.0) {
		// Only change the colors but not the alpha channel to preserve the transparent part of text
		color.xyz = vec3(1.0);
	}

	fragOut0 = color;
}

