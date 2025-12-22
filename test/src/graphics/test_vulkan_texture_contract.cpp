#include "graphics/vulkan/VulkanTextureManager.h"

#include <gtest/gtest.h>
#include <type_traits>

using graphics::vulkan::VulkanTextureManager;

TEST(VulkanTextureContract, GetTextureDescriptorInfoHasNoDummyFallbackParameters)
{
	using ExpectedSignature = vk::DescriptorImageInfo (VulkanTextureManager::*)(
		int, const VulkanTextureManager::SamplerKey&);

	const bool matches = std::is_same_v<ExpectedSignature, decltype(&VulkanTextureManager::getTextureDescriptorInfo)>;
	EXPECT_TRUE(matches) << "VulkanTextureManager::getTextureDescriptorInfo must not expose dummy image/sampler fallbacks";
}
