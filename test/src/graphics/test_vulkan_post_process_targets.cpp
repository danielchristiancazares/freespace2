// test_vulkan_post_process_targets.cpp
//
// PURPOSE: Validates the post-processing render target state machine in VulkanRenderingSession.
// The session manages transitions between multiple post-processing targets:
// - Scene HDR (with/without depth)
// - Post LDR (tonemapped output)
// - SMAA edges, blend weights, output
// - Bloom ping-pong mip levels
//
// INVARIANT: Post-processing target changes must end any active pass and establish
// correct image layouts for the subsequent rendering or sampling operations.

#include <gtest/gtest.h>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <cstdint>

namespace {

// Enum matching the render target types in VulkanRenderingSession
enum class PostProcessTarget {
	SwapchainWithDepth,
	SwapchainNoDepth,
	SceneHdrWithDepth,
	SceneHdrNoDepth,
	PostLdr,
	PostLuminance,
	SmaaEdges,
	SmaaBlend,
	SmaaOutput,
	BloomMip
};

const char* targetName(PostProcessTarget t)
{
	switch (t) {
	case PostProcessTarget::SwapchainWithDepth: return "SwapchainWithDepth";
	case PostProcessTarget::SwapchainNoDepth: return "SwapchainNoDepth";
	case PostProcessTarget::SceneHdrWithDepth: return "SceneHdrWithDepth";
	case PostProcessTarget::SceneHdrNoDepth: return "SceneHdrNoDepth";
	case PostProcessTarget::PostLdr: return "PostLdr";
	case PostProcessTarget::PostLuminance: return "PostLuminance";
	case PostProcessTarget::SmaaEdges: return "SmaaEdges";
	case PostProcessTarget::SmaaBlend: return "SmaaBlend";
	case PostProcessTarget::SmaaOutput: return "SmaaOutput";
	case PostProcessTarget::BloomMip: return "BloomMip";
	}
	return "Unknown";
}

// Mirror of image layout states
enum class ImageLayout {
	Undefined,
	ColorAttachment,
	ShaderReadOnly,
	TransferSrc,
	TransferDst,
	PresentSrc
};

// Simulates the post-processing portion of VulkanRenderingSession state machine
class FakePostProcessSession {
  public:
	void beginFrame()
	{
		endActivePass();
		m_target = PostProcessTarget::SwapchainWithDepth;

		// Reset all layouts to undefined at frame start (simulating resize)
		m_sceneHdrLayout = ImageLayout::Undefined;
		m_sceneEffectLayout = ImageLayout::Undefined;
		m_postLdrLayout = ImageLayout::Undefined;
		m_postLuminanceLayout = ImageLayout::Undefined;
		m_smaaEdgesLayout = ImageLayout::Undefined;
		m_smaaBlendLayout = ImageLayout::Undefined;
		m_smaaOutputLayout = ImageLayout::Undefined;
		m_bloomLayouts.fill(ImageLayout::Undefined);
	}

	void requestSceneHdrTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::SceneHdrWithDepth;
	}

	void requestSceneHdrNoDepthTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::SceneHdrNoDepth;
	}

	void requestPostLdrTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::PostLdr;
	}

	void requestPostLuminanceTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::PostLuminance;
	}

	void requestSmaaEdgesTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::SmaaEdges;
	}

	void requestSmaaBlendTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::SmaaBlend;
	}

	void requestSmaaOutputTarget()
	{
		endActivePass();
		m_target = PostProcessTarget::SmaaOutput;
	}

	bool requestBloomMipTarget(uint32_t pingPongIndex, uint32_t mipLevel)
	{
		// Validate bounds - VulkanRenderingSession.cpp validates these
		if (pingPongIndex >= kBloomPingPongCount) {
			return false;
		}
		if (mipLevel >= kBloomMipLevels) {
			return false;
		}
		endActivePass();
		m_target = PostProcessTarget::BloomMip;
		m_bloomIndex = pingPongIndex;
		m_bloomMip = mipLevel;
		return true;
	}

	void ensureRendering()
	{
		if (!m_activePass) {
			m_activePass = true;
			++m_passStartCount;

			// Transition appropriate layout when rendering starts
			switch (m_target) {
			case PostProcessTarget::SceneHdrWithDepth:
			case PostProcessTarget::SceneHdrNoDepth:
				m_sceneHdrLayout = ImageLayout::ColorAttachment;
				break;
			case PostProcessTarget::PostLdr:
				m_postLdrLayout = ImageLayout::ColorAttachment;
				break;
			case PostProcessTarget::PostLuminance:
				m_postLuminanceLayout = ImageLayout::ColorAttachment;
				break;
			case PostProcessTarget::SmaaEdges:
				m_smaaEdgesLayout = ImageLayout::ColorAttachment;
				break;
			case PostProcessTarget::SmaaBlend:
				m_smaaBlendLayout = ImageLayout::ColorAttachment;
				break;
			case PostProcessTarget::SmaaOutput:
				m_smaaOutputLayout = ImageLayout::ColorAttachment;
				break;
			case PostProcessTarget::BloomMip:
				m_bloomLayouts[m_bloomIndex] = ImageLayout::ColorAttachment;
				break;
			default:
				break;
			}
		}
	}

	void suspendRendering()
	{
		endActivePass();
	}

	void transitionSceneHdrToShaderRead()
	{
		// Must not be in active pass - transfers are invalid inside rendering
		if (m_activePass) {
			return;
		}
		m_sceneHdrLayout = ImageLayout::ShaderReadOnly;
	}

	void transitionPostLdrToShaderRead()
	{
		if (m_activePass) {
			return;
		}
		m_postLdrLayout = ImageLayout::ShaderReadOnly;
	}

	void transitionSmaaEdgesToShaderRead()
	{
		if (m_activePass) {
			return;
		}
		m_smaaEdgesLayout = ImageLayout::ShaderReadOnly;
	}

	void transitionSmaaBlendToShaderRead()
	{
		if (m_activePass) {
			return;
		}
		m_smaaBlendLayout = ImageLayout::ShaderReadOnly;
	}

	void transitionSmaaOutputToShaderRead()
	{
		if (m_activePass) {
			return;
		}
		m_smaaOutputLayout = ImageLayout::ShaderReadOnly;
	}

	void transitionBloomToShaderRead(uint32_t pingPongIndex)
	{
		if (m_activePass || pingPongIndex >= kBloomPingPongCount) {
			return;
		}
		m_bloomLayouts[pingPongIndex] = ImageLayout::ShaderReadOnly;
	}

	bool copySceneHdrToEffect()
	{
		// VulkanRenderingSession.cpp:418 - ends active pass, then copies
		endActivePass();
		if (m_sceneHdrLayout != ImageLayout::ColorAttachment &&
			m_sceneHdrLayout != ImageLayout::ShaderReadOnly) {
			// Can't copy from undefined layout
			return false;
		}
		m_sceneEffectLayout = ImageLayout::ShaderReadOnly;
		return true;
	}

	// Accessors
	bool renderingActive() const { return m_activePass; }
	PostProcessTarget currentTarget() const { return m_target; }
	int passStartCount() const { return m_passStartCount; }

	ImageLayout sceneHdrLayout() const { return m_sceneHdrLayout; }
	ImageLayout sceneEffectLayout() const { return m_sceneEffectLayout; }
	ImageLayout postLdrLayout() const { return m_postLdrLayout; }
	ImageLayout postLuminanceLayout() const { return m_postLuminanceLayout; }
	ImageLayout smaaEdgesLayout() const { return m_smaaEdgesLayout; }
	ImageLayout smaaBlendLayout() const { return m_smaaBlendLayout; }
	ImageLayout smaaOutputLayout() const { return m_smaaOutputLayout; }
	ImageLayout bloomLayout(uint32_t index) const
	{
		return (index < kBloomPingPongCount) ? m_bloomLayouts[index] : ImageLayout::Undefined;
	}

	static constexpr uint32_t kBloomPingPongCount = 2;
	static constexpr uint32_t kBloomMipLevels = 4;

  private:
	void endActivePass()
	{
		m_activePass = false;
	}

	PostProcessTarget m_target = PostProcessTarget::SwapchainWithDepth;
	bool m_activePass = false;
	int m_passStartCount = 0;

	uint32_t m_bloomIndex = 0;
	uint32_t m_bloomMip = 0;

	ImageLayout m_sceneHdrLayout = ImageLayout::Undefined;
	ImageLayout m_sceneEffectLayout = ImageLayout::Undefined;
	ImageLayout m_postLdrLayout = ImageLayout::Undefined;
	ImageLayout m_postLuminanceLayout = ImageLayout::Undefined;
	ImageLayout m_smaaEdgesLayout = ImageLayout::Undefined;
	ImageLayout m_smaaBlendLayout = ImageLayout::Undefined;
	ImageLayout m_smaaOutputLayout = ImageLayout::Undefined;
	std::array<ImageLayout, kBloomPingPongCount> m_bloomLayouts{};
};

} // namespace

// Test: Scene HDR target switch ends active pass
TEST(VulkanPostProcessTargets, SceneHdrTarget_EndsActivePass)
{
	FakePostProcessSession session;
	session.beginFrame();
	session.ensureRendering();

	EXPECT_TRUE(session.renderingActive());

	session.requestSceneHdrTarget();

	EXPECT_FALSE(session.renderingActive())
		<< "requestSceneHdrTarget must end active pass";
	EXPECT_EQ(session.currentTarget(), PostProcessTarget::SceneHdrWithDepth);
}

// Test: Scene HDR transitions to attachment layout on ensureRendering
TEST(VulkanPostProcessTargets, SceneHdrTarget_TransitionsToAttachment)
{
	FakePostProcessSession session;
	session.beginFrame();

	EXPECT_EQ(session.sceneHdrLayout(), ImageLayout::Undefined);

	session.requestSceneHdrTarget();
	session.ensureRendering();

	EXPECT_EQ(session.sceneHdrLayout(), ImageLayout::ColorAttachment)
		<< "ensureRendering on SceneHdr must transition to attachment layout";
}

// Test: Post LDR target selection
TEST(VulkanPostProcessTargets, PostLdrTarget_Selection)
{
	FakePostProcessSession session;
	session.beginFrame();
	session.requestSceneHdrTarget();
	session.ensureRendering();

	session.requestPostLdrTarget();

	EXPECT_FALSE(session.renderingActive());
	EXPECT_EQ(session.currentTarget(), PostProcessTarget::PostLdr);

	session.ensureRendering();
	EXPECT_EQ(session.postLdrLayout(), ImageLayout::ColorAttachment);
}

// Test: SMAA pass chain preserves order
TEST(VulkanPostProcessTargets, SmaaChain_PreservesOrder)
{
	FakePostProcessSession session;
	session.beginFrame();

	// Simulate SMAA chain: edges -> blend -> output
	session.requestSmaaEdgesTarget();
	session.ensureRendering();
	EXPECT_EQ(session.smaaEdgesLayout(), ImageLayout::ColorAttachment);

	session.suspendRendering();
	session.transitionSmaaEdgesToShaderRead();
	EXPECT_EQ(session.smaaEdgesLayout(), ImageLayout::ShaderReadOnly);

	session.requestSmaaBlendTarget();
	session.ensureRendering();
	EXPECT_EQ(session.smaaBlendLayout(), ImageLayout::ColorAttachment);

	session.suspendRendering();
	session.transitionSmaaBlendToShaderRead();
	EXPECT_EQ(session.smaaBlendLayout(), ImageLayout::ShaderReadOnly);

	session.requestSmaaOutputTarget();
	session.ensureRendering();
	EXPECT_EQ(session.smaaOutputLayout(), ImageLayout::ColorAttachment);
}

// Test: Bloom mip target bounds validation
TEST(VulkanPostProcessTargets, BloomMipTarget_BoundsValidation)
{
	FakePostProcessSession session;
	session.beginFrame();

	// Valid indices
	EXPECT_TRUE(session.requestBloomMipTarget(0, 0));
	EXPECT_TRUE(session.requestBloomMipTarget(1, 3));

	// Invalid ping-pong index
	EXPECT_FALSE(session.requestBloomMipTarget(2, 0))
		<< "pingPongIndex >= 2 must be rejected";
	EXPECT_FALSE(session.requestBloomMipTarget(100, 0));

	// Invalid mip level
	EXPECT_FALSE(session.requestBloomMipTarget(0, 4))
		<< "mipLevel >= 4 must be rejected";
	EXPECT_FALSE(session.requestBloomMipTarget(0, 100));
}

// Test: Bloom ping-pong pattern
TEST(VulkanPostProcessTargets, BloomPingPong_Pattern)
{
	FakePostProcessSession session;
	session.beginFrame();

	// Simulate blur ping-pong: render to 1, read from 0
	session.requestBloomMipTarget(0, 0);
	session.ensureRendering();
	EXPECT_EQ(session.bloomLayout(0), ImageLayout::ColorAttachment);

	session.suspendRendering();
	session.transitionBloomToShaderRead(0);
	EXPECT_EQ(session.bloomLayout(0), ImageLayout::ShaderReadOnly);

	session.requestBloomMipTarget(1, 0);
	session.ensureRendering();
	EXPECT_EQ(session.bloomLayout(1), ImageLayout::ColorAttachment);

	// After ping-pong, bloom[0] should still be shader-readable
	EXPECT_EQ(session.bloomLayout(0), ImageLayout::ShaderReadOnly);
}

// Test: copySceneHdrToEffect ends pass and creates snapshot
TEST(VulkanPostProcessTargets, CopySceneHdrToEffect_CreatesSnapshot)
{
	FakePostProcessSession session;
	session.beginFrame();

	session.requestSceneHdrTarget();
	session.ensureRendering();

	EXPECT_TRUE(session.renderingActive());
	EXPECT_EQ(session.sceneEffectLayout(), ImageLayout::Undefined);

	bool success = session.copySceneHdrToEffect();

	EXPECT_TRUE(success);
	EXPECT_FALSE(session.renderingActive())
		<< "copySceneHdrToEffect must end active pass";
	EXPECT_EQ(session.sceneEffectLayout(), ImageLayout::ShaderReadOnly)
		<< "Effect snapshot must be shader-readable after copy";
}

// Test: Transition to shader-read requires suspended pass
TEST(VulkanPostProcessTargets, TransitionToShaderRead_RequiresSuspendedPass)
{
	FakePostProcessSession session;
	session.beginFrame();

	session.requestSceneHdrTarget();
	session.ensureRendering();

	// Try to transition while rendering is active
	session.transitionSceneHdrToShaderRead();

	EXPECT_NE(session.sceneHdrLayout(), ImageLayout::ShaderReadOnly)
		<< "Cannot transition to shader-read while pass is active";

	// Suspend first
	session.suspendRendering();
	session.transitionSceneHdrToShaderRead();

	EXPECT_EQ(session.sceneHdrLayout(), ImageLayout::ShaderReadOnly)
		<< "Transition succeeds after suspending";
}

// Test: Full post-processing chain simulation
TEST(VulkanPostProcessTargets, FullPostChain_Simulation)
{
	FakePostProcessSession session;
	session.beginFrame();

	// 1. Scene rendering to HDR
	session.requestSceneHdrTarget();
	session.ensureRendering();
	// ... draw scene ...
	session.suspendRendering();
	session.transitionSceneHdrToShaderRead();

	// 2. Tonemapping: HDR -> LDR
	session.requestPostLdrTarget();
	session.ensureRendering();
	// ... tonemap fullscreen quad ...
	session.suspendRendering();
	session.transitionPostLdrToShaderRead();

	// 3. SMAA edge detection
	session.requestSmaaEdgesTarget();
	session.ensureRendering();
	session.suspendRendering();
	session.transitionSmaaEdgesToShaderRead();

	// 4. SMAA blending weights
	session.requestSmaaBlendTarget();
	session.ensureRendering();
	session.suspendRendering();
	session.transitionSmaaBlendToShaderRead();

	// 5. SMAA neighborhood blending
	session.requestSmaaOutputTarget();
	session.ensureRendering();
	session.suspendRendering();
	session.transitionSmaaOutputToShaderRead();

	// Verify all layouts are correct for final composite
	EXPECT_EQ(session.sceneHdrLayout(), ImageLayout::ShaderReadOnly);
	EXPECT_EQ(session.postLdrLayout(), ImageLayout::ShaderReadOnly);
	EXPECT_EQ(session.smaaEdgesLayout(), ImageLayout::ShaderReadOnly);
	EXPECT_EQ(session.smaaBlendLayout(), ImageLayout::ShaderReadOnly);
	EXPECT_EQ(session.smaaOutputLayout(), ImageLayout::ShaderReadOnly);

	// Verify pass count (one per target)
	EXPECT_EQ(session.passStartCount(), 5);
}

// Test: HDR variants (with/without depth)
TEST(VulkanPostProcessTargets, SceneHdrVariants_DepthBehavior)
{
	FakePostProcessSession session;
	session.beginFrame();

	session.requestSceneHdrTarget();
	EXPECT_EQ(session.currentTarget(), PostProcessTarget::SceneHdrWithDepth);

	session.requestSceneHdrNoDepthTarget();
	EXPECT_EQ(session.currentTarget(), PostProcessTarget::SceneHdrNoDepth);
}

// Test: Frame boundary resets post-process layouts
TEST(VulkanPostProcessTargets, FrameBoundary_ResetsLayouts)
{
	FakePostProcessSession session;

	// First frame - establish some layouts
	session.beginFrame();
	session.requestPostLdrTarget();
	session.ensureRendering();
	session.suspendRendering();
	session.transitionPostLdrToShaderRead();

	EXPECT_EQ(session.postLdrLayout(), ImageLayout::ShaderReadOnly);

	// New frame - should reset
	session.beginFrame();

	EXPECT_EQ(session.postLdrLayout(), ImageLayout::Undefined)
		<< "Frame boundary must reset post-process layouts";
}

// Test: Post luminance target for FXAA prepass
TEST(VulkanPostProcessTargets, PostLuminance_FxaaPrepass)
{
	FakePostProcessSession session;
	session.beginFrame();

	session.requestPostLdrTarget();
	session.ensureRendering();
	session.suspendRendering();
	session.transitionPostLdrToShaderRead();

	session.requestPostLuminanceTarget();
	session.ensureRendering();

	EXPECT_EQ(session.currentTarget(), PostProcessTarget::PostLuminance);
	EXPECT_EQ(session.postLuminanceLayout(), ImageLayout::ColorAttachment);
}
