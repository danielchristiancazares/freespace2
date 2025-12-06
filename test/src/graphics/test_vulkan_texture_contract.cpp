#include "graphics/vulkan/VulkanTextureManager.h"
#include "graphics/vulkan/VulkanFrame.h"

#include <gtest/gtest.h>
#include <type_traits>

using graphics::vulkan::VulkanTextureManager;
using graphics::vulkan::VulkanFrame;

TEST(VulkanTextureContract, DescriptorHasNoDummyFallbackParameters)
{
	using ExpectedSignature = vk::DescriptorImageInfo (VulkanTextureManager::*)(
		int, VulkanFrame&, vk::CommandBuffer, uint32_t, bool, const VulkanTextureManager::SamplerKey&);

	const bool matches = std::is_same<ExpectedSignature, decltype(&VulkanTextureManager::getDescriptor)>::value;
	EXPECT_TRUE(matches) << "VulkanTextureManager::getDescriptor must not expose dummy image/sampler fallbacks";
}
