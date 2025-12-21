
#include "VulkanRenderer.h"
#include "VulkanGraphics.h"
#include "VulkanConstants.h"
#include "VulkanFrameFlow.h"
#include "VulkanTextureBindings.h"
#include "graphics/util/uniform_structs.h"

#include "def_files/def_files.h"
#include "graphics/2d.h"
#include "graphics/matrix.h"
#include "VulkanModelValidation.h"
#include "VulkanDebug.h"
#include "osapi/outwnd.h"
#include "bmpman/bmpman.h"
#include "cmdline/cmdline.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::initialize() {
  // Initialize the device layer (instance, surface, physical device, logical device, swapchain)
  if (!m_vulkanDevice->initialize()) {
    return false;
  }

  // Create renderer-specific resources
  createDescriptorResources();
  createRenderTargets();
  createRenderingSession();
  createUploadCommandPool();
  createSubmitTimelineSemaphore();
  createFrames();

  // Initialize managers using VulkanDevice handles
  const SCP_string shaderRoot = "code/graphics/shaders/compiled";
  m_shaderManager = std::make_unique<VulkanShaderManager>(m_vulkanDevice->device(), shaderRoot);

  m_pipelineManager = std::make_unique<VulkanPipelineManager>(m_vulkanDevice->device(),
    m_descriptorLayouts->pipelineLayout(),
    m_descriptorLayouts->modelPipelineLayout(),
    m_descriptorLayouts->deferredPipelineLayout(),
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
  m_textureBindings = std::make_unique<VulkanTextureBindings>(*m_textureManager);
  m_textureUploader = std::make_unique<VulkanTextureUploader>(*m_textureManager);

  createDeferredLightingResources();

  m_inFlightFrames.clear();

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
  m_availableFrames.clear();
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();
    Assertion(modelSet, "Failed to allocate model descriptor set for frame %zu", i);

    m_frames[i] = std::make_unique<VulkanFrame>(
      m_vulkanDevice->device(),
      static_cast<uint32_t>(i),
      m_vulkanDevice->graphicsQueueIndex(),
      m_vulkanDevice->memoryProperties(),
      UNIFORM_RING_SIZE,
      props.limits.minUniformBufferOffsetAlignment,
      VERTEX_RING_SIZE,
      m_vulkanDevice->vertexBufferAlignment(),
      STAGING_RING_SIZE,
      props.limits.optimalBufferCopyOffsetAlignment,
      modelSet);

    // Newly created frames haven't been submitted yet; completedSerial is whatever we last observed.
    m_availableFrames.push_back(AvailableFrame{ m_frames[i].get(), m_completedSerial });
  }
}

void VulkanRenderer::createRenderTargets() {
  m_renderTargets = std::make_unique<VulkanRenderTargets>(*m_vulkanDevice);
  m_renderTargets->create(m_vulkanDevice->swapchainExtent());
}

void VulkanRenderer::createRenderingSession() {
  m_renderingSession = std::make_unique<VulkanRenderingSession>(
    *m_vulkanDevice, *m_renderTargets);
}

void VulkanRenderer::createUploadCommandPool() {
  vk::CommandPoolCreateInfo poolInfo;
  poolInfo.queueFamilyIndex = m_vulkanDevice->graphicsQueueIndex();
  poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
  m_uploadCommandPool = m_vulkanDevice->device().createCommandPoolUnique(poolInfo);
}

void VulkanRenderer::createSubmitTimelineSemaphore() {
  vk::SemaphoreTypeCreateInfo timelineType;
  timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
  timelineType.initialValue = 0;

  vk::SemaphoreCreateInfo semaphoreInfo;
  semaphoreInfo.pNext = &timelineType;
  m_submitTimeline = m_vulkanDevice->device().createSemaphoreUnique(semaphoreInfo);
  Assertion(m_submitTimeline, "Failed to create submit timeline semaphore");
}

uint64_t VulkanRenderer::queryCompletedSerial() const
{
  if (!m_submitTimeline) {
    return m_completedSerial;
  }

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  auto rv = m_vulkanDevice->device().getSemaphoreCounterValue(m_submitTimeline.get());
  if (rv.result != vk::Result::eSuccess) {
    return m_completedSerial;
  }
  return rv.value;
#else
  return m_vulkanDevice->device().getSemaphoreCounterValue(m_submitTimeline.get());
#endif
}

void VulkanRenderer::maybeRunVulkanStress()
{
  if (!Cmdline_vk_stress) {
    return;
  }

  Assertion(m_bufferManager != nullptr, "Vulkan stress mode requires an initialized buffer manager");

  constexpr size_t kScratchSize = 64 * 1024;
  constexpr size_t kBufferCount = 64;
  constexpr size_t kOpsPerFrame = 8;
  constexpr size_t kMinUpdateSize = 256;

  if (m_stressScratch.empty()) {
    m_stressScratch.resize(kScratchSize, 0xA5);
  }
  if (m_stressBuffers.empty()) {
    m_stressBuffers.reserve(kBufferCount);
    for (size_t i = 0; i < kBufferCount; ++i) {
      const BufferType type = (i % 3 == 0) ? BufferType::Vertex
                             : (i % 3 == 1) ? BufferType::Index
                                            : BufferType::Uniform;
      m_stressBuffers.push_back(m_bufferManager->createBuffer(type, BufferUsageHint::Dynamic));
    }
  }

  // Bounded churn: resize/update a subset each frame, periodically delete to exercise deferred releases.
  const size_t count = m_stressBuffers.size();
  const size_t maxUpdate = std::max(kMinUpdateSize + 1, m_stressScratch.size());
  for (size_t op = 0; op < kOpsPerFrame; ++op) {
    const size_t idx = (static_cast<size_t>(m_frameCounter) * 131u + op * 17u) % count;
    const size_t size = kMinUpdateSize +
                        ((static_cast<size_t>(m_frameCounter) * 4099u + idx * 97u) % (maxUpdate - kMinUpdateSize));
    m_bufferManager->updateBufferData(m_stressBuffers[idx], size, m_stressScratch.data());
  }

  if ((m_frameCounter % 4u) == 0u) {
    const size_t idx = (static_cast<size_t>(m_frameCounter) / 4u) % count;
    m_bufferManager->deleteBuffer(m_stressBuffers[idx]);
  }
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

uint32_t VulkanRenderer::acquireImageOrThrow(VulkanFrame& frame) {
  auto result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());

  if (result.needsRecreate) {
    const auto extent = m_vulkanDevice->swapchainExtent();
    if (!m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
      throw std::runtime_error("acquireImageOrThrow: swapchain recreation failed");
    }
    // Recreate render targets that depend on swapchain size
    m_renderTargets->resize(m_vulkanDevice->swapchainExtent());

    // Retry acquire after successful recreation
    result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());
    if (!result.success) {
      throw std::runtime_error("acquireImageOrThrow: acquire failed after swapchain recreation");
    }
    return result.imageIndex;
  }

  if (!result.success) {
    throw std::runtime_error("acquireImageOrThrow: acquire failed");
  }

  return result.imageIndex;
}

void VulkanRenderer::beginFrame(VulkanFrame& frame, uint32_t imageIndex) {
  Assertion(m_deferredBoundaryState == DeferredBoundaryState::Idle,
    "New frame started while deferred boundary state was not idle");
  m_deferredBoundaryState = DeferredBoundaryState::Idle;
  Assertion(m_renderingSession && !m_renderingSession->renderingActive(),
    "beginFrame called while rendering is still active (RenderScope not destroyed)");

  // Reset per-frame uniform bindings
  frame.resetPerFrameBindings();

  vk::CommandBuffer cmd = frame.commandBuffer();

  vk::CommandBufferBeginInfo beginInfo;
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
  cmd.begin(beginInfo);

  // Collect serial-gated deferred releases opportunistically at a known safe point.
  m_completedSerial = std::max(m_completedSerial, queryCompletedSerial());
  if (m_bufferManager) {
    m_bufferManager->collect(m_completedSerial);
  }
  if (m_textureManager) {
    m_textureManager->collect(m_completedSerial);
  }

  // Upload any pending textures before rendering begins (no render pass active yet).
  // This is the explicit upload flush point - textures requested before rendering starts
  // will be queued and flushed here.
  Assertion(m_textureManager != nullptr, "m_textureManager must be initialized before beginFrame");
  m_textureManager->setSafeRetireSerial(m_submitSerial);
  m_textureManager->setCurrentFrameIndex(m_frameCounter);

  Assertion(m_bufferManager != nullptr, "m_bufferManager must be initialized before beginFrame");
  m_bufferManager->setSafeRetireSerial(m_submitSerial);
  maybeRunVulkanStress();
  Assertion(m_textureUploader != nullptr, "m_textureUploader must be initialized before beginFrame");
  m_textureUploader->flushPendingUploads(frame, cmd, m_frameCounter);

  // Sync model descriptors AFTER upload flush so newly-resident textures are written this frame.
  Assertion(m_modelVertexHeapHandle.isValid(), "Model vertex heap handle must be valid");

  // Ensure vertex heap buffer exists and sync descriptors
  vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
  beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);

  // Setup swapchain/depth barriers and reset render state for the new frame
  m_renderingSession->beginFrame(cmd, imageIndex);
}

void VulkanRenderer::endFrame(graphics::vulkan::RecordingFrame& rec) {
  vk::CommandBuffer cmd = rec.cmd();

  // Terminate any active rendering and transition swapchain for present
  m_renderingSession->endFrame(cmd, rec.imageIndex);

  cmd.end();
}

graphics::vulkan::SubmitInfo VulkanRenderer::submitRecordedFrame(graphics::vulkan::RecordingFrame& rec) {
  VulkanFrame& frame = rec.ref();
  uint32_t imageIndex = rec.imageIndex;
  vk::CommandBufferSubmitInfo cmdInfo;
  cmdInfo.commandBuffer = frame.commandBuffer();

  // Fence must be unsignaled when used for submission.
  vk::Fence fence = frame.inflightFence();
  auto resetResult = m_vulkanDevice->device().resetFences(1, &fence);
  if (resetResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to reset fence for Vulkan frame submission");
  }

  vk::SemaphoreSubmitInfo waitSemaphore;
  waitSemaphore.semaphore = frame.imageAvailable();
  waitSemaphore.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

  vk::SemaphoreSubmitInfo signalSemaphores[2];
  signalSemaphores[0].semaphore = frame.renderFinished();
  signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

  signalSemaphores[1].semaphore = m_submitTimeline.get();
  signalSemaphores[1].value = m_submitSerial + 1; // will become submitSerial below
  signalSemaphores[1].stageMask = vk::PipelineStageFlagBits2::eAllCommands;

  vk::SubmitInfo2 submitInfo;
  submitInfo.waitSemaphoreInfoCount = 1;
  submitInfo.pWaitSemaphoreInfos = &waitSemaphore;
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;
  submitInfo.signalSemaphoreInfoCount = 2;
  submitInfo.pSignalSemaphoreInfos = signalSemaphores;

  const uint64_t submitSerial = ++m_submitSerial;
  signalSemaphores[1].value = submitSerial;
  if (m_textureManager) {
    m_textureManager->setSafeRetireSerial(m_submitSerial);
  }
  if (m_bufferManager) {
    m_bufferManager->setSafeRetireSerial(m_submitSerial);
  }
  const uint64_t timelineValue = submitSerial;

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
#else
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
#endif

  // Present the frame
  auto presentResult = m_vulkanDevice->present(frame.renderFinished(), imageIndex);

  if (presentResult.needsRecreate) {
    const auto extent = m_vulkanDevice->swapchainExtent();
    if (m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
      m_renderTargets->resize(m_vulkanDevice->swapchainExtent());
    }
  }

  graphics::vulkan::SubmitInfo info{};
  info.imageIndex = imageIndex;
  info.frameIndex = frame.frameIndex();
  info.serial = submitSerial;
  info.timeline = timelineValue;
  return info;
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


void VulkanRenderer::prepareFrameForReuse(VulkanFrame& frame, uint64_t completedSerial)
{
  Assertion(m_bufferManager != nullptr, "m_bufferManager must be initialized");
  m_bufferManager->collect(completedSerial);

  Assertion(m_textureManager != nullptr, "m_textureManager must be initialized");
  m_textureManager->collect(completedSerial);

  frame.reset();
}

void VulkanRenderer::recycleOneInFlight()
{
  graphics::vulkan::InFlightFrame inflight = std::move(m_inFlightFrames.front());
  m_inFlightFrames.pop_front();

  VulkanFrame& f = inflight.ref();

  // We recycle in FIFO order, so submission serials should complete monotonically.
  f.wait_for_gpu();
  const uint64_t completed = queryCompletedSerial();
  m_completedSerial = std::max(m_completedSerial, completed);
  Assertion(m_completedSerial >= inflight.submit.serial,
    "Completed serial (%llu) must be >= recycled submission serial (%llu)",
    static_cast<unsigned long long>(m_completedSerial),
    static_cast<unsigned long long>(inflight.submit.serial));
  prepareFrameForReuse(f, m_completedSerial);

  m_availableFrames.push_back(AvailableFrame{ &f, m_completedSerial });
}

VulkanRenderer::AvailableFrame VulkanRenderer::acquireAvailableFrame()
{
  while (m_availableFrames.empty()) {
    recycleOneInFlight();
  }

  AvailableFrame af = m_availableFrames.front();
  m_availableFrames.pop_front();
  return af;
}

graphics::vulkan::RecordingFrame VulkanRenderer::beginRecording()
{
  auto af = acquireAvailableFrame();

  const uint32_t imageIndex = acquireImageOrThrow(*af.frame);
  beginFrame(*af.frame, imageIndex);

  return graphics::vulkan::RecordingFrame{ *af.frame, imageIndex };
}

graphics::vulkan::RecordingFrame VulkanRenderer::advanceFrame(graphics::vulkan::RecordingFrame prev)
{
  endFrame(prev);
  auto submit = submitRecordedFrame(prev);

  m_inFlightFrames.emplace_back(prev.ref(), submit);

  logFrameCounters();
  m_frameModelDraws = 0;
  m_framePrimDraws = 0;

  return beginRecording();
}

VulkanRenderingSession::RenderScope VulkanRenderer::ensureRenderingStarted(graphics::vulkan::RecordingFrame& rec) {
  return m_renderingSession->beginRendering(rec.cmd(), rec.imageIndex);
}

void VulkanRenderer::beginDeferredLighting(graphics::vulkan::RecordingFrame& rec, bool clearNonColorBufs)
{
  m_renderingSession->beginDeferredPass(clearNonColorBufs);
  // Begin dynamic rendering immediately so clears execute even if no geometry draws occur.
  auto scope = m_renderingSession->beginRendering(rec.cmd(), rec.imageIndex);
}

void VulkanRenderer::endDeferredGeometry(vk::CommandBuffer cmd)
{
  m_renderingSession->endDeferredGeometry(cmd);
}

void VulkanRenderer::setPendingRenderTargetSwapchain()
{
  m_renderingSession->requestSwapchainTarget();
}

void VulkanRenderer::deferredLightingBegin(graphics::vulkan::RecordingFrame& rec, bool clearNonColorBufs)
{
  Assertion(m_deferredBoundaryState == DeferredBoundaryState::Idle,
    "deferredLightingBegin called while deferred boundary state was not idle");
  
  beginDeferredLighting(rec, clearNonColorBufs);
  m_deferredBoundaryState = DeferredBoundaryState::InGeometry;
}

void VulkanRenderer::deferredLightingEnd(graphics::vulkan::RecordingFrame& rec)
{
  Assertion(m_deferredBoundaryState == DeferredBoundaryState::InGeometry,
    "deferredLightingEnd called while not in geometry state");
  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "deferredLightingEnd called with null command buffer");

  endDeferredGeometry(cmd);
  m_deferredBoundaryState = DeferredBoundaryState::AwaitFinish;
}

void VulkanRenderer::deferredLightingFinish(graphics::vulkan::RecordingFrame& rec, const vk::Rect2D& restoreScissor)
{
  Assertion(m_deferredBoundaryState == DeferredBoundaryState::AwaitFinish,
    "deferredLightingFinish called while not awaiting finish");

  bindDeferredGlobalDescriptors();
  recordDeferredLighting(rec);

  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "deferredLightingFinish called with null command buffer");
  cmd.setScissor(0, 1, &restoreScissor);

  setPendingRenderTargetSwapchain();
  m_deferredBoundaryState = DeferredBoundaryState::Idle;
}

void VulkanRenderer::bindDeferredGlobalDescriptors() {
  std::vector<vk::WriteDescriptorSet> writes;
  std::vector<vk::DescriptorImageInfo> infos;
  writes.reserve(6);
  infos.reserve(6);



  // G-buffer 0..2
  for (uint32_t i = 0; i < 3; ++i) {
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

  // Depth (binding 3) - uses nearest-filter sampler (linear often unsupported for depth)
    vk::DescriptorImageInfo depthInfo{};
    depthInfo.sampler = m_renderTargets->depthSampler();
    depthInfo.imageView = m_renderTargets->depthSampledView();
    depthInfo.imageLayout = m_renderTargets->depthReadLayout();
    infos.push_back(depthInfo);

  vk::WriteDescriptorSet depthWrite{};
  depthWrite.dstSet = m_globalDescriptorSet;
  depthWrite.dstBinding = 3;
  depthWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  depthWrite.descriptorCount = 1;
  depthWrite.pImageInfo = &infos.back();
  writes.push_back(depthWrite);

  // Specular (binding 4): G-buffer attachment 3
  {
    vk::DescriptorImageInfo info{};
    info.sampler = m_renderTargets->gbufferSampler();
    info.imageView = m_renderTargets->gbufferView(3);
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    

    
    infos.push_back(info);

    vk::WriteDescriptorSet write{};
    write.dstSet = m_globalDescriptorSet;
    write.dstBinding = 4;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &infos.back();
    writes.push_back(write);
  }

  // Emissive (binding 5): G-buffer attachment 4
  {
    vk::DescriptorImageInfo info{};
    info.sampler = m_renderTargets->gbufferSampler();
    info.imageView = m_renderTargets->gbufferView(4);
    info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    

    
    infos.push_back(info);

    vk::WriteDescriptorSet write{};
    write.dstSet = m_globalDescriptorSet;
    write.dstBinding = 5;
    write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    write.descriptorCount = 1;
    write.pImageInfo = &infos.back();
    writes.push_back(write);
  }

  m_vulkanDevice->device().updateDescriptorSets(writes, {});
  

}

vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const
{
  Assertion(m_bufferManager != nullptr, "getBuffer called before buffer manager initialization");
  return m_bufferManager->getBuffer(handle);
}

vk::Buffer VulkanRenderer::queryModelVertexHeapBuffer() const
{
  Assertion(m_modelVertexHeapHandle.isValid(),
    "queryModelVertexHeapBuffer called without a valid model vertex heap handle");
  return getBuffer(m_modelVertexHeapHandle);
}

void VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle) {
  // Only store the handle - VkBuffer will be looked up lazily when needed.
  // At registration time, the buffer doesn't exist yet (VulkanBufferManager::createBuffer
  // defers actual VkBuffer creation until updateBufferData is called).
  m_modelVertexHeapHandle = handle;
}

gr_buffer_handle VulkanRenderer::createBuffer(BufferType type, BufferUsageHint usage) {
  Assertion(m_bufferManager != nullptr, "createBuffer called before buffer manager initialization");
  return m_bufferManager->createBuffer(type, usage);
}

void VulkanRenderer::deleteBuffer(gr_buffer_handle handle) {
  Assertion(m_bufferManager != nullptr, "deleteBuffer called before buffer manager initialization");
  m_bufferManager->deleteBuffer(handle);
}

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void* data) {
  Assertion(m_bufferManager != nullptr, "updateBufferData called before buffer manager initialization");
  m_bufferManager->updateBufferData(handle, size, data);
}

void VulkanRenderer::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data) {
  Assertion(m_bufferManager != nullptr, "updateBufferDataOffset called before buffer manager initialization");
  m_bufferManager->updateBufferDataOffset(handle, offset, size, data);
}

void* VulkanRenderer::mapBuffer(gr_buffer_handle handle) {
  Assertion(m_bufferManager != nullptr, "mapBuffer called before buffer manager initialization");
  return m_bufferManager->mapBuffer(handle);
}

void VulkanRenderer::flushMappedBuffer(gr_buffer_handle handle, size_t offset, size_t size) {
  Assertion(m_bufferManager != nullptr, "flushMappedBuffer called before buffer manager initialization");
  m_bufferManager->flushMappedBuffer(handle, offset, size);
}

void VulkanRenderer::resizeBuffer(gr_buffer_handle handle, size_t size) {
  Assertion(m_bufferManager != nullptr, "resizeBuffer called before buffer manager initialization");
  m_bufferManager->resizeBuffer(handle, size);
}

vk::DescriptorImageInfo VulkanRenderer::getTextureDescriptor(int bitmapHandle,
  const VulkanTextureManager::SamplerKey& samplerKey) {
  Assertion(m_textureManager != nullptr, "getTextureDescriptor called before texture manager initialization");
  Assertion(bitmapHandle >= 0, "getTextureDescriptor called with invalid bitmapHandle %d", bitmapHandle);

  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  Assertion(baseFrame >= 0, "Invalid bitmapHandle %d in getTextureDescriptor", bitmapHandle);

  Assertion(m_textureBindings != nullptr, "getTextureDescriptor called before texture bindings initialization");
  return m_textureBindings->descriptor(TextureId(baseFrame), m_frameCounter, samplerKey);
}

vk::DescriptorImageInfo VulkanRenderer::getDefaultTextureDescriptor(const VulkanTextureManager::SamplerKey& samplerKey)
{
  Assertion(m_textureManager != nullptr, "getDefaultTextureDescriptor called before texture manager initialization");

  const int handle = m_textureManager->getDefaultTextureHandle();
  Assertion(handle != -1, "Default texture handle must be initialized");

  auto info = m_textureManager->getTextureDescriptorInfo(handle, samplerKey);
  Assertion(info.imageView, "Default texture must have a valid imageView");
  return info;
}

uint32_t VulkanRenderer::getBindlessTextureIndex(int bitmapHandle)
{
  if (bitmapHandle < 0) {
    return MODEL_OFFSET_ABSENT;
  }

  Assertion(m_textureBindings != nullptr, "getBindlessTextureIndex called before texture bindings initialization");

  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return MODEL_OFFSET_ABSENT;
  }

  return m_textureBindings->bindlessIndex(TextureId(baseFrame));
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

  Assertion(frame.modelDescriptorSet(), "Model descriptor set must be allocated before binding uniform buffer");
  Assertion(handle.isValid(), "Invalid model uniform buffer handle");
  Assertion(m_bufferManager != nullptr, "setModelUniformBinding requires buffer manager");

  vk::Buffer vkBuffer = m_bufferManager->ensureBuffer(
    handle, static_cast<vk::DeviceSize>(offset + sizeof(model_uniform_data)));
  Assertion(vkBuffer, "Failed to resolve Vulkan buffer for handle %d", handle.value());

  // Check if buffer handle changed
  if (frame.modelUniformBinding.bufferHandle != handle) {
    vk::DescriptorBufferInfo info{};
    info.buffer = vkBuffer;
    info.offset = 0;
    info.range = sizeof(model_uniform_data);

    vk::WriteDescriptorSet write{};
    write.dstSet = frame.modelDescriptorSet();
    write.dstBinding = 2;
    write.dstArrayElement = 0;
    write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;

    m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
  }

  frame.modelUniformBinding = DynamicUniformBinding{ handle, dynOffset };
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

  frame.sceneUniformBinding = DynamicUniformBinding{ handle, dynOffset };
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

    (void)frame;
    (void)cmd;
    Assertion(m_textureManager != nullptr, "updateModelDescriptors called before texture manager initialization");
    vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(handle, samplerKey);
    Assertion(info.imageView, "updateModelDescriptors requires resident texture handle=%d", handle);
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
  Assertion(m_bufferManager != nullptr, "beginModelDescriptorSync requires buffer manager");

  // frameIndex MUST be ring index [0, FramesInFlight)
  Assertion(frameIndex < kFramesInFlight,
    "Invalid frame index %u (must be 0..%u)", frameIndex, kFramesInFlight - 1);

  // Descriptor set must be allocated at frame construction (not lazily)
  Assertion(frame.modelDescriptorSet(), "Model descriptor set must be allocated at frame construction");

  // Binding 0: Write vertex heap descriptor (once per frame)
  writeVertexHeapDescriptor(frame, vertexHeapBuffer);

  // Binding 1: Write all texture descriptors for RESIDENT textures
  // Note: We write all resident textures every frame. In the future, this could be optimized
  // to track dirty slots, but for now we keep it simple and write everything.
    uint32_t descriptorCount = 0;
    for (auto& [handle, record] : m_textureManager->allTextures()) {
      if (record.state != VulkanTextureManager::TextureState::Resident) {
        continue;
      }

      auto& state = record.bindingState;
      if (state.arrayIndex == MODEL_OFFSET_ABSENT) {
        continue;
      }
      writeTextureDescriptor(frame.modelDescriptorSet(), state.arrayIndex, handle);
      descriptorCount++;
    }
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
  write.dstSet = frame.modelDescriptorSet();
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
  Assertion(fallbackHandle != -1, "Fallback texture must be initialized");

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

void VulkanRenderer::zbufferClear(int mode) {
  if (mode) {
    // Enable zbuffering + clear
    gr_zbuffering = 1;
    gr_zbuffering_mode = GR_ZBUFF_FULL;
    gr_global_zbuffering = 1;
    m_renderingSession->setDepthTest(true);
    m_renderingSession->setDepthWrite(true);
    m_renderingSession->requestDepthClear();
  } else {
    // Disable zbuffering
    gr_zbuffering = 0;
    gr_zbuffering_mode = GR_ZBUFF_NONE;
    gr_global_zbuffering = 0;
    m_renderingSession->setDepthTest(false);
  }
}

void VulkanRenderer::createDeferredLightingResources() {
  // Fullscreen triangle (covers entire screen with 3 vertices, no clipping)
  // Positions are in clip space: vertex shader passes through directly
  struct FullscreenVertex {
    float x, y, z;
  };
  static const FullscreenVertex fullscreenVerts[] = {
    {-1.0f, -1.0f, 0.0f},
    { 3.0f, -1.0f, 0.0f},
    {-1.0f,  3.0f, 0.0f}
  };

  m_fullscreenMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_fullscreenMesh.vbo, sizeof(fullscreenVerts), fullscreenVerts);
  m_fullscreenMesh.indexCount = 3;

  // Sphere mesh (icosphere-like approximation)
  // Using octahedron subdivided once for reasonable approximation
  std::vector<float> sphereVerts;
  std::vector<uint32_t> sphereIndices;

  // Octahedron base vertices
  const float oct[] = {
     0.0f,  1.0f,  0.0f,  // top
     0.0f, -1.0f,  0.0f,  // bottom
     1.0f,  0.0f,  0.0f,  // +X
    -1.0f,  0.0f,  0.0f,  // -X
     0.0f,  0.0f,  1.0f,  // +Z
     0.0f,  0.0f, -1.0f   // -Z
  };

  // Octahedron faces (8 triangles)
  const uint32_t octFaces[] = {
    0, 4, 2,  0, 2, 5,  0, 5, 3,  0, 3, 4,  // top half
    1, 2, 4,  1, 5, 2,  1, 3, 5,  1, 4, 3   // bottom half
  };

  for (int i = 0; i < 18; i++) {
    sphereVerts.push_back(oct[i]);
  }
  for (int i = 0; i < 24; i++) {
    sphereIndices.push_back(octFaces[i]);
  }

  m_sphereMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_sphereMesh.vbo, sphereVerts.size() * sizeof(float), sphereVerts.data());
  m_sphereMesh.ibo = m_bufferManager->createBuffer(BufferType::Index, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_sphereMesh.ibo, sphereIndices.size() * sizeof(uint32_t), sphereIndices.data());
  m_sphereMesh.indexCount = static_cast<uint32_t>(sphereIndices.size());

  // Cylinder mesh (capped cylinder along -Z axis)
  // The model matrix will position and scale it
  std::vector<float> cylVerts;
  std::vector<uint32_t> cylIndices;

  const int segments = 12;
  const float twoPi = 6.283185307f;

  // Generate ring vertices at z=0 and z=-1
  for (int ring = 0; ring < 2; ++ring) {
    float z = (ring == 0) ? 0.0f : -1.0f;
    for (int i = 0; i < segments; ++i) {
      float angle = twoPi * i / segments;
      cylVerts.push_back(cosf(angle));  // x
      cylVerts.push_back(sinf(angle));  // y
      cylVerts.push_back(z);            // z
    }
  }

  // Center vertices for caps
  uint32_t capTop = static_cast<uint32_t>(cylVerts.size() / 3);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);

  uint32_t capBot = static_cast<uint32_t>(cylVerts.size() / 3);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(0.0f);
  cylVerts.push_back(-1.0f);

  // Side faces (quads as two triangles)
  for (int i = 0; i < segments; ++i) {
    uint32_t i0 = i;
    uint32_t i1 = (i + 1) % segments;
    uint32_t i2 = i + segments;
    uint32_t i3 = ((i + 1) % segments) + segments;

    // Two triangles per quad
    cylIndices.push_back(i0);
    cylIndices.push_back(i2);
    cylIndices.push_back(i1);

    cylIndices.push_back(i1);
    cylIndices.push_back(i2);
    cylIndices.push_back(i3);
  }

  // Top cap (z=0)
  for (int i = 0; i < segments; ++i) {
    cylIndices.push_back(capTop);
    cylIndices.push_back((i + 1) % segments);
    cylIndices.push_back(i);
  }

  // Bottom cap (z=-1)
  for (int i = 0; i < segments; ++i) {
    cylIndices.push_back(capBot);
    cylIndices.push_back(i + segments);
    cylIndices.push_back(((i + 1) % segments) + segments);
  }

  m_cylinderMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_cylinderMesh.vbo, cylVerts.size() * sizeof(float), cylVerts.data());
  m_cylinderMesh.ibo = m_bufferManager->createBuffer(BufferType::Index, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_cylinderMesh.ibo, cylIndices.size() * sizeof(uint32_t), cylIndices.data());
  m_cylinderMesh.indexCount = static_cast<uint32_t>(cylIndices.size());
}

void VulkanRenderer::recordDeferredLighting(graphics::vulkan::RecordingFrame& rec) {
  vk::CommandBuffer cmd = rec.cmd();
  vk::Buffer uniformBuffer = rec.ref().uniformBuffer().buffer();

  // Build lights from engine state
  std::vector<DeferredLight> lights = buildDeferredLights(
    rec.ref(),
    uniformBuffer,
    gr_view_matrix,
    gr_projection_matrix,
    getMinUniformBufferAlignment());
    


    if (lights.empty()) {
      return;
    }

    // Activate swapchain rendering without depth (target set by endDeferredGeometry)
    // ensureRenderingStarted starts the render pass if not already active
  auto renderScope = ensureRenderingStarted(rec);

  // Deferred lighting pass owns full-screen viewport/scissor and disables depth.
  {
    const auto extent = m_vulkanDevice->swapchainExtent();

    vk::Viewport viewport{};
    viewport.x = 0.f;
    viewport.y = static_cast<float>(extent.height);
    viewport.width = static_cast<float>(extent.width);
    viewport.height = -static_cast<float>(extent.height);
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;
    cmd.setViewport(0, 1, &viewport);

    vk::Rect2D scissor{{0, 0}, extent};
    cmd.setScissor(0, 1, &scissor);
    


    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eClockwise); // Matches Y-flipped viewport convention
    cmd.setDepthTestEnable(VK_FALSE);
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setDepthCompareOp(vk::CompareOp::eAlways);
    cmd.setStencilTestEnable(VK_FALSE);
  }

  // Get pipeline and layout for deferred shader
  // TODO: These should be cached, for now we create them on demand
  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_DEFERRED_LIGHTING);

  vertex_layout deferredLayout{};
  deferredLayout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);  // Position only for volume meshes

  // Create pipeline key for deferred lighting
  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_DEFERRED_LIGHTING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(m_vulkanDevice->swapchainFormat());
  key.depth_format = VK_FORMAT_UNDEFINED;  // No depth attachment
  key.color_attachment_count = 1;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = deferredLayout.hash();

  // Ambient pipeline (no blend, overwrites undefined swapchain)
  PipelineKey ambientKey = key;
  ambientKey.blend_mode = ALPHA_BLEND_NONE;

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, deferredLayout);
  vk::Pipeline ambientPipeline = m_pipelineManager->getPipeline(ambientKey, modules, deferredLayout);

  // Prepare draw context
  DeferredDrawContext ctx{};
  ctx.cmd = cmd;
  ctx.layout = m_descriptorLayouts->deferredPipelineLayout();
  ctx.uniformBuffer = uniformBuffer;
  ctx.pipeline = pipeline;
  ctx.ambientPipeline = ambientPipeline;
  ctx.dynamicBlendEnable = m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable;

  // Bind global (set=1) deferred descriptor set using the *deferred* pipeline layout.
  // Binding via the standard pipeline layout is not descriptor-set compatible because set 0 differs.
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                         ctx.layout,
                         1, 1,
                         &m_globalDescriptorSet,
                         0, nullptr);
  


  // Get mesh buffers
  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::Buffer sphereVB = m_bufferManager->getBuffer(m_sphereMesh.vbo);
  vk::Buffer sphereIB = m_bufferManager->getBuffer(m_sphereMesh.ibo);
  vk::Buffer cylinderVB = m_bufferManager->getBuffer(m_cylinderMesh.vbo);
  vk::Buffer cylinderIB = m_bufferManager->getBuffer(m_cylinderMesh.ibo);

  // Record each light
  for (const auto& light : lights) {
    std::visit([&](const auto& l) {
      using T = std::decay_t<decltype(l)>;
      if constexpr (std::is_same_v<T, FullscreenLight>) {
        l.record(ctx, fullscreenVB);
      } else if constexpr (std::is_same_v<T, SphereLight>) {
        l.record(ctx, sphereVB, sphereIB, m_sphereMesh.indexCount);
      } else if constexpr (std::is_same_v<T, CylinderLight>) {
        l.record(ctx, cylinderVB, cylinderIB, m_cylinderMesh.indexCount);
      }
    }, light);
  }
  // Note: render pass ends via RAII when target changes or frame ends
}

} // namespace vulkan
} // namespace graphics
