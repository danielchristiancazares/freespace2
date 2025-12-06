#include "graphics/vulkan/FrameLifecycleTracker.h"

#include <gtest/gtest.h>
#include <array>

using graphics::vulkan::FrameLifecycleTracker;

namespace {

struct DummyFrame {
	int id;
};

struct FakeRendererLifecycle {
	std::array<DummyFrame, 2> frames{{{0}, {1}}};
	FrameLifecycleTracker tracker;

	DummyFrame* getCurrentRecordingFrame()
	{
		if (!tracker.isRecording()) {
			return nullptr;
		}
		auto idx = tracker.currentFrameIndex() % frames.size();
		return &frames[idx];
	}
};

} // namespace

TEST(VulkanFrameLifecycle, Scenario_NotRecording_ReturnsNullFrame)
{
	FakeRendererLifecycle fake;
	EXPECT_FALSE(fake.tracker.isRecording());
	EXPECT_EQ(fake.getCurrentRecordingFrame(), nullptr);
}

TEST(VulkanFrameLifecycle, Scenario_BeginRecording_MakesFrameAvailable)
{
	FakeRendererLifecycle fake;
	fake.tracker.begin(0);
	EXPECT_TRUE(fake.tracker.isRecording());
	auto* frame = fake.getCurrentRecordingFrame();
	ASSERT_NE(frame, nullptr);
	EXPECT_EQ(frame->id, 0);
}

TEST(VulkanFrameLifecycle, Scenario_EndRecording_ClearsFrame)
{
	FakeRendererLifecycle fake;
	fake.tracker.begin(0);
	fake.tracker.end();
	EXPECT_FALSE(fake.tracker.isRecording());
	EXPECT_EQ(fake.getCurrentRecordingFrame(), nullptr);
}

TEST(VulkanFrameLifecycle, Scenario_FrameIndexAdvancesAcrossBegins)
{
	FakeRendererLifecycle fake;
	fake.tracker.begin(0);
	auto* frame0 = fake.getCurrentRecordingFrame();
	ASSERT_NE(frame0, nullptr);
	fake.tracker.begin(1);
	auto* frame1 = fake.getCurrentRecordingFrame();
	ASSERT_NE(frame1, nullptr);
	EXPECT_NE(frame0, frame1);
	EXPECT_EQ(frame1->id, 1);
}
