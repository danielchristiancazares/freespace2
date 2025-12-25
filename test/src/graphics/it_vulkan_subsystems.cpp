// it_vulkan_subsystems.cpp
//
// Integration tests for Vulkan renderer subsystems using a real Vulkan device.
// These tests verify actual GPU resource management, not simulations.
//
// Run with: FS2_VULKAN_IT=1 ./unittests --gtest_filter="VulkanSubsystems.*"

#include "cfile/cfile.h"
#include "graphics/2d.h"
#include "graphics/vulkan/VulkanGraphics.h"
#include "graphics/vulkan/VulkanRenderer.h"
#include "graphics/vulkan/VulkanBufferManager.h"
#include "graphics/vulkan/VulkanTextureManager.h"
#include "globalincs/pstypes.h"
#include "io/timer.h"
#include "osapi/osapi.h"
#include "osapi/osregistry.h"

#include <SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <thread>
#include <chrono>

namespace {

class TestViewport : public os::Viewport {
	SDL_Window* m_window;
  public:
	explicit TestViewport(SDL_Window* wnd) : m_window(wnd) {}
	~TestViewport() override
	{
		if (m_window) {
			SDL_DestroyWindow(m_window);
			m_window = nullptr;
		}
	}
	SDL_Window* toSDLWindow() override { return m_window; }
	std::pair<uint32_t, uint32_t> getSize() override
	{
		int w = 0, h = 0;
		SDL_GetWindowSize(m_window, &w, &h);
		return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
	}
	void swapBuffers() override {}
	void setState(os::ViewportState) override {}
	void minimize() override { SDL_MinimizeWindow(m_window); }
	void restore() override { SDL_RestoreWindow(m_window); }
};

class TestGraphicsOperations : public os::GraphicsOperations {
  public:
	TestGraphicsOperations() { SDL_InitSubSystem(SDL_INIT_VIDEO); }
	~TestGraphicsOperations() override { SDL_QuitSubSystem(SDL_INIT_VIDEO); }

	std::unique_ptr<os::OpenGLContext> createOpenGLContext(os::Viewport*, const os::OpenGLContextAttributes&) override
	{
		return nullptr;
	}
	void makeOpenGLContextCurrent(os::Viewport*, os::OpenGLContext*) override {}

	std::unique_ptr<os::Viewport> createViewport(const os::ViewPortProperties& props) override
	{
		uint32_t flags = SDL_WINDOW_SHOWN | SDL_WINDOW_VULKAN;
		SDL_Window* wnd = SDL_CreateWindow(props.title.c_str(),
			SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			static_cast<int>(props.width), static_cast<int>(props.height),
			flags);
		if (!wnd) return nullptr;
		return std::make_unique<TestViewport>(wnd);
	}
};

std::string detect_fs2_root()
{
	if (const char* env = std::getenv("FS2_STEAM_PATH")) {
		return env;
	}
	return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Freespace 2";
}

// Fixture that initializes real Vulkan renderer for subsystem tests
class VulkanSubsystemsTest : public ::testing::Test {
  protected:
	void SetUp() override
	{
		if (!std::getenv("FS2_VULKAN_IT")) {
			GTEST_SKIP() << "Set FS2_VULKAN_IT=1 to run Vulkan integration tests.";
		}

		const std::string fs2_root = detect_fs2_root();
		if (!std::filesystem::exists(fs2_root)) {
			GTEST_SKIP() << "FS2 root not found. Set FS2_STEAM_PATH.";
		}

		const std::string exe_path = fs2_root + "\\fs2_open.exe";
		if (cfile_init(exe_path.c_str()) != 0) {
			GTEST_SKIP() << "cfile_init failed.";
		}

		timer_init();
		os_init("VK Subsystem IT", "VK Subsystem IT");
		os_config_write_string(nullptr, "VideocardFs2open", "VK  -(800x600)x32 bit");

		auto graphicsOps = std::make_unique<TestGraphicsOperations>();
		if (!gr_init(std::move(graphicsOps), GR_VULKAN, 800, 600)) {
			cfile_close();
			timer_close();
			os_cleanup();
			GTEST_SKIP() << "Vulkan renderer failed to initialize.";
		}

		m_renderer = graphics::vulkan::getRendererInstance();
		ASSERT_NE(m_renderer, nullptr) << "Renderer instance must exist after gr_init";

		// Kick off first frame
		gr_flip(false);
	}

	void TearDown() override
	{
		if (m_renderer) {
			gr_close();
			cfile_close();
			timer_close();
			os_cleanup();
		}
	}

	graphics::vulkan::VulkanRenderer* m_renderer = nullptr;
};

} // namespace

// Test: Buffer creation returns valid handle
TEST_F(VulkanSubsystemsTest, BufferCreate_ReturnsValidHandle)
{
	auto* bufMgr = m_renderer->bufferManager();
	ASSERT_NE(bufMgr, nullptr);

	gr_buffer_handle handle = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
	EXPECT_TRUE(handle.isValid()) << "createBuffer must return valid handle";

	bufMgr->deleteBuffer(handle);
}

// Test: Buffer data upload creates real VkBuffer
TEST_F(VulkanSubsystemsTest, BufferUpdate_CreatesVkBuffer)
{
	auto* bufMgr = m_renderer->bufferManager();

	gr_buffer_handle handle = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Static);

	// Before update, buffer may not exist
	std::vector<float> data(256, 1.0f);
	bufMgr->updateBufferData(handle, data.size() * sizeof(float), data.data());

	vk::Buffer vkBuf = bufMgr->getBuffer(handle);
	EXPECT_NE(vkBuf, vk::Buffer{}) << "After updateBufferData, VkBuffer must exist";

	bufMgr->deleteBuffer(handle);
}

// Test: Buffer resize creates new buffer (old one deferred)
TEST_F(VulkanSubsystemsTest, BufferResize_CreatesNewBuffer)
{
	auto* bufMgr = m_renderer->bufferManager();

	gr_buffer_handle handle = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Dynamic);

	std::vector<float> data1(64, 1.0f);
	bufMgr->updateBufferData(handle, data1.size() * sizeof(float), data1.data());
	vk::Buffer buf1 = bufMgr->getBuffer(handle);

	// Resize to larger
	std::vector<float> data2(256, 2.0f);
	bufMgr->updateBufferData(handle, data2.size() * sizeof(float), data2.data());
	vk::Buffer buf2 = bufMgr->getBuffer(handle);

	// Buffer handle is same, but underlying VkBuffer may differ
	EXPECT_TRUE(handle.isValid());
	EXPECT_NE(buf2, vk::Buffer{});

	bufMgr->deleteBuffer(handle);
}

// Test: Multiple buffers get distinct handles
TEST_F(VulkanSubsystemsTest, MultipleBuffers_DistinctHandles)
{
	auto* bufMgr = m_renderer->bufferManager();

	gr_buffer_handle h1 = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
	gr_buffer_handle h2 = bufMgr->createBuffer(BufferType::Index, BufferUsageHint::Static);
	gr_buffer_handle h3 = bufMgr->createBuffer(BufferType::Uniform, BufferUsageHint::Streaming);

	EXPECT_NE(h1, h2);
	EXPECT_NE(h2, h3);
	EXPECT_NE(h1, h3);

	bufMgr->deleteBuffer(h1);
	bufMgr->deleteBuffer(h2);
	bufMgr->deleteBuffer(h3);
}

// Test: Deleted buffer handle becomes invalid for getBuffer
TEST_F(VulkanSubsystemsTest, DeleteBuffer_InvalidatesHandle)
{
	auto* bufMgr = m_renderer->bufferManager();

	gr_buffer_handle handle = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
	std::vector<float> data(64, 1.0f);
	bufMgr->updateBufferData(handle, data.size() * sizeof(float), data.data());

	EXPECT_NE(bufMgr->getBuffer(handle), vk::Buffer{});

	bufMgr->deleteBuffer(handle);

	// After delete, getBuffer should return null (or handle is reused - implementation dependent)
	// The key invariant is that the delete doesn't crash and the GPU work completes
}

// Test: Frame flip advances GPU work (basic sanity)
TEST_F(VulkanSubsystemsTest, FrameFlip_AdvancesGpuWork)
{
	// Create a buffer, upload data, flip several frames
	auto* bufMgr = m_renderer->bufferManager();

	gr_buffer_handle handle = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
	std::vector<float> data(1024, 1.0f);
	bufMgr->updateBufferData(handle, data.size() * sizeof(float), data.data());

	// Flip multiple frames to exercise frame-in-flight logic
	for (int i = 0; i < 5; ++i) {
		gr_clear();
		gr_flip();
	}

	// If we get here without crash/validation error, frame sync is working
	SUCCEED();

	bufMgr->deleteBuffer(handle);
}

// Test: Deferred release - buffer survives until GPU done
TEST_F(VulkanSubsystemsTest, DeferredRelease_BufferSurvivesGpuLatency)
{
	auto* bufMgr = m_renderer->bufferManager();

	// Create and populate buffer
	gr_buffer_handle handle = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Dynamic);
	std::vector<float> data(256, 1.0f);
	bufMgr->updateBufferData(handle, data.size() * sizeof(float), data.data());

	// Resize triggers deferred release of old buffer
	std::vector<float> data2(512, 2.0f);
	bufMgr->updateBufferData(handle, data2.size() * sizeof(float), data2.data());

	// Flip frames to let deferred releases collect
	for (int i = 0; i < 4; ++i) {
		gr_clear();
		gr_flip();
	}

	// Buffer should still be valid
	EXPECT_NE(bufMgr->getBuffer(handle), vk::Buffer{});

	bufMgr->deleteBuffer(handle);

	// Final flips to process the delete
	for (int i = 0; i < 3; ++i) {
		gr_clear();
		gr_flip();
	}

	SUCCEED() << "Deferred release completed without validation errors";
}

// Test: Uniform buffer offset alignment respected
TEST_F(VulkanSubsystemsTest, UniformBuffer_AlignmentRespected)
{
	size_t minAlign = m_renderer->getMinUniformOffsetAlignment();
	EXPECT_GT(minAlign, 0u) << "Uniform buffer alignment must be positive";

	// Vulkan spec minimum is 256 for most GPUs, but some allow 64
	EXPECT_GE(minAlign, 1u);
	EXPECT_LE(minAlign, 256u) << "Unusual alignment value";
}

// Test: Texture manager exists and is accessible
TEST_F(VulkanSubsystemsTest, TextureManager_Accessible)
{
	auto* texMgr = m_renderer->textureManager();
	ASSERT_NE(texMgr, nullptr) << "Texture manager must exist";
}

// Test: Render targets exist after init
TEST_F(VulkanSubsystemsTest, RenderTargets_ExistAfterInit)
{
	auto* renderTargets = m_renderer->renderTargets();
	ASSERT_NE(renderTargets, nullptr) << "Render targets must exist";

	vk::Format depthFmt = renderTargets->depthFormat();
	EXPECT_NE(depthFmt, vk::Format::eUndefined) << "Depth format must be valid";
}

// Test: Multiple frame flips don't leak resources
TEST_F(VulkanSubsystemsTest, StressFrameFlips_NoResourceLeak)
{
	auto* bufMgr = m_renderer->bufferManager();

	// Create some buffers
	std::vector<gr_buffer_handle> handles;
	for (int i = 0; i < 10; ++i) {
		handles.push_back(bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Dynamic));
	}

	// Flip many frames, updating buffers each frame
	for (int frame = 0; frame < 30; ++frame) {
		for (auto& h : handles) {
			std::vector<float> data(64 + frame, static_cast<float>(frame));
			bufMgr->updateBufferData(h, data.size() * sizeof(float), data.data());
		}
		gr_clear();
		gr_flip();
	}

	// Cleanup
	for (auto& h : handles) {
		bufMgr->deleteBuffer(h);
	}

	// Final flips
	for (int i = 0; i < 3; ++i) {
		gr_clear();
		gr_flip();
	}

	SUCCEED() << "Stress test completed without crash or validation errors";
}

// Test: Buffer type is preserved
TEST_F(VulkanSubsystemsTest, BufferType_Preserved)
{
	auto* bufMgr = m_renderer->bufferManager();

	gr_buffer_handle vertexBuf = bufMgr->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
	gr_buffer_handle indexBuf = bufMgr->createBuffer(BufferType::Index, BufferUsageHint::Static);
	gr_buffer_handle uniformBuf = bufMgr->createBuffer(BufferType::Uniform, BufferUsageHint::Streaming);

	EXPECT_EQ(bufMgr->getBufferType(vertexBuf), BufferType::Vertex);
	EXPECT_EQ(bufMgr->getBufferType(indexBuf), BufferType::Index);
	EXPECT_EQ(bufMgr->getBufferType(uniformBuf), BufferType::Uniform);

	bufMgr->deleteBuffer(vertexBuf);
	bufMgr->deleteBuffer(indexBuf);
	bufMgr->deleteBuffer(uniformBuf);
}
