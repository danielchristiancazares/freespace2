#include "graphics/vulkan/VulkanPipelineManager.h"

#include <gtest/gtest.h>
#include <algorithm>

using graphics::vulkan::ExtendedDynamicState3Caps;
using graphics::vulkan::VulkanPipelineManager;

static bool contains(const std::vector<vk::DynamicState>& states, vk::DynamicState needle)
{
	return std::find(states.begin(), states.end(), needle) != states.end();
}

TEST(VulkanPipelineDynamicState, BaseStatesWithoutEDS3)
{
	ExtendedDynamicState3Caps caps{};
	auto states = VulkanPipelineManager::BuildDynamicStateList(false, caps);

	EXPECT_FALSE(contains(states, vk::DynamicState::eColorBlendEnableEXT));
	EXPECT_FALSE(contains(states, vk::DynamicState::eColorWriteMaskEXT));
	EXPECT_FALSE(contains(states, vk::DynamicState::ePolygonModeEXT));
	EXPECT_FALSE(contains(states, vk::DynamicState::eRasterizationSamplesEXT));

	// Base dynamic states should always be present
	EXPECT_TRUE(contains(states, vk::DynamicState::eViewport));
	EXPECT_TRUE(contains(states, vk::DynamicState::eDepthTestEnable));
	EXPECT_TRUE(contains(states, vk::DynamicState::ePrimitiveTopology));
}

TEST(VulkanPipelineDynamicState, AddsEDS3StatesWhenSupported)
{
	ExtendedDynamicState3Caps caps{};
	caps.colorBlendEnable = true;
	caps.colorWriteMask = true;
	caps.polygonMode = true;
	caps.rasterizationSamples = true;

	auto states = VulkanPipelineManager::BuildDynamicStateList(true, caps);

	EXPECT_TRUE(contains(states, vk::DynamicState::eColorBlendEnableEXT));
	EXPECT_TRUE(contains(states, vk::DynamicState::eColorWriteMaskEXT));
	EXPECT_TRUE(contains(states, vk::DynamicState::ePolygonModeEXT));
	EXPECT_TRUE(contains(states, vk::DynamicState::eRasterizationSamplesEXT));
}

TEST(VulkanPipelineDynamicState, SkipsUnsupportedEDS3Caps)
{
	ExtendedDynamicState3Caps caps{};
	caps.colorBlendEnable = true;
	// others remain false

	auto states = VulkanPipelineManager::BuildDynamicStateList(true, caps);

	EXPECT_TRUE(contains(states, vk::DynamicState::eColorBlendEnableEXT));
	EXPECT_FALSE(contains(states, vk::DynamicState::eColorWriteMaskEXT));
	EXPECT_FALSE(contains(states, vk::DynamicState::ePolygonModeEXT));
	EXPECT_FALSE(contains(states, vk::DynamicState::eRasterizationSamplesEXT));
}
