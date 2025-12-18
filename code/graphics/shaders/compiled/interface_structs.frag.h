#pragma once

#include <cstdint>

struct genericData_interface_frag {
	SPIRV_FLOAT_VEC4 color;
	std::int32_t baseMapIndex;
	std::int32_t alphaTexture;
	std::int32_t noTexturing;
	std::int32_t srgb;
	float intensity;
	float alphaThreshold;
};
static_assert(sizeof(genericData_interface_frag) == 40, "Size of struct genericData_interface_frag does not match what is expected for the uniform block!");
static_assert(offsetof(genericData_interface_frag, color) == 0, "Offset of member color does not match the uniform buffer offset!");
static_assert(offsetof(genericData_interface_frag, baseMapIndex) == 16, "Offset of member baseMapIndex does not match the uniform buffer offset!");
static_assert(offsetof(genericData_interface_frag, alphaTexture) == 20, "Offset of member alphaTexture does not match the uniform buffer offset!");
static_assert(offsetof(genericData_interface_frag, noTexturing) == 24, "Offset of member noTexturing does not match the uniform buffer offset!");
static_assert(offsetof(genericData_interface_frag, srgb) == 28, "Offset of member srgb does not match the uniform buffer offset!");
static_assert(offsetof(genericData_interface_frag, intensity) == 32, "Offset of member intensity does not match the uniform buffer offset!");
static_assert(offsetof(genericData_interface_frag, alphaThreshold) == 36, "Offset of member alphaThreshold does not match the uniform buffer offset!");
