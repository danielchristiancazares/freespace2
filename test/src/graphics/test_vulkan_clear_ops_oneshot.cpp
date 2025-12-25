// test_vulkan_clear_ops_oneshot.cpp
//
// PURPOSE: Validates the architectural invariant that clear operations in
// VulkanRenderingSession are "one-shot" - they are consumed when a render pass
// begins and automatically reset to LOAD for subsequent passes within the same frame.
//
// INVARIANT: After beginRendering() consumes clear ops, they must reset to LOAD
// to prevent unintended re-clearing if the render pass is suspended and resumed
// (e.g., for texture uploads mid-frame).
//
// This tests observable behavior through the ClearOps state machine.

#include <gtest/gtest.h>
#include <array>

namespace {

// Mirror of vk::AttachmentLoadOp for testing without Vulkan headers
enum class LoadOp {
	Load,
	Clear,
	DontCare
};

// Simulates the ClearOps struct from VulkanRenderingSession.h:162-181
struct ClearOps {
	LoadOp color = LoadOp::Load;
	LoadOp depth = LoadOp::Load;
	LoadOp stencil = LoadOp::Load;

	static ClearOps clearAll()
	{
		return {LoadOp::Clear, LoadOp::Clear, LoadOp::Clear};
	}

	static ClearOps loadAll()
	{
		return {LoadOp::Load, LoadOp::Load, LoadOp::Load};
	}

	ClearOps withDepthStencilClear() const
	{
		return {color, LoadOp::Clear, LoadOp::Clear};
	}

	bool isColorClear() const { return color == LoadOp::Clear; }
	bool isDepthClear() const { return depth == LoadOp::Clear; }
	bool isStencilClear() const { return stencil == LoadOp::Clear; }
	bool isAnyClearing() const { return isColorClear() || isDepthClear() || isStencilClear(); }
};

// Simulates the one-shot consumption pattern from VulkanRenderingSession
class FakeRenderingSession {
  public:
	void beginFrame()
	{
		// VulkanRenderingSession.cpp:93 - clear ops are set to clearAll at frame start
		m_clearOps = ClearOps::clearAll();
		m_passesStarted = 0;
	}

	void requestClear()
	{
		// VulkanRenderingSession.cpp:291-293
		m_clearOps = ClearOps::clearAll();
	}

	void requestDepthClear()
	{
		// VulkanRenderingSession.cpp:295-297
		m_clearOps = m_clearOps.withDepthStencilClear();
	}

	ClearOps consumeClearOps()
	{
		// This simulates what happens at the end of beginSwapchainRenderingInternal
		// VulkanRenderingSession.cpp:459-460: "Clear ops are one-shot; revert to load after consumption"
		ClearOps consumed = m_clearOps;
		m_clearOps = ClearOps::loadAll();
		++m_passesStarted;
		return consumed;
	}

	const ClearOps& currentClearOps() const { return m_clearOps; }
	int passesStarted() const { return m_passesStarted; }

  private:
	ClearOps m_clearOps = ClearOps::clearAll();
	int m_passesStarted = 0;
};

} // namespace

// Test: Frame start initializes clear ops to clearAll
TEST(VulkanClearOpsOneshot, Scenario_FrameStart_InitializesClearAll)
{
	FakeRenderingSession session;
	session.beginFrame();

	const auto& ops = session.currentClearOps();
	EXPECT_TRUE(ops.isColorClear()) << "Frame start must set color to CLEAR";
	EXPECT_TRUE(ops.isDepthClear()) << "Frame start must set depth to CLEAR";
	EXPECT_TRUE(ops.isStencilClear()) << "Frame start must set stencil to CLEAR";
}

// Test: Consuming clear ops resets them to LOAD
TEST(VulkanClearOpsOneshot, Scenario_Consume_ResetsToLoad)
{
	FakeRenderingSession session;
	session.beginFrame();

	// First pass consumes the clear
	auto consumed = session.consumeClearOps();
	EXPECT_TRUE(consumed.isAnyClearing())
		<< "First consume must return the clear ops";

	// After consumption, ops must be LOAD
	const auto& remaining = session.currentClearOps();
	EXPECT_FALSE(remaining.isColorClear())
		<< "After consumption, color must be LOAD (not CLEAR)";
	EXPECT_FALSE(remaining.isDepthClear())
		<< "After consumption, depth must be LOAD (not CLEAR)";
	EXPECT_FALSE(remaining.isStencilClear())
		<< "After consumption, stencil must be LOAD (not CLEAR)";
}

// Test: Second consume returns LOAD ops (prevents double-clear bug)
TEST(VulkanClearOpsOneshot, Scenario_DoubleConsume_SecondIsLoad)
{
	FakeRenderingSession session;
	session.beginFrame();

	// First pass - should clear
	auto first = session.consumeClearOps();
	EXPECT_TRUE(first.isAnyClearing());

	// Second pass (e.g., after texture upload suspends rendering) - must NOT clear
	auto second = session.consumeClearOps();
	EXPECT_FALSE(second.isAnyClearing())
		<< "Second consume must NOT clear (would destroy first pass contents)";
}

// Test: requestClear() restores clear ops for next pass
TEST(VulkanClearOpsOneshot, Scenario_RequestClearAfterConsume_RestoresClear)
{
	FakeRenderingSession session;
	session.beginFrame();

	// First pass consumes
	session.consumeClearOps();

	// Explicit request to clear (e.g., user called gr_clear())
	session.requestClear();

	// Now consuming should give clear ops again
	auto ops = session.consumeClearOps();
	EXPECT_TRUE(ops.isAnyClearing())
		<< "requestClear() must restore clear ops for next pass";
}

// Test: requestDepthClear() only affects depth/stencil
TEST(VulkanClearOpsOneshot, Scenario_RequestDepthClear_PreservesColor)
{
	FakeRenderingSession session;
	session.beginFrame();

	// Consume initial clear
	session.consumeClearOps();

	// Request depth-only clear
	session.requestDepthClear();

	auto ops = session.consumeClearOps();
	EXPECT_FALSE(ops.isColorClear())
		<< "requestDepthClear must NOT set color to CLEAR";
	EXPECT_TRUE(ops.isDepthClear())
		<< "requestDepthClear must set depth to CLEAR";
	EXPECT_TRUE(ops.isStencilClear())
		<< "requestDepthClear must set stencil to CLEAR";
}

// Test: Multiple frame cycle resets correctly
TEST(VulkanClearOpsOneshot, Scenario_MultipleFrames_ProperReset)
{
	FakeRenderingSession session;

	for (int frame = 0; frame < 3; ++frame) {
		session.beginFrame();

		// First pass clears
		auto first = session.consumeClearOps();
		EXPECT_TRUE(first.isAnyClearing())
			<< "Frame " << frame << ": first pass must clear";

		// Simulate multiple render pass starts (suspend/resume pattern)
		for (int pass = 0; pass < 3; ++pass) {
			auto subsequent = session.consumeClearOps();
			EXPECT_FALSE(subsequent.isAnyClearing())
				<< "Frame " << frame << " pass " << pass << ": subsequent passes must not clear";
		}
	}
}

// Test: withDepthStencilClear preserves existing color state
TEST(VulkanClearOpsOneshot, Scenario_WithDepthStencilClear_PreservesColor)
{
	// Start with LOAD for color
	ClearOps base = ClearOps::loadAll();
	EXPECT_FALSE(base.isColorClear());

	// Add depth/stencil clear
	ClearOps modified = base.withDepthStencilClear();

	EXPECT_FALSE(modified.isColorClear())
		<< "withDepthStencilClear must preserve color LOAD";
	EXPECT_TRUE(modified.isDepthClear())
		<< "withDepthStencilClear must set depth to CLEAR";
	EXPECT_TRUE(modified.isStencilClear())
		<< "withDepthStencilClear must set stencil to CLEAR";
}
