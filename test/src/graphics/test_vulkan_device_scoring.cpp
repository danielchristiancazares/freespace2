#include "graphics/vulkan/VulkanDevice.h"

#include <gtest/gtest.h>

using graphics::vulkan::deviceTypeScore;
using graphics::vulkan::PhysicalDeviceValues;
using graphics::vulkan::scoreDevice;

// Helper to create a minimal PhysicalDeviceValues for testing
static PhysicalDeviceValues makeDevice(vk::PhysicalDeviceType type, uint32_t apiVersion) {
	PhysicalDeviceValues device{};
	device.properties.deviceType = type;
	device.properties.apiVersion = apiVersion;
	return device;
}

TEST(VulkanDeviceScoring, DiscreteGpuHasHighestTypeScore)
{
	EXPECT_GT(deviceTypeScore(vk::PhysicalDeviceType::eDiscreteGpu),
	          deviceTypeScore(vk::PhysicalDeviceType::eIntegratedGpu));
	EXPECT_GT(deviceTypeScore(vk::PhysicalDeviceType::eIntegratedGpu),
	          deviceTypeScore(vk::PhysicalDeviceType::eVirtualGpu));
	EXPECT_GT(deviceTypeScore(vk::PhysicalDeviceType::eVirtualGpu),
	          deviceTypeScore(vk::PhysicalDeviceType::eCpu));
}

TEST(VulkanDeviceScoring, DiscreteBeatsIntegratedRegardlessOfVersion)
{
	// Discrete GPU with Vulkan 1.3 should beat integrated GPU with Vulkan 1.4
	auto discreteVk13 = makeDevice(vk::PhysicalDeviceType::eDiscreteGpu, VK_MAKE_API_VERSION(0, 1, 3, 0));
	auto integratedVk14 = makeDevice(vk::PhysicalDeviceType::eIntegratedGpu, VK_MAKE_API_VERSION(0, 1, 4, 0));

	EXPECT_GT(scoreDevice(discreteVk13), scoreDevice(integratedVk14))
		<< "Discrete GPU with older Vulkan version should beat integrated GPU with newer version";
}

TEST(VulkanDeviceScoring, SameTypePrefersHigherVersion)
{
	// Between two discrete GPUs, prefer the one with higher Vulkan version
	auto discreteVk13 = makeDevice(vk::PhysicalDeviceType::eDiscreteGpu, VK_MAKE_API_VERSION(0, 1, 3, 0));
	auto discreteVk14 = makeDevice(vk::PhysicalDeviceType::eDiscreteGpu, VK_MAKE_API_VERSION(0, 1, 4, 0));

	EXPECT_GT(scoreDevice(discreteVk14), scoreDevice(discreteVk13))
		<< "Same device type should prefer higher Vulkan version";
}

TEST(VulkanDeviceScoring, PatchVersionIgnored)
{
	// Patch version should not affect scoring
	auto vk14_0 = makeDevice(vk::PhysicalDeviceType::eDiscreteGpu, VK_MAKE_API_VERSION(0, 1, 4, 0));
	auto vk14_290 = makeDevice(vk::PhysicalDeviceType::eDiscreteGpu, VK_MAKE_API_VERSION(0, 1, 4, 290));

	EXPECT_EQ(scoreDevice(vk14_0), scoreDevice(vk14_290))
		<< "Patch version should not affect device score";
}

TEST(VulkanDeviceScoring, IntegratedBeatsVirtual)
{
	auto integrated = makeDevice(vk::PhysicalDeviceType::eIntegratedGpu, VK_MAKE_API_VERSION(0, 1, 3, 0));
	auto virtualGpu = makeDevice(vk::PhysicalDeviceType::eVirtualGpu, VK_MAKE_API_VERSION(0, 1, 4, 0));

	EXPECT_GT(scoreDevice(integrated), scoreDevice(virtualGpu))
		<< "Integrated GPU should beat virtual GPU regardless of version";
}

TEST(VulkanDeviceScoring, ScoreValuesAreSane)
{
	// Verify the actual score breakdown is reasonable
	auto discrete14 = makeDevice(vk::PhysicalDeviceType::eDiscreteGpu, VK_MAKE_API_VERSION(0, 1, 4, 0));
	auto integrated14 = makeDevice(vk::PhysicalDeviceType::eIntegratedGpu, VK_MAKE_API_VERSION(0, 1, 4, 0));

	uint32_t discreteScore = scoreDevice(discrete14);
	uint32_t integratedScore = scoreDevice(integrated14);

	// Device type contributes millions, version contributes hundreds
	// Discrete (3) * 1M + 1*100 + 4 = 3,000,104
	// Integrated (2) * 1M + 1*100 + 4 = 2,000,104
	EXPECT_EQ(discreteScore, 3000104u);
	EXPECT_EQ(integratedScore, 2000104u);
}
