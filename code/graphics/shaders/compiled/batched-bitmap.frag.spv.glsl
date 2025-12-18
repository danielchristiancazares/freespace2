#version 330
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(binding = 1, std140) uniform genericData
{
    vec4 color;
    float intensity;
} _80;

layout(binding = 2) uniform sampler2DArray baseMap;

in vec4 fragTexCoord;
in vec4 fragColor;
layout(location = 0) out vec4 fragOut0;

void main()
{
    vec4 _50 = texture(baseMap, vec3(fragTexCoord.x, fragTexCoord.y / fragTexCoord.w, fragTexCoord.z));
    vec3 _91 = pow(_50.xyz, vec3(2.2000000476837158203125));
    vec4 _97 = _50;
    _97.x = _91.x;
    _97.y = _91.y;
    _97.z = _91.z;
    fragOut0 = (_97 * vec4(pow(fragColor.xyz, vec3(2.2000000476837158203125)), fragColor.w)) * _80.intensity;
}

