// Compile-time contract: RecordingFrame must not be constructible outside VulkanRenderer.
#include "graphics/vulkan/VulkanFrameFlow.h"

#include <gtest/gtest.h>

#include <type_traits>

TEST(VulkanFrameFlow, Scenario_RecordingFrameIsUnforgeable)
{
	using graphics::vulkan::RecordingFrame;
	using graphics::vulkan::VulkanFrame;

	static_assert(!std::is_constructible_v<RecordingFrame, VulkanFrame&, uint32_t>,
	              "RecordingFrame must only be constructible by VulkanRenderer");

	SUCCEED();
}


