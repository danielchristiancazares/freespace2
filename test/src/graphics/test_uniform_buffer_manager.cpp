#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <cstdint>

namespace {

// Minimal simulation of the frame-counted deletion logic
// (Tests the algorithm without full graphics backend dependency)
//
// This mirrors the logic in UniformBufferManager::onFrameEnd() and
// UniformBufferManager::changeSegmentSize() to verify correctness of
// the deferred buffer deletion mechanism.

struct RetiredBuffer {
	int handle; // Simplified handle (real code uses gr_buffer_handle)
	uint32_t retiredAtFrame;
};

class FakeBufferRetirementTracker {
  public:
	static constexpr uint32_t FRAMES_BEFORE_DELETE = 3;

	uint32_t currentFrame = 0;
	std::vector<RetiredBuffer> retiredBuffers;
	std::vector<int> deletedHandles; // Track what was deleted

	void retireBuffer(int handle) { retiredBuffers.push_back({handle, currentFrame}); }

	void onFrameEnd()
	{
		++currentFrame;

		auto it = retiredBuffers.begin();
		while (it != retiredBuffers.end()) {
			if (currentFrame - it->retiredAtFrame >= FRAMES_BEFORE_DELETE) {
				deletedHandles.push_back(it->handle);
				it = retiredBuffers.erase(it);
			} else {
				++it;
			}
		}
	}
};

} // namespace

// Test: Retired buffer is NOT deleted immediately
TEST(UniformBufferRetirement, Scenario_RetireBuffer_NotDeletedImmediately)
{
	FakeBufferRetirementTracker tracker;
	tracker.retireBuffer(100);

	// Frame 0: retire buffer
	// Frame 1: onFrameEnd (currentFrame becomes 1)
	tracker.onFrameEnd();

	EXPECT_EQ(tracker.deletedHandles.size(), 0u);
	EXPECT_EQ(tracker.retiredBuffers.size(), 1u);
}

// Test: Retired buffer is NOT deleted after 1 frame
TEST(UniformBufferRetirement, Scenario_RetireBuffer_NotDeletedAfter1Frame)
{
	FakeBufferRetirementTracker tracker;
	tracker.retireBuffer(100);

	tracker.onFrameEnd(); // Frame 1
	tracker.onFrameEnd(); // Frame 2

	EXPECT_EQ(tracker.deletedHandles.size(), 0u);
	EXPECT_EQ(tracker.retiredBuffers.size(), 1u);
}

// Test: Retired buffer IS deleted after FRAMES_BEFORE_DELETE frames
TEST(UniformBufferRetirement, Scenario_RetireBuffer_DeletedAfter3Frames)
{
	FakeBufferRetirementTracker tracker;
	tracker.retireBuffer(100); // Retired at frame 0

	tracker.onFrameEnd(); // Frame 1
	tracker.onFrameEnd(); // Frame 2
	tracker.onFrameEnd(); // Frame 3 - should delete (3 - 0 >= 3)

	EXPECT_EQ(tracker.deletedHandles.size(), 1u);
	EXPECT_EQ(tracker.deletedHandles[0], 100);
	EXPECT_EQ(tracker.retiredBuffers.size(), 0u);
}

// Test: Multiple buffers retired at different frames are deleted at correct times
TEST(UniformBufferRetirement, Scenario_MultipleBuffers_DeletedInOrder)
{
	FakeBufferRetirementTracker tracker;

	tracker.retireBuffer(100); // Retired at frame 0
	tracker.onFrameEnd();      // Frame 1

	tracker.retireBuffer(200); // Retired at frame 1
	tracker.onFrameEnd();      // Frame 2

	tracker.retireBuffer(300); // Retired at frame 2
	tracker.onFrameEnd();      // Frame 3 - buffer 100 should be deleted

	EXPECT_EQ(tracker.deletedHandles.size(), 1u);
	EXPECT_EQ(tracker.deletedHandles[0], 100);
	EXPECT_EQ(tracker.retiredBuffers.size(), 2u);

	tracker.onFrameEnd(); // Frame 4 - buffer 200 should be deleted

	EXPECT_EQ(tracker.deletedHandles.size(), 2u);
	EXPECT_EQ(tracker.deletedHandles[1], 200);
	EXPECT_EQ(tracker.retiredBuffers.size(), 1u);

	tracker.onFrameEnd(); // Frame 5 - buffer 300 should be deleted

	EXPECT_EQ(tracker.deletedHandles.size(), 3u);
	EXPECT_EQ(tracker.deletedHandles[2], 300);
	EXPECT_EQ(tracker.retiredBuffers.size(), 0u);
}

// Test: Multiple buffers retired in same frame are all deleted together
TEST(UniformBufferRetirement, Scenario_MultipleBuffersSameFrame_DeletedTogether)
{
	FakeBufferRetirementTracker tracker;

	tracker.retireBuffer(100); // Retired at frame 0
	tracker.retireBuffer(200); // Retired at frame 0
	tracker.retireBuffer(300); // Retired at frame 0

	tracker.onFrameEnd(); // Frame 1
	tracker.onFrameEnd(); // Frame 2

	EXPECT_EQ(tracker.deletedHandles.size(), 0u);

	tracker.onFrameEnd(); // Frame 3 - all should be deleted

	EXPECT_EQ(tracker.deletedHandles.size(), 3u);
	EXPECT_EQ(tracker.retiredBuffers.size(), 0u);
}

// Test: Frame counter handles uint32_t wraparound correctly
TEST(UniformBufferRetirement, Scenario_FrameCounterWraparound_HandledCorrectly)
{
	FakeBufferRetirementTracker tracker;
	tracker.currentFrame = UINT32_MAX - 1; // About to wrap

	tracker.retireBuffer(100); // Retired at frame UINT32_MAX - 1
	tracker.onFrameEnd();      // Frame UINT32_MAX
	tracker.onFrameEnd();      // Frame 0 (wrapped)
	tracker.onFrameEnd();      // Frame 1

	// Due to unsigned arithmetic: 1 - (UINT32_MAX - 1) = 3, so should be deleted
	// Actually: 1 - (UINT32_MAX - 1) wraps to 3 in unsigned math
	EXPECT_EQ(tracker.deletedHandles.size(), 1u);
}

// Test: Empty retired list doesn't cause issues
TEST(UniformBufferRetirement, Scenario_NoRetiredBuffers_OnFrameEndSafe)
{
	FakeBufferRetirementTracker tracker;

	// Should not crash or cause issues
	tracker.onFrameEnd();
	tracker.onFrameEnd();
	tracker.onFrameEnd();

	EXPECT_EQ(tracker.deletedHandles.size(), 0u);
	EXPECT_EQ(tracker.retiredBuffers.size(), 0u);
}
