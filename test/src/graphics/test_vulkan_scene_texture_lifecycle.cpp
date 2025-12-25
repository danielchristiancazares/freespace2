// test_vulkan_scene_texture_lifecycle.cpp
//
// PURPOSE: Validates the scene texture lifecycle in VulkanRenderer, which mirrors
// OpenGL's scene_texture_begin/scene_texture_end pattern. This controls:
// - Whether rendering goes to the HDR scene target vs directly to swapchain
// - When the HDR pipeline (bloom, tonemapping) is active
// - The effect texture snapshot for distortion/shader effects
//
// INVARIANT: Scene texture state is strictly per-frame. The m_sceneTexture optional
// must be reset at frame boundaries and beginSceneTexture must be idempotent within
// a frame (matching OpenGL's Scene_framebuffer_in_frame guard).

#include <gtest/gtest.h>
#include <optional>
#include <cstdint>

namespace {

// Simulates the SceneTextureState from VulkanRenderer
struct SceneTextureState {
	bool hdrEnabled = false;
};

// Simulates the scene texture portions of VulkanRenderer
class FakeSceneTextureRenderer {
  public:
	void beginFrame()
	{
		// VulkanRenderer.cpp:289 - Scene texture state is strictly per-frame
		m_sceneTexture.reset();
		m_frameCounter++;
		m_renderTarget = RenderTarget::Swapchain;
		m_effectSnapshotTaken = false;
	}

	// VulkanRenderer.cpp:652-675
	void beginSceneTexture(bool enableHdrPipeline)
	{
		// VulkanRenderer.cpp:659 - if already active, ignore (OpenGL parity)
		if (m_sceneTexture.has_value()) {
			++m_ignoredBeginCalls;
			return;
		}

		m_sceneTexture = SceneTextureState{enableHdrPipeline};
		m_renderTarget = RenderTarget::SceneHdr;
	}

	// VulkanRenderer.cpp:677-692
	void copySceneEffectTexture()
	{
		// VulkanRenderer.cpp:684 - no-op if not in scene framebuffer
		if (!m_sceneTexture.has_value()) {
			return;
		}

		m_effectSnapshotTaken = true;
	}

	// VulkanRenderer.cpp:694-788 (simplified)
	void endSceneTexture(bool enablePostProcessing)
	{
		// VulkanRenderer.cpp:701-703 - no-op if not active
		if (!m_sceneTexture.has_value()) {
			return;
		}

		bool hdrEnabled = m_sceneTexture->hdrEnabled;

		// Simulate post-processing chain
		if (enablePostProcessing && hdrEnabled) {
			m_bloomExecuted = true;
		}

		m_tonemappingExecuted = true;

		// VulkanRenderer.cpp:784 - Exit scene texture mode
		m_sceneTexture.reset();
		m_renderTarget = RenderTarget::Swapchain;
	}

	// VulkanRenderer.cpp:797-809 - Used by deferred to check target
	void requestMainTargetWithDepth()
	{
		if (m_sceneTexture.has_value()) {
			m_renderTarget = RenderTarget::SceneHdr;
		} else {
			m_renderTarget = RenderTarget::Swapchain;
		}
	}

	// Accessors
	bool isSceneTextureActive() const { return m_sceneTexture.has_value(); }
	bool isHdrEnabled() const { return m_sceneTexture.has_value() && m_sceneTexture->hdrEnabled; }
	bool effectSnapshotTaken() const { return m_effectSnapshotTaken; }
	bool bloomExecuted() const { return m_bloomExecuted; }
	bool tonemappingExecuted() const { return m_tonemappingExecuted; }
	uint32_t frameCounter() const { return m_frameCounter; }
	int ignoredBeginCalls() const { return m_ignoredBeginCalls; }

	enum class RenderTarget {
		Swapchain,
		SceneHdr
	};

	RenderTarget currentTarget() const { return m_renderTarget; }

  private:
	std::optional<SceneTextureState> m_sceneTexture;
	RenderTarget m_renderTarget = RenderTarget::Swapchain;
	uint32_t m_frameCounter = 0;
	bool m_effectSnapshotTaken = false;
	bool m_bloomExecuted = false;
	bool m_tonemappingExecuted = false;
	int m_ignoredBeginCalls = 0;
};

} // namespace

// Test: Frame start resets scene texture state
TEST(VulkanSceneTextureLifecycle, FrameStart_ResetsState)
{
	FakeSceneTextureRenderer renderer;

	renderer.beginFrame();
	renderer.beginSceneTexture(true);

	EXPECT_TRUE(renderer.isSceneTextureActive());

	// New frame should reset
	renderer.beginFrame();

	EXPECT_FALSE(renderer.isSceneTextureActive())
		<< "Scene texture state must be reset at frame boundary";
}

// Test: beginSceneTexture is idempotent within a frame
TEST(VulkanSceneTextureLifecycle, BeginSceneTexture_Idempotent)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();

	renderer.beginSceneTexture(true);
	EXPECT_TRUE(renderer.isSceneTextureActive());

	// Second call should be ignored (OpenGL parity)
	renderer.beginSceneTexture(false); // Different HDR setting

	EXPECT_TRUE(renderer.isHdrEnabled())
		<< "HDR setting from first call must be preserved";
	EXPECT_EQ(renderer.ignoredBeginCalls(), 1)
		<< "Second begin call must be ignored";
}

// Test: beginSceneTexture routes to HDR target
TEST(VulkanSceneTextureLifecycle, BeginSceneTexture_RoutesToHdrTarget)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();

	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::Swapchain);

	renderer.beginSceneTexture(true);

	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::SceneHdr)
		<< "beginSceneTexture must route to HDR target";
}

// Test: endSceneTexture returns to swapchain
TEST(VulkanSceneTextureLifecycle, EndSceneTexture_ReturnsToSwapchain)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();
	renderer.beginSceneTexture(true);

	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::SceneHdr);

	renderer.endSceneTexture(true);

	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::Swapchain)
		<< "endSceneTexture must return to swapchain";
	EXPECT_FALSE(renderer.isSceneTextureActive());
}

// Test: copySceneEffectTexture requires active scene texture
TEST(VulkanSceneTextureLifecycle, CopyEffectTexture_RequiresActive)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();

	// Call without active scene texture
	renderer.copySceneEffectTexture();

	EXPECT_FALSE(renderer.effectSnapshotTaken())
		<< "copySceneEffectTexture must be no-op without active scene texture";

	// Now with active scene texture
	renderer.beginSceneTexture(true);
	renderer.copySceneEffectTexture();

	EXPECT_TRUE(renderer.effectSnapshotTaken());
}

// Test: HDR pipeline only runs when enabled
TEST(VulkanSceneTextureLifecycle, HdrPipeline_OnlyWhenEnabled)
{
	FakeSceneTextureRenderer renderer;

	// HDR disabled
	renderer.beginFrame();
	renderer.beginSceneTexture(false);
	renderer.endSceneTexture(true); // post-processing enabled, but HDR disabled

	EXPECT_FALSE(renderer.bloomExecuted())
		<< "Bloom must not run when HDR is disabled";
	EXPECT_TRUE(renderer.tonemappingExecuted())
		<< "Tonemapping always runs (passthrough when HDR disabled)";

	// HDR enabled
	renderer.beginFrame();
	renderer.beginSceneTexture(true);
	renderer.endSceneTexture(true);

	EXPECT_TRUE(renderer.bloomExecuted())
		<< "Bloom must run when HDR is enabled and post-processing requested";
}

// Test: Post-processing disabled skips bloom
TEST(VulkanSceneTextureLifecycle, PostProcessingDisabled_SkipsBloom)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();
	renderer.beginSceneTexture(true);
	renderer.endSceneTexture(false); // post-processing disabled

	EXPECT_FALSE(renderer.bloomExecuted())
		<< "Bloom must not run when post-processing is disabled";
}

// Test: requestMainTargetWithDepth respects scene texture state
TEST(VulkanSceneTextureLifecycle, RequestMainTarget_RespectsSceneState)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();

	renderer.requestMainTargetWithDepth();
	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::Swapchain)
		<< "Without scene texture, main target is swapchain";

	renderer.beginSceneTexture(true);
	renderer.requestMainTargetWithDepth();
	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::SceneHdr)
		<< "With scene texture, main target is HDR scene";
}

// Test: endSceneTexture is no-op without active scene
TEST(VulkanSceneTextureLifecycle, EndSceneTexture_NoopWithoutActive)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();

	// Should not crash or change state
	renderer.endSceneTexture(true);

	EXPECT_FALSE(renderer.tonemappingExecuted())
		<< "endSceneTexture must be no-op without active scene";
}

// Test: Multiple frames with scene texture
TEST(VulkanSceneTextureLifecycle, MultipleFrames_IndependentState)
{
	FakeSceneTextureRenderer renderer;

	for (uint32_t i = 0; i < 3; ++i) {
		renderer.beginFrame();

		EXPECT_FALSE(renderer.isSceneTextureActive())
			<< "Frame " << i << " must start without scene texture";

		renderer.beginSceneTexture(i % 2 == 0); // Alternate HDR setting
		renderer.copySceneEffectTexture();
		renderer.endSceneTexture(true);

		EXPECT_FALSE(renderer.isSceneTextureActive())
			<< "Frame " << i << " must end without scene texture";
	}
}

// Test: Effect snapshot reset between frames
TEST(VulkanSceneTextureLifecycle, EffectSnapshot_ResetBetweenFrames)
{
	FakeSceneTextureRenderer renderer;

	renderer.beginFrame();
	renderer.beginSceneTexture(true);
	renderer.copySceneEffectTexture();
	EXPECT_TRUE(renderer.effectSnapshotTaken());

	renderer.endSceneTexture(true);
	renderer.beginFrame();

	EXPECT_FALSE(renderer.effectSnapshotTaken())
		<< "Effect snapshot flag must be reset at frame boundary";
}

// Test: Complete scene texture workflow
TEST(VulkanSceneTextureLifecycle, CompleteWorkflow)
{
	FakeSceneTextureRenderer renderer;
	renderer.beginFrame();

	// 1. Begin scene texture (HDR enabled)
	renderer.beginSceneTexture(true);
	EXPECT_TRUE(renderer.isSceneTextureActive());
	EXPECT_TRUE(renderer.isHdrEnabled());
	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::SceneHdr);

	// 2. Simulate scene rendering...

	// 3. Copy effect texture mid-scene (for distortion)
	renderer.copySceneEffectTexture();
	EXPECT_TRUE(renderer.effectSnapshotTaken());

	// 4. End scene texture (runs post-processing)
	renderer.endSceneTexture(true);

	// Verify final state
	EXPECT_FALSE(renderer.isSceneTextureActive());
	EXPECT_EQ(renderer.currentTarget(), FakeSceneTextureRenderer::RenderTarget::Swapchain);
	EXPECT_TRUE(renderer.bloomExecuted());
	EXPECT_TRUE(renderer.tonemappingExecuted());
}
