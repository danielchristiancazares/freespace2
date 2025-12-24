#include "graphics/vulkan/VulkanTextureManager.h"

#include <gtest/gtest.h>

#include <type_traits>

using graphics::vulkan::VulkanTextureManager;

// Contract tests: verify the Vulkan backend provides stable bindless reserved slots and
// a builtin-descriptor API (no sentinel "fake handles" for defaults).

TEST(VulkanFallbackTextureContract, BindlessReservedSlotsAreDistinctAndPinned)
{
	using namespace graphics::vulkan;

	constexpr uint32_t fallback = kBindlessTextureSlotFallback;
	constexpr uint32_t base = kBindlessTextureSlotDefaultBase;
	constexpr uint32_t normal = kBindlessTextureSlotDefaultNormal;
	constexpr uint32_t spec = kBindlessTextureSlotDefaultSpec;

	EXPECT_LT(fallback, kBindlessFirstDynamicTextureSlot);
	EXPECT_LT(base, kBindlessFirstDynamicTextureSlot);
	EXPECT_LT(normal, kBindlessFirstDynamicTextureSlot);
	EXPECT_LT(spec, kBindlessFirstDynamicTextureSlot);

	EXPECT_NE(fallback, base);
	EXPECT_NE(fallback, normal);
	EXPECT_NE(fallback, spec);
	EXPECT_NE(base, normal);
	EXPECT_NE(base, spec);
	EXPECT_NE(normal, spec);
}

TEST(VulkanFallbackTextureContract, BuiltinDescriptorApiSignatures)
{
	using SamplerKey = VulkanTextureManager::SamplerKey;

	using FallbackReturn = decltype(std::declval<VulkanTextureManager>().fallbackDescriptor(std::declval<SamplerKey>()));
	using DefaultBaseReturn = decltype(std::declval<VulkanTextureManager>().defaultBaseDescriptor(std::declval<SamplerKey>()));
	using DefaultNormalReturn = decltype(std::declval<VulkanTextureManager>().defaultNormalDescriptor(std::declval<SamplerKey>()));
	using DefaultSpecReturn = decltype(std::declval<VulkanTextureManager>().defaultSpecDescriptor(std::declval<SamplerKey>()));

	EXPECT_TRUE((std::is_same_v<FallbackReturn, vk::DescriptorImageInfo>));
	EXPECT_TRUE((std::is_same_v<DefaultBaseReturn, vk::DescriptorImageInfo>));
	EXPECT_TRUE((std::is_same_v<DefaultNormalReturn, vk::DescriptorImageInfo>));
	EXPECT_TRUE((std::is_same_v<DefaultSpecReturn, vk::DescriptorImageInfo>));
}

TEST(VulkanTextureHelpers, CalculateCompressedSizeBC1)
{
	using graphics::vulkan::calculateCompressedSize;

	// 4x4 BC1 = 1 block * 8 bytes
	EXPECT_EQ(calculateCompressedSize(4, 4, vk::Format::eBc1RgbaUnormBlock), 8u);

	// 8x4 BC1 = 2 blocks * 8 bytes
	EXPECT_EQ(calculateCompressedSize(8, 4, vk::Format::eBc1RgbaUnormBlock), 16u);
}

