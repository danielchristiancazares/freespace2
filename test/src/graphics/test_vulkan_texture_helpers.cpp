#include "graphics/vulkan/VulkanTextureManager.h"

#include <gtest/gtest.h>

using graphics::vulkan::calculateCompressedSize;

TEST(VulkanTextureHelpers, CompressedSizeWholeBlocks)
{
	EXPECT_EQ(calculateCompressedSize(4, 4, vk::Format::eBc1RgbaUnormBlock), 8u);
	EXPECT_EQ(calculateCompressedSize(4, 4, vk::Format::eBc3UnormBlock), 16u);
	EXPECT_EQ(calculateCompressedSize(4, 4, vk::Format::eBc7UnormBlock), 16u);
}

TEST(VulkanTextureHelpers, CompressedSizePartialBlocks)
{
	// Even sub-4 sizes still allocate one block.
	EXPECT_EQ(calculateCompressedSize(2, 2, vk::Format::eBc1RgbaUnormBlock), 8u);
	EXPECT_EQ(calculateCompressedSize(1, 3, vk::Format::eBc3UnormBlock), 16u);

	// Non-square, rounded up per dimension.
	EXPECT_EQ(calculateCompressedSize(5, 7, vk::Format::eBc1RgbaUnormBlock),
	          /* (ceil(5/4)=2) * (ceil(7/4)=2) * 8 */ 32u);
}
