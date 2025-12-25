// test_vulkan_depth_attachment_switch.cpp
//
// PURPOSE: Validates depth attachment switching in VulkanRenderingSession.
// The session supports two depth attachments:
// - Main depth: holds scene depth (ships, weapons, effects)
// - Cockpit depth: holds cockpit-only depth (populated between save/restore zbuffer calls)
//
// This enables OpenGL post-processing parity where cockpit objects are depth-tested
// against cockpit-only geometry, not the full scene.
//
// INVARIANT: Depth attachment selection must end any active pass (attachment change)
// and subsequent ensureRendering() must use the newly selected depth attachment.

#include <gtest/gtest.h>
#include <optional>

namespace {

enum class DepthAttachment {
	Main,
	Cockpit
};

// Simulates the depth attachment switching portion of VulkanRenderingSession
class FakeDepthAttachmentSession {
  public:
	void beginFrame()
	{
		endActivePass();
		m_depthAttachment = DepthAttachment::Main;
		m_mainDepthCleared = true;
		m_cockpitDepthCleared = true;
	}

	void useMainDepthAttachment()
	{
		if (m_depthAttachment != DepthAttachment::Main) {
			endActivePass();
			m_depthAttachment = DepthAttachment::Main;
		}
	}

	void useCockpitDepthAttachment()
	{
		if (m_depthAttachment != DepthAttachment::Cockpit) {
			endActivePass();
			m_depthAttachment = DepthAttachment::Cockpit;
		}
	}

	void ensureRendering()
	{
		if (!m_activePass) {
			m_activePass = true;
			++m_passStartCount;
			m_activeDepthAttachment = m_depthAttachment;
		}
	}

	void suspendRendering()
	{
		endActivePass();
	}

	// Simulates gr_zbuffer_save (OpenGL semantics: copies main depth to cockpit depth)
	void saveZBuffer()
	{
		endActivePass(); // Transfer requires no active rendering
		m_mainDepthCleared = false; // Main depth has scene content
		m_cockpitDepthCleared = true; // Cockpit depth was just copied into
	}

	// Simulates gr_zbuffer_restore (OpenGL semantics: copies cockpit depth back to main depth)
	void restoreZBuffer()
	{
		endActivePass();
		// After restore, cockpit-only depth is now in main buffer
	}

	bool renderingActive() const { return m_activePass; }
	DepthAttachment selectedDepthAttachment() const { return m_depthAttachment; }
	DepthAttachment activeDepthAttachment() const { return m_activeDepthAttachment; }
	int passStartCount() const { return m_passStartCount; }

  private:
	void endActivePass()
	{
		m_activePass = false;
	}

	DepthAttachment m_depthAttachment = DepthAttachment::Main;
	DepthAttachment m_activeDepthAttachment = DepthAttachment::Main;
	bool m_activePass = false;
	bool m_mainDepthCleared = true;
	bool m_cockpitDepthCleared = true;
	int m_passStartCount = 0;
};

} // namespace

// Test: Frame start selects main depth
TEST(VulkanDepthAttachmentSwitch, FrameStart_SelectsMainDepth)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Main)
		<< "Frame start must select main depth attachment";
}

// Test: Switching to same attachment is no-op
TEST(VulkanDepthAttachmentSwitch, SwitchToSame_IsNoop)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	session.ensureRendering();
	EXPECT_TRUE(session.renderingActive());

	int passCount = session.passStartCount();

	// Switching to already-selected main depth should not end pass
	session.useMainDepthAttachment();

	EXPECT_TRUE(session.renderingActive())
		<< "Switching to same attachment must not end pass";
	EXPECT_EQ(session.passStartCount(), passCount);
}

// Test: Switching depth attachment ends active pass
TEST(VulkanDepthAttachmentSwitch, SwitchDifferentAttachment_EndsPass)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	session.ensureRendering();
	EXPECT_TRUE(session.renderingActive());
	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Main);

	session.useCockpitDepthAttachment();

	EXPECT_FALSE(session.renderingActive())
		<< "Switching depth attachment must end active pass";
	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Cockpit);
}

// Test: ensureRendering uses selected attachment
TEST(VulkanDepthAttachmentSwitch, EnsureRendering_UsesSelectedAttachment)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	session.useCockpitDepthAttachment();
	session.ensureRendering();

	EXPECT_EQ(session.activeDepthAttachment(), DepthAttachment::Cockpit)
		<< "ensureRendering must use currently selected depth attachment";
}

// Test: Cockpit depth workflow (save/draw cockpit/restore)
TEST(VulkanDepthAttachmentSwitch, CockpitWorkflow_FullSequence)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	// 1. Render scene with main depth
	session.useMainDepthAttachment();
	session.ensureRendering();
	EXPECT_EQ(session.activeDepthAttachment(), DepthAttachment::Main);

	// 2. Save zbuffer (copies main -> cockpit)
	session.saveZBuffer();
	EXPECT_FALSE(session.renderingActive())
		<< "saveZBuffer must end active pass for transfer";

	// 3. Clear main depth and render cockpit with fresh depth
	session.useMainDepthAttachment();
	session.ensureRendering();
	// ... draw cockpit geometry ...
	session.suspendRendering();

	// 4. Switch to cockpit depth for cockpit-relative effects
	session.useCockpitDepthAttachment();
	session.ensureRendering();
	EXPECT_EQ(session.activeDepthAttachment(), DepthAttachment::Cockpit);
	// ... draw cockpit effects depth-tested against cockpit depth ...

	// 5. Restore zbuffer
	session.restoreZBuffer();
	EXPECT_FALSE(session.renderingActive());
}

// Test: Multiple switches within frame
TEST(VulkanDepthAttachmentSwitch, MultipleSwitches_TrackCorrectly)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	// Main -> Cockpit
	session.useMainDepthAttachment();
	session.ensureRendering();
	session.useCockpitDepthAttachment();
	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Cockpit);
	EXPECT_FALSE(session.renderingActive());

	// Cockpit -> Main
	session.ensureRendering();
	session.useMainDepthAttachment();
	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Main);
	EXPECT_FALSE(session.renderingActive());

	// Start new pass with main
	session.ensureRendering();
	EXPECT_EQ(session.activeDepthAttachment(), DepthAttachment::Main);
}

// Test: Frame boundary resets to main depth
TEST(VulkanDepthAttachmentSwitch, FrameBoundary_ResetsToMain)
{
	FakeDepthAttachmentSession session;

	// Frame 1: end with cockpit depth
	session.beginFrame();
	session.useCockpitDepthAttachment();
	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Cockpit);

	// Frame 2: must start with main depth
	session.beginFrame();
	EXPECT_EQ(session.selectedDepthAttachment(), DepthAttachment::Main)
		<< "New frame must reset to main depth attachment";
}

// Test: Pass count with depth switching
TEST(VulkanDepthAttachmentSwitch, PassCount_WithDepthSwitching)
{
	FakeDepthAttachmentSession session;
	session.beginFrame();

	session.ensureRendering(); // Pass 1
	session.useCockpitDepthAttachment(); // Ends pass 1
	session.ensureRendering(); // Pass 2
	session.useMainDepthAttachment(); // Ends pass 2
	session.ensureRendering(); // Pass 3

	EXPECT_EQ(session.passStartCount(), 3)
		<< "Each depth switch and re-start should create a new pass";
}

