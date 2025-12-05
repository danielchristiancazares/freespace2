#pragma once

#ifdef WITH_VULKAN

#include <gtest/gtest.h>
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <SDL.h>
#include <SDL_vulkan.h>

#include "globalincs/pstypes.h"
#include "osapi/osapi.h"
#include "graphics/2d.h"
#include "graphics/grinternal.h"
#include "graphics/vulkan/VulkanRenderer.h"
#include "graphics/vulkan/gr_vulkan.h"
#include "mod_table/mod_table.h"

namespace graphics {
namespace vulkan {
namespace testing {

// ============================================================================
// Test Utilities
// ============================================================================

/**
 * @brief Test-specific GraphicsOperations that creates an SDL Vulkan window
 * 
 * Creates a hidden window by default for headless testing.
 */
class TestGraphicsOperations : public os::GraphicsOperations {
public:
	SDL_Window* m_window = nullptr;
	bool m_visible = false;
	int m_width = 800;
	int m_height = 600;

	explicit TestGraphicsOperations(bool visible = false, int width = 800, int height = 600)
		: m_visible(visible), m_width(width), m_height(height) {}

	~TestGraphicsOperations() override {
		if (m_window) {
			SDL_DestroyWindow(m_window);
			m_window = nullptr;
		}
	}

	std::unique_ptr<os::OpenGLContext> createOpenGLContext(os::Viewport*,
		const os::OpenGLContextAttributes&) override {
		return nullptr; // Not used for Vulkan
	}

	void makeOpenGLContextCurrent(os::Viewport*, os::OpenGLContext*) override {
		// Not used for Vulkan
	}

	std::unique_ptr<os::Viewport> createViewport(const os::ViewPortProperties& props) override;
};

/**
 * @brief Minimal test viewport wrapping an SDL window
 */
class TestViewport : public os::Viewport {
public:
	SDL_Window* m_sdlWindow;

	explicit TestViewport(SDL_Window* window) : m_sdlWindow(window) {}

	~TestViewport() override = default;

	SDL_Window* toSDLWindow() override { return m_sdlWindow; }

	std::pair<uint32_t, uint32_t> getSize() override {
		int w, h;
		SDL_GetWindowSize(m_sdlWindow, &w, &h);
		return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
	}

	void swapBuffers() override {}
	void setState(os::ViewportState) override {}
	void minimize() override {}
	void restore() override {}
};

inline std::unique_ptr<os::Viewport> TestGraphicsOperations::createViewport(const os::ViewPortProperties& props) {
	Uint32 windowFlags = SDL_WINDOW_VULKAN;

	if (!m_visible) {
		windowFlags |= SDL_WINDOW_HIDDEN;
	}

	m_window = SDL_CreateWindow(
		props.title.c_str(),
		m_visible ? SDL_WINDOWPOS_CENTERED : SDL_WINDOWPOS_UNDEFINED,
		m_visible ? SDL_WINDOWPOS_CENTERED : SDL_WINDOWPOS_UNDEFINED,
		m_width,
		m_height,
		windowFlags
	);

	if (!m_window) {
		return nullptr;
	}

	return std::make_unique<TestViewport>(m_window);
}

// ============================================================================
// Base Test Fixture for Vulkan Integration Tests
// ============================================================================

/**
 * @brief Base fixture for tests requiring full Vulkan initialization
 * 
 * Handles SDL init, renderer creation, and cleanup.
 * Skips tests gracefully when Vulkan is unavailable.
 */
class VulkanTestFixture : public ::testing::Test {
protected:
	VulkanRenderer* m_renderer = nullptr;
	bool m_initialized = false;
	bool m_visible = false;

	explicit VulkanTestFixture(bool visible = false) : m_visible(visible) {}

	void SetUp() override {
		// Initialize SDL with video support
		if (SDL_Init(SDL_INIT_VIDEO) != 0) {
			GTEST_SKIP() << "SDL_Init failed: " << SDL_GetError();
			return;
		}

		// Full engine init path so shader loading and global systems are ready
		auto graphicsOps = std::make_unique<TestGraphicsOperations>(m_visible);
		const int width = graphicsOps->m_width;
		const int height = graphicsOps->m_height;
		Window_icon_path = "app_icon_sse"; // avoid missing-icon warning/error during headless init
		Cmdline_noshadercache = true;      // avoid filesystem cache lookups during tests
		if (!gr_init(std::move(graphicsOps), GR_VULKAN, width, height, 32)) {
			SDL_Quit();
			GTEST_SKIP() << "gr_init failed - no Vulkan support?";
			return;
		}

		m_renderer = graphics::vulkan::getRendererInstance();
		if (!m_renderer) {
			gr_close();
			SDL_Quit();
			GTEST_SKIP() << "Vulkan renderer instance unavailable after initialize";
			return;
		}

		m_initialized = true;
	}

	void TearDown() override {
		if (m_renderer) {
			gr_close();
			m_renderer = nullptr;
		}
		SDL_Quit();
	}

	bool isInitialized() const { return m_initialized; }

	VulkanRenderer* renderer() { return m_renderer; }
};

/**
 * @brief Fixture for hidden window tests (default)
 */
class VulkanHiddenWindowTest : public VulkanTestFixture {
protected:
	VulkanHiddenWindowTest() : VulkanTestFixture(false) {}
};

/**
 * @brief Fixture for visible window tests (manual verification)
 */
class VulkanVisibleWindowTest : public VulkanTestFixture {
protected:
	VulkanVisibleWindowTest() : VulkanTestFixture(true) {}
};

// ============================================================================
// Mock Device for Unit Tests (No GPU Required)
// ============================================================================

/**
 * @brief Mock device limits for unit testing without GPU
 */
struct MockDeviceLimits {
	size_t minUniformBufferOffsetAlignment = 256;
	size_t maxUniformBufferRange = 65536;
	float maxSamplerAnisotropy = 16.0f;
	uint32_t maxDescriptorSetUniformBuffers = 12;
	uint32_t maxDescriptorSetSampledImages = 16;
};

/**
 * @brief Helper to check if a vk::Format is a depth format
 */
inline bool isDepthFormat(vk::Format format) {
	switch (format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD32Sfloat:
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
			return true;
		default:
			return false;
	}
}

/**
 * @brief Helper to check if a vk::Format has stencil
 */
inline bool hasStencilComponent(vk::Format format) {
	switch (format) {
		case vk::Format::eD16UnormS8Uint:
		case vk::Format::eD24UnormS8Uint:
		case vk::Format::eD32SfloatS8Uint:
		case vk::Format::eS8Uint:
			return true;
		default:
			return false;
	}
}

/**
 * @brief Helper to get bytes per pixel for common formats
 */
inline size_t getBytesPerPixel(vk::Format format) {
	switch (format) {
		case vk::Format::eR8Unorm:
		case vk::Format::eR8Snorm:
		case vk::Format::eR8Uint:
		case vk::Format::eR8Sint:
			return 1;
		case vk::Format::eR8G8Unorm:
		case vk::Format::eR8G8Snorm:
		case vk::Format::eR16Sfloat:
		case vk::Format::eR5G6B5UnormPack16:
		case vk::Format::eR5G5B5A1UnormPack16:
			return 2;
		case vk::Format::eR8G8B8Unorm:
		case vk::Format::eB8G8R8Unorm:
			return 3;
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
		case vk::Format::eR32Sfloat:
		case vk::Format::eR16G16Sfloat:
		case vk::Format::eA2B10G10R10UnormPack32:
			return 4;
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR32G32Sfloat:
			return 8;
		case vk::Format::eR32G32B32Sfloat:
			return 12;
		case vk::Format::eR32G32B32A32Sfloat:
			return 16;
		default:
			return 4; // Default assumption
	}
}

/**
 * @brief Generate test data of specified size
 */
inline SCP_vector<uint8_t> generateTestData(size_t size, uint8_t seed = 0) {
	SCP_vector<uint8_t> data(size);
	for (size_t i = 0; i < size; ++i) {
		data[i] = static_cast<uint8_t>((i + seed) & 0xFF);
	}
	return data;
}

/**
 * @brief Compare two data buffers
 */
inline bool compareData(const void* a, const void* b, size_t size) {
	return std::memcmp(a, b, size) == 0;
}

/**
 * @brief Simple float RGBA container for readback assertions
 */
struct ReadbackPixel {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
	float a = 0.0f;
};

/**
 * @brief Helper that copies a single scene-color pixel to host memory for assertions.
 *
 * Uses the renderer's command pool and graphics queue; keeps the original image layout intact.
 */
class RendererReadbackHelper {
  public:
	explicit RendererReadbackHelper(VulkanRenderer* renderer) : m_renderer(renderer) {}

	/**
	 * @brief Read a single pixel from the scene color attachment.
	 * @param outPixel Receives the decoded RGBA values (normalized floats)
	 * @param x X coordinate to sample (clamped to width-1)
	 * @param y Y coordinate to sample (clamped to height-1)
	 * @return true on success, false if renderer state is not ready
	 */
	bool readScenePixel(ReadbackPixel& outPixel, uint32_t x = 0, uint32_t y = 0) {
		if (!m_renderer || !m_renderer->m_sceneFramebuffer) {
			return false;
		}

		if (m_renderer->m_scenePassActive || m_renderer->m_directPassActive || m_renderer->m_auxiliaryPassActive) {
			// Avoid reading while a pass is active; call after endScenePass().
			return false;
		}

		const auto extent = m_renderer->m_sceneFramebuffer->getExtent();
		if (extent.width == 0 || extent.height == 0) {
			return false;
		}

		x = std::min<uint32_t>(x, extent.width - 1);
		y = std::min<uint32_t>(y, extent.height - 1);

		const auto format = m_renderer->m_sceneFramebuffer->getColorFormat(0);
		const size_t bytesPerPixel = getBytesPerPixel(format);

		if (bytesPerPixel == 0 || !supportsFormat(format)) {
			return false;
		}

		auto sceneImage = m_renderer->m_sceneFramebuffer->getColorImage(0);
		if (!sceneImage) {
			return false;
		}

		auto device = m_renderer->m_device.get();
		auto commandPool = m_renderer->m_graphicsCommandPool.get();

		auto cmdBuffers = device.allocateCommandBuffersUnique(
			vk::CommandBufferAllocateInfo(commandPool, vk::CommandBufferLevel::ePrimary, 1));
		if (cmdBuffers.empty()) {
			return false;
		}

		auto stagingBuffer = device.createBufferUnique(
			vk::BufferCreateInfo({}, bytesPerPixel, vk::BufferUsageFlagBits::eTransferDst));

		auto memReq = device.getBufferMemoryRequirements(stagingBuffer.get());
		auto allocInfo = vk::MemoryAllocateInfo(memReq.size,
			findMemoryType(memReq.memoryTypeBits,
			               vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent));
		auto stagingMemory = device.allocateMemoryUnique(allocInfo);
		device.bindBufferMemory(stagingBuffer.get(), stagingMemory.get(), 0);

		auto cmd = cmdBuffers.front().get();
		cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		const vk::ImageLayout originalLayout =
			m_renderer->m_sceneColorInShaderReadLayout ? vk::ImageLayout::eShaderReadOnlyOptimal
			                                           : vk::ImageLayout::eColorAttachmentOptimal;

		transitionImage(cmd,
		                sceneImage,
		                originalLayout,
		                vk::ImageLayout::eTransferSrcOptimal,
		                format);

		vk::BufferImageCopy copyRegion;
		copyRegion.bufferOffset = 0;
		copyRegion.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageOffset = vk::Offset3D(static_cast<int32_t>(x), static_cast<int32_t>(y), 0);
		copyRegion.imageExtent = vk::Extent3D(1, 1, 1);

		cmd.copyImageToBuffer(sceneImage, vk::ImageLayout::eTransferSrcOptimal, stagingBuffer.get(), copyRegion);

		transitionImage(cmd,
		                sceneImage,
		                vk::ImageLayout::eTransferSrcOptimal,
		                originalLayout,
		                format);

		cmd.end();

		vk::SubmitInfo submitInfo;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;

		m_renderer->m_graphicsQueue.submit(submitInfo);
		m_renderer->m_graphicsQueue.waitIdle();

		void* mapped = device.mapMemory(stagingMemory.get(), 0, bytesPerPixel);
		if (!mapped) {
			return false;
		}

		const auto decoded = decodePixel(format, static_cast<const uint8_t*>(mapped));
		device.unmapMemory(stagingMemory.get());

		outPixel = decoded;
		return true;
	}

  private:
	VulkanRenderer* m_renderer = nullptr;

	static float halfToFloat(uint16_t value) {
		const uint16_t sign = (value >> 15) & 0x1;
		uint16_t exp = (value >> 10) & 0x1F;
		uint16_t mantissa = value & 0x3FF;

		uint32_t outSign = static_cast<uint32_t>(sign) << 31;
		uint32_t outExp;
		uint32_t outMantissa;

		if (exp == 0) {
			if (mantissa == 0) {
				outExp = 0;
				outMantissa = 0;
			} else {
				// Subnormal
				exp = 1;
				while ((mantissa & 0x400) == 0) {
					mantissa <<= 1;
					--exp;
				}
				mantissa &= 0x3FF;
				outExp = static_cast<uint32_t>(exp + (127 - 15)) << 23;
				outMantissa = static_cast<uint32_t>(mantissa) << 13;
			}
		} else if (exp == 31) {
			// Inf/NaN
			outExp = 0xFF << 23;
			outMantissa = static_cast<uint32_t>(mantissa) << 13;
		} else {
			outExp = static_cast<uint32_t>(exp + (127 - 15)) << 23;
			outMantissa = static_cast<uint32_t>(mantissa) << 13;
		}

		const uint32_t bits = outSign | outExp | outMantissa;
		float result;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}

	static ReadbackPixel decodePixel(vk::Format format, const uint8_t* data) {
		ReadbackPixel pixel{};
		switch (format) {
			case vk::Format::eR16G16B16A16Sfloat: {
				auto comps = reinterpret_cast<const uint16_t*>(data);
				pixel.r = halfToFloat(comps[0]);
				pixel.g = halfToFloat(comps[1]);
				pixel.b = halfToFloat(comps[2]);
				pixel.a = halfToFloat(comps[3]);
				break;
			}
			case vk::Format::eR8G8B8A8Unorm:
				pixel.r = static_cast<float>(data[0]) / 255.0f;
				pixel.g = static_cast<float>(data[1]) / 255.0f;
				pixel.b = static_cast<float>(data[2]) / 255.0f;
				pixel.a = static_cast<float>(data[3]) / 255.0f;
				break;
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eB8G8R8A8Srgb:
				pixel.b = static_cast<float>(data[0]) / 255.0f;
				pixel.g = static_cast<float>(data[1]) / 255.0f;
				pixel.r = static_cast<float>(data[2]) / 255.0f;
				pixel.a = static_cast<float>(data[3]) / 255.0f;
				break;
			case vk::Format::eA2B10G10R10UnormPack32: {
				uint32_t packed = *reinterpret_cast<const uint32_t*>(data);
				const float denom10 = 1.0f / 1023.0f;
				const float denom2 = 1.0f / 3.0f;
				pixel.r = static_cast<float>(packed & 0x3FF) * denom10;
				pixel.g = static_cast<float>((packed >> 10) & 0x3FF) * denom10;
				pixel.b = static_cast<float>((packed >> 20) & 0x3FF) * denom10;
				pixel.a = static_cast<float>((packed >> 30) & 0x3) * denom2;
				break;
			}
			default:
				break;
		}
		return pixel;
	}

	static bool supportsFormat(vk::Format format) {
		switch (format) {
			case vk::Format::eR16G16B16A16Sfloat:
			case vk::Format::eR8G8B8A8Unorm:
			case vk::Format::eB8G8R8A8Unorm:
			case vk::Format::eB8G8R8A8Srgb:
			case vk::Format::eA2B10G10R10UnormPack32:
				return true;
			default:
				return false;
		}
	}

	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) {
		auto memProps = m_renderer->m_physicalDevice.getMemoryProperties();
		for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
			if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
				return i;
			}
		}
		return 0;
	}

	static void transitionImage(vk::CommandBuffer cmd,
	                            vk::Image image,
	                            vk::ImageLayout oldLayout,
	                            vk::ImageLayout newLayout,
	                            vk::Format format) {
		vk::ImageMemoryBarrier2 barrier;
		barrier.srcStageMask = (oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
			                       ? vk::PipelineStageFlagBits2::eFragmentShader
			                       : vk::PipelineStageFlagBits2::eColorAttachmentOutput;
		barrier.srcAccessMask = (oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
			                        ? vk::AccessFlagBits2::eShaderSampledRead
			                        : vk::AccessFlagBits2::eColorAttachmentWrite;
		barrier.dstStageMask = (newLayout == vk::ImageLayout::eTransferSrcOptimal)
			                       ? vk::PipelineStageFlagBits2::eTransfer
			                       : vk::PipelineStageFlagBits2::eFragmentShader;
		barrier.dstAccessMask = (newLayout == vk::ImageLayout::eTransferSrcOptimal)
			                        ? vk::AccessFlagBits2::eTransferRead
			                        : vk::AccessFlagBits2::eShaderSampledRead;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		std::array<vk::ImageMemoryBarrier2, 1> barriers{barrier};
		vk::DependencyInfo depInfo;
		depInfo.setImageMemoryBarriers(barriers);
		cmd.pipelineBarrier2(depInfo);
	}
};

/**
 * @brief Lightweight accessor for inspecting renderer state in tests.
 */
class RendererStateAccessor {
  public:
	explicit RendererStateAccessor(VulkanRenderer* renderer) : m_renderer(renderer) {}

	bool scenePassActive() const { return m_renderer ? m_renderer->m_scenePassActive : false; }
	bool directPassActive() const { return m_renderer ? m_renderer->m_directPassActive : false; }
	bool scenePassRecorded() const { return m_renderer ? m_renderer->m_scenePassRecorded : false; }
	vk::CommandBuffer sceneCommandBuffer() const { return m_renderer ? m_renderer->m_sceneCommandBuffer : nullptr; }
	vk::Format currentColorFormat() const { return m_renderer ? m_renderer->m_currentColorFormat : vk::Format::eUndefined; }
	vk::Format currentDepthFormat() const { return m_renderer ? m_renderer->m_currentDepthFormat : vk::Format::eUndefined; }

  private:
	VulkanRenderer* m_renderer = nullptr;
};

} // namespace testing
} // namespace vulkan
} // namespace graphics

#endif // WITH_VULKAN
