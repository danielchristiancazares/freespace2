#include "VulkanModelValidation.h"

#include "VulkanDebug.h"
#include "globalincs/pstypes.h"

#include <stdexcept>

namespace graphics {
namespace vulkan {

bool ValidateModelDescriptorIndexingSupport(const vk::PhysicalDeviceDescriptorIndexingFeatures& features)
{
	// Required features for bindless model rendering
	if (!features.shaderSampledImageArrayNonUniformIndexing) {
		return false;
	}

	if (!features.runtimeDescriptorArray) {
		return false;
	}

	if (!features.descriptorBindingPartiallyBound) {
		return false;
	}

	// REMOVED: descriptorBindingSampledImageUpdateAfterBind (no longer used)
	// REMOVED: descriptorBindingVariableDescriptorCount (no longer used)

	return true;
}

bool ValidateModelDescriptorIndexingSupport(const vk::PhysicalDeviceVulkan12Features& features12)
{
	// Extract descriptor indexing fields from Vulkan 1.2 features struct.
	// This is the wiring point tested by integration tests.
	vk::PhysicalDeviceDescriptorIndexingFeatures idx{};
	idx.shaderSampledImageArrayNonUniformIndexing = features12.shaderSampledImageArrayNonUniformIndexing;
	idx.runtimeDescriptorArray = features12.runtimeDescriptorArray;
	idx.descriptorBindingPartiallyBound = features12.descriptorBindingPartiallyBound;
	return ValidateModelDescriptorIndexingSupport(idx);
}

bool ValidatePushDescriptorSupport(const vk::PhysicalDeviceVulkan14Features& features14)
{
	if (!features14.pushDescriptor) {
		return false;
	}

	return true;
}

void EnsurePushDescriptorSupport(const vk::PhysicalDeviceVulkan14Features& features14)
{
	if (!ValidatePushDescriptorSupport(features14)) {
		throw std::runtime_error("Vulkan: pushDescriptor feature is required but not supported");
	}
}

void EnsureModelPushConstantBudget(uint32_t requiredBytes, uint32_t deviceLimitBytes)
{
	if (requiredBytes > deviceLimitBytes) {
		throw std::runtime_error("Model push constants exceed device limit");
	}
}

} // namespace vulkan
} // namespace graphics
