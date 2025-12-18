#pragma once

#include <cstdint>

struct genericData_batched_bitmap_frag {
	SPIRV_FLOAT_VEC4 color;
	float intensity;
};
static_assert(sizeof(genericData_batched_bitmap_frag) == 20, "Size of struct genericData_batched_bitmap_frag does not match what is expected for the uniform block!");
static_assert(offsetof(genericData_batched_bitmap_frag, color) == 0, "Offset of member color does not match the uniform buffer offset!");
static_assert(offsetof(genericData_batched_bitmap_frag, intensity) == 16, "Offset of member intensity does not match the uniform buffer offset!");
