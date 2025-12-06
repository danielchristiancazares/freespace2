#include <gtest/gtest.h>

#include <graphics/2d.h>
#include <graphics/post_processing.h>

#include "util/FSTestFixture.h"

// Ensures gr_lightshafts_enabled() tolerates a missing Post_processing_manager (Vulkan backend scenario)
class PostProcessingNullSafety : public test::FSTestFixture {
 public:
	PostProcessingNullSafety() : test::FSTestFixture(INIT_CFILE | INIT_GRAPHICS) {}
};

TEST_F(PostProcessingNullSafety, Scenario_NullManager_DoesNotCrash_ReturnsFalse)
{
	// gr_init in the fixture sets GR_STUB; simulate Vulkan without creating the manager
	ASSERT_EQ(gr_screen.mode, GR_STUB);
	const auto originalMode = gr_screen.mode;

	gr_screen.mode = GR_VULKAN;
	graphics::Post_processing_manager = nullptr;

	const auto result = gr_lightshafts_enabled();
	EXPECT_FALSE(result);

	// Restore to avoid incorrect cleanup path in TearDown
	gr_screen.mode = originalMode;
}
