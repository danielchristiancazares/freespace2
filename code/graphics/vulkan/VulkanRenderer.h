#pragma once

#include "osapi/osapi.h"

#include "graphics/grinternal.h"

#include "VulkanBufferManager.h"
#include "VulkanDescriptorLayouts.h"
#include "VulkanFrame.h"
#include "VulkanPipelineManager.h"
#include "VulkanShaderManager.h"
#include "VulkanTextureManager.h"
#include "FrameLifecycleTracker.h"

#include "graphics/2d.h"

#include <array>
#include <functional>
#include <memory>
#include <vulkan/vulkan.hpp>

#if SDL_VERSION_ATLEAST(2, 0, 6)
#define SDL_SUPPORTS_VULKAN 1
#else
#define SDL_SUPPORTS_VULKAN 0
#endif

namespace graphics {
namespace vulkan {

struct QueueIndex {
	bool initialized = false;
	uint32_t index = 0;
};

struct PhysicalDeviceValues {
	vk::PhysicalDevice device;
	vk::PhysicalDeviceProperties properties;
	vk::PhysicalDeviceFeatures features;
	vk::PhysicalDeviceVulkan12Features features12;
	vk::PhysicalDeviceVulkan13Features features13;
	vk::PhysicalDeviceVulkan14Features features14;
	vk::PhysicalDevicePushDescriptorPropertiesKHR pushDescriptorProps{};
	vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT extDynamicState{};
	vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT extDynamicState2{};
	vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT extDynamicState3{};

	std::vector<vk::ExtensionProperties> extensions;

	vk::SurfaceCapabilitiesKHR surfaceCapabilities;
	std::vector<vk::SurfaceFormatKHR> surfaceFormats;
	std::vector<vk::PresentModeKHR> presentModes;

	std::vector<vk::QueueFamilyProperties> queueProperties;
	QueueIndex graphicsQueueIndex;
	QueueIndex presentQueueIndex;
};

class VulkanRenderer {
  public:
	explicit VulkanRenderer(std::unique_ptr<os::GraphicsOperations> graphicsOps);

	bool initialize();
	void flip();
	void shutdown();

	void setClearColor(int r, int g, int b);
	int setCullMode(int cull);
	int setZbufferMode(int mode);
	int getZbufferMode() const;
	void requestClear();

	// Get current recording frame for draw calls (called during frame recording)
	VulkanFrame* getCurrentRecordingFrame();
	bool isRecording() const { return m_frameLifecycle.isRecording(); }

	// Helper methods for rendering
	vk::Sampler getDummySampler() const { return m_dummySampler.get(); }
	vk::ImageView getDummyImageView() const { return m_dummyImageView.get(); }
	vk::DescriptorImageInfo getTextureDescriptor(int bitmapHandle,
		VulkanFrame& frame,
		vk::CommandBuffer cmd,
		const VulkanTextureManager::SamplerKey& samplerKey);
	vk::PipelineLayout getPipelineLayout() const { return m_descriptorLayouts->pipelineLayout(); }
	VkFormat getSwapChainImageFormat() const { return static_cast<VkFormat>(m_swapChainImageFormat); }
	VkFormat getDepthFormat() const { return static_cast<VkFormat>(m_depthFormat); }
	vk::SampleCountFlagBits getSampleCount() const { return m_sampleCount; }
	uint32_t getColorAttachmentCount() const { return 1; }
	uint32_t getMinUniformBufferAlignment() const { return static_cast<uint32_t>(m_deviceProperties.limits.minUniformBufferOffsetAlignment); }
	uint32_t getVertexBufferAlignment() const { return m_vertexBufferAlignment; }
	ShaderModules getShaderModules(shader_type type) const { return m_shaderManager->getModules(type); }
	vk::Pipeline getPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout) const { return m_pipelineManager->getPipeline(key, modules, layout); }
	vk::Buffer getBuffer(gr_buffer_handle handle) const;
	const ExtendedDynamicState3Caps& getExtendedDynamicState3Caps() const { return m_extDyn3Caps; }
	bool supportsExtendedDynamicState3() const { return m_supportsExtendedDynamicState3; }
	uint32_t getCurrentFrameIndex() const { return m_frameLifecycle.currentFrameIndex(); }
	bool warnOnceIfNotRecording() { return m_frameLifecycle.warnOnceIfNotRecording(); }
	bool supportsVertexAttributeDivisor() const { return m_supportsVertexAttributeDivisor; }

	// Buffer management
	gr_buffer_handle createBuffer(BufferType type, BufferUsageHint usage);
	void deleteBuffer(gr_buffer_handle handle);
	void updateBufferData(gr_buffer_handle handle, size_t size, const void* data);
	void updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data);
	void resizeBuffer(gr_buffer_handle handle, size_t size);
	void* mapBuffer(gr_buffer_handle handle);
	void flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size);
	int preloadTexture(int bitmapHandle, bool isAABitmap);

  private:
	static constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
	static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;
	static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;
	static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024; // 12 MiB for on-demand uploads

	bool initDisplayDevice() const;
	bool initializeInstance();
	bool initializeSurface();
	bool pickPhysicalDevice(PhysicalDeviceValues& deviceValues);
	bool createLogicalDevice(const PhysicalDeviceValues& deviceValues);
	bool createSwapChain(const PhysicalDeviceValues& deviceValues);
	bool recreateSwapChain();

	void createPipelineCache();
	void createUploadCommandPool();
	void createDescriptorResources();
	void createFrames();
	void createDummyTexture();
	void createVertexBuffer();
	void createDepthResources();
	vk::Format findDepthFormat() const;

	uint32_t acquireImage(VulkanFrame& frame);
	void beginFrame(VulkanFrame& frame, uint32_t imageIndex);
	void endFrame(VulkanFrame& frame, uint32_t imageIndex);
	void submitFrame(VulkanFrame& frame, uint32_t imageIndex);

	void immediateSubmit(const std::function<void(vk::CommandBuffer)>& recorder);
	uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;

	std::unique_ptr<os::GraphicsOperations> m_graphicsOps;

	vk::UniqueInstance m_vkInstance;
	vk::UniqueDebugUtilsMessengerEXT m_debugMessenger;
	vk::UniqueSurfaceKHR m_vkSurface{};

	vk::PhysicalDevice m_physicalDevice;
	vk::PhysicalDeviceMemoryProperties m_memoryProperties{};
	vk::PhysicalDeviceProperties m_deviceProperties{};
	vk::UniqueDevice m_device;
	vk::UniquePipelineCache m_pipelineCache;

	vk::Queue m_graphicsQueue;
	vk::Queue m_presentQueue;
	uint32_t m_graphicsQueueIndex = 0;
	uint32_t m_presentQueueIndex = 0;

	vk::UniqueSwapchainKHR m_swapChain;
	vk::Format m_swapChainImageFormat;
	vk::Extent2D m_swapChainExtent;
	SCP_vector<vk::Image> m_swapChainImages;
	SCP_vector<vk::UniqueImageView> m_swapChainImageViews;
	vk::SampleCountFlagBits m_sampleCount = vk::SampleCountFlagBits::e1;
	vk::Format m_depthFormat = vk::Format::eUndefined;
	vk::UniqueImage m_depthImage;
	vk::UniqueDeviceMemory m_depthImageMemory;
	vk::UniqueImageView m_depthImageView;
	bool m_depthImageInitialized = false;

	std::unique_ptr<VulkanDescriptorLayouts> m_descriptorLayouts;
	vk::DescriptorSet m_globalDescriptorSet{};

	std::unique_ptr<VulkanShaderManager> m_shaderManager;
	std::unique_ptr<VulkanPipelineManager> m_pipelineManager;
	std::unique_ptr<VulkanBufferManager> m_bufferManager;
	std::unique_ptr<VulkanTextureManager> m_textureManager;

	std::array<std::unique_ptr<VulkanFrame>, MAX_FRAMES_IN_FLIGHT> m_frames;
	uint32_t m_currentFrame = 0;
	uint32_t m_recordingFrame = 0;
	uint32_t m_recordingImage = 0;
	bool m_isRecording = false;
	FrameLifecycleTracker m_frameLifecycle;

	vk::UniqueCommandPool m_uploadCommandPool;

	vk::UniqueImage m_dummyImage;
	vk::UniqueDeviceMemory m_dummyImageMemory;
	vk::UniqueImageView m_dummyImageView;
	vk::UniqueSampler m_dummySampler;

	vk::UniqueBuffer m_vertexBuffer;
	vk::UniqueDeviceMemory m_vertexBufferMemory;

	std::array<float, 4> m_clearColor = {0.f, 0.f, 0.f, 1.f};
	float m_clearDepth = 1.f;
	bool m_shouldClearColor = true;
	bool m_shouldClearDepth = true;
	vk::CullModeFlagBits m_cullMode = vk::CullModeFlagBits::eBack;
	bool m_depthTestEnable = true;
	bool m_depthWriteEnable = true;
	gr_zbuffer_type m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;

	bool m_supportsExtendedDynamicState = true;
	bool m_supportsExtendedDynamicState2 = false;
	bool m_supportsExtendedDynamicState3 = false;
	bool m_supportsVertexAttributeDivisor = false;
	ExtendedDynamicState3Caps m_extDyn3Caps;
	uint32_t m_vertexBufferAlignment = static_cast<uint32_t>(sizeof(float)); // Alignment for vertex buffer ring allocations (bytes)
};

} // namespace vulkan
} // namespace graphics
