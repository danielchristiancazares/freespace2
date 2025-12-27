// test_vulkan_pending_upload_queue.cpp
//
// Tests for PendingUploadQueue behavior in VulkanTextureManager.
//
// These tests verify two critical invariants:
// 1. UNIQUENESS: The queue does not allow duplicate entries for the same bitmap handle.
//    Re-enqueuing an already-pending upload is idempotent.
// 2. SLOT-ALLOCATION GATING: Bindless slot assignment only proceeds when a slot is available.
//    The queue gates dequeuing based on slot availability.
//
// Architecture: VulkanTextureManager declares PendingUploadQueueTest as a friend class,
// allowing these tests to directly access and exercise the private PendingUploadQueue.

#include "graphics/vulkan/VulkanTextureManager.h"
#include "graphics/vulkan/VulkanConstants.h"
#include "graphics/vulkan/VulkanTextureId.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

using graphics::vulkan::TextureId;
using graphics::vulkan::TextureIdHasher;
using graphics::vulkan::VulkanTextureManager;
using graphics::vulkan::kBindlessFirstDynamicTextureSlot;
using graphics::vulkan::kMaxBindlessTextures;

// Friend class that can access VulkanTextureManager::PendingUploadQueue
class PendingUploadQueueTest : public ::testing::Test {
protected:
	using Queue = VulkanTextureManager::PendingUploadQueue;

	Queue queue;

	static TextureId makeId(int baseFrame) {
		auto id = TextureId::tryFromBaseFrame(baseFrame);
		EXPECT_TRUE(id.has_value()) << "Failed to create TextureId for base frame " << baseFrame;
		return *id;
	}

	// Check if queue contains an id by attempting to erase and re-add
	bool contains(TextureId id) {
		// enqueue returns false if already present
		bool wasNew = queue.enqueue(id);
		if (wasNew) {
			// It wasn't there, remove what we just added
			queue.erase(id);
			return false;
		}
		return true;
	}

	size_t size() {
		auto items = queue.takeAll();
		size_t count = items.size();
		// Re-enqueue all items to restore state
		for (const auto& id : items) {
			queue.enqueue(id);
		}
		return count;
	}
};

// =============================================================================
// SECTION 1: Uniqueness Invariant Tests
// =============================================================================

TEST_F(PendingUploadQueueTest, EnqueueSameId_ReturnsFalseOnSecondCall)
{
	auto id = makeId(42);

	EXPECT_TRUE(queue.enqueue(id)) << "First enqueue should return true";
	EXPECT_FALSE(queue.enqueue(id)) << "Second enqueue should return false (already present)";
	EXPECT_FALSE(queue.enqueue(id)) << "Third enqueue should also return false";
}

TEST_F(PendingUploadQueueTest, EnqueueSameId_DoesNotDuplicateInFifo)
{
	auto id = makeId(42);

	queue.enqueue(id);
	queue.enqueue(id);
	queue.enqueue(id);

	auto items = queue.takeAll();
	EXPECT_EQ(items.size(), 1u) << "Queue should contain exactly one entry despite multiple enqueues";
	EXPECT_EQ(items[0], id);
}

TEST_F(PendingUploadQueueTest, EnqueueMultipleDistinctIds_AllAccepted)
{
	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);

	EXPECT_TRUE(queue.enqueue(id1));
	EXPECT_TRUE(queue.enqueue(id2));
	EXPECT_TRUE(queue.enqueue(id3));

	EXPECT_EQ(size(), 3u);
}

TEST_F(PendingUploadQueueTest, InterleavedDuplicatesAndNewIds)
{
	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);
	auto id4 = makeId(4);

	EXPECT_TRUE(queue.enqueue(id1));   // New
	EXPECT_TRUE(queue.enqueue(id2));   // New
	EXPECT_FALSE(queue.enqueue(id1));  // Duplicate
	EXPECT_TRUE(queue.enqueue(id3));   // New
	EXPECT_FALSE(queue.enqueue(id2));  // Duplicate
	EXPECT_FALSE(queue.enqueue(id3));  // Duplicate
	EXPECT_TRUE(queue.enqueue(id4));   // New

	EXPECT_EQ(size(), 4u);
}

TEST_F(PendingUploadQueueTest, EraseAllowsReenqueue)
{
	auto id = makeId(42);

	queue.enqueue(id);
	EXPECT_FALSE(queue.enqueue(id)) << "Cannot re-enqueue while still present";

	EXPECT_TRUE(queue.erase(id)) << "Erase should succeed";
	EXPECT_TRUE(queue.enqueue(id)) << "After erase, re-enqueue should succeed";

	EXPECT_EQ(size(), 1u);
}

TEST_F(PendingUploadQueueTest, TakeAllAllowsReenqueue)
{
	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);

	queue.enqueue(id1);
	queue.enqueue(id2);
	queue.enqueue(id3);

	auto items = queue.takeAll();
	EXPECT_EQ(items.size(), 3u);
	EXPECT_TRUE(queue.empty());

	// After takeAll, all ids should be re-enqueueable
	EXPECT_TRUE(queue.enqueue(id1));
	EXPECT_TRUE(queue.enqueue(id2));
	EXPECT_TRUE(queue.enqueue(id3));
}

// =============================================================================
// SECTION 2: FIFO Order Preservation Tests
// =============================================================================

TEST_F(PendingUploadQueueTest, EnqueuePreservesFifoOrder)
{
	auto id10 = makeId(10);
	auto id20 = makeId(20);
	auto id30 = makeId(30);

	queue.enqueue(id10);
	queue.enqueue(id20);
	queue.enqueue(id30);

	auto items = queue.takeAll();

	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[0], id10) << "First enqueued should be first in FIFO";
	EXPECT_EQ(items[1], id20);
	EXPECT_EQ(items[2], id30) << "Last enqueued should be last in FIFO";
}

TEST_F(PendingUploadQueueTest, DuplicateDoesNotChangeOrder)
{
	auto id10 = makeId(10);
	auto id20 = makeId(20);
	auto id30 = makeId(30);

	queue.enqueue(id10);
	queue.enqueue(id20);
	queue.enqueue(id10);  // Duplicate - should not move to end
	queue.enqueue(id30);

	auto items = queue.takeAll();

	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[0], id10) << "Duplicate enqueue should not change position";
	EXPECT_EQ(items[1], id20);
	EXPECT_EQ(items[2], id30);
}

// =============================================================================
// SECTION 3: Erase Behavior Tests
// =============================================================================

TEST_F(PendingUploadQueueTest, EraseNonexistent_ReturnsFalse)
{
	auto id = makeId(999);
	EXPECT_FALSE(queue.erase(id)) << "Erasing non-existent id should return false";
}

TEST_F(PendingUploadQueueTest, EraseFromMiddle_PreservesOrder)
{
	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);
	auto id4 = makeId(4);

	queue.enqueue(id1);
	queue.enqueue(id2);
	queue.enqueue(id3);
	queue.enqueue(id4);

	EXPECT_TRUE(queue.erase(id2));

	auto items = queue.takeAll();
	ASSERT_EQ(items.size(), 3u);
	EXPECT_EQ(items[0], id1);
	EXPECT_EQ(items[1], id3);
	EXPECT_EQ(items[2], id4);
}

TEST_F(PendingUploadQueueTest, EraseFromFront)
{
	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);

	queue.enqueue(id1);
	queue.enqueue(id2);
	queue.enqueue(id3);

	EXPECT_TRUE(queue.erase(id1));

	auto items = queue.takeAll();
	ASSERT_EQ(items.size(), 2u);
	EXPECT_EQ(items[0], id2);
	EXPECT_EQ(items[1], id3);
}

TEST_F(PendingUploadQueueTest, EraseFromBack)
{
	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);

	queue.enqueue(id1);
	queue.enqueue(id2);
	queue.enqueue(id3);

	EXPECT_TRUE(queue.erase(id3));

	auto items = queue.takeAll();
	ASSERT_EQ(items.size(), 2u);
	EXPECT_EQ(items[0], id1);
	EXPECT_EQ(items[1], id2);
}

TEST_F(PendingUploadQueueTest, DoubleEraseReturnsFalse)
{
	auto id = makeId(42);

	queue.enqueue(id);

	EXPECT_TRUE(queue.erase(id));
	EXPECT_FALSE(queue.erase(id)) << "Second erase of same id should return false";
}

// =============================================================================
// SECTION 4: Boundary Conditions
// =============================================================================

TEST_F(PendingUploadQueueTest, EmptyQueueOperations)
{
	EXPECT_TRUE(queue.empty());

	auto id = makeId(0);
	EXPECT_FALSE(queue.erase(id));

	auto items = queue.takeAll();
	EXPECT_TRUE(items.empty());
}

TEST_F(PendingUploadQueueTest, LargeNumberOfEntries)
{
	constexpr int count = 1000;
	std::vector<TextureId> ids;
	ids.reserve(count);

	for (int i = 0; i < count; ++i) {
		ids.push_back(makeId(i));
		EXPECT_TRUE(queue.enqueue(ids.back()));
	}

	EXPECT_EQ(size(), static_cast<size_t>(count));

	// Duplicates should all fail
	for (const auto& id : ids) {
		EXPECT_FALSE(queue.enqueue(id));
	}

	EXPECT_EQ(size(), static_cast<size_t>(count)) << "Size unchanged after duplicate attempts";
}

TEST_F(PendingUploadQueueTest, ZeroBaseFrame)
{
	auto id = makeId(0);

	EXPECT_TRUE(queue.enqueue(id));
	EXPECT_FALSE(queue.enqueue(id));
	EXPECT_TRUE(contains(id));
	EXPECT_TRUE(queue.erase(id));
	EXPECT_FALSE(contains(id));
}

// =============================================================================
// SECTION 5: Slot Allocation Gating Simulation
// =============================================================================
// These tests verify the slot allocation algorithm that gates upload processing.
// The simulation mirrors the logic in VulkanTextureManager::assignBindlessSlots.

namespace {

class SlotPool {
public:
	explicit SlotPool(uint32_t numSlots) {
		for (uint32_t i = numSlots; i-- > 0;) {
			m_freeSlots.push_back(i);
		}
	}

	std::optional<uint32_t> acquire() {
		if (m_freeSlots.empty()) {
			return std::nullopt;
		}
		uint32_t slot = m_freeSlots.back();
		m_freeSlots.pop_back();
		return slot;
	}

	void release(uint32_t slot) {
		m_freeSlots.push_back(slot);
	}

	size_t available() const { return m_freeSlots.size(); }

private:
	std::vector<uint32_t> m_freeSlots;
};

} // namespace

// Test fixture for combined queue + slot simulation using production queue
class SlotGatingTest : public PendingUploadQueueTest {
protected:
	SlotPool slots{10};
	std::unordered_map<TextureId, uint32_t, TextureIdHasher> assignments;

	// Simulates processUploads + assignBindlessSlots
	int processUploads() {
		auto pending = queue.takeAll();
		int slotsAssigned = 0;

		for (const auto& id : pending) {
			auto slotOpt = slots.acquire();
			if (slotOpt.has_value()) {
				assignments[id] = *slotOpt;
				++slotsAssigned;
			} else {
				// Re-queue for next frame (slot pressure)
				queue.enqueue(id);
			}
		}
		return slotsAssigned;
	}

	bool hasSlot(TextureId id) const {
		return assignments.find(id) != assignments.end();
	}

	void releaseSlot(TextureId id) {
		auto it = assignments.find(id);
		if (it != assignments.end()) {
			slots.release(it->second);
			assignments.erase(it);
		}
	}
};

TEST_F(SlotGatingTest, UniquenessPrevents_DuplicateSlotAssignment)
{
	auto id = makeId(42);

	// Queue same texture multiple times before processing
	queue.enqueue(id);
	queue.enqueue(id);
	queue.enqueue(id);

	int assigned = processUploads();

	EXPECT_EQ(assigned, 1) << "Only one slot should be assigned despite multiple enqueues";
	EXPECT_TRUE(hasSlot(id));
	EXPECT_EQ(slots.available(), 9u) << "Only one slot consumed";
}

TEST_F(SlotGatingTest, SlotPressure_QueuesDeferredUploads)
{
	slots = SlotPool(2);  // Only 2 slots

	auto id1 = makeId(1);
	auto id2 = makeId(2);
	auto id3 = makeId(3);

	queue.enqueue(id1);
	queue.enqueue(id2);
	queue.enqueue(id3);  // This one won't get a slot

	int assigned = processUploads();

	EXPECT_EQ(assigned, 2);
	EXPECT_TRUE(hasSlot(id1));
	EXPECT_TRUE(hasSlot(id2));
	EXPECT_FALSE(hasSlot(id3)) << "Texture 3 should not have a slot due to pressure";
	EXPECT_EQ(size(), 1u) << "Texture 3 should be re-queued";

	// Release a slot and process again
	releaseSlot(id1);
	assigned = processUploads();

	EXPECT_EQ(assigned, 1);
	EXPECT_TRUE(hasSlot(id3)) << "Texture 3 should now have a slot";
}

TEST_F(SlotGatingTest, SlotGating_ProcessesInFifoOrder)
{
	slots = SlotPool(2);  // Limited slots

	auto id10 = makeId(10);
	auto id20 = makeId(20);
	auto id30 = makeId(30);
	auto id40 = makeId(40);

	queue.enqueue(id10);
	queue.enqueue(id20);
	queue.enqueue(id30);
	queue.enqueue(id40);

	int assigned1 = processUploads();
	EXPECT_EQ(assigned1, 2);

	// First two should have slots (FIFO order)
	EXPECT_TRUE(hasSlot(id10));
	EXPECT_TRUE(hasSlot(id20));
	EXPECT_FALSE(hasSlot(id30));
	EXPECT_FALSE(hasSlot(id40));

	// Release one and process
	releaseSlot(id10);
	int assigned2 = processUploads();
	EXPECT_EQ(assigned2, 1);

	// 30 should now have a slot (next in FIFO order)
	EXPECT_TRUE(hasSlot(id30));
	EXPECT_FALSE(hasSlot(id40));
}

// =============================================================================
// SECTION 6: TextureId Contract Tests
// =============================================================================

TEST(TextureIdContract, TryFromBaseFrame_RequiresNonNegative)
{
	auto validId = TextureId::tryFromBaseFrame(0);
	EXPECT_TRUE(validId.has_value()) << "Base frame 0 is valid";

	auto validId2 = TextureId::tryFromBaseFrame(100);
	EXPECT_TRUE(validId2.has_value()) << "Positive base frame is valid";

	auto invalidId = TextureId::tryFromBaseFrame(-1);
	EXPECT_FALSE(invalidId.has_value()) << "Negative base frame is invalid";

	auto invalidId2 = TextureId::tryFromBaseFrame(-100);
	EXPECT_FALSE(invalidId2.has_value()) << "Negative base frame is invalid";
}

TEST(TextureIdContract, HasherProducesConsistentHashes)
{
	auto id1 = TextureId::tryFromBaseFrame(42);
	auto id2 = TextureId::tryFromBaseFrame(42);
	auto id3 = TextureId::tryFromBaseFrame(43);

	ASSERT_TRUE(id1.has_value());
	ASSERT_TRUE(id2.has_value());
	ASSERT_TRUE(id3.has_value());

	TextureIdHasher hasher;

	EXPECT_EQ(hasher(*id1), hasher(*id2)) << "Same base frame must produce same hash";
}

TEST(TextureIdContract, EqualitySemantics)
{
	auto id1 = TextureId::tryFromBaseFrame(42);
	auto id2 = TextureId::tryFromBaseFrame(42);
	auto id3 = TextureId::tryFromBaseFrame(43);

	ASSERT_TRUE(id1.has_value());
	ASSERT_TRUE(id2.has_value());
	ASSERT_TRUE(id3.has_value());

	EXPECT_EQ(*id1, *id2) << "Same base frame must be equal";
	EXPECT_NE(*id1, *id3) << "Different base frames must not be equal";
}

// =============================================================================
// SECTION 7: Vulkan Constants Sanity Check
// =============================================================================

TEST(VulkanConstants, BindlessSlotConfiguration)
{
	const uint32_t dynamicSlots = kMaxBindlessTextures - kBindlessFirstDynamicTextureSlot;

	EXPECT_GT(dynamicSlots, 0u) << "Must have at least one dynamic slot";
	EXPECT_EQ(kBindlessFirstDynamicTextureSlot, 4u) << "First 4 slots are reserved for builtins";
	EXPECT_EQ(kMaxBindlessTextures, 1024u) << "Max bindless textures is 1024";
	EXPECT_EQ(dynamicSlots, 1020u) << "1020 dynamic slots available";
}
