#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 1, std140) uniform NanoVGUniformData {
	mat3 scissorMat;

	mat3 paintMat;

	vec4 innerCol;

	vec4 outerCol;

	vec2 scissorExt;
	vec2 scissorScale;

	vec2 extent;
	float radius;
	float feather;

	float strokeMult;
	float strokeThr;
	int texType;
	int type;

	vec2 viewSize;
	int texArrayIndex;
};

layout(location = 0) in vec2 vertPosition;
layout(location = 2) in vec2 vertTexCoord;

layout(location = 0) out vec2 ftcoord;
layout(location = 1) out vec2 fpos;

void main(void)
{
	ftcoord = vertTexCoord;
	fpos = vertPosition;
	gl_Position = vec4(2.0 * vertPosition.x / viewSize.x - 1.0,
		1.0 - 2.0 * vertPosition.y / viewSize.y,
		0.0,
		1.0);
}

