
#pragma once

#include <cstddef>
#include <cstdint>

struct decalGlobalData_decal_vert {
	SPIRV_FLOAT_MAT_4x4 viewMatrix;
	SPIRV_FLOAT_MAT_4x4 projMatrix;
	SPIRV_FLOAT_MAT_4x4 invViewMatrix;
	SPIRV_FLOAT_MAT_4x4 invProjMatrix;

	SPIRV_FLOAT_VEC2 viewportSize;
	float pad0;
	float pad1;
};
static_assert(sizeof(decalGlobalData_decal_vert) == 272,
	"Size of struct decalGlobalData_decal_vert does not match what is expected for the uniform block!");
static_assert(offsetof(decalGlobalData_decal_vert, viewMatrix) == 0,
	"Offset of member viewMatrix does not match the uniform buffer offset!");
static_assert(offsetof(decalGlobalData_decal_vert, projMatrix) == 64,
	"Offset of member projMatrix does not match the uniform buffer offset!");
static_assert(offsetof(decalGlobalData_decal_vert, invViewMatrix) == 128,
	"Offset of member invViewMatrix does not match the uniform buffer offset!");
static_assert(offsetof(decalGlobalData_decal_vert, invProjMatrix) == 192,
	"Offset of member invProjMatrix does not match the uniform buffer offset!");
static_assert(offsetof(decalGlobalData_decal_vert, viewportSize) == 256,
	"Offset of member viewportSize does not match the uniform buffer offset!");
static_assert(offsetof(decalGlobalData_decal_vert, pad0) == 264,
	"Offset of member pad0 does not match the uniform buffer offset!");
static_assert(offsetof(decalGlobalData_decal_vert, pad1) == 268,
	"Offset of member pad1 does not match the uniform buffer offset!");


