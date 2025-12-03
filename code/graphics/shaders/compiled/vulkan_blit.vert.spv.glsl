#version 150

const vec2 _19[3] = vec2[](vec2(-1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
const vec2 _26[3] = vec2[](vec2(0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));

out vec2 fragTexCoord;

void main()
{
    gl_Position = vec4(_19[gl_VertexID], 0.0, 1.0);
    fragTexCoord = _26[gl_VertexID];
}

