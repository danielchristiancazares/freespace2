#version 330
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(binding = 1, std140) uniform genericData
{
    vec4 color;
    int baseMapIndex;
    int alphaTexture;
    int noTexturing;
    int srgb;
    float intensity;
    float alphaThreshold;
} _36;

layout(binding = 2) uniform sampler2DArray baseMap;

in vec2 fragTexCoord;
layout(location = 0) out vec4 fragOut0;

void main()
{
    vec4 _45 = texture(baseMap, vec3(fragTexCoord, float(_36.baseMapIndex)));
    if (_36.alphaThreshold > _45.w)
    {
        discard;
    }
    bool _63 = _36.srgb == 1;
    vec3 _159;
    if (_63)
    {
        _159 = pow(_45.xyz, vec3(2.2000000476837158203125));
    }
    else
    {
        _159 = _45.xyz;
    }
    _45.x = _159.x;
    _45.y = _159.y;
    _45.z = _159.z;
    vec4 _161;
    if (_63)
    {
        _161 = vec4(pow(_36.color.xyz, vec3(2.2000000476837158203125)), _36.color.w);
    }
    else
    {
        _161 = _36.color;
    }
    fragOut0 = mix(mix(_45 * _161, vec4(_161.xyz, _159.x * _161.w), vec4(float(_36.alphaTexture))), _161, vec4(float(_36.noTexturing))) * _36.intensity;
}

