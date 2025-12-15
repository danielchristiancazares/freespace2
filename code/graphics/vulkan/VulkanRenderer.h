#pragma once

#include "osapi/osapi.h"

#include "graphics/grinternal.h"

#include "VulkanBufferManager.h"
#include "VulkanConstants.h"
#include "VulkanDescriptorLayouts.h"
#include "VulkanDevice.h"
#include "VulkanFrame.h"
#include "VulkanPipelineManager.h"
#include "VulkanRenderTargets.h"
#include "VulkanRenderingSession.h"
#include "VulkanShaderManager.h"
#include "VulkanTextureManager.h"
#include "FrameLifecycleTracker.h"
#include "VulkanDebug.h"

#include "graphics/2d.h"
#include "VulkanDeferredLights.h"

#include <array>
#include <functional>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

// Light volume mesh for deferred rendering
struct VolumeMesh {
	gr_buffer_handle vbo;
	gr_buffer_handle ibo{};
	uint32_t indexCount = 0;
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
	void zbufferClear(int mode);

	// Get current recording frame for draw calls (called during frame recording)
	VulkanFrame* getCurrentRecordingFrame();
	bool isRecording() const { return m_frameLifecycle.isRecording(); }

	// Helper methods for rendering
	vk::DescriptorImageInfo getTextureDescriptor(int bitmapHandle,
		VulkanFrame& frame,
		vk::CommandBuffer cmd,
		const VulkanTextureManager::SamplerKey& samplerKey);
	void setModelUniformBinding(VulkanFrame& frame,
		gr_buffer_handle handle,
		size_t offset,
		size_t size);
	void setSceneUniformBinding(VulkanFrame& frame,
		gr_buffer_handle handle,
		size_t offset,
		size_t size);
	void updateModelDescriptors(vk::DescriptorSet set,
		vk::Buffer vertexBuffer,
		const std::vector<std::pair<uint32_t, int>>& textures,
		VulkanFrame& frame,
		vk::CommandBuffer cmd);

	// Frame sync for model descriptors - called at frame start after fence wait
	// vertexHeapBuffer must be valid (caller is responsible for checking)
	void beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer);

	// For debug asserts in draw path - lazy lookup since buffer may not exist at registration time
	vk::Buffer getModelVertexHeapBuffer() const { return queryModelVertexHeapBuffer(); }
		const VulkanRenderingSession::RenderTargetInfo& ensureRenderingStarted(vk::CommandBuffer cmd);
	vk::PipelineLayout getPipelineLayout() const { return m_descriptorLayouts->pipelineLayout(); }
	vk::PipelineLayout getModelPipelineLayout() const { return m_descriptorLayouts->modelPipelineLayout(); }
	size_t getMinUniformOffsetAlignment() const { return m_vulkanDevice->minUniformBufferOffsetAlignment(); }

	// Per-frame instrumentation
	void incrementModelDraw();
	void incrementPrimDraw();

	VkFormat getSwapChainImageFormat() const { return static_cast<VkFormat>(m_vulkanDevice->swapchainFormat()); }
	VkFormat getDepthFormat() const { return static_cast<VkFormat>(m_renderTargets->depthFormat()); }
	vk::SampleCountFlagBits getSampleCount() const { return vk::SampleCountFlagBits::e1; }
	uint32_t getColorAttachmentCount() const { return 1; }

	// Manager accessors
	VulkanRenderTargets* renderTargets() { return m_renderTargets.get(); }
	VulkanRenderingSession* renderingSession() { return m_renderingSession.get(); }
	VulkanBufferManager* bufferManager() { return m_bufferManager.get(); }
	const VulkanBufferManager* bufferManager() const { return m_bufferManager.get(); }
	VulkanTextureManager* textureManager() { return m_textureManager.get(); }
	const VulkanTextureManager* textureManager() const { return m_textureManager.get(); }

		// Deferred rendering hooks
		void beginDeferredLighting(vk::CommandBuffer cmd, bool clearNonColorBufs);
		void endDeferredGeometry(vk::CommandBuffer cmd);
		void bindDeferredGlobalDescriptors();
		void setPendingRenderTargetSwapchain();
		void recordDeferredLighting(VulkanFrame& frame);
		uint32_t getMinUniformBufferAlignment() const { return static_cast<uint32_t>(m_vulkanDevice->minUniformBufferOffsetAlignment()); }
		uint32_t getVertexBufferAlignment() const { return m_vulkanDevice->vertexBufferAlignment(); }
		ShaderModules getShaderModules(shader_type type) const { return m_shaderManager->getModules(type); }
	vk::Pipeline getPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout) const { return m_pipelineManager->getPipeline(key, modules, layout); }
	vk::Buffer getBuffer(gr_buffer_handle handle) const;
	const ExtendedDynamicState3Caps& getExtendedDynamicState3Caps() const { return m_vulkanDevice->extDyn3Caps(); }
	bool supportsExtendedDynamicState3() const { return m_vulkanDevice->supportsExtendedDynamicState3(); }
	uint32_t getCurrentFrameIndex() const { return m_frameLifecycle.currentFrameIndex(); }
	bool warnOnceIfNotRecording() { return m_frameLifecycle.warnOnceIfNotRecording(); }
	bool supportsVertexAttributeDivisor() const { return m_vulkanDevice->supportsVertexAttributeDivisor(); }

	// Buffer management
	gr_buffer_handle createBuffer(BufferType type, BufferUsageHint usage);
	void deleteBuffer(gr_buffer_handle handle);
	void updateBufferData(gr_buffer_handle handle, size_t size, const void* data);
	void updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data);
	void resizeBuffer(gr_buffer_handle handle, size_t size);
	void* mapBuffer(gr_buffer_handle handle);
	void flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size);
	int preloadTexture(int bitmapHandle, bool isAABitmap);

	// Model vertex heap registration (called from GPUMemoryHeap when ModelVertex heap is created)
	void setModelVertexHeapHandle(gr_buffer_handle handle);
	vk::Buffer queryModelVertexHeapBuffer() const;

	// Per-draw descriptor set allocation for model rendering
	vk::DescriptorSet allocateModelDescriptorSet() {
		return m_descriptorLayouts->allocateModelDescriptorSet();
	}

	// Access to VulkanDevice for managers
	VulkanDevice* vulkanDevice() { return m_vulkanDevice.get(); }
	const VulkanDevice* vulkanDevice() const { return m_vulkanDevice.get(); }

  private:
	static constexpr vk::DeviceSize UNIFORM_RING_SIZE = 512 * 1024;
	static constexpr vk::DeviceSize VERTEX_RING_SIZE = 1024 * 1024;
	static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024; // 12 MiB for on-demand uploads

	void createUploadCommandPool();
	void createDescriptorResources();
	void createFrames();
	void createFallbackResources();
	void createVertexBuffer();
	void createRenderTargets();
	void createRenderingSession();
	void createDeferredLightingResources();

	uint32_t acquireImage(VulkanFrame& frame);
	void beginFrame(VulkanFrame& frame, uint32_t imageIndex);
	void endFrame(VulkanFrame& frame, uint32_t imageIndex);
	void submitFrame(VulkanFrame& frame, uint32_t imageIndex);
	void logFrameCounters();

	void immediateSubmit(const std::function<void(vk::CommandBuffer)>& recorder);

	// Descriptor sync helpers
	void writeVertexHeapDescriptor(VulkanFrame& frame, vk::Buffer vertexHeapBuffer);
	void writeTextureDescriptor(vk::DescriptorSet set, uint32_t arrayIndex, int textureHandle);
	void writeFallbackDescriptor(vk::DescriptorSet set, uint32_t arrayIndex);

	// Device layer - owns instance, surface, physical device, logical device, swapchain
	std::unique_ptr<VulkanDevice> m_vulkanDevice;

	// Render targets - owns depth buffer and G-buffer
	std::unique_ptr<VulkanRenderTargets> m_renderTargets;

			// Rendering session - manages render pass state machine
			std::unique_ptr<VulkanRenderingSession> m_renderingSession;

	std::unique_ptr<VulkanDescriptorLayouts> m_descriptorLayouts;
	vk::DescriptorSet m_globalDescriptorSet{};

	std::unique_ptr<VulkanShaderManager> m_shaderManager;
	std::unique_ptr<VulkanPipelineManager> m_pipelineManager;
	std::unique_ptr<VulkanBufferManager> m_bufferManager;
	std::unique_ptr<VulkanTextureManager> m_textureManager;

	std::array<std::unique_ptr<VulkanFrame>, kFramesInFlight> m_frames;
	uint32_t m_currentFrame = 0;
	uint32_t m_recordingFrame = 0;
	uint32_t m_recordingImage = 0;
	bool m_isRecording = false;
	FrameLifecycleTracker m_frameLifecycle;

	vk::UniqueCommandPool m_uploadCommandPool;

	vk::UniqueBuffer m_vertexBuffer;
	vk::UniqueDeviceMemory m_vertexBufferMemory;

	// Per-frame draw counters
	uint32_t m_frameModelDraws = 0;
	uint32_t m_framePrimDraws = 0;
	uint32_t m_frameCounter = 0;
	uint64_t m_submitSerial = 0;

	// Model vertex heap handle - set via setModelVertexHeapHandle when ModelVertex heap is created.
	// The actual VkBuffer is looked up lazily via queryModelVertexHeapBuffer() since the buffer
	// may not exist at registration time (VulkanBufferManager defers buffer creation).
	gr_buffer_handle m_modelVertexHeapHandle;

	// Fallback resources bound at frame start when uniforms not explicitly set
	gr_buffer_handle m_fallbackModelUniformHandle = gr_buffer_handle::invalid();
	gr_buffer_handle m_fallbackModelVertexHeapHandle = gr_buffer_handle::invalid();

	// Z-buffer mode tracking (for getZbufferMode)
	gr_zbuffer_type m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;

	// Deferred lighting meshes
	VolumeMesh m_fullscreenMesh;
	VolumeMesh m_sphereMesh;
	VolumeMesh m_cylinderMesh;
};

} // namespace vulkan
} // namespace graphics
