#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec2 fragTexCoord;

layout(push_constant) uniform MoviePushConstants {
	layout(offset = 0) vec2 screenSize;
	layout(offset = 8) vec2 rectMin;
	layout(offset = 16) vec2 rectMax;
	layout(offset = 24) float alpha;
	layout(offset = 28) float _pad;
} pc;

void main()
{
	// Two triangles, using vertex-less drawing.
	vec2 positions[6] = vec2[](
		vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0),
		vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0)
	);

	vec2 uv = positions[gl_VertexIndex];
	fragTexCoord = uv;

	// Inputs are in screen space with top-left origin (y down).
	vec2 screen = mix(pc.rectMin, pc.rectMax, uv);

	// Convert to NDC. The Vulkan backend uses a flipped viewport to preserve OpenGL-style conventions.
	vec2 ndc;
	ndc.x = 2.0 * (screen.x / pc.screenSize.x) - 1.0;
	ndc.y = 1.0 - 2.0 * (screen.y / pc.screenSize.y);
	gl_Position = vec4(ndc, 0.0, 1.0);
}

