#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

layout(binding = 2) uniform sampler2D tex;

void main()
{
	vec4 color = texture(tex, fragTexCoord);
	fragOut0 = vec4(color.rgb, dot(color.rgb, vec3(0.299, 0.587, 0.114)));
}


