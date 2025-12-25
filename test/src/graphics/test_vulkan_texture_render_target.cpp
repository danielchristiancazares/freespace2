// test_vulkan_texture_render_target.cpp
//
// PURPOSE: Validates the bitmap render target (RTT) operations in VulkanTextureManager.
// The texture manager maintains GPU-backed render targets for bmpman RTT handles.
// These are used for effects like environment mapping, dynamic textures, etc.
//
// INVARIANT: Render target creation must:
// - Register the target for the given bmpman base-frame handle
// - Track extent, format, and mip levels correctly
// - Provide valid attachment views for rendering
// - Support layout transitions between attachment and shader-read states

#include <gtest/gtest.h>
#include <array>
#include <optional>
#include <cstdint>

namespace {

// Simulated render target record from VulkanTextureManager
struct RenderTargetRecord {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t format = 0; // Placeholder for vk::Format
	uint32_t mipLevels = 1;
	uint32_t layers = 1;
	bool isCubemap = false;
	std::array<bool, 6> faceViewsValid{};
};

enum class ImageLayout {
	Undefined,
	ColorAttachment,
	ShaderReadOnly
};

// Simulates the render target portion of VulkanTextureManager
class FakeTextureManagerRTT {
  public:
	bool createRenderTarget(int baseFrameHandle, uint32_t width, uint32_t height, int flags, uint32_t* outMipLevels)
	{
		if (width == 0 || height == 0) {
			return false;
		}

		RenderTargetRecord record{};
		record.width = width;
		record.height = height;
		record.format = kDefaultFormat;
		record.mipLevels = calculateMipLevels(width, height, flags);
		record.isCubemap = (flags & kFlagCubemap) != 0;
		record.layers = record.isCubemap ? 6 : 1;

		// Create face views
		for (uint32_t i = 0; i < record.layers; ++i) {
			record.faceViewsValid[i] = true;
		}

		m_renderTargets[baseFrameHandle] = record;
		m_layouts[baseFrameHandle] = ImageLayout::ShaderReadOnly; // Initial state

		if (outMipLevels) {
			*outMipLevels = record.mipLevels;
		}

		return true;
	}

	bool hasRenderTarget(int baseFrameHandle) const
	{
		return m_renderTargets.find(baseFrameHandle) != m_renderTargets.end();
	}

	std::pair<uint32_t, uint32_t> renderTargetExtent(int baseFrameHandle) const
	{
		auto it = m_renderTargets.find(baseFrameHandle);
		if (it == m_renderTargets.end()) {
			return {0, 0};
		}
		return {it->second.width, it->second.height};
	}

	uint32_t renderTargetFormat(int baseFrameHandle) const
	{
		auto it = m_renderTargets.find(baseFrameHandle);
		if (it == m_renderTargets.end()) {
			return 0;
		}
		return it->second.format;
	}

	uint32_t renderTargetMipLevels(int baseFrameHandle) const
	{
		auto it = m_renderTargets.find(baseFrameHandle);
		if (it == m_renderTargets.end()) {
			return 0;
		}
		return it->second.mipLevels;
	}

	bool renderTargetAttachmentViewValid(int baseFrameHandle, int face) const
	{
		auto it = m_renderTargets.find(baseFrameHandle);
		if (it == m_renderTargets.end()) {
			return false;
		}
		if (face < 0 || face >= 6) {
			return false;
		}
		return it->second.faceViewsValid[static_cast<size_t>(face)];
	}

	bool transitionRenderTargetToAttachment(int baseFrameHandle)
	{
		auto it = m_layouts.find(baseFrameHandle);
		if (it == m_layouts.end()) {
			return false;
		}
		it->second = ImageLayout::ColorAttachment;
		++m_transitionCount;
		return true;
	}

	bool transitionRenderTargetToShaderRead(int baseFrameHandle)
	{
		auto it = m_layouts.find(baseFrameHandle);
		if (it == m_layouts.end()) {
			return false;
		}
		it->second = ImageLayout::ShaderReadOnly;
		++m_transitionCount;
		return true;
	}

	ImageLayout renderTargetLayout(int baseFrameHandle) const
	{
		auto it = m_layouts.find(baseFrameHandle);
		if (it == m_layouts.end()) {
			return ImageLayout::Undefined;
		}
		return it->second;
	}

	void deleteRenderTarget(int baseFrameHandle)
	{
		m_renderTargets.erase(baseFrameHandle);
		m_layouts.erase(baseFrameHandle);
	}

	int transitionCount() const { return m_transitionCount; }

	static constexpr int kFlagCubemap = 0x01;
	static constexpr int kFlagNoMipmaps = 0x02;
	static constexpr uint32_t kDefaultFormat = 44; // Placeholder for vk::Format::eR8G8B8A8Unorm

  private:
	uint32_t calculateMipLevels(uint32_t w, uint32_t h, int flags) const
	{
		if (flags & kFlagNoMipmaps) {
			return 1;
		}
		uint32_t maxDim = std::max(w, h);
		uint32_t levels = 1;
		while (maxDim > 1) {
			maxDim /= 2;
			++levels;
		}
		return levels;
	}

	std::unordered_map<int, RenderTargetRecord> m_renderTargets;
	std::unordered_map<int, ImageLayout> m_layouts;
	int m_transitionCount = 0;
};

} // namespace

// Test: Create render target - basic 2D
TEST(VulkanTextureRenderTarget, Create_Basic2D)
{
	FakeTextureManagerRTT mgr;

	uint32_t mipLevels = 0;
	bool success = mgr.createRenderTarget(100, 512, 512, 0, &mipLevels);

	EXPECT_TRUE(success);
	EXPECT_TRUE(mgr.hasRenderTarget(100));
	EXPECT_EQ(mgr.renderTargetExtent(100), std::make_pair(512u, 512u));
	EXPECT_GT(mipLevels, 1u) << "512x512 should have multiple mip levels";
}

// Test: Create render target - cubemap
TEST(VulkanTextureRenderTarget, Create_Cubemap)
{
	FakeTextureManagerRTT mgr;

	uint32_t mipLevels = 0;
	bool success = mgr.createRenderTarget(200, 256, 256, FakeTextureManagerRTT::kFlagCubemap, &mipLevels);

	EXPECT_TRUE(success);
	EXPECT_TRUE(mgr.hasRenderTarget(200));

	// All 6 faces should have valid views
	for (int face = 0; face < 6; ++face) {
		EXPECT_TRUE(mgr.renderTargetAttachmentViewValid(200, face))
			<< "Cubemap face " << face << " should have valid attachment view";
	}
}

// Test: Create render target - no mipmaps flag
TEST(VulkanTextureRenderTarget, Create_NoMipmaps)
{
	FakeTextureManagerRTT mgr;

	uint32_t mipLevels = 0;
	bool success = mgr.createRenderTarget(300, 1024, 1024, FakeTextureManagerRTT::kFlagNoMipmaps, &mipLevels);

	EXPECT_TRUE(success);
	EXPECT_EQ(mipLevels, 1u) << "No mipmaps flag should result in single mip level";
}

// Test: Create render target - zero size rejected
TEST(VulkanTextureRenderTarget, Create_ZeroSize_Rejected)
{
	FakeTextureManagerRTT mgr;

	EXPECT_FALSE(mgr.createRenderTarget(400, 0, 256, 0, nullptr));
	EXPECT_FALSE(mgr.createRenderTarget(401, 256, 0, 0, nullptr));
	EXPECT_FALSE(mgr.createRenderTarget(402, 0, 0, 0, nullptr));

	EXPECT_FALSE(mgr.hasRenderTarget(400));
	EXPECT_FALSE(mgr.hasRenderTarget(401));
	EXPECT_FALSE(mgr.hasRenderTarget(402));
}

// Test: hasRenderTarget - not found
TEST(VulkanTextureRenderTarget, HasRenderTarget_NotFound)
{
	FakeTextureManagerRTT mgr;

	EXPECT_FALSE(mgr.hasRenderTarget(999))
		<< "Non-existent handle should not be found";
}

// Test: Attachment view - invalid face index
TEST(VulkanTextureRenderTarget, AttachmentView_InvalidFace)
{
	FakeTextureManagerRTT mgr;
	mgr.createRenderTarget(500, 128, 128, 0, nullptr);

	EXPECT_FALSE(mgr.renderTargetAttachmentViewValid(500, -1));
	EXPECT_FALSE(mgr.renderTargetAttachmentViewValid(500, 6));
	EXPECT_FALSE(mgr.renderTargetAttachmentViewValid(500, 100));
}

// Test: Layout transitions - attachment to shader read
TEST(VulkanTextureRenderTarget, LayoutTransition_AttachmentToShaderRead)
{
	FakeTextureManagerRTT mgr;
	mgr.createRenderTarget(600, 256, 256, 0, nullptr);

	// Initial state is shader read
	EXPECT_EQ(mgr.renderTargetLayout(600), ImageLayout::ShaderReadOnly);

	// Transition to attachment for rendering
	EXPECT_TRUE(mgr.transitionRenderTargetToAttachment(600));
	EXPECT_EQ(mgr.renderTargetLayout(600), ImageLayout::ColorAttachment);

	// Transition back to shader read for sampling
	EXPECT_TRUE(mgr.transitionRenderTargetToShaderRead(600));
	EXPECT_EQ(mgr.renderTargetLayout(600), ImageLayout::ShaderReadOnly);
}

// Test: Layout transition - non-existent target
TEST(VulkanTextureRenderTarget, LayoutTransition_NonExistent)
{
	FakeTextureManagerRTT mgr;

	EXPECT_FALSE(mgr.transitionRenderTargetToAttachment(999));
	EXPECT_FALSE(mgr.transitionRenderTargetToShaderRead(999));
}

// Test: Delete render target
TEST(VulkanTextureRenderTarget, Delete_RemovesTarget)
{
	FakeTextureManagerRTT mgr;
	mgr.createRenderTarget(700, 128, 128, 0, nullptr);

	EXPECT_TRUE(mgr.hasRenderTarget(700));

	mgr.deleteRenderTarget(700);

	EXPECT_FALSE(mgr.hasRenderTarget(700));
	EXPECT_EQ(mgr.renderTargetLayout(700), ImageLayout::Undefined);
}

// Test: Multiple render targets - independent
TEST(VulkanTextureRenderTarget, MultipleTargets_Independent)
{
	FakeTextureManagerRTT mgr;

	mgr.createRenderTarget(800, 256, 256, 0, nullptr);
	mgr.createRenderTarget(801, 512, 512, 0, nullptr);
	mgr.createRenderTarget(802, 128, 64, FakeTextureManagerRTT::kFlagNoMipmaps, nullptr);

	EXPECT_TRUE(mgr.hasRenderTarget(800));
	EXPECT_TRUE(mgr.hasRenderTarget(801));
	EXPECT_TRUE(mgr.hasRenderTarget(802));

	// Different extents
	EXPECT_EQ(mgr.renderTargetExtent(800), std::make_pair(256u, 256u));
	EXPECT_EQ(mgr.renderTargetExtent(801), std::make_pair(512u, 512u));
	EXPECT_EQ(mgr.renderTargetExtent(802), std::make_pair(128u, 64u));

	// Delete one - others unaffected
	mgr.deleteRenderTarget(801);

	EXPECT_TRUE(mgr.hasRenderTarget(800));
	EXPECT_FALSE(mgr.hasRenderTarget(801));
	EXPECT_TRUE(mgr.hasRenderTarget(802));
}

// Test: Mip level calculation for power-of-two
TEST(VulkanTextureRenderTarget, MipLevelCalc_PowerOfTwo)
{
	FakeTextureManagerRTT mgr;

	uint32_t mip1, mip2, mip3;
	mgr.createRenderTarget(900, 1, 1, 0, &mip1);
	mgr.createRenderTarget(901, 2, 2, 0, &mip2);
	mgr.createRenderTarget(902, 256, 256, 0, &mip3);

	EXPECT_EQ(mip1, 1u) << "1x1 has 1 mip level";
	EXPECT_EQ(mip2, 2u) << "2x2 has 2 mip levels";
	EXPECT_EQ(mip3, 9u) << "256x256 has 9 mip levels (256 -> 1)";
}

// Test: Mip level calculation for non-power-of-two
TEST(VulkanTextureRenderTarget, MipLevelCalc_NonPowerOfTwo)
{
	FakeTextureManagerRTT mgr;

	uint32_t mip;
	mgr.createRenderTarget(903, 300, 200, 0, &mip);

	// max(300, 200) = 300, log2(300) ~= 8.2, so 9 mip levels
	EXPECT_GE(mip, 9u);
}

// Test: Transition count tracking
TEST(VulkanTextureRenderTarget, TransitionCount_Tracked)
{
	FakeTextureManagerRTT mgr;
	mgr.createRenderTarget(950, 128, 128, 0, nullptr);

	EXPECT_EQ(mgr.transitionCount(), 0);

	mgr.transitionRenderTargetToAttachment(950);
	EXPECT_EQ(mgr.transitionCount(), 1);

	mgr.transitionRenderTargetToShaderRead(950);
	EXPECT_EQ(mgr.transitionCount(), 2);

	mgr.transitionRenderTargetToAttachment(950);
	mgr.transitionRenderTargetToShaderRead(950);
	EXPECT_EQ(mgr.transitionCount(), 4);
}

// Test: Face view validity for 2D (only face 0 valid)
TEST(VulkanTextureRenderTarget, FaceView_2D_OnlyFace0Valid)
{
	FakeTextureManagerRTT mgr;
	mgr.createRenderTarget(960, 128, 128, 0, nullptr);

	EXPECT_TRUE(mgr.renderTargetAttachmentViewValid(960, 0))
		<< "2D render target should have valid face 0";

	// Faces 1-5 should not be valid for 2D target
	for (int face = 1; face < 6; ++face) {
		EXPECT_FALSE(mgr.renderTargetAttachmentViewValid(960, face))
			<< "2D render target should not have valid face " << face;
	}
}

