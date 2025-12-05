#version 450
#extension GL_ARB_separate_shader_objects : enable

// Blit fragment shader
// Samples the scene texture and outputs to swapchain

layout(location = 0) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

#ifdef VULKAN
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(set = set_id, binding = binding_id)
#else
#define LAYOUT_SET_BINDING(set_id, binding_id) layout(binding = binding_id)
#endif

LAYOUT_SET_BINDING(0, 0) uniform sampler2D sceneTexture;

void main() {
    vec4 sceneColor = texture(sceneTexture, fragTexCoord);
    outColor = sceneColor;
}
