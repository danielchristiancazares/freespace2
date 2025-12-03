#version 150

uniform sampler2D sceneTexture;

out vec4 outColor;
in vec2 fragTexCoord;

void main()
{
    outColor = texture(sceneTexture, fragTexCoord);
}

