#pragma once

#include "osapi/osapi.h"

#include "graphics/grinternal.h"

#include "VulkanBufferManager.h"
#include "VulkanConstants.h"
#include "VulkanDescriptorLayouts.h"
#include "VulkanDevice.h"
#include "VulkanFrame.h"
#include "VulkanFrameFlow.h"
#include "VulkanPipelineManager.h"
#include "VulkanRenderTargets.h"
#include "VulkanRenderingSession.h"
#include "VulkanShaderManager.h"
#include "VulkanTextureManager.h"
#include "VulkanDebug.h"

#include "graphics/2d.h"
#include "VulkanDeferredLights.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

class VulkanTextureBindings;
class VulkanTextureUploader;

// Light volume mesh for deferred rendering
struct VolumeMesh {
	gr_buffer_handle vbo;
	gr_buffer_handle ibo{};
	uint32_t indexCount = 0;
};

class VulkanRenderer {
  public:
	explicit VulkanRenderer(std::unique_ptr<os::GraphicsOperations> graphicsOps);
	~VulkanRenderer();

	bool initialize();
	void shutdown();

	// New frame flow API - Phase 1
	graphics::vulkan::RecordingFrame beginRecording();
	graphics::vulkan::RecordingFrame advanceFrame(graphics::vulkan::RecordingFrame prev);

	void setClearColor(int r, int g, int b);
	int setCullMode(int cull);
	int setZbufferMode(int mode);
	int getZbufferMode() const;
	void requestClear();
	void zbufferClear(int mode);


		// Helper methods for rendering
		vk::DescriptorImageInfo getTextureDescriptor(int bitmapHandle,
			const VulkanTextureManager::SamplerKey& samplerKey);
		vk::DescriptorImageInfo getDefaultTextureDescriptor(const VulkanTextureManager::SamplerKey& samplerKey);
		// Returns a valid bindless slot index. Invalid handles return slot 0 (fallback).
		uint32_t getBindlessTextureIndex(int bitmapHandle);
		void setModelUniformBinding(VulkanFrame& frame,
			gr_buffer_handle handle,
			size_t offset,
			size_t size);
	void setSceneUniformBinding(VulkanFrame& frame,
		gr_buffer_handle handle,
		size_t offset,
		size_t size);
	void updateModelDescriptors(vk::DescriptorSet set,
			vk::Buffer vertexHeapBuffer,
			const std::vector<std::pair<uint32_t, int>>& textures);

	// Frame sync for model descriptors - called at frame start after fence wait
	// vertexHeapBuffer must be valid (caller is responsible for checking)
	void beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer);

	// For debug asserts in draw path - lazy lookup since buffer may not exist at registration time
	vk::Buffer getModelVertexHeapBuffer() const { return queryModelVertexHeapBuffer(); }
	VulkanRenderingSession::RenderScope ensureRenderingStarted(graphics::vulkan::RecordingFrame& rec); // Recording-only
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
		enum class DeferredBoundaryState { Idle, InGeometry, AwaitFinish };

		void beginDeferredLighting(graphics::vulkan::RecordingFrame& rec, bool clearNonColorBufs); // Recording-only
		void endDeferredGeometry(vk::CommandBuffer cmd);
		void bindDeferredGlobalDescriptors();
		void setPendingRenderTargetSwapchain();
		void recordDeferredLighting(graphics::vulkan::RecordingFrame& rec);
		void deferredLightingBegin(graphics::vulkan::RecordingFrame& rec, bool clearNonColorBufs);
		void deferredLightingEnd(graphics::vulkan::RecordingFrame& rec);
		void deferredLightingFinish(graphics::vulkan::RecordingFrame& rec, const vk::Rect2D& restoreScissor);
		uint32_t getMinUniformBufferAlignment() const { return static_cast<uint32_t>(m_vulkanDevice->minUniformBufferOffsetAlignment()); }
		uint32_t getVertexBufferAlignment() const { return m_vulkanDevice->vertexBufferAlignment(); }
		ShaderModules getShaderModules(shader_type type) const { return m_shaderManager->getModules(type); }
	vk::Pipeline getPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout) const { return m_pipelineManager->getPipeline(key, modules, layout); }
	vk::Buffer getBuffer(gr_buffer_handle handle) const;
	const ExtendedDynamicState3Caps& getExtendedDynamicState3Caps() const { return m_vulkanDevice->extDyn3Caps(); }
	bool supportsExtendedDynamicState3() const { return m_vulkanDevice->supportsExtendedDynamicState3(); }
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
		void createSubmitTimelineSemaphore();
		void createDescriptorResources();
		void createFrames();
		void createVertexBuffer();
		void createRenderTargets();
		void createRenderingSession();
	void createDeferredLightingResources();

	uint32_t acquireImage(VulkanFrame& frame);
	uint32_t acquireImageOrThrow(VulkanFrame& frame); // Throws on failure, no sentinels
		void beginFrame(VulkanFrame& frame, uint32_t imageIndex);
		void endFrame(graphics::vulkan::RecordingFrame& rec); // Recording-only
		void logFrameCounters();

		// Container-based frame management (replaces warmup/steady states)
		struct AvailableFrame {
			VulkanFrame* frame;
			uint64_t completedSerial;
		};
		AvailableFrame acquireAvailableFrame(); // Non-recording: pulls from containers
		void recycleOneInFlight(); // Consumes one in-flight frame, adds to available
		graphics::vulkan::SubmitInfo submitRecordedFrame(graphics::vulkan::RecordingFrame& rec); // Recording-only

		void immediateSubmit(const std::function<void(vk::CommandBuffer)>& recorder);

		uint64_t queryCompletedSerial() const;
		void maybeRunVulkanStress();

		void prepareFrameForReuse(VulkanFrame& frame, uint64_t completedSerial);

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
		// Frames ready for CPU reuse (already waited/reset); completedSerial is the latest known safe serial.
		std::deque<AvailableFrame> m_availableFrames;
		std::deque<graphics::vulkan::InFlightFrame> m_inFlightFrames;

	vk::UniqueCommandPool m_uploadCommandPool;

	vk::UniqueBuffer m_vertexBuffer;
		vk::UniqueDeviceMemory m_vertexBufferMemory;

		// Global GPU completion timeline (serial == timeline value).
		vk::UniqueSemaphore m_submitTimeline;

		// Per-frame draw counters
		uint32_t m_frameModelDraws = 0;
		uint32_t m_framePrimDraws = 0;
		uint32_t m_frameCounter = 0;
		uint64_t m_submitSerial = 0;
		uint64_t m_completedSerial = 0;

		// Optional stress harness state (enabled via -vk_stress).
		std::vector<gr_buffer_handle> m_stressBuffers;
		std::vector<uint8_t> m_stressScratch;

		// Texture responsibilities are split: bindings are draw-path safe, uploader is upload-phase only.
		std::unique_ptr<VulkanTextureBindings> m_textureBindings;
		std::unique_ptr<VulkanTextureUploader> m_textureUploader;

		DeferredBoundaryState m_deferredBoundaryState = DeferredBoundaryState::Idle;

	// Model vertex heap handle - set via setModelVertexHeapHandle when ModelVertex heap is created.
	// The actual VkBuffer is looked up lazily via queryModelVertexHeapBuffer() since the buffer
	// may not exist at registration time (VulkanBufferManager defers buffer creation).
	gr_buffer_handle m_modelVertexHeapHandle;

	// Z-buffer mode tracking (for getZbufferMode)
	gr_zbuffer_type m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;

	// Deferred lighting meshes
	VolumeMesh m_fullscreenMesh;
	VolumeMesh m_sphereMesh;
	VolumeMesh m_cylinderMesh;
};

} // namespace vulkan
} // namespace graphics
