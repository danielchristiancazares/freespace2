#pragma once

#include <vulkan/vulkan.hpp>
#include <cstdint>

namespace graphics {
namespace vulkan {

// Returns true if all descriptor-indexing features required by the Vulkan model path are supported.
// Required: shaderSampledImageArrayNonUniformIndexing, runtimeDescriptorArray,
// (descriptorBindingPartiallyBound is no longer required; the bindless array is fully written each frame).
bool ValidateModelDescriptorIndexingSupport(const vk::PhysicalDeviceDescriptorIndexingFeatures& features);

// Overload for Vulkan 1.2 features struct (used during device selection).
// Extracts the descriptor indexing fields and validates them.
bool ValidateModelDescriptorIndexingSupport(const vk::PhysicalDeviceVulkan12Features& features12);

// Returns true if push descriptors are supported (core in Vulkan 1.4).
bool ValidatePushDescriptorSupport(const vk::PhysicalDeviceVulkan14Features& features14);

// Throws if push descriptors are not supported; used at descriptor setup call sites.
void EnsurePushDescriptorSupport(const vk::PhysicalDeviceVulkan14Features& features14);

void EnsureModelPushConstantBudget(uint32_t requiredBytes, uint32_t deviceLimitBytes);

} // namespace vulkan
} // namespace graphics
