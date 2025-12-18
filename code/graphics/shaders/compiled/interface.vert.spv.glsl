#version 330
#ifdef GL_ARB_shading_language_420pack
#extension GL_ARB_shading_language_420pack : require
#endif

layout(binding = 0, std140) uniform matrixData
{
    mat4 modelViewMatrix;
    mat4 projMatrix;
} _27;

out vec2 fragTexCoord;
layout(location = 2) in vec4 vertTexCoord;
layout(location = 0) in vec4 vertPosition;

void main()
{
    fragTexCoord = vertTexCoord.xy;
    gl_Position = (_27.projMatrix * _27.modelViewMatrix) * vertPosition;
}

