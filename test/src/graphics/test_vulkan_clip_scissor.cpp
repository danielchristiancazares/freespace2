#include "graphics/vulkan/VulkanClip.h"

#include <gtest/gtest.h>

namespace {
class ScreenStateGuard {
  public:
	ScreenStateGuard() : _saved(gr_screen) {}
	~ScreenStateGuard() { gr_screen = _saved; }

	ScreenStateGuard(const ScreenStateGuard&) = delete;
	ScreenStateGuard& operator=(const ScreenStateGuard&) = delete;

  private:
	::screen _saved;
};
} // namespace

TEST(VulkanClipScissor, ApplyClipUpdatesScreenStateAndScissor)
{
	ScreenStateGuard guard;

	gr_screen.max_w = 640;
	gr_screen.max_h = 480;
	gr_screen.max_w_unscaled = 640;
	gr_screen.max_h_unscaled = 480;
	gr_screen.custom_size = false;
	gr_screen.rendering_to_texture = -1;

	graphics::vulkan::applyClipToScreen(10, 20, 100, 200, GR_RESIZE_FULL);

	EXPECT_EQ(gr_screen.offset_x, 10);
	EXPECT_EQ(gr_screen.offset_y, 20);
	EXPECT_EQ(gr_screen.clip_width, 100);
	EXPECT_EQ(gr_screen.clip_height, 200);

	// Clip bounds are relative to the clip origin (offset_x/y).
	EXPECT_EQ(gr_screen.clip_left, 0);
	EXPECT_EQ(gr_screen.clip_top, 0);
	EXPECT_EQ(gr_screen.clip_right, 99);
	EXPECT_EQ(gr_screen.clip_bottom, 199);

	const auto scissor = graphics::vulkan::getClipScissorFromScreen(gr_screen);
	EXPECT_EQ(scissor.x, 10);
	EXPECT_EQ(scissor.y, 20);
	EXPECT_EQ(scissor.width, 100u);
	EXPECT_EQ(scissor.height, 200u);
}

TEST(VulkanClipScissor, ClampClipScissorToFramebufferClampsNegativeOffsets)
{
	graphics::vulkan::ClipScissorRect in{};
	in.x = -3;
	in.y = -3;
	in.width = 10;
	in.height = 10;

	const auto out = graphics::vulkan::clampClipScissorToFramebuffer(in, /*fbWidth=*/8, /*fbHeight=*/8);
	EXPECT_EQ(out.x, 0);
	EXPECT_EQ(out.y, 0);
	EXPECT_EQ(out.width, 7u);  // [-3,7) intersect [0,8) => [0,7)
	EXPECT_EQ(out.height, 7u);
}

TEST(VulkanClipScissor, ClampClipScissorToFramebufferClampsPastFramebufferEdge)
{
	graphics::vulkan::ClipScissorRect in{};
	in.x = 6;
	in.y = 6;
	in.width = 10;
	in.height = 10;

	const auto out = graphics::vulkan::clampClipScissorToFramebuffer(in, /*fbWidth=*/8, /*fbHeight=*/8);
	EXPECT_EQ(out.x, 6);
	EXPECT_EQ(out.y, 6);
	EXPECT_EQ(out.width, 2u);  // [6,16) intersect [0,8) => [6,8)
	EXPECT_EQ(out.height, 2u);
}
