#version 330
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(binding = 1, std140) uniform genericData
{
    vec4 color;
    float intensity;
} _16;

layout(binding = 0, std140) uniform matrixData
{
    mat4 modelViewMatrix;
    mat4 projMatrix;
} _32;

out vec4 fragColor;
layout(location = 1) in vec4 vertColor;
layout(location = 0) in vec4 vertPosition;
out vec4 fragTexCoord;
layout(location = 2) in vec4 vertTexCoord;

void main()
{
    fragColor = vertColor * _16.color;
    gl_Position = (_32.projMatrix * _32.modelViewMatrix) * vertPosition;
    fragTexCoord = vertTexCoord;
}

