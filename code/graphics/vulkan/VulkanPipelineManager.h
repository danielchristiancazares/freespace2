#pragma once

#include "VulkanDebug.h"
#include "VulkanLayoutContracts.h"
#include "VulkanShaderManager.h"

#include "graphics/2d.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

struct PipelineKey {
  shader_type type = SDR_TYPE_NONE;
  uint32_t variant_flags = 0;
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits sample_count{VK_SAMPLE_COUNT_1_BIT};
  uint32_t color_attachment_count{1};
  gr_alpha_blend blend_mode{ALPHA_BLEND_NONE};
  size_t layout_hash = 0; // Hash of vertex_layout for pipeline keying
  uint32_t color_write_mask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  bool stencil_test_enable = false;
  VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;
  uint32_t stencil_compare_mask = 0xFF;
  uint32_t stencil_write_mask = 0xFF;
  uint32_t stencil_reference = 0;
  VkStencilOp front_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp front_depth_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp front_pass_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_depth_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_pass_op = VK_STENCIL_OP_KEEP;

  bool operator==(const PipelineKey &other) const {
    if (type != other.type) {
      return false;
    }
    const bool ignoreLayout = usesVertexPulling(type);
    return variant_flags == other.variant_flags && color_format == other.color_format &&
           depth_format == other.depth_format && sample_count == other.sample_count &&
           color_attachment_count == other.color_attachment_count && blend_mode == other.blend_mode &&
           color_write_mask == other.color_write_mask && stencil_test_enable == other.stencil_test_enable &&
           stencil_compare_op == other.stencil_compare_op && stencil_compare_mask == other.stencil_compare_mask &&
           stencil_write_mask == other.stencil_write_mask && stencil_reference == other.stencil_reference &&
           front_fail_op == other.front_fail_op && front_depth_fail_op == other.front_depth_fail_op &&
           front_pass_op == other.front_pass_op && back_fail_op == other.back_fail_op &&
           back_depth_fail_op == other.back_depth_fail_op && back_pass_op == other.back_pass_op &&
           (ignoreLayout || layout_hash == other.layout_hash);
  }
};

struct PipelineKeyHasher {
  std::size_t operator()(const PipelineKey &key) const {
    std::size_t h = static_cast<std::size_t>(key.type);
    h ^= static_cast<std::size_t>(key.variant_flags + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.color_format + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.depth_format + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.sample_count + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.color_attachment_count + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.blend_mode + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.color_write_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_test_enable + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_compare_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_compare_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_write_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_reference + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.front_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.front_depth_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.front_pass_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.back_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.back_depth_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.back_pass_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    if (!usesVertexPulling(key.type)) {
      h ^= key.layout_hash;
    }
    return h;
  }
};

struct VertexInputState {
  std::vector<vk::VertexInputBindingDescription> bindings;
  std::vector<vk::VertexInputAttributeDescription> attributes;
  std::vector<vk::VertexInputBindingDivisorDescription> divisors;
};

VertexInputState convertVertexLayoutToVulkan(const vertex_layout &layout);

class VulkanPipelineManager {
public:
  VulkanPipelineManager(vk::Device device, vk::PipelineLayout pipelineLayout, vk::PipelineLayout modelPipelineLayout,
                        vk::PipelineLayout deferredPipelineLayout, vk::PipelineCache pipelineCache,
                        bool supportsExtendedDynamicState3, const ExtendedDynamicState3Caps &extDyn3Caps,
                        bool supportsVertexAttributeDivisor, bool dynamicRenderingEnabled);

  vk::Pipeline getPipeline(const PipelineKey &key, const ShaderModules &modules, const vertex_layout &layout);
  static std::vector<vk::DynamicState> BuildDynamicStateList(bool supportsExtendedDynamicState3,
                                                             const ExtendedDynamicState3Caps &caps);

private:
  vk::Device m_device;
  vk::PipelineLayout m_pipelineLayout;
  vk::PipelineLayout m_modelPipelineLayout;
  vk::PipelineLayout m_deferredPipelineLayout;
  vk::PipelineCache m_pipelineCache;
  std::mutex m_mutex;
  bool m_supportsExtendedDynamicState3 = false;
  ExtendedDynamicState3Caps m_extDyn3Caps{};
  bool m_supportsVertexAttributeDivisor = false;
  bool m_dynamicRenderingEnabled = false;

  std::unordered_map<PipelineKey, vk::UniquePipeline, PipelineKeyHasher> m_pipelines;
  std::unordered_map<size_t, VertexInputState> m_vertexInputCache;

  vk::UniquePipeline createPipeline(const PipelineKey &key, const ShaderModules &modules, const vertex_layout &layout);
  const VertexInputState &getVertexInputState(const vertex_layout &layout);
};

} // namespace vulkan
} // namespace graphics
