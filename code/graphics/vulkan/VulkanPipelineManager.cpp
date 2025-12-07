#include "VulkanPipelineManager.h"

#include "VulkanVertexTypes.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace graphics {
namespace vulkan {

// Maps vertex_format_data::vertex_format to Vulkan format and shader location
struct VertexFormatMapping {
	vk::Format format;
	uint32_t location;
	uint32_t componentCount; // Number of components (1-4)
};

// Maps FSO vertex format types to Vulkan formats and locations
// Location mapping follows OpenGL convention:
// 0 = POSITION, 1 = COLOR, 2 = TEXCOORD, 3 = NORMAL, 4 = TANGENT, etc.
static const std::unordered_map<vertex_format_data::vertex_format, VertexFormatMapping> VERTEX_FORMAT_MAP = {
	// Position formats -> location 0
	{vertex_format_data::POSITION4, {vk::Format::eR32G32B32A32Sfloat, 0, 4}},
	{vertex_format_data::POSITION3, {vk::Format::eR32G32B32Sfloat, 0, 3}},
	{vertex_format_data::POSITION2, {vk::Format::eR32G32Sfloat, 0, 2}},
	{vertex_format_data::SCREEN_POS, {vk::Format::eR32G32Sfloat, 0, 2}},

	// Color formats -> location 1
	{vertex_format_data::COLOR3, {vk::Format::eR8G8B8Unorm, 1, 3}},
	{vertex_format_data::COLOR4, {vk::Format::eR8G8B8A8Unorm, 1, 4}},
	{vertex_format_data::COLOR4F, {vk::Format::eR32G32B32A32Sfloat, 1, 4}},

	// Texture coordinate formats -> location 2
	{vertex_format_data::TEX_COORD2, {vk::Format::eR32G32Sfloat, 2, 2}},
	{vertex_format_data::TEX_COORD4, {vk::Format::eR32G32B32A32Sfloat, 2, 4}},

	// Normal -> location 3
	{vertex_format_data::NORMAL, {vk::Format::eR32G32B32Sfloat, 3, 3}},

	// Tangent -> location 4
	{vertex_format_data::TANGENT, {vk::Format::eR32G32B32A32Sfloat, 4, 4}},

	// Model ID -> location 5
	{vertex_format_data::MODEL_ID, {vk::Format::eR32Sfloat, 5, 1}},

	// Radius -> location 6
	{vertex_format_data::RADIUS, {vk::Format::eR32Sfloat, 6, 1}},

	// UVec -> location 7
	{vertex_format_data::UVEC, {vk::Format::eR32G32B32Sfloat, 7, 3}},

	// Matrix4 -> locations 8-11 (4 vec4s)
	{vertex_format_data::MATRIX4, {vk::Format::eR32G32B32A32Sfloat, 8, 4}}, // handled specially
};

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

	// Group components by buffer_number to create bindings
	std::unordered_map<size_t, std::vector<const vertex_format_data*>> componentsByBuffer;

	for (size_t i = 0; i < layout.get_num_vertex_components(); ++i) {
		const vertex_format_data* component = layout.get_vertex_component(i);
		componentsByBuffer[component->buffer_number].push_back(component);
	}

	std::unordered_map<uint32_t, uint32_t> divisorsByBinding;

	// Create bindings for each buffer
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

		// Check if any component has divisor (instanced)
		for (const auto* comp : components) {
			if (comp->divisor != 0) {
				binding.inputRate = vk::VertexInputRate::eInstance;
				divisorsByBinding[binding.binding] = static_cast<uint32_t>(comp->divisor);
				break;
			}
		}

		result.bindings.push_back(binding);

		// Create attributes for each component
		for (const auto* component : components) {
			auto it = VERTEX_FORMAT_MAP.find(component->format_type);
			if (it == VERTEX_FORMAT_MAP.end()) {
				// Unknown format - skip or use default
				continue;
			}

			const auto& mapping = it->second;

			// Handle MATRIX4 specially (spans 4 locations)
			if (component->format_type == vertex_format_data::MATRIX4) {
				// Matrix4 is stored as 4 vec4s, each at 16-byte offset
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
		vk::VertexInputBindingDivisorDescriptionEXT desc{};
		desc.binding = kv.first;
		desc.divisor = kv.second;
		result.divisors.push_back(desc);
	}

	return result;
}

VulkanPipelineManager::VulkanPipelineManager(vk::Device device,
	vk::PipelineLayout pipelineLayout,
	vk::PipelineCache pipelineCache,
	bool supportsExtendedDynamicState,
	bool supportsExtendedDynamicState2,
	bool supportsExtendedDynamicState3,
	const ExtendedDynamicState3Caps& extDyn3Caps,
	bool supportsVertexAttributeDivisor)
	: m_device(device),
	  m_pipelineLayout(pipelineLayout),
	  m_pipelineCache(pipelineCache),
	  m_supportsExtendedDynamicState(supportsExtendedDynamicState),
	  m_supportsExtendedDynamicState2(supportsExtendedDynamicState2),
	  m_supportsExtendedDynamicState3(supportsExtendedDynamicState3),
	  m_extDyn3Caps(extDyn3Caps),
	  m_supportsVertexAttributeDivisor(supportsVertexAttributeDivisor)
{
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

	// Convert vertex_layout to Vulkan vertex input state
	const VertexInputState& vertexInputState = getVertexInputState(layout);

	vk::PipelineVertexInputStateCreateInfo vertexInput{};
	vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputState.bindings.size());
	vertexInput.pVertexBindingDescriptions = vertexInputState.bindings.data();
	vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputState.attributes.size());
	vertexInput.pVertexAttributeDescriptions = vertexInputState.attributes.data();
	vk::PipelineVertexInputDivisorStateCreateInfoEXT divisorInfo{};
	if (m_supportsVertexAttributeDivisor && !vertexInputState.divisors.empty()) {
		divisorInfo.vertexBindingDivisorCount = static_cast<uint32_t>(vertexInputState.divisors.size());
		divisorInfo.pVertexBindingDivisors = vertexInputState.divisors.data();
		vertexInput.pNext = &divisorInfo;
	} else if (!vertexInputState.divisors.empty() && !m_supportsVertexAttributeDivisor) {
		throw std::runtime_error("VK_EXT_vertex_attribute_divisor not available but divisor > 1 requested in vertex layout.");
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
	pipelineInfo.layout = m_pipelineLayout;
	pipelineInfo.renderPass = VK_NULL_HANDLE;
	pipelineInfo.pNext = &renderingInfo;

	auto pipelineResult = m_device.createGraphicsPipelineUnique(m_pipelineCache, pipelineInfo);
	if (pipelineResult.result != vk::Result::eSuccess) {
		throw std::runtime_error("Failed to create Vulkan graphics pipeline.");
	}

	return std::move(pipelineResult.value);
}

} // namespace vulkan
} // namespace graphics
