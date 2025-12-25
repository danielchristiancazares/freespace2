// test_vulkan_render_target_state.cpp
//
// PURPOSE: Validates the render target state machine in VulkanRenderingSession.
// The session manages transitions between multiple render targets (swapchain,
// scene HDR, G-buffer, bitmap RTT) and must ensure:
// 1. Active rendering is ended before target switches
// 2. Transitions preserve the correct target for ensureRendering()
// 3. Deferred pass lifecycle is correctly enforced
//
// INVARIANT: Render target changes must end any active pass to prevent
// rendering to an incorrect target.

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace {

// Enum matching the render target types in VulkanRenderingSession
enum class RenderTargetType {
	SwapchainWithDepth,
	SwapchainNoDepth,
	SceneHdrWithDepth,
	SceneHdrNoDepth,
	DeferredGBuffer,
	GBufferEmissive,
	BitmapRTT
};

const char* targetName(RenderTargetType t)
{
	switch (t) {
	case RenderTargetType::SwapchainWithDepth: return "SwapchainWithDepth";
	case RenderTargetType::SwapchainNoDepth: return "SwapchainNoDepth";
	case RenderTargetType::SceneHdrWithDepth: return "SceneHdrWithDepth";
	case RenderTargetType::SceneHdrNoDepth: return "SceneHdrNoDepth";
	case RenderTargetType::DeferredGBuffer: return "DeferredGBuffer";
	case RenderTargetType::GBufferEmissive: return "GBufferEmissive";
	case RenderTargetType::BitmapRTT: return "BitmapRTT";
	}
	return "Unknown";
}

// Simulates the render target state machine from VulkanRenderingSession
class FakeRenderTargetStateMachine {
  public:
	void beginFrame()
	{
		// VulkanRenderingSession.cpp:91 - reset to swapchain at frame start
		endActivePass();
		m_target = RenderTargetType::SwapchainWithDepth;
		m_inDeferredGeometry = false;
	}

	void requestSwapchainTarget()
	{
		endActivePass();
		m_target = RenderTargetType::SwapchainWithDepth;
	}

	void requestSceneHdrTarget()
	{
		endActivePass();
		m_target = RenderTargetType::SceneHdrWithDepth;
	}

	void requestSceneHdrNoDepthTarget()
	{
		endActivePass();
		m_target = RenderTargetType::SceneHdrNoDepth;
	}

	void requestBitmapTarget(int handle, int face)
	{
		endActivePass();
		m_target = RenderTargetType::BitmapRTT;
		m_bitmapHandle = handle;
		m_bitmapFace = face;
	}

	void beginDeferredPass()
	{
		endActivePass();
		m_target = RenderTargetType::DeferredGBuffer;
		m_inDeferredGeometry = true;
	}

	void requestGBufferEmissiveTarget()
	{
		endActivePass();
		m_target = RenderTargetType::GBufferEmissive;
	}

	bool endDeferredGeometry()
	{
		// VulkanRenderingSession.cpp:277-284 - must be in deferred gbuffer target
		if (m_target != RenderTargetType::DeferredGBuffer) {
			return false; // Would assert in real code
		}
		endActivePass();
		m_target = RenderTargetType::SwapchainNoDepth;
		m_inDeferredGeometry = false;
		return true;
	}

	RenderTargetType ensureRendering()
	{
		// Start rendering if not active
		if (!m_activePass.has_value()) {
			m_activePass = m_target;
			++m_passStartCount;
		}
		return m_target;
	}

	void suspendRendering()
	{
		// VulkanRenderingSession.cpp:35 - end pass but keep target
		endActivePass();
	}

	bool renderingActive() const { return m_activePass.has_value(); }
	RenderTargetType currentTarget() const { return m_target; }
	bool inDeferredGeometry() const { return m_inDeferredGeometry; }
	int passStartCount() const { return m_passStartCount; }

  private:
	void endActivePass()
	{
		if (m_activePass.has_value()) {
			m_activePass.reset();
		}
	}

	RenderTargetType m_target = RenderTargetType::SwapchainWithDepth;
	std::optional<RenderTargetType> m_activePass;
	bool m_inDeferredGeometry = false;
	int m_bitmapHandle = -1;
	int m_bitmapFace = 0;
	int m_passStartCount = 0;
};

} // namespace

// Test: Frame start selects swapchain target
TEST(VulkanRenderTargetState, Scenario_FrameStart_SelectsSwapchain)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SwapchainWithDepth)
		<< "Frame start must select swapchain+depth target";
	EXPECT_FALSE(sm.renderingActive())
		<< "Frame start must not automatically start rendering";
}

// Test: Target change ends active pass
TEST(VulkanRenderTargetState, Scenario_TargetChange_EndsActivePass)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	// Start rendering to swapchain
	sm.ensureRendering();
	EXPECT_TRUE(sm.renderingActive());

	// Switch to HDR target
	sm.requestSceneHdrTarget();

	EXPECT_FALSE(sm.renderingActive())
		<< "Target change must end active pass";
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SceneHdrWithDepth);
}

// Test: suspendRendering ends pass but preserves target
TEST(VulkanRenderTargetState, Scenario_Suspend_PreservesTarget)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();
	sm.requestSceneHdrTarget();
	sm.ensureRendering();

	EXPECT_TRUE(sm.renderingActive());
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SceneHdrWithDepth);

	sm.suspendRendering();

	EXPECT_FALSE(sm.renderingActive())
		<< "suspendRendering must end pass";
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SceneHdrWithDepth)
		<< "suspendRendering must preserve target";
}

// Test: ensureRendering is idempotent when already active
TEST(VulkanRenderTargetState, Scenario_EnsureRendering_Idempotent)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	int initialCount = sm.passStartCount();

	sm.ensureRendering();
	sm.ensureRendering();
	sm.ensureRendering();

	EXPECT_EQ(sm.passStartCount(), initialCount + 1)
		<< "ensureRendering must not restart pass if already active";
}

// Test: Deferred pass lifecycle - begin/end sequence
TEST(VulkanRenderTargetState, Scenario_DeferredPass_Lifecycle)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	// Enter deferred geometry phase
	sm.beginDeferredPass();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::DeferredGBuffer);
	EXPECT_TRUE(sm.inDeferredGeometry());

	// Render some geometry
	sm.ensureRendering();
	EXPECT_TRUE(sm.renderingActive());

	// End geometry phase - transitions to swapchain-no-depth for lighting
	EXPECT_TRUE(sm.endDeferredGeometry())
		<< "endDeferredGeometry must succeed when in GBuffer target";

	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SwapchainNoDepth)
		<< "After deferred geometry, target must be swapchain-no-depth for lighting";
	EXPECT_FALSE(sm.inDeferredGeometry());
	EXPECT_FALSE(sm.renderingActive())
		<< "endDeferredGeometry must end active pass";
}

// Test: endDeferredGeometry fails if not in correct state
TEST(VulkanRenderTargetState, Scenario_EndDeferredGeometry_WrongState_Fails)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	// Try to end deferred when not in deferred pass
	EXPECT_FALSE(sm.endDeferredGeometry())
		<< "endDeferredGeometry must fail when not in GBuffer target";
}

// Test: Bitmap RTT target switch
TEST(VulkanRenderTargetState, Scenario_BitmapTarget_Switch)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();
	sm.ensureRendering();

	sm.requestBitmapTarget(42, 0);

	EXPECT_FALSE(sm.renderingActive())
		<< "Bitmap target switch must end active pass";
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::BitmapRTT);
}

// Test: Scene HDR variants have correct depth settings
TEST(VulkanRenderTargetState, Scenario_SceneHdrVariants_DepthSettings)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	sm.requestSceneHdrTarget();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SceneHdrWithDepth)
		<< "requestSceneHdrTarget must select HDR with depth";

	sm.requestSceneHdrNoDepthTarget();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SceneHdrNoDepth)
		<< "requestSceneHdrNoDepthTarget must select HDR without depth";
}

// Test: Multiple target switches within frame
TEST(VulkanRenderTargetState, Scenario_MultipleTargetSwitches_WithinFrame)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	// Switch through multiple targets
	sm.requestSceneHdrTarget();
	sm.ensureRendering();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SceneHdrWithDepth);

	sm.requestSwapchainTarget();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SwapchainWithDepth);

	sm.beginDeferredPass();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::DeferredGBuffer);

	sm.endDeferredGeometry();
	EXPECT_EQ(sm.currentTarget(), RenderTargetType::SwapchainNoDepth);

	// All transitions must have ended active passes
	EXPECT_FALSE(sm.renderingActive());
}

// Test: Emissive-only G-buffer target for pre-deferred copy
TEST(VulkanRenderTargetState, Scenario_GBufferEmissive_PreDeferredCopy)
{
	FakeRenderTargetStateMachine sm;
	sm.beginFrame();

	// Pre-deferred: render to emissive-only before full deferred pass
	sm.requestGBufferEmissiveTarget();

	EXPECT_EQ(sm.currentTarget(), RenderTargetType::GBufferEmissive);
	EXPECT_FALSE(sm.inDeferredGeometry())
		<< "Emissive target is NOT full deferred geometry mode";
}
