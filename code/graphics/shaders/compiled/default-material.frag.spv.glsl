#version 150

layout(std140) uniform genericData
{
    mat4 modelMatrix;
    vec4 color;
    vec4 clipEquation;
    int baseMapIndex;
    int alphaTexture;
    int noTexturing;
    int srgb;
    float intensity;
    float alphaThreshold;
    uint clipEnabled;
} _39;

uniform sampler2DArray baseMap;

in vec4 fragTexCoord;
in vec4 fragColor;
out vec4 fragOut0;

void main()
{
    vec4 _48 = texture(baseMap, vec3(fragTexCoord.xy, float(_39.baseMapIndex)));
    if (_39.alphaThreshold > _48.w)
    {
        discard;
    }
    bool _66 = _39.srgb == 1;
    vec3 _165;
    if (_66)
    {
        _165 = pow(_48.xyz, vec3(2.2000000476837158203125));
    }
    else
    {
        _165 = _48.xyz;
    }
    vec4 _158 = _48;
    _158.x = _165.x;
    vec4 _160 = _158;
    _160.y = _165.y;
    vec4 _162 = _160;
    _162.z = _165.z;
    vec4 _167;
    if (_66)
    {
        _167 = vec4(pow(fragColor.xyz, vec3(2.2000000476837158203125)), fragColor.w);
    }
    else
    {
        _167 = fragColor;
    }
    fragOut0 = mix(mix(_162 * _167, vec4(_167.xyz, _165.x * _167.w), vec4(float(_39.alphaTexture))), _167, vec4(float(_39.noTexturing))) * _39.intensity;
}

