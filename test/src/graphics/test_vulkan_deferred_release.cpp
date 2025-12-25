// test_vulkan_deferred_release.cpp
//
// Tests the ACTUAL DeferredReleaseQueue class from VulkanDeferredRelease.h.
// This class is pure C++ with no Vulkan dependencies, so we can test it directly.

#include "graphics/vulkan/VulkanDeferredRelease.h"

#include <gtest/gtest.h>
#include <vector>

using graphics::vulkan::DeferredReleaseQueue;

namespace {

// Helper to track destruction order
class DestructionTracker {
  public:
	void record(int id) { m_order.push_back(id); }
	const std::vector<int>& order() const { return m_order; }
	size_t count() const { return m_order.size(); }

  private:
	std::vector<int> m_order;
};

} // namespace

// Test: Empty queue collect is safe
TEST(VulkanDeferredRelease, EmptyQueue_CollectIsSafe)
{
	DeferredReleaseQueue queue;

	EXPECT_EQ(queue.size(), 0u);

	queue.collect(0);
	queue.collect(100);
	queue.collect(UINT64_MAX);

	EXPECT_EQ(queue.size(), 0u);
}

// Test: Enqueued resource is NOT released before its serial
TEST(VulkanDeferredRelease, Enqueue_NotReleasedBeforeSerial)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(10, [&tracker]() { tracker.record(1); });

	EXPECT_EQ(queue.size(), 1u);

	queue.collect(5);
	EXPECT_EQ(tracker.count(), 0u) << "Resource must not be released before serial";
	EXPECT_EQ(queue.size(), 1u);

	queue.collect(9);
	EXPECT_EQ(tracker.count(), 0u) << "Resource must not be released before serial";
	EXPECT_EQ(queue.size(), 1u);
}

// Test: Enqueued resource IS released at exact serial
TEST(VulkanDeferredRelease, Enqueue_ReleasedAtSerial)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(10, [&tracker]() { tracker.record(1); });

	queue.collect(10);

	EXPECT_EQ(tracker.count(), 1u) << "Resource must be released at exact serial";
	EXPECT_EQ(queue.size(), 0u);
}

// Test: Enqueued resource IS released after serial exceeded
TEST(VulkanDeferredRelease, Enqueue_ReleasedAfterSerial)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(10, [&tracker]() { tracker.record(1); });

	queue.collect(100);

	EXPECT_EQ(tracker.count(), 1u);
	EXPECT_EQ(queue.size(), 0u);
}

// Test: Multiple resources at same serial released together
TEST(VulkanDeferredRelease, MultipleResources_SameSerial)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(5, [&tracker]() { tracker.record(1); });
	queue.enqueue(5, [&tracker]() { tracker.record(2); });
	queue.enqueue(5, [&tracker]() { tracker.record(3); });

	queue.collect(5);

	EXPECT_EQ(tracker.count(), 3u);
	EXPECT_EQ(queue.size(), 0u);
}

// Test: Resources with different serials released in order
TEST(VulkanDeferredRelease, DifferentSerials_PartialRelease)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(5, [&tracker]() { tracker.record(1); });
	queue.enqueue(10, [&tracker]() { tracker.record(2); });
	queue.enqueue(15, [&tracker]() { tracker.record(3); });

	EXPECT_EQ(queue.size(), 3u);

	queue.collect(7);
	EXPECT_EQ(tracker.count(), 1u);
	EXPECT_EQ(tracker.order()[0], 1);
	EXPECT_EQ(queue.size(), 2u);

	queue.collect(12);
	EXPECT_EQ(tracker.count(), 2u);
	EXPECT_EQ(tracker.order()[1], 2);
	EXPECT_EQ(queue.size(), 1u);

	queue.collect(20);
	EXPECT_EQ(tracker.count(), 3u);
	EXPECT_EQ(tracker.order()[2], 3);
	EXPECT_EQ(queue.size(), 0u);
}

// Test: clear() releases all resources immediately
TEST(VulkanDeferredRelease, Clear_ReleasesAll)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(100, [&tracker]() { tracker.record(1); });
	queue.enqueue(200, [&tracker]() { tracker.record(2); });
	queue.enqueue(300, [&tracker]() { tracker.record(3); });

	queue.clear();

	EXPECT_EQ(tracker.count(), 3u) << "clear() must release all resources";
	EXPECT_EQ(queue.size(), 0u);
}

// Test: Boundary condition - serial 0
TEST(VulkanDeferredRelease, BoundarySerial_Zero)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	queue.enqueue(0, [&tracker]() { tracker.record(1); });
	queue.enqueue(1, [&tracker]() { tracker.record(2); });

	queue.collect(0);
	EXPECT_EQ(tracker.count(), 1u);

	queue.collect(1);
	EXPECT_EQ(tracker.count(), 2u);
}

// Test: Large serial values work correctly
TEST(VulkanDeferredRelease, LargeSerials)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	uint64_t largeSerial = UINT64_MAX - 1;
	queue.enqueue(largeSerial, [&tracker]() { tracker.record(1); });

	queue.collect(largeSerial - 1);
	EXPECT_EQ(tracker.count(), 0u);

	queue.collect(largeSerial);
	EXPECT_EQ(tracker.count(), 1u);
}

// Test: Out-of-order enqueue still respects serial ordering
TEST(VulkanDeferredRelease, OutOfOrderEnqueue)
{
	DeferredReleaseQueue queue;
	DestructionTracker tracker;

	// Enqueue in non-monotonic serial order
	queue.enqueue(15, [&tracker]() { tracker.record(3); });
	queue.enqueue(5, [&tracker]() { tracker.record(1); });
	queue.enqueue(10, [&tracker]() { tracker.record(2); });

	// Collect should release based on serial value
	queue.collect(7);
	EXPECT_EQ(tracker.count(), 1u);
	EXPECT_EQ(tracker.order()[0], 1) << "Serial 5 resource should be released";
}

// Test: Move-only callback works (captures unique_ptr)
TEST(VulkanDeferredRelease, MoveOnlyCallback)
{
	DeferredReleaseQueue queue;
	bool destroyed = false;

	auto ptr = std::make_unique<int>(42);
	queue.enqueue(10, [p = std::move(ptr), &destroyed]() mutable {
		destroyed = true;
		p.reset();
	});

	EXPECT_FALSE(destroyed);

	queue.collect(10);

	EXPECT_TRUE(destroyed);
}
