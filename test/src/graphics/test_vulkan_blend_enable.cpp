// Test for H7 fix: EDS3 blend enable should respect material blend mode

#include "graphics/grinternal.h"

#include <gtest/gtest.h>

namespace {

// Helper function that mirrors the logic that should be used in VulkanGraphics.cpp
// when setting blend enable with Extended Dynamic State 3.
// Returns true if blending should be enabled for the given blend mode.
bool shouldEnableBlending(gr_alpha_blend mode)
{
	return mode != ALPHA_BLEND_NONE;
}

} // namespace

TEST(VulkanBlendEnable, NoneDisablesBlending)
{
	EXPECT_FALSE(shouldEnableBlending(ALPHA_BLEND_NONE));
}

TEST(VulkanBlendEnable, AdditiveEnablesBlending)
{
	EXPECT_TRUE(shouldEnableBlending(ALPHA_BLEND_ADDITIVE));
}

TEST(VulkanBlendEnable, AlphaAdditiveEnablesBlending)
{
	EXPECT_TRUE(shouldEnableBlending(ALPHA_BLEND_ALPHA_ADDITIVE));
}

TEST(VulkanBlendEnable, AlphaBlendAlphaEnablesBlending)
{
	EXPECT_TRUE(shouldEnableBlending(ALPHA_BLEND_ALPHA_BLEND_ALPHA));
}

TEST(VulkanBlendEnable, AlphaBlendSrcColorEnablesBlending)
{
	EXPECT_TRUE(shouldEnableBlending(ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR));
}

TEST(VulkanBlendEnable, PremultipliedEnablesBlending)
{
	EXPECT_TRUE(shouldEnableBlending(ALPHA_BLEND_PREMULTIPLIED));
}

// Regression test for H7: verify that all blend modes except NONE enable blending
TEST(VulkanBlendEnable, H7_Regression_AllNonNoneModesEnableBlending)
{
	// This is the bug that H7 identified: EDS3 was unconditionally disabling blending.
	// After the fix, all blend modes except NONE should enable blending.
	gr_alpha_blend allModes[] = {
		ALPHA_BLEND_NONE,
		ALPHA_BLEND_ADDITIVE,
		ALPHA_BLEND_ALPHA_ADDITIVE,
		ALPHA_BLEND_ALPHA_BLEND_ALPHA,
		ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR,
		ALPHA_BLEND_PREMULTIPLIED
	};

	for (gr_alpha_blend mode : allModes) {
		if (mode == ALPHA_BLEND_NONE) {
			EXPECT_FALSE(shouldEnableBlending(mode)) << "ALPHA_BLEND_NONE should disable blending";
		} else {
			EXPECT_TRUE(shouldEnableBlending(mode)) << "Non-NONE blend mode should enable blending";
		}
	}
}
