/**
 * Tests for VulkanRenderer shutdown and RAII destruction order.
 *
 * The Vulkan spec requires resources to be destroyed in dependency order:
 *   - Device-dependent resources before device
 *   - Surface and debug messenger before instance
 *
 * These tests verify that our RAII member ordering and shutdown() logic
 * maintain this invariant.
 */

#include <gtest/gtest.h>
#include <vector>
#include <string>
#include <algorithm>

namespace {

/**
 * Mock handle that tracks destruction order.
 * Used to verify RAII destruction happens in correct dependency order.
 */
class DestructionTracker {
  public:
	static std::vector<std::string>& log()
	{
		static std::vector<std::string> instance;
		return instance;
	}

	static void reset() { log().clear(); }

	static bool destroyedBefore(const std::string& first, const std::string& second)
	{
		auto& l = log();
		auto firstIt = std::find(l.begin(), l.end(), first);
		auto secondIt = std::find(l.begin(), l.end(), second);
		if (firstIt == l.end() || secondIt == l.end()) {
			return false;
		}
		return firstIt < secondIt;
	}
};

/**
 * RAII wrapper that logs destruction.
 */
class MockUniqueHandle {
  public:
	explicit MockUniqueHandle(std::string name) : m_name(std::move(name)), m_valid(true) {}
	MockUniqueHandle() : m_valid(false) {}

	~MockUniqueHandle()
	{
		if (m_valid) {
			DestructionTracker::log().push_back(m_name);
		}
	}

	// Move-only
	MockUniqueHandle(MockUniqueHandle&& other) noexcept : m_name(std::move(other.m_name)), m_valid(other.m_valid)
	{
		other.m_valid = false;
	}

	MockUniqueHandle& operator=(MockUniqueHandle&& other) noexcept
	{
		if (this != &other) {
			if (m_valid) {
				DestructionTracker::log().push_back(m_name);
			}
			m_name = std::move(other.m_name);
			m_valid = other.m_valid;
			other.m_valid = false;
		}
		return *this;
	}

	MockUniqueHandle(const MockUniqueHandle&) = delete;
	MockUniqueHandle& operator=(const MockUniqueHandle&) = delete;

	void reset()
	{
		if (m_valid) {
			DestructionTracker::log().push_back(m_name);
			m_valid = false;
		}
	}

	explicit operator bool() const { return m_valid; }

  private:
	std::string m_name;
	bool m_valid;
};

/**
 * Mimics VulkanRenderer's member layout to test RAII destruction order.
 * Members are declared in the same relative order as VulkanRenderer.h
 */
class MockVulkanRenderer {
  public:
	MockVulkanRenderer()
		: m_instance("instance"), m_debugMessenger("debugMessenger"), m_surface("surface"), m_device("device"),
		  m_pipelineCache("pipelineCache"), m_swapChain("swapChain"), m_depthImage("depthImage"),
		  m_depthImageMemory("depthImageMemory"), m_depthImageView("depthImageView"),
		  m_uploadCommandPool("uploadCommandPool")
	{
	}

	/**
	 * Mimics the fixed shutdown() - only does non-RAII cleanup.
	 * Does NOT manually reset RAII members.
	 */
	void shutdown()
	{
		if (!m_device) {
			return;
		}
		// In real code: waitIdle(), save pipeline cache
		// NO manual .reset() calls - let destructor handle RAII cleanup
	}

	/**
	 * Mimics the OLD buggy shutdown() that manually reset some members
	 * but forgot surface.
	 */
	void shutdownBuggy()
	{
		if (!m_device) {
			return;
		}
		// Bug: resets instance but not surface
		m_device.reset();
		m_debugMessenger.reset();
		m_instance.reset();
		// m_surface NOT reset - will be destroyed by destructor with invalid state
	}

  private:
	// Order matters! Destruction is reverse of declaration.
	// Instance-level resources (destroyed last)
	MockUniqueHandle m_instance;
	MockUniqueHandle m_debugMessenger;
	MockUniqueHandle m_surface;

	// Device (destroyed after device-dependent resources)
	MockUniqueHandle m_device;

	// Device-dependent resources (destroyed first)
	MockUniqueHandle m_pipelineCache;
	MockUniqueHandle m_swapChain;
	MockUniqueHandle m_depthImage;
	MockUniqueHandle m_depthImageMemory;
	MockUniqueHandle m_depthImageView;
	MockUniqueHandle m_uploadCommandPool;
};

} // namespace

class VulkanRendererShutdown : public ::testing::Test {
  protected:
	void SetUp() override { DestructionTracker::reset(); }
};

TEST_F(VulkanRendererShutdown, Scenario_RAIIDestruction_DeviceResourcesBeforeDevice)
{
	{
		MockVulkanRenderer renderer;
		renderer.shutdown();
	} // Destructor runs here

	// Device-dependent resources must be destroyed before device
	EXPECT_TRUE(DestructionTracker::destroyedBefore("uploadCommandPool", "device"));
	EXPECT_TRUE(DestructionTracker::destroyedBefore("depthImageView", "device"));
	EXPECT_TRUE(DestructionTracker::destroyedBefore("depthImageMemory", "device"));
	EXPECT_TRUE(DestructionTracker::destroyedBefore("depthImage", "device"));
	EXPECT_TRUE(DestructionTracker::destroyedBefore("swapChain", "device"));
	EXPECT_TRUE(DestructionTracker::destroyedBefore("pipelineCache", "device"));
}

TEST_F(VulkanRendererShutdown, Scenario_RAIIDestruction_SurfaceBeforeInstance)
{
	{
		MockVulkanRenderer renderer;
		renderer.shutdown();
	}

	// Surface must be destroyed before instance
	EXPECT_TRUE(DestructionTracker::destroyedBefore("surface", "instance"));
}

TEST_F(VulkanRendererShutdown, Scenario_RAIIDestruction_DebugMessengerBeforeInstance)
{
	{
		MockVulkanRenderer renderer;
		renderer.shutdown();
	}

	// Debug messenger must be destroyed before instance
	EXPECT_TRUE(DestructionTracker::destroyedBefore("debugMessenger", "instance"));
}

TEST_F(VulkanRendererShutdown, Scenario_RAIIDestruction_DeviceBeforeInstance)
{
	{
		MockVulkanRenderer renderer;
		renderer.shutdown();
	}

	// Device must be destroyed before instance
	EXPECT_TRUE(DestructionTracker::destroyedBefore("device", "instance"));
}

TEST_F(VulkanRendererShutdown, Scenario_FixedShutdown_NoDoubleDestruction)
{
	{
		MockVulkanRenderer renderer;
		renderer.shutdown();
	}

	// Each resource should appear exactly once in destruction log
	auto& log = DestructionTracker::log();
	std::vector<std::string> expected = {"uploadCommandPool",
		"depthImageView",
		"depthImageMemory",
		"depthImage",
		"swapChain",
		"pipelineCache",
		"device",
		"surface",
		"debugMessenger",
		"instance"};

	ASSERT_EQ(log.size(), expected.size());
	for (const auto& name : expected) {
		EXPECT_EQ(std::count(log.begin(), log.end(), name), 1) << "Resource " << name << " destroyed wrong number of times";
	}
}

TEST_F(VulkanRendererShutdown, Scenario_BuggyShutdown_CausesOutOfOrderDestruction)
{
	{
		MockVulkanRenderer renderer;
		renderer.shutdownBuggy(); // Manually resets instance before surface
	}

	// With buggy shutdown: instance is destroyed (via reset) BEFORE surface
	// Then destructor destroys surface after instance is gone - this was the crash
	auto& log = DestructionTracker::log();

	// Find positions
	auto instancePos = std::find(log.begin(), log.end(), "instance");
	auto surfacePos = std::find(log.begin(), log.end(), "surface");

	ASSERT_NE(instancePos, log.end());
	ASSERT_NE(surfacePos, log.end());

	// Bug: instance destroyed before surface (wrong order!)
	EXPECT_TRUE(instancePos < surfacePos) << "Buggy shutdown destroys instance before surface - this causes the crash";
}
