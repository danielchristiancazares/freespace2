#version 460 core

// Vertex pulling: no vertex attribute inputs. All data fetched from a storage buffer using gl_VertexIndex.

#define MODEL_SDR_FLAG_MODE_GLSL
#include "model_shader_flags.h"

layout(set = 0, binding = 0) readonly buffer VertexBuffer
{
	float data[];
} vertexBuffer;

layout(set = 0, binding = 3, std430) readonly buffer TransformBuffer
{
	// 4 vec4 texels per matrix (column-major) to mirror the OpenGL transform_tex path.
	vec4 data[];
} transformBuffer;

// Keep the Vulkan model UBO minimal to stay compatible with shadertool's struct generation.
// Offsets must match graphics::model_uniform_data (std140) in code/graphics/util/uniform_structs.h.
layout(set = 0, binding = 2, std140) uniform ModelUniforms
{
	mat4 modelViewMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
	mat4 projMatrix;

	layout(offset = 256) mat4 textureMatrix;
	layout(offset = 1180) int buffer_matrix_offset;
	layout(offset = 1200) float thruster_scale;
} uModel;

layout(push_constant) uniform ModelPushConstants
{
	uint vertexOffset;        // byte offset into vertex heap for this draw
	uint stride;              // byte stride between vertices
	uint vertexAttribMask;    // MODEL_ATTRIB_* bits from VulkanModelTypes.h
	uint posOffset;           // byte offset of position (vec3) - valid if vertexAttribMask has MODEL_ATTRIB_POS
	uint normalOffset;        // byte offset of normal (vec3) - valid if vertexAttribMask has MODEL_ATTRIB_NORMAL
	uint texCoordOffset;      // byte offset of texcoord (vec2) - valid if vertexAttribMask has MODEL_ATTRIB_TEXCOORD
	uint tangentOffset;       // byte offset of tangent (vec4) - valid if vertexAttribMask has MODEL_ATTRIB_TANGENT
	uint modelIdOffset;       // byte offset of model id (float) - valid if vertexAttribMask has MODEL_ATTRIB_MODEL_ID
	uint boneIndicesOffset;   // byte offset of bone indices (vec4/ivec4)
	uint boneWeightsOffset;   // byte offset of bone weights (vec4)
	uint baseMapIndex;        // texture indices for bindless sampling
	uint glowMapIndex;
	uint normalMapIndex;
	uint specMapIndex;
	uint matrixIndex;         // instance/model matrix index (implementation-defined)
	uint flags;               // bitfield for variant flags
} pcs;

const uint MODEL_ATTRIB_POS = 1u << 0;
const uint MODEL_ATTRIB_NORMAL = 1u << 1;
const uint MODEL_ATTRIB_TEXCOORD = 1u << 2;
const uint MODEL_ATTRIB_TANGENT = 1u << 3;
const uint MODEL_ATTRIB_BONEINDICES = 1u << 4;
const uint MODEL_ATTRIB_BONEWEIGHTS = 1u << 5;
const uint MODEL_ATTRIB_MODEL_ID = 1u << 6;

layout(location = 0) out vec3 vPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out vec4 vTangent;

bool hasAttrib(uint bit)
{
	return (pcs.vertexAttribMask & bit) != 0u;
}

uint wordIndex(uint byteBase, uint byteOffset)
{
	return (byteBase + byteOffset) >> 2; // divide by 4
}

float loadFloat(uint byteBase, uint offset)
{
	uint idx = wordIndex(byteBase, offset);
	return vertexBuffer.data[idx];
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

const int TEXELS_PER_MATRIX = 4;

void getModelTransform(inout mat4 transform, out bool invisible, int id, int matrix_offset)
{
	int base = (matrix_offset + id) * TEXELS_PER_MATRIX;
	transform[0] = transformBuffer.data[base + 0];
	transform[1] = transformBuffer.data[base + 1];
	transform[2] = transformBuffer.data[base + 2];
	transform[3] = transformBuffer.data[base + 3];
	invisible = transform[3].w >= 0.9;
	transform[3].w = 1.0;
}

void main()
{
	uint byteBase = pcs.vertexOffset + uint(gl_VertexIndex) * pcs.stride;

	vec3 position = hasAttrib(MODEL_ATTRIB_POS) ? loadVec3(byteBase, pcs.posOffset) : vec3(0.0);
	vec3 normal = hasAttrib(MODEL_ATTRIB_NORMAL) ? loadVec3(byteBase, pcs.normalOffset) : vec3(0.0, 0.0, 1.0);
	if (hasAttrib(MODEL_ATTRIB_NORMAL)) {
		float lenSq = dot(normal, normal);
		if (lenSq > 0.0) {
			normal = normalize(normal);
		}
	}

	vec2 texCoord = hasAttrib(MODEL_ATTRIB_TEXCOORD) ? loadVec2(byteBase, pcs.texCoordOffset) : vec2(0.0);
	vec4 tangent = hasAttrib(MODEL_ATTRIB_TANGENT) ? loadVec4(byteBase, pcs.tangentOffset) : vec4(0.0, 0.0, 1.0, 1.0);
	float modelId = hasAttrib(MODEL_ATTRIB_MODEL_ID) ? loadFloat(byteBase, pcs.modelIdOffset) : 0.0;

	mat4 orient = mat4(1.0);
	bool clipModel = false;
	if ((pcs.flags & uint(MODEL_SDR_FLAG_TRANSFORM)) != 0u) {
		getModelTransform(orient, clipModel, int(modelId), uModel.buffer_matrix_offset);
	}

	vec3 vertex = position;
	if ((pcs.flags & uint(MODEL_SDR_FLAG_THRUSTER)) != 0u) {
		if (vertex.z < -1.5) {
			vertex.z *= uModel.thruster_scale;
		}
	}

	vec4 viewPos = uModel.modelViewMatrix * orient * vec4(vertex, 1.0);
	gl_Position = uModel.projMatrix * viewPos;

	mat3 mv3 = mat3(uModel.modelViewMatrix);
	mat3 orient3 = mat3(orient);

	// Transform normal to view-space for deferred lighting
	vPosition = viewPos.xyz;
	vec3 viewNormal = mv3 * orient3 * normal;
	float nLenSq = dot(viewNormal, viewNormal);
	if (nLenSq > 0.0) {
		viewNormal *= inversesqrt(nLenSq);
	} else {
		viewNormal = vec3(0.0, 0.0, 1.0);
	}

	// Tangent is provided in object space; transform to view space and orthonormalize against the view normal.
	vec3 viewTangent = mv3 * orient3 * tangent.xyz;
	float tLenSq = dot(viewTangent, viewTangent);
	if (tLenSq > 0.0) {
		viewTangent *= inversesqrt(tLenSq);
		viewTangent = viewTangent - viewNormal * dot(viewTangent, viewNormal);
		float orthoLenSq = dot(viewTangent, viewTangent);
		if (orthoLenSq > 0.0) {
			viewTangent *= inversesqrt(orthoLenSq);
		} else {
			// Degenerate tangent (parallel to normal): pick an arbitrary perpendicular axis.
			vec3 up = (abs(viewNormal.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
			viewTangent = normalize(cross(up, viewNormal));
		}
	} else {
		vec3 up = (abs(viewNormal.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
		viewTangent = normalize(cross(up, viewNormal));
	}

	vec4 texCoord4 = uModel.textureMatrix * vec4(texCoord, 0.0, 1.0);

	vNormal = viewNormal;
	vTexCoord = texCoord4.xy;
	vTangent = vec4(viewTangent, tangent.w);

	if (clipModel) {
		// Match OpenGL batched transform clip convention.
		gl_Position = vec4(vec3(-2.0), 1.0);
	}
}
