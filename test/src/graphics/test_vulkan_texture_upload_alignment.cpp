#include "graphics/vulkan/VulkanTextureManager.h"

#include <gtest/gtest.h>

TEST(VulkanTextureUploadAlignment, R8TextureArrayOffsetsAre4ByteAligned)
{
	constexpr uint32_t width = 3;
	constexpr uint32_t height = 3;
	constexpr uint32_t layers = 3;

	const auto layout =
		graphics::vulkan::buildImmediateUploadLayout(width, height, vk::Format::eR8Unorm, layers);

	EXPECT_EQ(layout.layerSize, 9u);
	ASSERT_EQ(layout.layerOffsets.size(), layers);

	EXPECT_EQ(layout.layerOffsets[0], 0u);
	EXPECT_EQ(layout.layerOffsets[1], 12u);
	EXPECT_EQ(layout.layerOffsets[2], 24u);

	for (auto offset : layout.layerOffsets) {
		EXPECT_EQ(offset % 4u, 0u) << "vkCmdCopyBufferToImage requires bufferOffset % 4 == 0";
	}

	EXPECT_EQ(layout.totalSize, 36u);
}

