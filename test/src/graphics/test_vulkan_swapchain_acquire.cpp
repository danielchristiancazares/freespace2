/**
 * @file test_vulkan_swapchain_acquire.cpp
 * @brief Tests for swapchain acquire retry logic after recreation
 *
 * Documents the retry behavior added to address (NOT fix) bug C5.
 *
 * The bug: flip() asserts when acquireImage() returns sentinel. This is the
 * root cause - flip() cannot handle acquisition failure.
 *
 * What was addressed: Retry logic was added to acquireImage() so that after
 * successful swapchain recreation, it retries instead of returning sentinel.
 *
 * Why this is NOT a fix:
 * - If recreation fails, sentinel still returned -> crash
 * - If retry fails, sentinel still returned -> crash
 * - The assertion in flip() can still fire
 * - Invalid state can still occur, just less frequently
 *
 * A true fix would restructure flip() to handle acquisition failure gracefully
 * (skip the frame) instead of asserting.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <limits>

namespace {

/**
 * Result from a simulated acquireNextImage call
 */
struct MockAcquireResult {
	uint32_t imageIndex = 0;
	bool needsRecreate = false;
	bool success = false;
};

/**
 * Simulates VulkanDevice acquire/recreate behavior for testing
 */
class MockVulkanDevice {
public:
	MockAcquireResult acquireNextImage() {
		MockAcquireResult result;
		result.imageIndex = m_nextImageIndex;

		if (m_forceOutOfDate) {
			result.needsRecreate = true;
			result.success = false;
			m_forceOutOfDate = false;  // Clear after one call
			return result;
		}

		if (m_forceFailure) {
			result.success = false;
			return result;
		}

		result.success = true;
		return result;
	}

	bool recreateSwapchain() {
		if (m_recreateShouldFail) {
			return false;
		}
		m_swapchainRecreated = true;
		return true;
	}

	void resizeRenderTargets() {
		m_renderTargetsResized = true;
	}

	// Test configuration
	void setForceOutOfDate(bool force) { m_forceOutOfDate = force; }
	void setForceFailure(bool force) { m_forceFailure = force; }
	void setRecreateShouldFail(bool fail) { m_recreateShouldFail = fail; }
	void setNextImageIndex(uint32_t idx) { m_nextImageIndex = idx; }

	// Test verification
	bool wasSwapchainRecreated() const { return m_swapchainRecreated; }
	bool wereRenderTargetsResized() const { return m_renderTargetsResized; }

private:
	bool m_forceOutOfDate = false;
	bool m_forceFailure = false;
	bool m_recreateShouldFail = false;
	bool m_swapchainRecreated = false;
	bool m_renderTargetsResized = false;
	uint32_t m_nextImageIndex = 0;
};

/**
 * Implements the FIXED acquire logic with retry after recreation
 */
class SwapchainAcquireLogic {
public:
	static constexpr uint32_t kInvalidImageIndex = std::numeric_limits<uint32_t>::max();

	explicit SwapchainAcquireLogic(MockVulkanDevice& device)
		: m_device(device) {}

	/**
	 * Acquire next swapchain image with retry after recreation.
	 *
	 * This implements the FIXED behavior:
	 * 1. Try to acquire
	 * 2. If OUT_OF_DATE, recreate swapchain and resize render targets
	 * 3. RETRY the acquire after successful recreation (this was the bug fix)
	 * 4. Return valid image index or sentinel on failure
	 */
	uint32_t acquireImage() {
		auto result = m_device.acquireNextImage();

		if (result.needsRecreate) {
			if (!m_device.recreateSwapchain()) {
				// Recreation failed - cannot recover
				return kInvalidImageIndex;
			}
			m_device.resizeRenderTargets();

			// CRITICAL FIX: Retry acquire after successful recreation
			result = m_device.acquireNextImage();
			if (!result.success) {
				return kInvalidImageIndex;
			}
			return result.imageIndex;
		}

		if (!result.success) {
			return kInvalidImageIndex;
		}

		return result.imageIndex;
	}

private:
	MockVulkanDevice& m_device;
};

} // namespace

// Test: Normal acquire succeeds
TEST(VulkanSwapchainAcquire, NormalAcquire_ReturnsValidIndex)
{
	MockVulkanDevice device;
	device.setNextImageIndex(2);

	SwapchainAcquireLogic logic(device);
	uint32_t imageIndex = logic.acquireImage();

	EXPECT_EQ(imageIndex, 2u);
	EXPECT_FALSE(device.wasSwapchainRecreated());
}

// Test: OUT_OF_DATE triggers recreation and retry
TEST(VulkanSwapchainAcquire, OutOfDate_RecreatesAndRetries)
{
	MockVulkanDevice device;
	device.setForceOutOfDate(true);  // First call returns OUT_OF_DATE
	device.setNextImageIndex(0);      // Second call returns index 0

	SwapchainAcquireLogic logic(device);
	uint32_t imageIndex = logic.acquireImage();

	// Should succeed after retry
	EXPECT_EQ(imageIndex, 0u);
	EXPECT_TRUE(device.wasSwapchainRecreated());
	EXPECT_TRUE(device.wereRenderTargetsResized());
}

// Test: Recreation failure returns sentinel
TEST(VulkanSwapchainAcquire, RecreationFails_ReturnsSentinel)
{
	MockVulkanDevice device;
	device.setForceOutOfDate(true);
	device.setRecreateShouldFail(true);

	SwapchainAcquireLogic logic(device);
	uint32_t imageIndex = logic.acquireImage();

	EXPECT_EQ(imageIndex, SwapchainAcquireLogic::kInvalidImageIndex);
}

// Test: Acquire failure after recreation returns sentinel
TEST(VulkanSwapchainAcquire, RetryAfterRecreationFails_ReturnsSentinel)
{
	MockVulkanDevice device;
	device.setForceOutOfDate(true);
	// Recreation succeeds but acquire after recreation fails
	device.setForceFailure(true);

	// Override: forceFailure takes effect after forceOutOfDate is cleared
	// Need custom mock behavior - simulate persistent failure

	SwapchainAcquireLogic logic(device);
	uint32_t imageIndex = logic.acquireImage();

	// Recreation happened but retry failed
	EXPECT_TRUE(device.wasSwapchainRecreated());
	EXPECT_EQ(imageIndex, SwapchainAcquireLogic::kInvalidImageIndex);
}

// Test: Persistent failure without OUT_OF_DATE returns sentinel
TEST(VulkanSwapchainAcquire, PersistentFailure_ReturnsSentinel)
{
	MockVulkanDevice device;
	device.setForceFailure(true);

	SwapchainAcquireLogic logic(device);
	uint32_t imageIndex = logic.acquireImage();

	EXPECT_EQ(imageIndex, SwapchainAcquireLogic::kInvalidImageIndex);
	EXPECT_FALSE(device.wasSwapchainRecreated());
}

/**
 * Test documenting the retry behavior added for C5 (NOT a fix)
 *
 * The OLD behavior (no retry):
 *   1. acquireNextImage returns needsRecreate=true
 *   2. recreateSwapchain() succeeds
 *   3. Returns sentinel WITHOUT retrying
 *   4. flip() assertion crashes
 *
 * The NEW behavior (with retry):
 *   1. acquireNextImage returns needsRecreate=true
 *   2. recreateSwapchain() succeeds
 *   3. RETRIES acquireNextImage
 *   4. Returns valid image index IF retry succeeds
 *
 * This is NOT a fix because:
 *   - If recreation fails, sentinel still returned -> crash
 *   - If retry fails, sentinel still returned -> crash
 *   - flip() still cannot handle failure (the root cause)
 */
TEST(VulkanSwapchainAcquire, C5_RetryAfterSuccessfulRecreation)
{
	MockVulkanDevice device;

	// Simulate resize: first acquire is OUT_OF_DATE, second succeeds
	device.setForceOutOfDate(true);
	device.setNextImageIndex(1);

	SwapchainAcquireLogic logic(device);
	uint32_t imageIndex = logic.acquireImage();

	// Retry logic returns valid index when retry succeeds (addresses but doesn't fix C5)
	ASSERT_NE(imageIndex, SwapchainAcquireLogic::kInvalidImageIndex)
		<< "Retry logic failed: acquireImage returned sentinel after successful "
		   "swapchain recreation even though retry should have succeeded";

	EXPECT_TRUE(device.wasSwapchainRecreated());
	EXPECT_TRUE(device.wereRenderTargetsResized());
}
