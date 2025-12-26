#pragma once

#include "VulkanDescriptorLayouts.h"

#include <map>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

struct DescriptorBinding {
  uint32_t set = 0;
  uint32_t binding = 0;
  vk::DescriptorType type = vk::DescriptorType::eSampler;
  vk::ShaderStageFlags stageFlags{};
};

struct ShaderReflectionResult {
  std::vector<DescriptorBinding> bindings;
  bool valid = true;
  std::string errorMessage;
};

// Reflect descriptor bindings from compiled SPIR-V shader binary
ShaderReflectionResult reflectShaderDescriptorBindings(const std::vector<uint32_t> &spirvCode,
                                                       vk::ShaderStageFlagBits stage);

// Validate shader descriptor bindings against expected layout
bool validateShaderBindings(const ShaderReflectionResult &reflection, const VulkanDescriptorLayouts &layouts,
                            vk::ShaderStageFlagBits stage, const std::string &shaderName);

// Validate a complete shader pair (vertex + fragment) against expected layouts
bool validateShaderPair(const std::string &vertPath, const std::string &fragPath,
                        const VulkanDescriptorLayouts &layouts);

} // namespace vulkan
} // namespace graphics
