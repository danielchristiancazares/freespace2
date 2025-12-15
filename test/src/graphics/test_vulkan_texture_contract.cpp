#include "graphics/vulkan/VulkanTextureManager.h"
#include "graphics/vulkan/VulkanFrame.h"

#include <gtest/gtest.h>
#include <type_traits>

using graphics::vulkan::VulkanTextureManager;
using graphics::vulkan::VulkanFrame;

TEST(VulkanTextureContract, DescriptorHasNoDummyFallbackParameters)
{
	using ExpectedSignature = VulkanTextureManager::TextureDescriptorQuery (VulkanTextureManager::*)(
		int, const VulkanTextureManager::SamplerKey&);

	const bool matches = std::is_same<ExpectedSignature, decltype(&VulkanTextureManager::queryDescriptor)>::value;
	EXPECT_TRUE(matches) << "VulkanTextureManager::queryDescriptor must not expose dummy image/sampler fallbacks";
}
