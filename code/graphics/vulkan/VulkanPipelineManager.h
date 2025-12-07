#pragma once

#include "VulkanShaderManager.h"
#include "VulkanLayoutContracts.h"

#include "graphics/2d.h"

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

struct PipelineKey {
	shader_type type;
	uint32_t variant_flags;
	VkFormat color_format;
	VkFormat depth_format;
	VkSampleCountFlagBits sample_count{VK_SAMPLE_COUNT_1_BIT};
	uint32_t color_attachment_count{1};
	gr_alpha_blend blend_mode{ALPHA_BLEND_NONE};
	size_t layout_hash; // Hash of vertex_layout for pipeline keying

	bool operator==(const PipelineKey& other) const
	{
		if (type != other.type) {
			return false;
		}
		const bool ignoreLayout = usesVertexPulling(type);
		return variant_flags == other.variant_flags && color_format == other.color_format &&
		       depth_format == other.depth_format && sample_count == other.sample_count &&
		       color_attachment_count == other.color_attachment_count && blend_mode == other.blend_mode &&
		       (ignoreLayout || layout_hash == other.layout_hash);
	}
};

struct PipelineKeyHasher {
	std::size_t operator()(const PipelineKey& key) const
	{
		std::size_t h = static_cast<std::size_t>(key.type);
		h ^= static_cast<std::size_t>(key.variant_flags + 0x9e3779b9 + (h << 6) + (h >> 2));
		h ^= static_cast<std::size_t>(key.color_format + 0x9e3779b9 + (h << 6) + (h >> 2));
		h ^= static_cast<std::size_t>(key.depth_format + 0x9e3779b9 + (h << 6) + (h >> 2));
		h ^= static_cast<std::size_t>(key.sample_count + 0x9e3779b9 + (h << 6) + (h >> 2));
		h ^= static_cast<std::size_t>(key.color_attachment_count + 0x9e3779b9 + (h << 6) + (h >> 2));
		h ^= static_cast<std::size_t>(key.blend_mode + 0x9e3779b9 + (h << 6) + (h >> 2));
		if (!usesVertexPulling(key.type)) {
			h ^= key.layout_hash;
		}
		return h;
	}
};

// Converts vertex_layout to Vulkan vertex input descriptions
struct VertexInputState {
	std::vector<vk::VertexInputBindingDescription> bindings;
	std::vector<vk::VertexInputAttributeDescription> attributes;
	std::vector<vk::VertexInputBindingDivisorDescription> divisors;
};

// Capabilities we care about from VK_EXT_extended_dynamic_state3
struct ExtendedDynamicState3Caps {
	bool colorBlendEnable = false;
	bool colorWriteMask = false;
	bool polygonMode = false;
	bool rasterizationSamples = false;
};

VertexInputState convertVertexLayoutToVulkan(const vertex_layout& layout);

class VulkanPipelineManager {
  public:
	VulkanPipelineManager(vk::Device device,
		vk::PipelineLayout pipelineLayout,
		vk::PipelineLayout modelPipelineLayout,
		vk::PipelineCache pipelineCache,
		bool supportsExtendedDynamicState,
		bool supportsExtendedDynamicState2,
		bool supportsExtendedDynamicState3,
		const ExtendedDynamicState3Caps& extDyn3Caps,
		bool supportsVertexAttributeDivisor,
		bool dynamicRenderingEnabled);

	vk::Pipeline getPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout);
	static std::vector<vk::DynamicState> BuildDynamicStateList(bool supportsExtendedDynamicState3,
		const ExtendedDynamicState3Caps& caps);

  private:
	vk::Device m_device;
	vk::PipelineLayout m_pipelineLayout;
	vk::PipelineLayout m_modelPipelineLayout;
	vk::PipelineCache m_pipelineCache;
	bool m_supportsExtendedDynamicState = false;
	bool m_supportsExtendedDynamicState2 = false;
	bool m_supportsExtendedDynamicState3 = false;
	ExtendedDynamicState3Caps m_extDyn3Caps{};
	bool m_supportsVertexAttributeDivisor = false;
	bool m_dynamicRenderingEnabled = false;

	std::unordered_map<PipelineKey, vk::UniquePipeline, PipelineKeyHasher> m_pipelines;
	// Cache vertex input state conversions
	std::unordered_map<size_t, VertexInputState> m_vertexInputCache;

	vk::UniquePipeline createPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout);
	const VertexInputState& getVertexInputState(const vertex_layout& layout);
};

} // namespace vulkan
} // namespace graphics
