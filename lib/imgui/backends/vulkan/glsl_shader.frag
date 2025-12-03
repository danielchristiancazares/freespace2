#version 450 core
#ifdef GL_KHR_vulkan_glsl
#extension GL_KHR_vulkan_glsl : enable
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(set = set_id, binding = binding_id)
#else
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(binding = binding_id)
#endif

layout(location = 0) out vec4 fColor;

LAYOUT_SET_BINDING(0, 0) uniform sampler2D sTexture;

layout(location = 0) in struct {
    vec4 Color;
    vec2 UV;
} In;

void main()
{
    fColor = In.Color * texture(sTexture, In.UV.st);
}
