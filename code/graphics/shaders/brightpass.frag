#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2D SceneColor;

void main()
{
	vec3 color = texture(SceneColor, fragTexCoord).rgb;
	fragOut0 = vec4(max(vec3(0.0), color - vec3(1.0)), 1.0);
}


