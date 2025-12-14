#include "VulkanPipelineManager.h"

#include "VulkanRenderer.h"
#include "VulkanVertexTypes.h"
#include "cmdline/cmdline.h"
#include "globalincs/systemvars.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <algorithm>

namespace graphics {
namespace vulkan {

struct VertexFormatMapping {
	vk::Format format;
	uint32_t location;
	uint32_t componentCount;
};

// Location mapping: 0=position, 1=color, 2=texcoord, 3=normal, 4=tangent, etc.
static const std::unordered_map<vertex_format_data::vertex_format, VertexFormatMapping> VERTEX_FORMAT_MAP = {
	{vertex_format_data::POSITION4, {vk::Format::eR32G32B32A32Sfloat, 0, 4}},
	{vertex_format_data::POSITION3, {vk::Format::eR32G32B32Sfloat, 0, 3}},
	{vertex_format_data::POSITION2, {vk::Format::eR32G32Sfloat, 0, 2}},
	{vertex_format_data::SCREEN_POS, {vk::Format::eR32G32Sfloat, 0, 2}},
	{vertex_format_data::COLOR3, {vk::Format::eR8G8B8Unorm, 1, 3}},
	{vertex_format_data::COLOR4, {vk::Format::eR8G8B8A8Unorm, 1, 4}},
	{vertex_format_data::COLOR4F, {vk::Format::eR32G32B32A32Sfloat, 1, 4}},
	{vertex_format_data::TEX_COORD2, {vk::Format::eR32G32Sfloat, 2, 2}},
	{vertex_format_data::TEX_COORD4, {vk::Format::eR32G32B32A32Sfloat, 2, 4}},
	{vertex_format_data::NORMAL, {vk::Format::eR32G32B32Sfloat, 3, 3}},
	{vertex_format_data::TANGENT, {vk::Format::eR32G32B32A32Sfloat, 4, 4}},
	{vertex_format_data::MODEL_ID, {vk::Format::eR32Sfloat, 5, 1}},
	{vertex_format_data::RADIUS, {vk::Format::eR32Sfloat, 6, 1}},
	{vertex_format_data::UVEC, {vk::Format::eR32G32B32Sfloat, 7, 3}},
	{vertex_format_data::MATRIX4, {vk::Format::eR32G32B32A32Sfloat, 8, 4}},
};

static const char* vertexFormatToString(vertex_format_data::vertex_format fmt)
{
	using vf = vertex_format_data::vertex_format;
	switch (fmt) {
	case vf::POSITION4: return "POSITION4";
	case vf::POSITION3: return "POSITION3";
	case vf::POSITION2: return "POSITION2";
	case vf::SCREEN_POS: return "SCREEN_POS";
	case vf::COLOR3: return "COLOR3";
	case vf::COLOR4: return "COLOR4";
	case vf::COLOR4F: return "COLOR4F";
	case vf::TEX_COORD2: return "TEX_COORD2";
	case vf::TEX_COORD4: return "TEX_COORD4";
	case vf::NORMAL: return "NORMAL";
	case vf::TANGENT: return "TANGENT";
	case vf::MODEL_ID: return "MODEL_ID";
	case vf::RADIUS: return "RADIUS";
	case vf::UVEC: return "UVEC";
	case vf::MATRIX4: return "MATRIX4";
	default: return "UNKNOWN";
	}
}

static vk::PipelineColorBlendAttachmentState buildBlendAttachment(gr_alpha_blend mode)
{
	vk::PipelineColorBlendAttachmentState state{};
	state.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
	                       vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

	switch (mode) {
	case ALPHA_BLEND_NONE:
		state.blendEnable = VK_FALSE;
		break;
	case ALPHA_BLEND_ADDITIVE:
		state.blendEnable = VK_TRUE;
		state.srcColorBlendFactor = vk::BlendFactor::eOne;
		state.dstColorBlendFactor = vk::BlendFactor::eOne;
		state.colorBlendOp = vk::BlendOp::eAdd;
		state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
		state.alphaBlendOp = vk::BlendOp::eAdd;
		break;
	case ALPHA_BLEND_ALPHA_ADDITIVE:
		state.blendEnable = VK_TRUE;
		state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
		state.dstColorBlendFactor = vk::BlendFactor::eOne;
		state.colorBlendOp = vk::BlendOp::eAdd;
		state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
		state.alphaBlendOp = vk::BlendOp::eAdd;
		break;
	case ALPHA_BLEND_ALPHA_BLEND_ALPHA:
		state.blendEnable = VK_TRUE;
		state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
		state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		state.colorBlendOp = vk::BlendOp::eAdd;
		state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		state.alphaBlendOp = vk::BlendOp::eAdd;
		break;
	case ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR:
		state.blendEnable = VK_TRUE;
		state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
		state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
		state.colorBlendOp = vk::BlendOp::eAdd;
		state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		state.alphaBlendOp = vk::BlendOp::eAdd;
		break;
	case ALPHA_BLEND_PREMULTIPLIED:
		state.blendEnable = VK_TRUE;
		state.srcColorBlendFactor = vk::BlendFactor::eOne;
		state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		state.colorBlendOp = vk::BlendOp::eAdd;
		state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
		state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
		state.alphaBlendOp = vk::BlendOp::eAdd;
		break;
	default:
		state.blendEnable = VK_FALSE;
		break;
	}

	return state;
}

VertexInputState convertVertexLayoutToVulkan(const vertex_layout& layout)
{
	VertexInputState result;
	std::unordered_map<size_t, std::vector<const vertex_format_data*>> componentsByBuffer;

	for (size_t i = 0; i < layout.get_num_vertex_components(); ++i) {
		const vertex_format_data* component = layout.get_vertex_component(i);
		componentsByBuffer[component->buffer_number].push_back(component);
	}

	std::unordered_map<uint32_t, uint32_t> divisorsByBinding;

	for (const auto& [bufferNum, components] : componentsByBuffer) {
		if (components.empty()) {
			continue;
		}

		// Get stride for this buffer
		size_t stride = layout.get_vertex_stride(bufferNum);

		vk::VertexInputBindingDescription binding{};
		binding.binding = static_cast<uint32_t>(bufferNum);
		binding.stride = static_cast<uint32_t>(stride);
		binding.inputRate = vk::VertexInputRate::eVertex;

		for (const auto* comp : components) {
			if (comp->divisor != 0) {
				binding.inputRate = vk::VertexInputRate::eInstance;
				if (comp->divisor > 1) {
					divisorsByBinding[binding.binding] = static_cast<uint32_t>(comp->divisor);
				}
				break;
			}
		}

		result.bindings.push_back(binding);

		for (const auto* component : components) {
			auto it = VERTEX_FORMAT_MAP.find(component->format_type);
			Assertion(it != VERTEX_FORMAT_MAP.end(),
			          "Unknown vertex format type %d - add to VERTEX_FORMAT_MAP",
			          static_cast<int>(component->format_type));

			const auto& mapping = it->second;

			if (component->format_type == vertex_format_data::MATRIX4) {
				// MATRIX4 spans 4 locations (4 vec4s at 16-byte intervals)
				for (uint32_t row = 0; row < 4; ++row) {
					vk::VertexInputAttributeDescription attr{};
					attr.binding = static_cast<uint32_t>(component->buffer_number);
					attr.location = mapping.location + row;
					attr.format = vk::Format::eR32G32B32A32Sfloat;
					attr.offset = static_cast<uint32_t>(component->offset + row * 16);
					result.attributes.push_back(attr);
				}
			} else {
				vk::VertexInputAttributeDescription attr{};
				attr.binding = static_cast<uint32_t>(component->buffer_number);
				attr.location = mapping.location;
				attr.format = mapping.format;
				attr.offset = static_cast<uint32_t>(component->offset);
				result.attributes.push_back(attr);
			}
		}
	}

	for (const auto& kv : divisorsByBinding) {
		vk::VertexInputBindingDivisorDescription desc{};
		desc.binding = kv.first;
		desc.divisor = kv.second;
		result.divisors.push_back(desc);
	}

	return result;
}

VulkanPipelineManager::VulkanPipelineManager(vk::Device device,
	vk::PipelineLayout pipelineLayout,
	vk::PipelineLayout modelPipelineLayout,
	vk::PipelineCache pipelineCache,
	bool supportsExtendedDynamicState,
	bool supportsExtendedDynamicState2,
	bool supportsExtendedDynamicState3,
	const ExtendedDynamicState3Caps& extDyn3Caps,
	bool supportsVertexAttributeDivisor,
	bool dynamicRenderingEnabled)
	: m_device(device),
	  m_pipelineLayout(pipelineLayout),
	  m_modelPipelineLayout(modelPipelineLayout),
	  m_pipelineCache(pipelineCache),
	  m_supportsExtendedDynamicState(supportsExtendedDynamicState),
	  m_supportsExtendedDynamicState2(supportsExtendedDynamicState2),
	  m_supportsExtendedDynamicState3(supportsExtendedDynamicState3),
	  m_extDyn3Caps(extDyn3Caps),
	  m_supportsVertexAttributeDivisor(supportsVertexAttributeDivisor),
	  m_dynamicRenderingEnabled(dynamicRenderingEnabled)
{
	if (!m_dynamicRenderingEnabled) {
		throw std::runtime_error("Vulkan dynamicRendering feature must be enabled when using renderPass=VK_NULL_HANDLE.");
	}
}

std::vector<vk::DynamicState> VulkanPipelineManager::BuildDynamicStateList(bool supportsExtendedDynamicState3,
	const ExtendedDynamicState3Caps& caps)
{
	std::vector<vk::DynamicState> dynamicStates = {
		vk::DynamicState::eViewport,
		vk::DynamicState::eScissor,
		vk::DynamicState::eCullMode,
		vk::DynamicState::eFrontFace,
		vk::DynamicState::ePrimitiveTopology,
		vk::DynamicState::eDepthTestEnable,
		vk::DynamicState::eDepthWriteEnable,
		vk::DynamicState::eDepthCompareOp,
		vk::DynamicState::eStencilTestEnable,
	};

	if (supportsExtendedDynamicState3) {
		if (caps.colorBlendEnable) {
			dynamicStates.push_back(vk::DynamicState::eColorBlendEnableEXT);
		}
		if (caps.colorWriteMask) {
			dynamicStates.push_back(vk::DynamicState::eColorWriteMaskEXT);
		}
		if (caps.polygonMode) {
			dynamicStates.push_back(vk::DynamicState::ePolygonModeEXT);
		}
		if (caps.rasterizationSamples) {
			dynamicStates.push_back(vk::DynamicState::eRasterizationSamplesEXT);
		}
	}

	return dynamicStates;
}

vk::Pipeline VulkanPipelineManager::getPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout)
{
	auto it = m_pipelines.find(key);
	if (it != m_pipelines.end()) {
		return it->second.get();
	}

	// Pipeline not found, need to create it
	vkprintf("Creating new pipeline - type=%d, blend=%d, layout_hash=0x%x, vert=%p, frag=%p\n",
		static_cast<int>(key.type), static_cast<int>(key.blend_mode), 
		static_cast<unsigned int>(key.layout_hash),
		static_cast<const void*>(modules.vert), static_cast<const void*>(modules.frag));

	auto pipeline = createPipeline(key, modules, layout);
	auto handle = pipeline.get();
	m_pipelines.emplace(key, std::move(pipeline));
	return handle;
}

const VertexInputState& VulkanPipelineManager::getVertexInputState(const vertex_layout& layout)
{
	size_t layoutHash = layout.hash();
	auto it = m_vertexInputCache.find(layoutHash);
	if (it != m_vertexInputCache.end()) {
		return it->second;
	}

	auto state = convertVertexLayoutToVulkan(layout);
	auto result = m_vertexInputCache.emplace(layoutHash, std::move(state));
	return result.first->second;
}

vk::UniquePipeline VulkanPipelineManager::createPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout)
{
	std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages{};
	shaderStages[0].stage = vk::ShaderStageFlagBits::eVertex;
	shaderStages[0].module = modules.vert;
	shaderStages[0].pName = "main";

	shaderStages[1].stage = vk::ShaderStageFlagBits::eFragment;
	shaderStages[1].module = modules.frag;
	shaderStages[1].pName = "main";

	vk::PipelineVertexInputStateCreateInfo vertexInput{};
	vk::PipelineVertexInputDivisorStateCreateInfo divisorInfo{};
	const auto& layoutSpec = getShaderLayoutSpec(key.type);
	const bool useVertexPulling = layoutSpec.vertexInput == VertexInputMode::VertexPulling;

	if (useVertexPulling) {
		vertexInput.vertexBindingDescriptionCount = 0;
		vertexInput.pVertexBindingDescriptions = nullptr;
		vertexInput.vertexAttributeDescriptionCount = 0;
		vertexInput.pVertexAttributeDescriptions = nullptr;

		vkprintf("Vulkan: creating pipeline type %d (vertex pulling) variant 0x%x\n",
		         static_cast<int>(key.type), key.variant_flags);
		vkprintf("  Vertex inputs: none (vertex pulling from storage buffer)\n");
	} else {
		const VertexInputState& vertexInputState = getVertexInputState(layout);

		vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputState.bindings.size());
		vertexInput.pVertexBindingDescriptions = vertexInputState.bindings.data();
		vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputState.attributes.size());
		vertexInput.pVertexAttributeDescriptions = vertexInputState.attributes.data();

		vkprintf("Vulkan: creating pipeline type %d variant 0x%x layout_hash %zu\n",
		         static_cast<int>(key.type), key.variant_flags, key.layout_hash);
		vkprintf("  Vertex bindings (%zu):\n", vertexInputState.bindings.size());
		for (const auto& b : vertexInputState.bindings) {
			vkprintf("    binding %u stride %u rate %s\n",
			         b.binding,
			         b.stride,
			         b.inputRate == vk::VertexInputRate::eInstance ? "instance" : "vertex");
		}
		vkprintf("  Vertex attributes (%zu):\n", vertexInputState.attributes.size());
		for (const auto& a : vertexInputState.attributes) {
			vkprintf("    loc %u bind %u fmt %d offset %u\n",
			         a.location, a.binding, static_cast<int>(a.format), a.offset);
		}
		// Also log the original layout components for cross-checking
		vkprintf("  Vertex layout components (%zu):\n", layout.get_num_vertex_components());
		for (size_t i = 0; i < layout.get_num_vertex_components(); ++i) {
			const auto* comp = layout.get_vertex_component(i);
			vkprintf("    %s buffer %zu offset %zu stride %zu divisor %zu\n",
			         vertexFormatToString(comp->format_type),
			         comp->buffer_number,
			         comp->offset,
			         layout.get_vertex_stride(comp->buffer_number),
			         comp->divisor);
		}

		if (m_supportsVertexAttributeDivisor && !vertexInputState.divisors.empty()) {
			divisorInfo.vertexBindingDivisorCount = static_cast<uint32_t>(vertexInputState.divisors.size());
			divisorInfo.pVertexBindingDivisors = vertexInputState.divisors.data();
			vertexInput.pNext = &divisorInfo;
		} else if (!vertexInputState.divisors.empty() && !m_supportsVertexAttributeDivisor) {
			throw std::runtime_error("vertexAttributeInstanceRateDivisor not enabled but divisor > 1 requested in vertex layout.");
		}
	}

	vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	vk::PipelineViewportStateCreateInfo viewportState{};
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	vk::PipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = vk::PolygonMode::eFill;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = vk::CullModeFlagBits::eBack;
	rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
	rasterizer.depthBiasEnable = VK_FALSE;

	vk::PipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = static_cast<vk::SampleCountFlagBits>(key.sample_count);

	if (key.color_attachment_count == 0) {
		throw std::runtime_error("PipelineKey.color_attachment_count must be at least 1.");
	}

	auto colorBlendAttachment = buildBlendAttachment(key.blend_mode);
	std::vector<vk::PipelineColorBlendAttachmentState> attachments(key.color_attachment_count, colorBlendAttachment);

	vk::PipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = static_cast<uint32_t>(attachments.size());
	colorBlending.pAttachments = attachments.data();

	auto dynamicStates = BuildDynamicStateList(m_supportsExtendedDynamicState3, m_extDyn3Caps);

	vk::PipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	vk::Format colorFormat = static_cast<vk::Format>(key.color_format);
	vk::Format depthFormat = static_cast<vk::Format>(key.depth_format);
	std::vector<vk::Format> colorFormats(key.color_attachment_count, colorFormat);

	Assertion(!colorFormats.empty(), "colorFormats must not be empty");
	if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
		const VertexInputState& vertexInputState = getVertexInputState(layout);
		bool hasLoc0 = false;
		for (const auto& a : vertexInputState.attributes) {
			if (a.location == 0) hasLoc0 = true;
		}
		Assertion(hasLoc0, "Vertex input pipeline created without Location 0 attribute");
	}

	vk::PipelineDepthStencilStateCreateInfo depthStencil{};
	if (depthFormat != vk::Format::eUndefined) {
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;
	} else {
		depthStencil.depthTestEnable = VK_FALSE;
		depthStencil.depthWriteEnable = VK_FALSE;
	}
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	vk::PipelineRenderingCreateInfo renderingInfo{};
	renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
	renderingInfo.pColorAttachmentFormats = colorFormats.data();
	renderingInfo.depthAttachmentFormat = depthFormat;

	vk::GraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	pipelineInfo.pVertexInputState = &vertexInput;
	pipelineInfo.pInputAssemblyState = &inputAssembly;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;

	switch (layoutSpec.pipelineLayout) {
	case PipelineLayoutKind::Model:
		pipelineInfo.layout = m_modelPipelineLayout;
		break;
	case PipelineLayoutKind::Standard:
		pipelineInfo.layout = m_pipelineLayout;
		break;
	}

	pipelineInfo.renderPass = VK_NULL_HANDLE;
	pipelineInfo.pNext = &renderingInfo;

	auto pipelineResult = m_device.createGraphicsPipelineUnique(m_pipelineCache, pipelineInfo);
	if (pipelineResult.result != vk::Result::eSuccess) {
		vkprintf("Vulkan: ERROR - Failed to create graphics pipeline! result=%s, type=%d\n",
			vk::to_string(pipelineResult.result).c_str(), static_cast<int>(key.type));
		throw std::runtime_error("Failed to create Vulkan graphics pipeline.");
	}

	vkprintf("Pipeline created successfully - type=%d, blend=%d, pipeline=%p\n",
		static_cast<int>(key.type), static_cast<int>(key.blend_mode),
		static_cast<const void*>(pipelineResult.value.get()));

	return std::move(pipelineResult.value);
}

} // namespace vulkan
} // namespace graphics
