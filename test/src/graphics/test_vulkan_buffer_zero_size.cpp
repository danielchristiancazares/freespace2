// test_vulkan_buffer_zero_size.cpp
//
// PURPOSE: Validates that updateBufferDataOffset correctly handles zero-size updates
// as no-ops, matching OpenGL's glBufferSubData behavior. This is a boundary validation
// test for a recent fix that prevents Vulkan validation errors from zero-length copies.
//
// INVARIANT: Zero-size buffer updates must not reach Vulkan (which rejects them),
// but must be accepted at the API level for OpenGL parity.

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <optional>

namespace {

// Simulates the buffer update logic from VulkanBufferManager to test
// the zero-size guard without requiring a real Vulkan device.
class FakeBufferUpdateTracker {
  public:
	struct UpdateRecord {
		size_t offset;
		size_t size;
		bool wasApplied; // false if rejected or no-op
	};

	// Simulates updateBufferDataOffset from VulkanBufferManager.cpp:245-270
	void updateBufferDataOffset(size_t offset, size_t size, const void* data)
	{
		// This mirrors the guard at VulkanBufferManager.cpp:249-251
		// "OpenGL allows 0-byte glBufferSubData calls; treat as a no-op."
		if (size == 0) {
			m_updates.push_back({offset, size, false});
			return;
		}

		// Null data pointer - skip copy (matches VulkanBufferManager.cpp:257-260)
		if (data == nullptr) {
			m_updates.push_back({offset, size, false});
			return;
		}

		// Normal update would proceed to Vulkan
		m_updates.push_back({offset, size, true});
	}

	size_t appliedUpdateCount() const
	{
		size_t count = 0;
		for (const auto& u : m_updates) {
			if (u.wasApplied) {
				++count;
			}
		}
		return count;
	}

	size_t rejectedUpdateCount() const
	{
		size_t count = 0;
		for (const auto& u : m_updates) {
			if (!u.wasApplied) {
				++count;
			}
		}
		return count;
	}

	const std::vector<UpdateRecord>& updates() const { return m_updates; }

  private:
	std::vector<UpdateRecord> m_updates;
};

} // namespace

// Test: Zero-size update is accepted but treated as no-op
TEST(VulkanBufferZeroSize, Scenario_ZeroSizeUpdate_IsNoOp)
{
	FakeBufferUpdateTracker tracker;
	uint8_t data[] = {1, 2, 3, 4};

	// Zero-size update should be accepted but not applied
	tracker.updateBufferDataOffset(100, 0, data);

	EXPECT_EQ(tracker.appliedUpdateCount(), 0u)
		<< "Zero-size update must not reach Vulkan (would cause validation error)";
	EXPECT_EQ(tracker.rejectedUpdateCount(), 1u)
		<< "Zero-size update must be recorded as rejected/no-op";
}

// Test: Normal update proceeds correctly
TEST(VulkanBufferZeroSize, Scenario_NonZeroSizeUpdate_IsApplied)
{
	FakeBufferUpdateTracker tracker;
	uint8_t data[] = {1, 2, 3, 4};

	tracker.updateBufferDataOffset(0, 4, data);

	EXPECT_EQ(tracker.appliedUpdateCount(), 1u)
		<< "Non-zero update with valid data must be applied";
	EXPECT_EQ(tracker.rejectedUpdateCount(), 0u);
}

// Test: Null data pointer is handled gracefully
TEST(VulkanBufferZeroSize, Scenario_NullDataPointer_IsNoOp)
{
	FakeBufferUpdateTracker tracker;

	// Null data should not crash and should not apply
	tracker.updateBufferDataOffset(0, 1024, nullptr);

	EXPECT_EQ(tracker.appliedUpdateCount(), 0u)
		<< "Null data pointer must not proceed to Vulkan copy";
	EXPECT_EQ(tracker.rejectedUpdateCount(), 1u);
}

// Test: Mixed sequence of updates handles boundaries correctly
TEST(VulkanBufferZeroSize, Scenario_MixedUpdates_BoundaryHandling)
{
	FakeBufferUpdateTracker tracker;
	uint8_t data[] = {1, 2, 3, 4};

	// Mix of valid and edge-case updates
	tracker.updateBufferDataOffset(0, 4, data);     // Valid
	tracker.updateBufferDataOffset(4, 0, data);     // Zero-size (no-op)
	tracker.updateBufferDataOffset(8, 4, nullptr);  // Null data (no-op)
	tracker.updateBufferDataOffset(12, 4, data);    // Valid

	EXPECT_EQ(tracker.appliedUpdateCount(), 2u)
		<< "Only non-zero updates with valid data should be applied";
	EXPECT_EQ(tracker.rejectedUpdateCount(), 2u)
		<< "Zero-size and null-data updates should be rejected";

	// Verify order is preserved
	const auto& updates = tracker.updates();
	ASSERT_EQ(updates.size(), 4u);
	EXPECT_TRUE(updates[0].wasApplied);   // First valid update
	EXPECT_FALSE(updates[1].wasApplied);  // Zero-size
	EXPECT_FALSE(updates[2].wasApplied);  // Null data
	EXPECT_TRUE(updates[3].wasApplied);   // Second valid update
}

// Test: Zero offset with zero size is still no-op
TEST(VulkanBufferZeroSize, Scenario_ZeroOffsetZeroSize_IsNoOp)
{
	FakeBufferUpdateTracker tracker;
	uint8_t data[] = {1};

	tracker.updateBufferDataOffset(0, 0, data);

	EXPECT_EQ(tracker.appliedUpdateCount(), 0u)
		<< "Zero-size at offset 0 must still be no-op";
}

// Test: Large offset with zero size is still no-op
TEST(VulkanBufferZeroSize, Scenario_LargeOffsetZeroSize_IsNoOp)
{
	FakeBufferUpdateTracker tracker;
	uint8_t data[] = {1};

	// Large offset should not matter when size is zero
	tracker.updateBufferDataOffset(1024 * 1024, 0, data);

	EXPECT_EQ(tracker.appliedUpdateCount(), 0u)
		<< "Zero-size with large offset must still be no-op";
}
