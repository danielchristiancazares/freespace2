#include <gtest/gtest.h>
#include <vector>
#include <memory>
#include <cstdint>
#include <optional>

namespace {

// Simulates VulkanBufferManager's buffer retirement logic.
// This tests the behavioral contract that ALL buffer destruction
// (updateBufferData with resize, resizeBuffer, deleteBuffer) must
// go through deferred deletion to avoid GPU use-after-free.
//
// Bug C4 in REPORT.md: deleteBuffer() was destroying buffers immediately
// instead of deferring like updateBufferData() and resizeBuffer() do.

struct FakeBuffer {
	int id;
	size_t size;
	bool hasGpuResources;
};

struct RetiredBuffer {
	int bufferId;
	uint32_t retiredAtFrame;
};

class FakeBufferManager {
  public:
	static constexpr uint32_t FRAMES_BEFORE_DELETE = 3;

	uint32_t currentFrame = 0;
	std::vector<FakeBuffer> buffers;
	std::vector<RetiredBuffer> retiredBuffers;
	std::vector<int> destroyedBufferIds;

	int createBuffer()
	{
		int id = static_cast<int>(buffers.size());
		buffers.push_back({id, 0, false});
		return id;
	}

	// Simulates updateBufferData when buffer needs resize
	void updateBufferData(int handle, size_t newSize)
	{
		auto& buffer = buffers[handle];
		if (newSize != buffer.size && buffer.hasGpuResources) {
			// Retire old buffer (deferred deletion)
			retiredBuffers.push_back({buffer.id, currentFrame});
		}
		buffer.size = newSize;
		buffer.hasGpuResources = true;
	}

	// Simulates resizeBuffer
	void resizeBuffer(int handle, size_t newSize)
	{
		auto& buffer = buffers[handle];
		if (newSize != buffer.size && buffer.hasGpuResources) {
			// Retire old buffer (deferred deletion)
			retiredBuffers.push_back({buffer.id, currentFrame});
		}
		buffer.size = newSize;
		buffer.hasGpuResources = true;
	}

	// Simulates deleteBuffer - MUST use deferred deletion
	// Bug C4 was: this function was NOT deferring, causing use-after-free
	void deleteBuffer(int handle)
	{
		auto& buffer = buffers[handle];
		if (buffer.hasGpuResources) {
			// Correct behavior: retire for deferred deletion
			retiredBuffers.push_back({buffer.id, currentFrame});
		}
		// Mark slot as invalid (size=0, no resources)
		buffer.size = 0;
		buffer.hasGpuResources = false;
	}

	// Bug C4 reproduction: immediate deletion (DO NOT USE)
	void deleteBufferImmediate_BUGGY(int handle)
	{
		auto& buffer = buffers[handle];
		if (buffer.hasGpuResources) {
			// BUGGY: immediate destruction, GPU may still be reading!
			destroyedBufferIds.push_back(buffer.id);
		}
		buffer.size = 0;
		buffer.hasGpuResources = false;
	}

	void onFrameEnd()
	{
		++currentFrame;
		auto it = retiredBuffers.begin();
		while (it != retiredBuffers.end()) {
			if (currentFrame - it->retiredAtFrame >= FRAMES_BEFORE_DELETE) {
				destroyedBufferIds.push_back(it->bufferId);
				it = retiredBuffers.erase(it);
			} else {
				++it;
			}
		}
	}

	size_t getRetiredCount() const { return retiredBuffers.size(); }
	size_t getDestroyedCount() const { return destroyedBufferIds.size(); }
};

} // namespace

// Test: deleteBuffer defers destruction (not immediate)
TEST(VulkanBufferManagerRetirement, Scenario_DeleteBuffer_DefersDestruction)
{
	FakeBufferManager mgr;
	int handle = mgr.createBuffer();
	mgr.updateBufferData(handle, 1024); // Create GPU resources

	mgr.deleteBuffer(handle);

	// Buffer should be retired, NOT destroyed
	EXPECT_EQ(mgr.getRetiredCount(), 1u) << "deleteBuffer must retire buffer for deferred deletion";
	EXPECT_EQ(mgr.getDestroyedCount(), 0u) << "deleteBuffer must NOT destroy immediately";
}

// Test: Immediate deletion is buggy (reproduces C4 bug)
TEST(VulkanBufferManagerRetirement, Scenario_ImmediateDelete_IsBuggy)
{
	FakeBufferManager mgr;
	int handle = mgr.createBuffer();
	mgr.updateBufferData(handle, 1024);

	mgr.deleteBufferImmediate_BUGGY(handle);

	// This demonstrates the bug: destroyed immediately with no deferral
	EXPECT_EQ(mgr.getRetiredCount(), 0u) << "Buggy path does not retire";
	EXPECT_EQ(mgr.getDestroyedCount(), 1u) << "Buggy path destroys immediately";
}

// Test: All three destruction paths use deferred deletion
TEST(VulkanBufferManagerRetirement, Scenario_AllPaths_UseDeferredDeletion)
{
	FakeBufferManager mgr;

	// Create three buffers
	int h1 = mgr.createBuffer();
	int h2 = mgr.createBuffer();
	int h3 = mgr.createBuffer();

	// Initialize all with GPU resources
	mgr.updateBufferData(h1, 1024);
	mgr.updateBufferData(h2, 1024);
	mgr.updateBufferData(h3, 1024);

	// Trigger destruction via three different paths
	mgr.updateBufferData(h1, 2048); // resize via updateBufferData
	mgr.resizeBuffer(h2, 2048);     // resize via resizeBuffer
	mgr.deleteBuffer(h3);           // explicit deletion

	// All three should be retired (deferred)
	EXPECT_EQ(mgr.getRetiredCount(), 3u) << "All three paths must defer destruction";
	EXPECT_EQ(mgr.getDestroyedCount(), 0u) << "None should be destroyed yet";

	// After FRAMES_BEFORE_DELETE frames, all should be destroyed
	mgr.onFrameEnd(); // Frame 1
	mgr.onFrameEnd(); // Frame 2
	mgr.onFrameEnd(); // Frame 3

	EXPECT_EQ(mgr.getRetiredCount(), 0u);
	EXPECT_EQ(mgr.getDestroyedCount(), 3u);
}

// Test: deleteBuffer on buffer without GPU resources is safe
TEST(VulkanBufferManagerRetirement, Scenario_DeleteBuffer_NoResources_NoRetirement)
{
	FakeBufferManager mgr;
	int handle = mgr.createBuffer();
	// Don't call updateBufferData - no GPU resources allocated

	mgr.deleteBuffer(handle);

	// Nothing to retire since no GPU resources
	EXPECT_EQ(mgr.getRetiredCount(), 0u);
	EXPECT_EQ(mgr.getDestroyedCount(), 0u);
}

// Test: Deleted buffer resources survive GPU latency window
TEST(VulkanBufferManagerRetirement, Scenario_DeleteBuffer_SurvivesGpuLatency)
{
	FakeBufferManager mgr;
	int handle = mgr.createBuffer();
	mgr.updateBufferData(handle, 1024);

	mgr.deleteBuffer(handle);

	// Simulate GPU still using buffer for next 2 frames
	mgr.onFrameEnd(); // Frame 1 - GPU might still be reading
	EXPECT_EQ(mgr.getDestroyedCount(), 0u) << "Buffer must survive frame 1";

	mgr.onFrameEnd(); // Frame 2 - GPU might still be reading
	EXPECT_EQ(mgr.getDestroyedCount(), 0u) << "Buffer must survive frame 2";

	mgr.onFrameEnd(); // Frame 3 - safe to destroy
	EXPECT_EQ(mgr.getDestroyedCount(), 1u) << "Buffer can be destroyed at frame 3";
}
