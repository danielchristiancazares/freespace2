#include "graphics/vulkan/VulkanTextureManager.h"

#include <gtest/gtest.h>
#include <type_traits>

using graphics::vulkan::VulkanTextureManager;

TEST(VulkanTextureContract, GetTextureDescriptorInfoHasNoDummyFallbackParameters)
{
		using graphics::vulkan::TextureId;
		using ExpectedSignature = std::optional<vk::DescriptorImageInfo> (VulkanTextureManager::*)(
			TextureId, const VulkanTextureManager::SamplerKey&) const;

		const bool matches = std::is_same_v<ExpectedSignature, decltype(&VulkanTextureManager::tryGetResidentDescriptor)>;
		EXPECT_TRUE(matches) << "VulkanTextureManager::tryGetResidentDescriptor must not expose dummy image/sampler fallbacks";
}
