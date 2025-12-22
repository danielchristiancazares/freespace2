#include "graphics/vulkan/VulkanTextureManager.h"

#include <gtest/gtest.h>

#include <type_traits>

using graphics::vulkan::VulkanTextureManager;

// Contract tests: verify that the Vulkan backend provides stable synthetic handles for builtin textures.
// These tests are compile-time only and do not require Vulkan runtime objects.

TEST(VulkanFallbackTextureContract, BuiltinHandlesAreNegativeAndDistinct)
{
	constexpr int fallback = VulkanTextureManager::kFallbackTextureHandle;
	constexpr int white = VulkanTextureManager::kDefaultTextureHandle;
	constexpr int normal = VulkanTextureManager::kDefaultNormalTextureHandle;
	constexpr int spec = VulkanTextureManager::kDefaultSpecTextureHandle;

	EXPECT_LT(fallback, 0);
	EXPECT_LT(white, 0);
	EXPECT_LT(normal, 0);
	EXPECT_LT(spec, 0);

	EXPECT_NE(fallback, white);
	EXPECT_NE(fallback, normal);
	EXPECT_NE(fallback, spec);
	EXPECT_NE(white, normal);
	EXPECT_NE(white, spec);
	EXPECT_NE(normal, spec);
}

TEST(VulkanFallbackTextureContract, GetFallbackTextureHandleSignature)
{
	using ReturnType = decltype(std::declval<VulkanTextureManager>().getFallbackTextureHandle());
	constexpr bool correct_return = std::is_same_v<ReturnType, int>;
	EXPECT_TRUE(correct_return) << "getFallbackTextureHandle() must return int";
}

TEST(VulkanTextureHelpers, CalculateCompressedSizeBC1)
{
	using graphics::vulkan::calculateCompressedSize;

	// 4x4 BC1 = 1 block * 8 bytes
	EXPECT_EQ(calculateCompressedSize(4, 4, vk::Format::eBc1RgbaUnormBlock), 8u);

	// 8x4 BC1 = 2 blocks * 8 bytes
	EXPECT_EQ(calculateCompressedSize(8, 4, vk::Format::eBc1RgbaUnormBlock), 16u);
}

