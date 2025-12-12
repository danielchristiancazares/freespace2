// Test-first: ensure every shader_type has an explicit layout contract
#include "graphics/vulkan/VulkanLayoutContracts.h"

#include <gtest/gtest.h>

using graphics::vulkan::PipelineLayoutKind;
using graphics::vulkan::VertexInputMode;
using graphics::vulkan::getShaderLayoutSpec;
using graphics::vulkan::getShaderLayoutSpecs;

TEST(VulkanLayoutContracts, Scenario_AllShaderTypesHaveSpecs)
{
	// Given the full shader_type enum
	// When requesting specs for each entry
	// Then we should get a one-to-one mapping with no gaps
	const auto& specs = getShaderLayoutSpecs();
	ASSERT_EQ(specs.size(), static_cast<size_t>(NUM_SHADER_TYPES))
		<< "Every shader_type (excluding SDR_TYPE_NONE) must have an explicit layout spec";

	for (int i = 0; i < static_cast<int>(NUM_SHADER_TYPES); ++i) {
		const auto type = static_cast<shader_type>(i);
		const auto& spec = getShaderLayoutSpec(type);
		EXPECT_EQ(spec.type, type) << "Spec index should match shader_type value";
	}
}

TEST(VulkanLayoutContracts, Scenario_ModelUsesModelLayoutAndVertexPulling)
{
	// Given the model shader
	const auto& spec = getShaderLayoutSpec(SDR_TYPE_MODEL);

	// Then it should use the model pipeline layout and vertex pulling
	EXPECT_EQ(spec.pipelineLayout, PipelineLayoutKind::Model);
	EXPECT_EQ(spec.vertexInput, VertexInputMode::VertexPulling);
}

TEST(VulkanLayoutContracts, Scenario_DefaultMaterialUsesStandardLayout)
{
	// Given the default material shader
	const auto& spec = getShaderLayoutSpec(SDR_TYPE_DEFAULT_MATERIAL);

	// Then it should use the standard pipeline layout with vertex attributes
	EXPECT_EQ(spec.pipelineLayout, PipelineLayoutKind::Standard);
	EXPECT_EQ(spec.vertexInput, VertexInputMode::VertexAttributes);
}










