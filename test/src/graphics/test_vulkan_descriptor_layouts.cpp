#include "graphics/vulkan/VulkanModelValidation.h"

#include <gtest/gtest.h>
#include <stdexcept>

using graphics::vulkan::EnsurePushDescriptorSupport;
using graphics::vulkan::ValidatePushDescriptorSupport;

TEST(VulkanPushDescriptors, FailsWhenFeatureDisabled)
{
	vk::PhysicalDeviceVulkan14Features features{};

	EXPECT_FALSE(ValidatePushDescriptorSupport(features));
}

TEST(VulkanPushDescriptors, SucceedsWhenFeatureEnabled)
{
	vk::PhysicalDeviceVulkan14Features features{};
	features.pushDescriptor = VK_TRUE;

	EXPECT_TRUE(ValidatePushDescriptorSupport(features));
}

TEST(VulkanPushDescriptors, EnsureThrowsWithoutSupport)
{
	vk::PhysicalDeviceVulkan14Features features{};

	EXPECT_THROW(EnsurePushDescriptorSupport(features), std::runtime_error);
}
