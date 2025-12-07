#version 460 core

// Vertex pulling: no vertex attribute inputs. All data fetched from a storage buffer using gl_VertexIndex.

layout(set = 0, binding = 0) readonly buffer VertexBuffer
{
	float data[];
} vertexBuffer;

layout(set = 0, binding = 2, std140) uniform ModelUniforms
{
	mat4 modelViewMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
	mat4 projMatrix;
} uModel;

layout(push_constant) uniform ModelPushConstants
{
	uint vertexOffset;        // byte offset into vertex heap for this draw
	uint stride;              // byte stride between vertices
	uint posOffset;           // byte offset of position (vec3)
	uint normalOffset;        // byte offset of normal (vec3)
	uint texCoordOffset;      // byte offset of texcoord (vec2)
	uint tangentOffset;       // byte offset of tangent (vec4)
	uint boneIndicesOffset;   // byte offset of bone indices (vec4/ivec4) or 0xFFFFFFFF when absent
	uint boneWeightsOffset;   // byte offset of bone weights (vec4) or 0xFFFFFFFF when absent
	uint baseMapIndex;        // texture indices for bindless sampling
	uint glowMapIndex;
	uint normalMapIndex;
	uint specMapIndex;
	uint matrixIndex;         // instance/model matrix index (implementation-defined)
	uint flags;               // bitfield for variant flags
} pcs;

const uint OFFSET_ABSENT = 0xFFFFFFFFu;

layout(location = 0) out vec3 vPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out vec4 vTangent;

bool hasOffset(uint offset)
{
	return offset != OFFSET_ABSENT;
}

uint wordIndex(uint byteBase, uint byteOffset)
{
	return (byteBase + byteOffset) >> 2; // divide by 4
}

vec2 loadVec2(uint byteBase, uint offset)
{
	uint idx = wordIndex(byteBase, offset);
	return vec2(vertexBuffer.data[idx], vertexBuffer.data[idx + 1]);
}

vec3 loadVec3(uint byteBase, uint offset)
{
	uint idx = wordIndex(byteBase, offset);
	return vec3(vertexBuffer.data[idx], vertexBuffer.data[idx + 1], vertexBuffer.data[idx + 2]);
}

vec4 loadVec4(uint byteBase, uint offset)
{
	uint idx = wordIndex(byteBase, offset);
	return vec4(vertexBuffer.data[idx], vertexBuffer.data[idx + 1], vertexBuffer.data[idx + 2], vertexBuffer.data[idx + 3]);
}

void main()
{
	uint byteBase = pcs.vertexOffset + uint(gl_VertexIndex) * pcs.stride;

	vec3 position = hasOffset(pcs.posOffset) ? loadVec3(byteBase, pcs.posOffset) : vec3(0.0);
	vec3 normal = hasOffset(pcs.normalOffset) ? loadVec3(byteBase, pcs.normalOffset) : vec3(0.0, 0.0, 1.0);
	if (hasOffset(pcs.normalOffset)) {
		float lenSq = dot(normal, normal);
		if (lenSq > 0.0) {
			normal = normalize(normal);
		}
	}

	vec2 texCoord = hasOffset(pcs.texCoordOffset) ? loadVec2(byteBase, pcs.texCoordOffset) : vec2(0.0);
	vec4 tangent = hasOffset(pcs.tangentOffset) ? loadVec4(byteBase, pcs.tangentOffset) : vec4(0.0, 0.0, 1.0, 1.0);

	// Bone data, texture indices, matrixIndex, and flags are currently unused in this stage.
	// They are kept in the push constant block for future skinning/instancing/bindless sampling logic.

	vec4 viewPos = uModel.modelViewMatrix * vec4(position, 1.0);
	gl_Position = uModel.projMatrix * viewPos;

	vPosition = viewPos.xyz;
	vNormal = normal;
	vTexCoord = texCoord;
	vTangent = tangent;
}
