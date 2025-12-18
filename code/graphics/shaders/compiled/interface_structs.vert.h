#pragma once

#include <cstdint>

struct matrixData_interface_vert {
	SPIRV_FLOAT_MAT_4x4 modelViewMatrix;
	SPIRV_FLOAT_MAT_4x4 projMatrix;
};
static_assert(sizeof(matrixData_interface_vert) == 128, "Size of struct matrixData_interface_vert does not match what is expected for the uniform block!");
static_assert(offsetof(matrixData_interface_vert, modelViewMatrix) == 0, "Offset of member modelViewMatrix does not match the uniform buffer offset!");
static_assert(offsetof(matrixData_interface_vert, projMatrix) == 64, "Offset of member projMatrix does not match the uniform buffer offset!");
