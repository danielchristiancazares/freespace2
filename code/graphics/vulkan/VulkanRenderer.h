#pragma once

#include "osapi/osapi.h"

#include "graphics/grinternal.h"

#include "VulkanBufferManager.h"
#include "VulkanConstants.h"
#include "VulkanDescriptorLayouts.h"
#include "VulkanDevice.h"
#include "VulkanFrame.h"
#include "VulkanPhaseContexts.h"
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
#include <optional>
#include <variant>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace generic_data {
struct lightshaft_data;
struct post_data;
} // namespace generic_data

namespace vulkan {

class VulkanTextureBindings;
class VulkanTextureUploader;
class VulkanMovieManager;
struct FrameCtx;

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
	int saveScreen();
	void freeScreen(int handle);
	int frozenScreenHandle() const;


		// Helper methods for rendering
		vk::DescriptorImageInfo getTextureDescriptor(const FrameCtx& ctx,
			int bitmapHandle,
			const VulkanTextureManager::SamplerKey& samplerKey);
		vk::DescriptorImageInfo getDefaultTextureDescriptor(const VulkanTextureManager::SamplerKey& samplerKey);
		// Prepare for decal rendering: decals sample the main depth buffer, so it must be shader-readable.
		void beginDecalPass(const FrameCtx& ctx);
		// Returns a valid bindless slot index. Invalid handles return slot 0 (fallback).
		uint32_t getBindlessTextureIndex(const FrameCtx& ctx, int bitmapHandle);
		// Recording-only: update dynamic viewport/scissor state without requiring an active rendering pass.
		void setViewport(const FrameCtx& ctx, const vk::Viewport& viewport);
		void setScissor(const FrameCtx& ctx, const vk::Rect2D& scissor);
		// Bitmap render targets (bmpman RTT API)
		bool createBitmapRenderTarget(int handle, int* width, int* height, int* bpp, int* mm_lvl, int flags);
		bool setBitmapRenderTarget(const FrameCtx& ctx, int handle, int face);

	// Scene texture (OpenGL parity): render the 3D scene into an offscreen HDR target, then post-process to swapchain.
	void beginSceneTexture(const FrameCtx& ctx, bool enableHdrPipeline);
	void copySceneEffectTexture(const FrameCtx& ctx);
	void endSceneTexture(const FrameCtx& ctx, bool enablePostProcessing);
		void setModelUniformBinding(VulkanFrame& frame,
			gr_buffer_handle handle,
			size_t offset,
			size_t size);
	void setSceneUniformBinding(VulkanFrame& frame,
		gr_buffer_handle handle,
		size_t offset,
		size_t size);

	// Frame sync for model descriptors - called at frame start after fence wait
	// vertexHeapBuffer must be valid (caller is responsible for checking)
	void beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer);

	// For debug asserts in draw path - lazy lookup since buffer may not exist at registration time
	vk::Buffer getModelVertexHeapBuffer() const { return queryModelVertexHeapBuffer(); }
		RenderCtx ensureRenderingStarted(const FrameCtx& ctx); // Recording-only (requires FrameCtx token)
		// Recording-only: apply dynamic state before rendering begins (viewport/scissor/line width).
		void applySetupFrameDynamicState(const FrameCtx& ctx,
			const vk::Viewport& viewport,
			const vk::Rect2D& scissor,
			float lineWidth);
		// Recording-only: debug labels (no render pass requirement).
		void pushDebugGroup(const FrameCtx& ctx, const char* name);
		void popDebugGroup(const FrameCtx& ctx);
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
	VulkanMovieManager* movieManager() { return m_movieManager.get(); }
	const VulkanMovieManager* movieManager() const { return m_movieManager.get(); }

		// Deferred rendering hooks
		void setPendingRenderTargetSwapchain();

		// Typestate API: begin -> end -> finish (no state enum).
		DeferredGeometryCtx deferredLightingBegin(graphics::vulkan::RecordingFrame& rec, bool clearNonColorBufs);
		DeferredLightingCtx deferredLightingEnd(graphics::vulkan::RecordingFrame& rec, DeferredGeometryCtx&& geometry);
		void deferredLightingFinish(graphics::vulkan::RecordingFrame& rec, DeferredLightingCtx&& lighting, const vk::Rect2D& restoreScissor);
		uint32_t getMinUniformBufferAlignment() const { return static_cast<uint32_t>(m_vulkanDevice->minUniformBufferOffsetAlignment()); }
		uint32_t getVertexBufferAlignment() const { return m_vulkanDevice->vertexBufferAlignment(); }
		ShaderModules getShaderModules(shader_type type) const { return m_shaderManager->getModules(type); }
		ShaderModules getShaderModulesByFilenames(const SCP_string& vertFilename, const SCP_string& fragFilename) const
		{
			return m_shaderManager->getModulesByFilenames(vertFilename, fragFilename);
		}
	vk::Pipeline getPipeline(const PipelineKey& key, const ShaderModules& modules, const vertex_layout& layout) const { return m_pipelineManager->getPipeline(key, modules, layout); }
		vk::DescriptorSet globalDescriptorSet() const { return m_globalDescriptorSet; }
		// Updates the global (set=1) descriptor set to point at current G-buffer/depth views.
		void refreshGlobalDescriptorSet() { bindDeferredGlobalDescriptors(); }
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
	// Recording-only: uploads new pixel data into an existing bitmap texture (streaming anims, NanoVG, etc.).
	void updateTexture(const FrameCtx& ctx, int bitmapHandle, int bpp, const ubyte* data, int width, int height);
	void releaseBitmap(int bitmapHandle);
	MovieTextureHandle createMovieTexture(uint32_t width, uint32_t height, MovieColorSpace colorspace, MovieColorRange range);
	void uploadMovieTexture(const FrameCtx& ctx,
		MovieTextureHandle handle,
		const ubyte* y,
		int yStride,
		const ubyte* u,
		int uStride,
		const ubyte* v,
		int vStride);
	void drawMovieTexture(const FrameCtx& ctx,
		MovieTextureHandle handle,
		float x1,
		float y1,
		float x2,
		float y2,
		float alpha);
	void releaseMovieTexture(MovieTextureHandle handle);

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

		// Initialization-phase capability token: proves we are in initialize() and not mid-frame.
		// This prevents "immediate submit" style helpers from being callable from draw/recording paths.
		struct InitCtx {
			InitCtx(const InitCtx&) = delete;
			InitCtx& operator=(const InitCtx&) = delete;
			InitCtx(InitCtx&&) = default;
			InitCtx& operator=(InitCtx&&) = default;

		private:
			InitCtx() = default;
			friend bool VulkanRenderer::initialize();
		};

		RenderCtx ensureRenderingStartedRecording(graphics::vulkan::RecordingFrame& rec); // Recording-only (internal)
		// Recording-only: flushes any queued bitmap uploads into the current command buffer.
		// If rendering was active, this will suspend and then resume dynamic rendering to make transfer ops legal.
		void flushQueuedTextureUploads(const FrameCtx& ctx, bool syncModelDescriptors);

		// Deferred implementation details (called by typestate wrapper API)
		void beginDeferredLighting(graphics::vulkan::RecordingFrame& rec, bool clearNonColorBufs); // Recording-only
		void endDeferredGeometry(vk::CommandBuffer cmd);
		void bindDeferredGlobalDescriptors();
		void recordPreDeferredSceneColorCopy(const RenderCtx& render, uint32_t imageIndex);
		void recordPreDeferredSceneHdrCopy(const RenderCtx& render);
		void recordDeferredLighting(const RenderCtx& render,
			vk::Buffer uniformBuffer,
			const std::vector<DeferredLight>& lights);
		void recordTonemappingToSwapchain(const RenderCtx& render, VulkanFrame& frame, bool hdrEnabled);
		void recordBloomBrightPass(const RenderCtx& render, VulkanFrame& frame);
		void recordBloomBlurPass(const RenderCtx& render,
			VulkanFrame& frame,
			uint32_t srcPingPongIndex,
			uint32_t variantFlags,
			int mipLevel,
			uint32_t bloomWidth,
			uint32_t bloomHeight);
		void recordBloomCompositePass(const RenderCtx& render, VulkanFrame& frame, int mipLevels);
		void generateBloomMipmaps(vk::CommandBuffer cmd, uint32_t pingPongIndex, vk::Extent2D baseExtent);
		void recordSmaaEdgePass(const RenderCtx& render, VulkanFrame& frame, const vk::DescriptorImageInfo& colorInput);
		void recordSmaaBlendWeightsPass(const RenderCtx& render,
			VulkanFrame& frame,
			const vk::DescriptorImageInfo& edgesInput,
			const vk::DescriptorImageInfo& areaTex,
			const vk::DescriptorImageInfo& searchTex);
		void recordSmaaNeighborhoodPass(const RenderCtx& render,
			VulkanFrame& frame,
			const vk::DescriptorImageInfo& colorInput,
			const vk::DescriptorImageInfo& blendTex);
		void recordFxaaPrepass(const RenderCtx& render, VulkanFrame& frame, const vk::DescriptorImageInfo& ldrInput);
		void recordFxaaPass(const RenderCtx& render, VulkanFrame& frame, const vk::DescriptorImageInfo& luminanceInput);
		void recordLightshaftsPass(const RenderCtx& render, VulkanFrame& frame, const graphics::generic_data::lightshaft_data& params);
		void recordPostEffectsPass(const RenderCtx& render, VulkanFrame& frame, const graphics::generic_data::post_data& params, const vk::DescriptorImageInfo& ldrInput, const vk::DescriptorImageInfo& depthInput);
		void recordCopyToSwapchain(const RenderCtx& render, vk::DescriptorImageInfo src);

		void requestMainTargetWithDepth();

		// Descriptor sync helpers (called from beginFrame).
		void updateModelDescriptors(uint32_t frameIndex,
				vk::DescriptorSet set,
				vk::Buffer vertexHeapBuffer,
				vk::Buffer transformBuffer,
				const std::vector<std::pair<uint32_t, int>>& textures);

			void createUploadCommandPool();
			void createSubmitTimelineSemaphore();
		void createDescriptorResources();
		void createFrames();
		void createVertexBuffer();
		void createRenderTargets();
		void createRenderingSession();
	void createDeferredLightingResources();
	void createSmaaLookupTextures(const InitCtx& init);

	uint32_t acquireImage(VulkanFrame& frame);
	uint32_t acquireImageOrThrow(VulkanFrame& frame); // Throws on failure, no sentinels
		void beginFrame(VulkanFrame& frame, uint32_t imageIndex);
		void endFrame(graphics::vulkan::RecordingFrame& rec); // Recording-only
		void updateSavedScreenCopy(graphics::vulkan::RecordingFrame& rec); // Recording-only
		void logFrameCounters();

		// Container-based frame management (replaces warmup/steady states)
		struct AvailableFrame {
			VulkanFrame* frame;
			uint64_t completedSerial;
		};
		AvailableFrame acquireAvailableFrame(); // Non-recording: pulls from containers
		void recycleOneInFlight(); // Consumes one in-flight frame, adds to available
		graphics::vulkan::SubmitInfo submitRecordedFrame(graphics::vulkan::RecordingFrame& rec); // Recording-only

		// Init-only: records a one-time command buffer, submits it, and blocks until it has completed.
		// Uses the renderer's global timeline semaphore to integrate with serial-gated deferred releases.
		void submitInitCommandsAndWait(const InitCtx& init, const std::function<void(vk::CommandBuffer)>& recorder);

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
		std::unique_ptr<VulkanMovieManager> m_movieManager;

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

		// Model vertex heap handle - set via setModelVertexHeapHandle when ModelVertex heap is created.
	// The actual VkBuffer is looked up lazily via queryModelVertexHeapBuffer() since the buffer
	// may not exist at registration time (VulkanBufferManager defers buffer creation).
	gr_buffer_handle m_modelVertexHeapHandle;

	// Per-frame cache of bindless descriptor contents so we can update only dirty slots.
	struct ModelBindlessDescriptorCache {
		bool initialized = false;
		std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> infos{};
	};
	std::array<ModelBindlessDescriptorCache, kFramesInFlight> m_modelBindlessCache;

	// Z-buffer mode tracking (for getZbufferMode)
	gr_zbuffer_type m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;

	// Deferred lighting meshes
	VolumeMesh m_fullscreenMesh;
	VolumeMesh m_sphereMesh;
	VolumeMesh m_cylinderMesh;

	// SMAA lookup textures (area/search) - immutable, sampled-only resources.
	struct SmaaLookupTexture {
		vk::UniqueImage image;
		vk::UniqueDeviceMemory memory;
		vk::UniqueImageView view;
	};
	SmaaLookupTexture m_smaaAreaTex;
	SmaaLookupTexture m_smaaSearchTex;

	// Saved-screen capture state: tracking updates or frozen for restore.
	struct SavedScreenCapture {
		struct Tracking {
			int handle = -1;
			vk::Extent2D extent{};
		};
		struct Frozen {
			int handle = -1;
			vk::Extent2D extent{};
		};

		using State = std::variant<std::monostate, Tracking, Frozen>;
		State state{};

		bool isTracking() const { return std::holds_alternative<Tracking>(state); }
		bool isFrozen() const { return std::holds_alternative<Frozen>(state); }
		bool hasHandle() const { return !std::holds_alternative<std::monostate>(state); }

		int handle() const
		{
			if (auto* tracking = std::get_if<Tracking>(&state)) {
				return tracking->handle;
			}
			if (auto* frozen = std::get_if<Frozen>(&state)) {
				return frozen->handle;
			}
			return -1;
		}

		vk::Extent2D extent() const
		{
			if (auto* tracking = std::get_if<Tracking>(&state)) {
				return tracking->extent;
			}
			if (auto* frozen = std::get_if<Frozen>(&state)) {
				return frozen->extent;
			}
			return vk::Extent2D{};
		}

		void setTracking(int handle, vk::Extent2D extent) { state = Tracking{handle, extent}; }
		void freeze()
		{
			if (auto* tracking = std::get_if<Tracking>(&state)) {
				state = Frozen{tracking->handle, tracking->extent};
			}
		}
		void unfreeze()
		{
			if (auto* frozen = std::get_if<Frozen>(&state)) {
				state = Tracking{frozen->handle, frozen->extent};
			}
		}
		void reset() { state = std::monostate{}; }
	};
	SavedScreenCapture m_savedScreen;

	// Scene texture typestate: presence == active (state as location).
	struct SceneTextureState {
		bool hdrEnabled = false;
	};
	std::optional<SceneTextureState> m_sceneTexture;
};

} // namespace vulkan
} // namespace graphics
