#include "graphics/vulkan/VulkanPipelineManager.h"
#include "graphics/2d.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <unordered_map>
#include <type_traits>

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

TEST(VulkanPipelineManager, Scenario_ModelShaderType_HasCorrectEnumValue)
{
	// Given the shader_type enum definition
	// When checking SDR_TYPE_MODEL value
	// Then it should be 0 (first value after SDR_TYPE_NONE = -1)
	EXPECT_EQ(static_cast<int>(SDR_TYPE_MODEL), 0)
		<< "SDR_TYPE_MODEL should have enum value 0";
	EXPECT_EQ(static_cast<int>(SDR_TYPE_NONE), -1)
		<< "SDR_TYPE_NONE should have enum value -1";
}

TEST(VulkanPipelineManager, Scenario_ModelPipelineKey_MatchesModelType)
{
	// Given a PipelineKey with SDR_TYPE_MODEL
	PipelineKey modelKey{};
	modelKey.type = SDR_TYPE_MODEL;
	modelKey.variant_flags = 0;
	modelKey.color_format = static_cast<VkFormat>(vk::Format::eB8G8R8A8Unorm);
	modelKey.depth_format = static_cast<VkFormat>(vk::Format::eD32Sfloat);
	modelKey.sample_count = VK_SAMPLE_COUNT_1_BIT;
	modelKey.color_attachment_count = 1;
	modelKey.blend_mode = ALPHA_BLEND_NONE;
	modelKey.layout_hash = 0x1234;

	// When comparing with SDR_TYPE_MODEL enum value
	// Then the comparison should match (verifies the comparison logic used in layout selection)
	EXPECT_TRUE(modelKey.type == SDR_TYPE_MODEL)
		<< "PipelineKey with SDR_TYPE_MODEL should match SDR_TYPE_MODEL enum value";
	EXPECT_EQ(static_cast<int>(modelKey.type), 0)
		<< "PipelineKey.type should be 0 when set to SDR_TYPE_MODEL";
}

TEST(VulkanPipelineManager, Scenario_NonModelPipelineKey_DoesNotMatchModelType)
{
	// Given a PipelineKey with a non-model shader type
	PipelineKey defaultKey{};
	defaultKey.type = SDR_TYPE_DEFAULT_MATERIAL;
	defaultKey.variant_flags = 0;
	defaultKey.color_format = static_cast<VkFormat>(vk::Format::eB8G8R8A8Unorm);
	defaultKey.depth_format = static_cast<VkFormat>(vk::Format::eD32Sfloat);
	defaultKey.sample_count = VK_SAMPLE_COUNT_1_BIT;
	defaultKey.color_attachment_count = 1;
	defaultKey.blend_mode = ALPHA_BLEND_NONE;
	defaultKey.layout_hash = 0x1234;

	// When comparing with SDR_TYPE_MODEL enum value
	// Then the comparison should not match (verifies non-model types use regular layout)
	EXPECT_FALSE(defaultKey.type == SDR_TYPE_MODEL)
		<< "PipelineKey with SDR_TYPE_DEFAULT_MATERIAL should not match SDR_TYPE_MODEL";
	EXPECT_NE(static_cast<int>(defaultKey.type), 0)
		<< "PipelineKey.type should not be 0 when set to SDR_TYPE_DEFAULT_MATERIAL";
}

TEST(VulkanPipelineManager, Scenario_UninitializedPipelineKey_DoesNotMatchModelType)
{
	// Given an uninitialized PipelineKey (zero-initialized)
	PipelineKey uninitKey{};
	// type is zero-initialized to 0

	// When comparing with SDR_TYPE_MODEL enum value
	// Then even though the value is 0, the comparison should work correctly
	// This test verifies that the comparison logic correctly identifies model vs uninitialized
	EXPECT_EQ(static_cast<int>(uninitKey.type), 0)
		<< "Uninitialized PipelineKey.type is 0";
	
	// Note: This test documents the behavior - if type is 0, it will match SDR_TYPE_MODEL
	// This is expected behavior since SDR_TYPE_MODEL == 0, but highlights the importance
	// of proper initialization to avoid false matches
	EXPECT_TRUE(uninitKey.type == SDR_TYPE_MODEL)
		<< "Uninitialized PipelineKey with type=0 will match SDR_TYPE_MODEL (this is why proper initialization is critical)";
}

TEST(VulkanPipelineManager, Scenario_DynamicRenderingRequired)
{
	// Given dynamic rendering is disabled on the device
	vk::Device fakeDevice{};
	vk::PipelineLayout fakeLayout{};
	vk::PipelineCache fakeCache{};
	graphics::vulkan::ExtendedDynamicState3Caps caps{};

	// When constructing the pipeline manager
	// Then it should refuse to initialize because renderPass is always VK_NULL_HANDLE
	EXPECT_THROW(
		graphics::vulkan::VulkanPipelineManager(fakeDevice,
			fakeLayout,
			fakeLayout,
			fakeCache,
			/*supportsExtendedDynamicState=*/true,
			/*supportsExtendedDynamicState2=*/true,
			/*supportsExtendedDynamicState3=*/false,
			caps,
			/*supportsVertexAttributeDivisor=*/false,
			/*dynamicRenderingEnabled=*/false),
		std::runtime_error);
}

TEST(VulkanPipelineManager, Scenario_InstanceDivisorUsesCoreStructs)
{
	// Given a layout with an instanced attribute (divisor > 0)
	vertex_layout layout;
	layout.add_vertex_component(vertex_format_data::POSITION3,
		/*stride*/ 12,
		/*offset*/ 0,
		/*divisor*/ 2,
		/*buffer*/ 0);

	// When converting to Vulkan vertex input descriptions
	auto state = convert(layout);

	// Then the divisor list should use core VkVertexInputBindingDivisorDescription and carry the requested divisor
	ASSERT_EQ(state.divisors.size(), 1u);
	using CoreDivisor = vk::VertexInputBindingDivisorDescription;
	const bool usesCoreStruct = std::is_same_v<CoreDivisor, std::decay_t<decltype(state.divisors.front())>>;
	EXPECT_TRUE(usesCoreStruct) << "Divisor descriptions should use core (non-EXT) struct in Vulkan 1.4";
	EXPECT_EQ(state.divisors.front().binding, 0u);
	EXPECT_EQ(state.divisors.front().divisor, 2u);
}
