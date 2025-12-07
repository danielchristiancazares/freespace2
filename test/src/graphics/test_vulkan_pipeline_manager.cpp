#include "graphics/vulkan/VulkanPipelineManager.h"
#include "graphics/2d.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <unordered_map>

using graphics::vulkan::PipelineKey;
using graphics::vulkan::VertexInputState;
using graphics::vulkan::convertVertexLayoutToVulkan;

namespace {

VertexInputState convert(const vertex_layout& layout)
{
	return convertVertexLayoutToVulkan(layout);
}

const vk::VertexInputAttributeDescription* find_attr_by_location(
	const std::vector<vk::VertexInputAttributeDescription>& attrs, uint32_t location)
{
	auto it = std::find_if(attrs.begin(), attrs.end(), [location](const auto& a) { return a.location == location; });
	return it == attrs.end() ? nullptr : &(*it);
}

} // namespace

TEST(VulkanPipelineManager, Scenario_Matrix4Layout_EmitsFourRowAttributes)
{
	// Given a vertex layout that includes a MATRIX4 component (e.g., per-instance transform)
	vertex_layout layout;
	layout.add_vertex_component(vertex_format_data::MATRIX4, /*stride*/ 64, /*offset*/ 0, /*divisor*/ 1, /*buffer*/ 0);

	// When converting to Vulkan vertex input descriptions
	auto state = convert(layout);

	// Then four attributes should be emitted at consecutive locations, each a vec4 with 16-byte stride
	EXPECT_EQ(state.attributes.size(), 4u) << "Expected four row attributes for MATRIX4";

	const uint32_t baseLoc = 8;
	for (uint32_t row = 0; row < 4; ++row) {
		auto* attr = find_attr_by_location(state.attributes, baseLoc + row);
		ASSERT_NE(attr, nullptr) << "Missing attribute at location " << baseLoc + row;
		EXPECT_EQ(attr->binding, 0u);
		EXPECT_EQ(attr->format, vk::Format::eR32G32B32A32Sfloat);
		EXPECT_EQ(attr->offset, row * 16u);
	}
}

TEST(VulkanPipelineManager, Scenario_ScreenPos_UsesFloatFormat)
{
	// Given a layout with SCREEN_POS used for 2D vertices
	vertex_layout layout;
	layout.add_vertex_component(vertex_format_data::SCREEN_POS, /*stride*/ 8, /*offset*/ 0, /*divisor*/ 0, /*buffer*/ 0);

	// When converting to Vulkan
	auto state = convert(layout);

	// Then the attribute should be provided as two floats (not ints) to match shader expectations
	ASSERT_EQ(state.attributes.size(), 1u);
	const auto& attr = state.attributes.front();
	EXPECT_EQ(attr.location, 0u);
	EXPECT_EQ(attr.format, vk::Format::eR32G32Sfloat);
}

TEST(VulkanPipelineManager, Scenario_PipelineKey_ChangesWithSampleCountAndBlend)
{
	// Given two render paths that differ in sample count and blend state
	const uint32_t sampleCountA = 1;
	const uint32_t sampleCountB = 4;

	PipelineKey a{};
	a.type = shader_type::SDR_TYPE_DEFAULT_MATERIAL;
	a.variant_flags = 0;
	a.color_format = static_cast<VkFormat>(vk::Format::eB8G8R8A8Unorm);
	a.depth_format = static_cast<VkFormat>(vk::Format::eD32Sfloat);
	a.sample_count = static_cast<VkSampleCountFlagBits>(sampleCountA);
	a.blend_mode = ALPHA_BLEND_NONE;
	a.color_attachment_count = 1;
	a.layout_hash = 0x1234;

	PipelineKey b = a;
	b.sample_count = static_cast<VkSampleCountFlagBits>(sampleCountB);
	b.blend_mode = ALPHA_BLEND_ALPHA_BLEND_ALPHA;

	EXPECT_FALSE(a == b) << "PipelineKey should differ when sample count, blend mode, or shader modules differ.";
}
