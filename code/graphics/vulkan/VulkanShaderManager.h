#pragma once

#include "graphics/2d.h"

#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

struct ShaderModules {
  vk::ShaderModule vert;
  vk::ShaderModule frag;
};

class VulkanShaderManager {
public:
  VulkanShaderManager(vk::Device device, const SCP_string &shaderRoot);

  ShaderModules getModules(shader_type type, uint32_t variantFlags = 0);

  // Filename-based lookup for Vulkan-only shaders that don't map cleanly to shader_type/layout contracts.
  // filename is expected to be a SPIR-V filename like "movie.vert.spv".
  ShaderModules getModulesByFilenames(const SCP_string &vertFilename, const SCP_string &fragFilename);

private:
  vk::Device m_device;
  SCP_string m_shaderRoot;

  struct Key {
    shader_type type;
    uint32_t flags;

    bool operator==(const Key &other) const { return type == other.type && flags == other.flags; }
  };

  struct KeyHasher {
    std::size_t operator()(const Key &key) const {
      return (static_cast<std::size_t>(key.type) << 16) ^ static_cast<std::size_t>(key.flags);
    }
  };

  std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_vertexModules;
  std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_fragmentModules;
  std::unordered_map<SCP_string, vk::UniqueShaderModule> m_filenameModules;

  vk::UniqueShaderModule loadModule(const SCP_string &path);
  vk::ShaderModule loadModuleByFilename(const SCP_string &filename);
};

} // namespace vulkan
} // namespace graphics
