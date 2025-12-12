
#include "VulkanRenderer.h"
#include "VulkanGraphics.h"
#include "VulkanConstants.h"
#include "graphics/util/uniform_structs.h"

#include "def_files/def_files.h"
#include "graphics/2d.h"
#include "VulkanModelValidation.h"
#include "VulkanDebug.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace graphics {
namespace vulkan {

VulkanRenderer::VulkanRenderer(std::unique_ptr<os::GraphicsOperations> graphicsOps)
	: m_vulkanDevice(std::make_unique<VulkanDevice>(std::move(graphicsOps)))
{
}

bool VulkanRenderer::initialize() {
	// Initialize the device layer (instance, surface, physical device, logical device, swapchain)
	if (!m_vulkanDevice->initialize()) {
		vkprintf("VulkanDevice initialization failed\n");
		return false;
	}

	// Create renderer-specific resources
	createDescriptorResources();
	createRenderTargets();
	createRenderingSession();
	createUploadCommandPool();
	createFrames();

	// Initialize managers using VulkanDevice handles
	const SCP_string shaderRoot = "code/graphics/shaders/compiled";
	m_shaderManager = std::make_unique<VulkanShaderManager>(m_vulkanDevice->device(), shaderRoot);

	m_pipelineManager = std::make_unique<VulkanPipelineManager>(m_vulkanDevice->device(),
		m_descriptorLayouts->pipelineLayout(),
		m_descriptorLayouts->modelPipelineLayout(),
		m_vulkanDevice->pipelineCache(),
		m_vulkanDevice->supportsExtendedDynamicState(),
		m_vulkanDevice->supportsExtendedDynamicState2(),
		m_vulkanDevice->supportsExtendedDynamicState3(),
		m_vulkanDevice->extDyn3Caps(),
		m_vulkanDevice->supportsVertexAttributeDivisor(),
		m_vulkanDevice->features13().dynamicRendering == VK_TRUE);

	m_bufferManager = std::make_unique<VulkanBufferManager>(m_vulkanDevice->device(),
		m_vulkanDevice->memoryProperties(),
		m_vulkanDevice->graphicsQueue(),
		m_vulkanDevice->graphicsQueueIndex());

	m_textureManager = std::make_unique<VulkanTextureManager>(m_vulkanDevice->device(),
		m_vulkanDevice->memoryProperties(),
		m_vulkanDevice->graphicsQueue(),
		m_vulkanDevice->graphicsQueueIndex());

	return true;
}

void VulkanRenderer::createDescriptorResources() {
	// Validate device limits before creating layouts - hard assert on failure
	VulkanDescriptorLayouts::validateDeviceLimits(m_vulkanDevice->properties().limits);
	EnsurePushDescriptorSupport(m_vulkanDevice->features14());

	m_descriptorLayouts = std::make_unique<VulkanDescriptorLayouts>(m_vulkanDevice->device());
	m_globalDescriptorSet = m_descriptorLayouts->allocateGlobalSet();
}

void VulkanRenderer::createFrames() {
	const auto& props = m_vulkanDevice->properties();
	for (size_t i = 0; i < kFramesInFlight; ++i) {
		m_frames[i] = std::make_unique<VulkanFrame>(
			m_vulkanDevice->device(),
			m_vulkanDevice->graphicsQueueIndex(),
			m_vulkanDevice->memoryProperties(),
			UNIFORM_RING_SIZE,
			props.limits.minUniformBufferOffsetAlignment,
			VERTEX_RING_SIZE,
			m_vulkanDevice->vertexBufferAlignment(),
			STAGING_RING_SIZE,
			props.limits.optimalBufferCopyOffsetAlignment);
	}
}

void VulkanRenderer::createRenderTargets() {
	m_renderTargets = std::make_unique<VulkanRenderTargets>(*m_vulkanDevice);
	m_renderTargets->create(m_vulkanDevice->swapchainExtent());
}

void VulkanRenderer::createRenderingSession() {
	m_renderingSession = std::make_unique<VulkanRenderingSession>(
		*m_vulkanDevice, *m_renderTargets, *m_descriptorLayouts);
}

void VulkanRenderer::createUploadCommandPool() {
	vk::CommandPoolCreateInfo poolInfo;
	poolInfo.queueFamilyIndex = m_vulkanDevice->graphicsQueueIndex();
	poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
	m_uploadCommandPool = m_vulkanDevice->device().createCommandPoolUnique(poolInfo);
}

uint32_t VulkanRenderer::acquireImage(VulkanFrame& frame) {
	auto result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());

	if (result.needsRecreate) {
		const auto extent = m_vulkanDevice->swapchainExtent();
		if (!m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
			// Swapchain recreation failed - cannot recover
			return std::numeric_limits<uint32_t>::max();
		}
		// Recreate render targets that depend on swapchain size
		m_renderTargets->resize(m_vulkanDevice->swapchainExtent());

		// Retry acquire after successful recreation (fixes C5: crash on resize)
		result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());
		if (!result.success) {
			return std::numeric_limits<uint32_t>::max();
		}
		return result.imageIndex;
	}

	if (!result.success) {
		return std::numeric_limits<uint32_t>::max();
	}

	return result.imageIndex;
}

void VulkanRenderer::beginFrame(VulkanFrame& frame, uint32_t imageIndex) {
	m_frameLifecycle.begin(m_currentFrame);
	m_isRecording = true;

	// Sync model descriptors BEFORE command buffer recording
	// This is the ONLY place descriptors are updated for this frame
	Assertion(m_textureManager != nullptr, "m_textureManager must be initialized before beginFrame");

	// Only sync model descriptors if the model vertex heap exists
	vk::Buffer vertexHeapBuffer = queryModelVertexHeapBuffer();
	if (vertexHeapBuffer) {
		beginModelDescriptorSync(frame, m_currentFrame, vertexHeapBuffer);
	}

	vk::CommandBuffer cmd = frame.commandBuffer();

	vk::CommandBufferBeginInfo beginInfo;
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	cmd.begin(beginInfo);

	// Upload any pending textures before rendering begins (no render pass active yet)
	// This is the explicit upload flush point - textures requested before rendering starts
	// will be queued and flushed here
	Assertion(m_textureManager != nullptr, "m_textureManager must be initialized before beginFrame");
	m_textureManager->flushPendingUploads(frame, cmd, m_currentFrame);

	// Delegate barrier setup and render state reset to VulkanRenderingSession
	m_renderingSession->beginFrame(cmd, imageIndex, m_globalDescriptorSet);
}

void VulkanRenderer::endFrame(VulkanFrame& frame, uint32_t imageIndex) {
	if (!m_isRecording) {
		vkprintf("endFrame() - NOT RECORDING, skipping\n");
		return;
	}
	vk::CommandBuffer cmd = frame.commandBuffer();

	// Delegate render pass termination and present barrier to VulkanRenderingSession
	m_renderingSession->endFrame(cmd, imageIndex);

	cmd.end();
	m_frameLifecycle.end();
	m_isRecording = false;
}

void VulkanRenderer::submitFrame(VulkanFrame& frame, uint32_t imageIndex) {
	vk::CommandBufferSubmitInfo cmdInfo;
	cmdInfo.commandBuffer = frame.commandBuffer();

	vk::SemaphoreSubmitInfo waitSemaphore;
	waitSemaphore.semaphore = frame.imageAvailable();
	waitSemaphore.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

	vk::SemaphoreSubmitInfo signalSemaphores[2];
	signalSemaphores[0].semaphore = frame.renderFinished();
	signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

	signalSemaphores[1].semaphore = frame.timelineSemaphore();
	signalSemaphores[1].value = frame.nextTimelineValue();
	signalSemaphores[1].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

	vk::SubmitInfo2 submitInfo;
	submitInfo.waitSemaphoreInfoCount = 1;
	submitInfo.pWaitSemaphoreInfos = &waitSemaphore;
	submitInfo.commandBufferInfoCount = 1;
	submitInfo.pCommandBufferInfos = &cmdInfo;
	submitInfo.signalSemaphoreInfoCount = 2;
	submitInfo.pSignalSemaphoreInfos = signalSemaphores;

	const uint64_t submitSerial = ++m_submitSerial;
	frame.record_submit_info(m_currentFrame, imageIndex, frame.nextTimelineValue(), submitSerial);

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
	m_vulkanDevice->graphicsQueue().submit2(submitInfo, frame.inflightFence());
#else
	m_vulkanDevice->graphicsQueue().submit2(submitInfo, frame.inflightFence());
#endif

	// Present the frame
	auto presentResult = m_vulkanDevice->present(frame.renderFinished(), imageIndex);

	if (presentResult.needsRecreate) {
		vkprintf("Swapchain out of date/suboptimal, recreating...\n");
		const auto extent = m_vulkanDevice->swapchainExtent();
		if (m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
			m_renderTargets->resize(m_vulkanDevice->swapchainExtent());
		}
	}

	frame.advanceTimeline();
}

void VulkanRenderer::incrementModelDraw() {
	m_frameModelDraws++;
}

void VulkanRenderer::incrementPrimDraw() {
	m_framePrimDraws++;
}

void VulkanRenderer::logFrameCounters() {
	m_frameCounter++;
}

void VulkanRenderer::flip() {
	// #region agent log
	auto agent_log = [](const char* hypothesisId, const char* location, const char* message, auto&& dataWriter) {
		std::ofstream logFile("c:\\Users\\danie\\Documents\\freespace2\\.cursor\\debug.log", std::ios::app);
		const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
		logFile << "{\"sessionId\":\"debug-session\",\"runId\":\"run1\",\"hypothesisId\":\""
		        << hypothesisId << "\",\"location\":\"" << location << "\",\"message\":\""
		        << message << "\",\"data\":";
		dataWriter(logFile);
		logFile << ",\"timestamp\":" << now << "}\n";
	};
	// #endregion

	// Finish previously recorded frame
	if (m_isRecording) {
		auto& recordingFrame = *m_frames[m_recordingFrame];
		// #region agent log
		agent_log("H5", "VulkanRenderer.cpp:flip", "before submitFrame",
			[this, &recordingFrame](std::ofstream& out) {
				out << "{\"recordingFrame\":" << m_recordingFrame
				    << ",\"recordingImage\":" << m_recordingImage
				    << ",\"hasSubmitInfo\":" << (recordingFrame.hasSubmitInfo() ? 1 : 0)
				    << "}";
			});
		// #endregion
		endFrame(recordingFrame, m_recordingImage);
		submitFrame(recordingFrame, m_recordingImage);
		// #region agent log
		agent_log("H5", "VulkanRenderer.cpp:flip", "after submitFrame",
			[&recordingFrame](std::ofstream& out) {
				out << "{\"hasSubmitInfo\":" << (recordingFrame.hasSubmitInfo() ? 1 : 0)
				    << ",\"lastSubmitSerial\":" << recordingFrame.lastSubmitSerial()
				    << "}";
			});
		// #endregion

		// Log per-frame draw counters now that the frame is finalized
		logFrameCounters();
		m_frameModelDraws = 0;
		m_framePrimDraws = 0;
	}

	// Advance frame index and prepare next frame
	uint32_t prevFrame = m_currentFrame;
	m_currentFrame = (m_currentFrame + 1) % kFramesInFlight;
	auto& frame = *m_frames[m_currentFrame];
	// #region agent log
	agent_log("H5", "VulkanRenderer.cpp:flip", "before wait_for_gpu",
		[this, prevFrame, &frame](std::ofstream& out) {
			out << "{\"prevFrame\":" << prevFrame
			    << ",\"currentFrame\":" << m_currentFrame
			    << ",\"hasSubmitInfo\":" << (frame.hasSubmitInfo() ? 1 : 0)
			    << ",\"lastSubmitSerial\":" << frame.lastSubmitSerial()
			    << ",\"lastSubmitFrameIndex\":" << frame.lastSubmitFrameIndex()
			    << "}";
		});
	// #endregion
	frame.wait_for_gpu();

	// Get completed serial from the frame that just finished
	// The fence wait ensures GPU has completed this frame's work
	uint64_t completedSerial = 0;
	if (frame.hasSubmitInfo()) {
		completedSerial = frame.lastSubmitSerial();
	}

	// Process deferred buffer deletions
	Assertion(m_bufferManager != nullptr, "m_bufferManager must be initialized");
	m_bufferManager->onFrameEnd();
	Assertion(m_textureManager != nullptr, "m_textureManager must be initialized");
	m_textureManager->markUploadsCompleted(m_currentFrame);
	m_textureManager->processPendingDestructions(completedSerial);
	frame.reset();

	uint32_t imageIndex = acquireImage(frame);
	Assertion(imageIndex != std::numeric_limits<uint32_t>::max(), "flip() failed to acquire swapchain image");

	m_recordingFrame = m_currentFrame;
	m_recordingImage = imageIndex;
	beginFrame(frame, imageIndex);
}

VulkanFrame* VulkanRenderer::getCurrentRecordingFrame() {
	if (!m_isRecording) {
		return nullptr;
	}
	return m_frames[m_recordingFrame].get();
}

void VulkanRenderer::ensureRenderingStarted(vk::CommandBuffer cmd) {
	m_renderingSession->ensureRenderingActive(cmd, m_recordingImage);
	m_renderingSession->applyDynamicState(cmd, m_globalDescriptorSet);
}

void VulkanRenderer::bindDeferredGlobalDescriptors() {
	std::vector<vk::WriteDescriptorSet> writes;
	std::vector<vk::DescriptorImageInfo> infos;
	writes.reserve(VulkanRenderTargets::kGBufferCount + 1);
	infos.reserve(VulkanRenderTargets::kGBufferCount + 1);

	for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
		vk::DescriptorImageInfo info{};
		info.sampler = m_renderTargets->gbufferSampler();
		info.imageView = m_renderTargets->gbufferView(i);
		info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		infos.push_back(info);

		vk::WriteDescriptorSet write{};
		write.dstSet = m_globalDescriptorSet;
		write.dstBinding = i;
		write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
		write.descriptorCount = 1;
		write.pImageInfo = &infos.back();
		writes.push_back(write);
	}

	// Depth as extra binding
	vk::DescriptorImageInfo depthInfo{};
	depthInfo.sampler = m_renderTargets->gbufferSampler();
	depthInfo.imageView = m_renderTargets->depthSampledView();
	depthInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
	infos.push_back(depthInfo);

	vk::WriteDescriptorSet depthWrite{};
	depthWrite.dstSet = m_globalDescriptorSet;
	depthWrite.dstBinding = VulkanRenderTargets::kGBufferCount;
	depthWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
	depthWrite.descriptorCount = 1;
	depthWrite.pImageInfo = &infos.back();
	writes.push_back(depthWrite);

	m_vulkanDevice->device().updateDescriptorSets(writes, {});
}

vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const
{
	if (!m_bufferManager) {
		return nullptr;
	}
	return m_bufferManager->getBuffer(handle);
}

vk::Buffer VulkanRenderer::queryModelVertexHeapBuffer() const
{
	if (!m_modelVertexHeapHandle.isValid()) {
		return vk::Buffer{};
	}
	return getBuffer(m_modelVertexHeapHandle);
}

void VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle) {
	// Only store the handle - VkBuffer will be looked up lazily when needed.
	// At registration time, the buffer doesn't exist yet (VulkanBufferManager::createBuffer
	// defers actual VkBuffer creation until updateBufferData is called).
	m_modelVertexHeapHandle = handle;
}

gr_buffer_handle VulkanRenderer::createBuffer(BufferType type, BufferUsageHint usage) {
	if (!m_bufferManager) {
		return gr_buffer_handle::invalid();
	}
	return m_bufferManager->createBuffer(type, usage);
}

void VulkanRenderer::deleteBuffer(gr_buffer_handle handle) {
	if (m_bufferManager) {
		m_bufferManager->deleteBuffer(handle);
	}
}

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void* data) {
	if (m_bufferManager) {
		m_bufferManager->updateBufferData(handle, size, data);
	}
}

void VulkanRenderer::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data) {
	if (m_bufferManager) {
		m_bufferManager->updateBufferDataOffset(handle, offset, size, data);
	}
}

void* VulkanRenderer::mapBuffer(gr_buffer_handle handle) {
	if (!m_bufferManager) {
		return nullptr;
	}
	return m_bufferManager->mapBuffer(handle);
}

void VulkanRenderer::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size) {
	if (m_bufferManager) {
		m_bufferManager->flushMappedBuffer(handle, offset, size);
	}
}

void VulkanRenderer::resizeBuffer(gr_buffer_handle handle, size_t size) {
	if (m_bufferManager) {
		m_bufferManager->resizeBuffer(handle, size);
	}
}

vk::DescriptorImageInfo VulkanRenderer::getTextureDescriptor(int bitmapHandle,
	VulkanFrame& frame,
	vk::CommandBuffer cmd,
	const VulkanTextureManager::SamplerKey& samplerKey) {
	if (!m_textureManager) {
		// Return empty descriptor if texture manager not initialized
		vk::DescriptorImageInfo empty{};
		return empty;
	}

	// Query descriptor - this never throws and always returns a valid descriptor (possibly fallback)
	auto query = m_textureManager->queryDescriptor(bitmapHandle, samplerKey);
	return query.info;
}

void VulkanRenderer::setModelUniformBinding(VulkanFrame& frame,
	gr_buffer_handle handle,
	size_t offset,
	size_t size) {
	const auto alignment = getMinUniformOffsetAlignment();
	Assertion(offset <= std::numeric_limits<uint32_t>::max(),
		"Model uniform offset %zu exceeds uint32_t range", offset);
	const auto dynOffset = static_cast<uint32_t>(offset);

	Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
	Assertion((dynOffset % alignment) == 0,
		"Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
	Assertion(size >= sizeof(model_uniform_data),
		"Model uniform size %zu is smaller than sizeof(model_uniform_data) %zu",
		size, sizeof(model_uniform_data));

	Assertion(frame.modelDescriptorSet, "Model descriptor set must be allocated before binding uniform buffer");
	Assertion(handle.isValid(), "Invalid model uniform buffer handle");

	if (frame.modelUniformState.bufferHandle != handle) {
		vk::Buffer vkBuffer = getBuffer(handle);
		Assertion(vkBuffer, "Failed to resolve Vulkan buffer for handle %d", handle.value());

		vk::DescriptorBufferInfo info{};
		info.buffer = vkBuffer;
		info.offset = 0;
		info.range = sizeof(model_uniform_data);

		vk::WriteDescriptorSet write{};
		write.dstSet = frame.modelDescriptorSet;
		write.dstBinding = 2;
		write.dstArrayElement = 0;
		write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
		write.descriptorCount = 1;
		write.pBufferInfo = &info;

		m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);

		frame.modelUniformState.bufferHandle = handle;
	}

	frame.modelUniformState.dynamicOffset = dynOffset;
}

void VulkanRenderer::setSceneUniformBinding(VulkanFrame& frame,
	gr_buffer_handle handle,
	size_t offset,
	size_t size) {
	// For now, we just track the state in the frame.
	// In the future, this will update a descriptor set for the scene/view block (binding 6).
	// Currently, the engine binds this, but the shaders might not use it via a dedicated set yet.
	// We store it so it's available when we add the descriptor wiring.

	const auto alignment = getMinUniformOffsetAlignment();
	Assertion(offset <= std::numeric_limits<uint32_t>::max(),
		"Scene uniform offset %zu exceeds uint32_t range", offset);
	const auto dynOffset = static_cast<uint32_t>(offset);

	Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
	Assertion((dynOffset % alignment) == 0,
		"Scene uniform offset %u is not aligned to %zu", dynOffset, alignment);

	frame.sceneUniformState.bufferHandle = handle;
	frame.sceneUniformState.dynamicOffset = dynOffset;
}

void VulkanRenderer::updateModelDescriptors(vk::DescriptorSet set,
	vk::Buffer vertexBuffer,
	const std::vector<std::pair<uint32_t, int>>& textures,
	VulkanFrame& frame,
	vk::CommandBuffer cmd) {
	std::vector<vk::WriteDescriptorSet> writes;
	writes.reserve(textures.size() + 1);

	// Binding 0: Vertex heap SSBO (required for per-draw descriptor sets)
	// Look up buffer lazily - it may not exist at registration time
	vk::Buffer modelVertexHeapBuffer = queryModelVertexHeapBuffer();
	Assertion(static_cast<VkBuffer>(modelVertexHeapBuffer) != VK_NULL_HANDLE,
		"Model vertex heap buffer not available (handle=%d)", m_modelVertexHeapHandle.value());

	vk::DescriptorBufferInfo heapInfo{};
	heapInfo.buffer = modelVertexHeapBuffer;
	heapInfo.offset = 0;
	heapInfo.range = VK_WHOLE_SIZE;

	vk::WriteDescriptorSet heapWrite{};
	heapWrite.dstSet = set;
	heapWrite.dstBinding = 0;
	heapWrite.dstArrayElement = 0;
	heapWrite.descriptorCount = 1;
	heapWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
	heapWrite.pBufferInfo = &heapInfo;
	writes.push_back(heapWrite);

	// Binding 1: Textures
	std::vector<vk::DescriptorImageInfo> imageInfos;
	imageInfos.reserve(textures.size());
	for (const auto& [arrayIndex, handle] : textures) {
		VulkanTextureManager::SamplerKey samplerKey{};
		samplerKey.address = vk::SamplerAddressMode::eRepeat;
		samplerKey.filter = vk::Filter::eLinear;

		vk::DescriptorImageInfo info = getTextureDescriptor(handle, frame, cmd, samplerKey);
		imageInfos.push_back(info);
		writes.push_back({set, 1, arrayIndex, 1, vk::DescriptorType::eCombinedImageSampler,
			&imageInfos.back(), nullptr, nullptr});
	}

	m_vulkanDevice->device().updateDescriptorSets(writes, {});
}

void VulkanRenderer::beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer) {
	// Precondition: vertexHeapBuffer is valid (caller checked)
	Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
		"beginModelDescriptorSync called with null vertexHeapBuffer");

	// frameIndex MUST be ring index [0, FramesInFlight)
	Assertion(frameIndex < kFramesInFlight,
		"Invalid frame index %u (must be 0..%u)", frameIndex, kFramesInFlight - 1);

	// Allocate descriptor set if needed
	if (!frame.modelDescriptorSet) {
		frame.modelDescriptorSet = m_descriptorLayouts->allocateModelDescriptorSet();
		Assertion(frame.modelDescriptorSet, "Failed to allocate model descriptor set");
	}

	// Binding 0: Write vertex heap descriptor (once per frame)
	writeVertexHeapDescriptor(frame, vertexHeapBuffer);

	// Binding 1: Write all texture descriptors for RESIDENT textures
	// Note: We write all resident textures every frame. In the future, this could be optimized
	// to track dirty slots, but for now we keep it simple and write everything.
	for (auto& [handle, record] : m_textureManager->allTextures()) {
		if (record.state != VulkanTextureManager::TextureState::Resident) {
			continue;
		}

		auto& state = record.bindingState;
		writeTextureDescriptor(frame.modelDescriptorSet, state.arrayIndex, handle);
	}

	// Write fallback descriptor into retired slots
	// This ensures any stale references sample black instead of destroyed image
	for (uint32_t slot : m_textureManager->getRetiredSlots()) {
		writeFallbackDescriptor(frame.modelDescriptorSet, slot);
	}

	// Clear retired slots once all frames have been updated
	m_textureManager->clearRetiredSlotsIfAllFramesUpdated(frameIndex);
}

void VulkanRenderer::writeVertexHeapDescriptor(VulkanFrame& frame, vk::Buffer vertexHeapBuffer) {
	// Precondition: vertexHeapBuffer is valid (caller checked)
	Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
		"writeVertexHeapDescriptor called with null vertexHeapBuffer");

	vk::DescriptorBufferInfo info;
	info.buffer = vertexHeapBuffer;
	info.offset = 0;
	info.range = VK_WHOLE_SIZE;

	vk::WriteDescriptorSet write;
	write.dstSet = frame.modelDescriptorSet;
	write.dstBinding = 0;
	write.dstArrayElement = 0;
	write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eStorageBuffer;
	write.pBufferInfo = &info;

	m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
}

void VulkanRenderer::writeTextureDescriptor(vk::DescriptorSet set,
	uint32_t arrayIndex,
	int textureHandle) {
	Assertion(arrayIndex < kMaxBindlessTextures,
		"Texture array index %u out of bounds", arrayIndex);

	VulkanTextureManager::SamplerKey samplerKey{};
	samplerKey.address = vk::SamplerAddressMode::eRepeat;
	samplerKey.filter = vk::Filter::eLinear;

	vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(
		textureHandle, samplerKey);

	Assertion(info.imageView, "Texture %d must be resident when writing descriptor", textureHandle);

	vk::WriteDescriptorSet write;
	write.dstSet = set;
	write.dstBinding = 1;
	write.dstArrayElement = arrayIndex;
	write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo = &info;

	m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
}

void VulkanRenderer::writeFallbackDescriptor(vk::DescriptorSet set, uint32_t arrayIndex) {
	Assertion(arrayIndex < kMaxBindlessTextures,
		"Fallback slot %u out of bounds", arrayIndex);

	// Use the fallback texture (black 1x1, initialized at startup)
	int fallbackHandle = m_textureManager->getFallbackTextureHandle();
	Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");

	VulkanTextureManager::SamplerKey samplerKey{};
	samplerKey.address = vk::SamplerAddressMode::eRepeat;
	samplerKey.filter = vk::Filter::eNearest;

	vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(
		fallbackHandle, samplerKey);

	Assertion(info.imageView, "Fallback texture must be resident");

	vk::WriteDescriptorSet write;
	write.dstSet = set;
	write.dstBinding = 1;
	write.dstArrayElement = arrayIndex; // THE ORIGINAL SLOT, not 0
	write.descriptorCount = 1;
	write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
	write.pImageInfo = &info;

	m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
}

int VulkanRenderer::preloadTexture(int bitmapHandle, bool isAABitmap) {
	if (m_textureManager && bitmapHandle >= 0) {
		return m_textureManager->preloadTexture(bitmapHandle, isAABitmap) ? 1 : 0;
	}
	return 0;
}

void VulkanRenderer::immediateSubmit(const std::function<void(vk::CommandBuffer)>& recorder) {
	vk::CommandBufferAllocateInfo allocInfo;
	allocInfo.level = vk::CommandBufferLevel::ePrimary;
	allocInfo.commandPool = m_uploadCommandPool.get();
	allocInfo.commandBufferCount = 1;

	auto cmdBuffers = m_vulkanDevice->device().allocateCommandBuffersUnique(allocInfo);
	auto& cmdBuffer = cmdBuffers[0];

	vk::CommandBufferBeginInfo beginInfo;
	beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	cmdBuffer->begin(beginInfo);

	recorder(cmdBuffer.get());

	cmdBuffer->end();

	vk::SubmitInfo submitInfo;
	submitInfo.commandBufferCount = 1;
	auto cmdBufferHandle = cmdBuffer.get();
	submitInfo.pCommandBuffers = &cmdBufferHandle;

	m_vulkanDevice->graphicsQueue().submit(submitInfo, nullptr);
	m_vulkanDevice->graphicsQueue().waitIdle();
}

void VulkanRenderer::shutdown() {
	if (!m_vulkanDevice) {
		return; // Already shut down or never initialized
	}

	m_vulkanDevice->device().waitIdle();

	// Clear non-owned handles
	// All RAII members are cleaned up by destructors in reverse declaration order

	// VulkanDevice shutdown is handled by its destructor
}

void VulkanRenderer::setClearColor(int r, int g, int b) {
	m_renderingSession->setClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

int VulkanRenderer::setCullMode(int cull) {
	switch (cull) {
	case 0:
		m_renderingSession->setCullMode(vk::CullModeFlagBits::eNone);
		break;
	case 1:
		m_renderingSession->setCullMode(vk::CullModeFlagBits::eBack);
		break;
	case 2:
		m_renderingSession->setCullMode(vk::CullModeFlagBits::eFront);
		break;
	default:
		return 0;
	}
	return 1;
}

int VulkanRenderer::setZbufferMode(int mode) {
	switch (mode) {
	case 0: // ZBUFFER_TYPE_NONE
		m_renderingSession->setDepthTest(false);
		m_renderingSession->setDepthWrite(false);
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_NONE;
		break;
	case 1: // ZBUFFER_TYPE_READ
		m_renderingSession->setDepthTest(true);
		m_renderingSession->setDepthWrite(false);
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_READ;
		break;
	case 2: // ZBUFFER_TYPE_WRITE
		m_renderingSession->setDepthTest(false);
		m_renderingSession->setDepthWrite(true);
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_WRITE;
		break;
	case 3: // ZBUFFER_TYPE_FULL
		m_renderingSession->setDepthTest(true);
		m_renderingSession->setDepthWrite(true);
		m_zbufferMode = gr_zbuffer_type::ZBUFFER_TYPE_FULL;
		break;
	default:
		return 0;
	}
	return 1;
}

int VulkanRenderer::getZbufferMode() const
{
	return static_cast<int>(m_zbufferMode);
}

void VulkanRenderer::requestClear() {
	m_renderingSession->requestClear();
}

} // namespace vulkan
} // namespace graphics
