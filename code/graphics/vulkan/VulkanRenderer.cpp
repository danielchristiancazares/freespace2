
#include "VulkanRenderer.h"
#include "VulkanClip.h"
#include "VulkanConstants.h"
#include "VulkanFrameCaps.h"
#include "VulkanFrameFlow.h"
#include "VulkanGraphics.h"
#include "VulkanMovieManager.h"
#include "VulkanTextureBindings.h"
#include "graphics/util/uniform_structs.h"
#include "lighting/lighting_profiles.h"

#include "VulkanDebug.h"
#include "VulkanModelValidation.h"
#include "bmpman/bmpman.h"
#include "cmdline/cmdline.h"
#include "def_files/def_files.h"
#include "freespace.h"
#include "graphics/2d.h"
#include "graphics/matrix.h"
#include "io/timer.h"
#include "lighting/lighting.h"
#include "osapi/outwnd.h"

// Reuse the engine's canonical SMAA lookup textures (precomputed area/search).
// These are tiny immutable resources and are backend-agnostic.
#include "graphics/opengl/SmaaAreaTex.h"
#include "graphics/opengl/SmaaSearchTex.h"

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
    : m_vulkanDevice(std::make_unique<VulkanDevice>(std::move(graphicsOps))) {}

VulkanRenderer::~VulkanRenderer() = default;

bool VulkanRenderer::initialize() {
  // Initialize the device layer (instance, surface, physical device, logical device, swapchain)
  if (!m_vulkanDevice->initialize()) {
    return false;
  }

  createDescriptorResources();
  createRenderTargets();
  createUploadCommandPool();
  createSubmitTimelineSemaphore();
  createFrames();

  const SCP_string shaderRoot = "code/graphics/shaders/compiled";
  m_shaderManager = std::make_unique<VulkanShaderManager>(m_vulkanDevice->device(), shaderRoot);

  m_movieManager = std::make_unique<VulkanMovieManager>(*m_vulkanDevice, *m_shaderManager);
  constexpr uint32_t kMaxMovieTextures = 8;
  m_movieManager->initialize(kMaxMovieTextures);

  m_pipelineManager = std::make_unique<VulkanPipelineManager>(
      m_vulkanDevice->device(), m_descriptorLayouts->pipelineLayout(), m_descriptorLayouts->modelPipelineLayout(),
      m_descriptorLayouts->deferredPipelineLayout(), m_vulkanDevice->pipelineCache(),
      m_vulkanDevice->supportsExtendedDynamicState3(), m_vulkanDevice->extDyn3Caps(),
      m_vulkanDevice->supportsVertexAttributeDivisor(), m_vulkanDevice->features13().dynamicRendering == VK_TRUE);

  m_bufferManager =
      std::make_unique<VulkanBufferManager>(m_vulkanDevice->device(), m_vulkanDevice->memoryProperties(),
                                            m_vulkanDevice->graphicsQueue(), m_vulkanDevice->graphicsQueueIndex());

  m_textureManager =
      std::make_unique<VulkanTextureManager>(m_vulkanDevice->device(), m_vulkanDevice->memoryProperties(),
                                             m_vulkanDevice->graphicsQueue(), m_vulkanDevice->graphicsQueueIndex());
  m_textureBindings = std::make_unique<VulkanTextureBindings>(*m_textureManager);
  m_textureUploader = std::make_unique<VulkanTextureUploader>(*m_textureManager);

  // Rendering session depends on the texture manager for bitmap render targets (RTT).
  createRenderingSession();

  createDeferredLightingResources();
  const InitCtx initCtx{};
  createSmaaLookupTextures(initCtx);

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
  const auto &props = m_vulkanDevice->properties();
  m_availableFrames.clear();
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();
    Assertion(modelSet, "Failed to allocate model descriptor set for frame %zu", i);

    m_frames[i] = std::make_unique<VulkanFrame>(
        m_vulkanDevice->device(), static_cast<uint32_t>(i), m_vulkanDevice->graphicsQueueIndex(),
        m_vulkanDevice->memoryProperties(), UNIFORM_RING_SIZE, props.limits.minUniformBufferOffsetAlignment,
        VERTEX_RING_SIZE, m_vulkanDevice->vertexBufferAlignment(), STAGING_RING_SIZE,
        props.limits.optimalBufferCopyOffsetAlignment, modelSet);

    // Newly created frames haven't been submitted yet; completedSerial is whatever we last observed.
    m_availableFrames.push_back(AvailableFrame{m_frames[i].get(), m_completedSerial});
  }
}

void VulkanRenderer::createRenderTargets() {
  m_renderTargets = std::make_unique<VulkanRenderTargets>(*m_vulkanDevice);
  m_renderTargets->create(m_vulkanDevice->swapchainExtent());
}

void VulkanRenderer::createRenderingSession() {
  Assertion(m_textureManager != nullptr, "createRenderingSession requires a valid texture manager");
  m_renderingSession = std::make_unique<VulkanRenderingSession>(*m_vulkanDevice, *m_renderTargets, *m_textureManager);
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

uint64_t VulkanRenderer::queryCompletedSerial() const {
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

void VulkanRenderer::maybeRunVulkanStress() {
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
      const BufferType type = (i % 3 == 0)   ? BufferType::Vertex
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
    const size_t size =
        kMinUpdateSize + ((static_cast<size_t>(m_frameCounter) * 4099u + idx * 97u) % (maxUpdate - kMinUpdateSize));
    m_bufferManager->updateBufferData(m_stressBuffers[idx], size, m_stressScratch.data());
  }

  if ((m_frameCounter % 4u) == 0u) {
    const size_t idx = (static_cast<size_t>(m_frameCounter) / 4u) % count;
    m_bufferManager->deleteBuffer(m_stressBuffers[idx]);
  }
}

uint32_t VulkanRenderer::acquireImage(VulkanFrame &frame) {
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

uint32_t VulkanRenderer::acquireImageOrThrow(VulkanFrame &frame) {
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

void VulkanRenderer::beginFrame(VulkanFrame &frame, uint32_t imageIndex) {
  Assertion(m_renderingSession != nullptr, "beginFrame requires an active rendering session");

  // Reset per-frame uniform bindings
  frame.resetPerFrameBindings();

  // Scene texture state is strictly per-frame; clear any leaked state at the frame boundary.
  m_sceneTexture.reset();

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
  if (m_movieManager) {
    m_movieManager->collect(m_completedSerial);
  }

  // Upload any pending textures before rendering begins (no render pass active yet).
  // This is the explicit upload flush point - textures requested before rendering starts
  // will be queued and flushed here.
  Assertion(m_textureManager != nullptr, "m_textureManager must be initialized before beginFrame");
  // Resources retired during this frame's recording may still be referenced by the upcoming submission.
  m_textureManager->setSafeRetireSerial(m_submitSerial + 1);
  m_textureManager->setCurrentFrameIndex(m_frameCounter);
  if (m_movieManager) {
    m_movieManager->setSafeRetireSerial(m_submitSerial + 1);
  }

  Assertion(m_bufferManager != nullptr, "m_bufferManager must be initialized before beginFrame");
  m_bufferManager->setSafeRetireSerial(m_submitSerial + 1);
  maybeRunVulkanStress();
  Assertion(m_textureUploader != nullptr, "m_textureUploader must be initialized before beginFrame");
  const UploadCtx uploadCtx{frame, cmd, m_frameCounter};
  m_textureUploader->flushPendingUploads(uploadCtx);

  // Sync model descriptors AFTER upload flush so newly-resident textures are written this frame.
  Assertion(m_modelVertexHeapHandle.isValid(), "Model vertex heap handle must be valid");

  // Ensure vertex heap buffer exists and sync descriptors
  vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
  beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);

  // Setup swapchain/depth barriers and reset render state for the new frame
  m_renderingSession->beginFrame(cmd, imageIndex);
}

void VulkanRenderer::endFrame(graphics::vulkan::RecordingFrame &rec) {
  vk::CommandBuffer cmd = rec.cmd();

  updateSavedScreenCopy(rec);

  // Terminate any active rendering and transition swapchain for present
  m_renderingSession->endFrame(cmd, rec.imageIndex);

  cmd.end();
}

void VulkanRenderer::updateSavedScreenCopy(graphics::vulkan::RecordingFrame &rec) {
  if (!m_renderingSession || !m_textureManager || !m_vulkanDevice) {
    return;
  }

  if ((m_vulkanDevice->swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
    return;
  }

  const auto extent = m_vulkanDevice->swapchainExtent();
  if (extent.width == 0 || extent.height == 0) {
    return;
  }

  if (m_savedScreen.hasHandle()) {
    const auto savedExtent = m_savedScreen.extent();
    if (savedExtent.width != extent.width || savedExtent.height != extent.height) {
      const int handle = m_savedScreen.handle();
      if (handle >= 0) {
        bm_release(handle, 1);
      }
      m_savedScreen.reset();
    }
  }

  if (!m_savedScreen.hasHandle()) {
    const int handle = bm_make_render_target(static_cast<int>(extent.width), static_cast<int>(extent.height),
                                             BMP_FLAG_RENDER_TARGET_DYNAMIC);
    if (handle < 0) {
      return;
    }
    m_savedScreen.setTracking(handle, extent);
  }

  if (m_savedScreen.isFrozen()) {
    return;
  }

  const int handle = m_savedScreen.handle();
  if (handle < 0 || !m_textureManager->hasRenderTarget(handle)) {
    m_savedScreen.reset();
    return;
  }

  vk::CommandBuffer cmd = rec.cmd();
  if (!cmd) {
    return;
  }

  m_renderingSession->captureSwapchainColorToRenderTarget(cmd, rec.imageIndex, handle);
}

graphics::vulkan::SubmitInfo VulkanRenderer::submitRecordedFrame(graphics::vulkan::RecordingFrame &rec) {
  VulkanFrame &frame = rec.ref();
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

void VulkanRenderer::incrementModelDraw() { m_frameModelDraws++; }

void VulkanRenderer::incrementPrimDraw() { m_framePrimDraws++; }

void VulkanRenderer::logFrameCounters() { m_frameCounter++; }

void VulkanRenderer::prepareFrameForReuse(VulkanFrame &frame, uint64_t completedSerial) {
  Assertion(m_bufferManager != nullptr, "m_bufferManager must be initialized");
  m_bufferManager->collect(completedSerial);

  Assertion(m_textureManager != nullptr, "m_textureManager must be initialized");
  m_textureManager->collect(completedSerial);

  frame.reset();
}

void VulkanRenderer::recycleOneInFlight() {
  graphics::vulkan::InFlightFrame inflight = std::move(m_inFlightFrames.front());
  m_inFlightFrames.pop_front();

  VulkanFrame &f = inflight.ref();

  // We recycle in FIFO order, so submission serials should complete monotonically.
  f.wait_for_gpu();
  const uint64_t completed = queryCompletedSerial();
  m_completedSerial = std::max(m_completedSerial, completed);
  Assertion(m_completedSerial >= inflight.submit.serial,
            "Completed serial (%llu) must be >= recycled submission serial (%llu)",
            static_cast<unsigned long long>(m_completedSerial),
            static_cast<unsigned long long>(inflight.submit.serial));
  prepareFrameForReuse(f, m_completedSerial);

  m_availableFrames.push_back(AvailableFrame{&f, m_completedSerial});
}

VulkanRenderer::AvailableFrame VulkanRenderer::acquireAvailableFrame() {
  while (m_availableFrames.empty()) {
    recycleOneInFlight();
  }

  AvailableFrame af = m_availableFrames.front();
  m_availableFrames.pop_front();
  return af;
}

graphics::vulkan::RecordingFrame VulkanRenderer::beginRecording() {
  auto af = acquireAvailableFrame();

  const uint32_t imageIndex = acquireImageOrThrow(*af.frame);
  beginFrame(*af.frame, imageIndex);

  return graphics::vulkan::RecordingFrame{*af.frame, imageIndex};
}

graphics::vulkan::RecordingFrame VulkanRenderer::advanceFrame(graphics::vulkan::RecordingFrame prev) {
  endFrame(prev);
  auto submit = submitRecordedFrame(prev);

  m_inFlightFrames.emplace_back(prev.ref(), submit);

  logFrameCounters();
  m_frameModelDraws = 0;
  m_framePrimDraws = 0;

  return beginRecording();
}

RenderCtx VulkanRenderer::ensureRenderingStarted(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this,
            "ensureRenderingStarted called with FrameCtx from a different VulkanRenderer instance");
  return ensureRenderingStartedRecording(ctx.m_recording);
}

void VulkanRenderer::applySetupFrameDynamicState(const FrameCtx &ctx, const vk::Viewport &viewport,
                                                 const vk::Rect2D &scissor, float lineWidth) {
  Assertion(&ctx.renderer == this,
            "applySetupFrameDynamicState called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  Assertion(cmd, "applySetupFrameDynamicState called with null command buffer");

  cmd.setViewport(0, 1, &viewport);
  cmd.setScissor(0, 1, &scissor);
  cmd.setLineWidth(lineWidth);
}

void VulkanRenderer::pushDebugGroup(const FrameCtx &ctx, const char *name) {
  Assertion(name != nullptr, "pushDebugGroup called with null name");
  Assertion(&ctx.renderer == this, "pushDebugGroup called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  vk::DebugUtilsLabelEXT label{};
  label.pLabelName = name;
  // Default color (white with alpha)
  label.color[0] = 1.0f;
  label.color[1] = 1.0f;
  label.color[2] = 1.0f;
  label.color[3] = 1.0f;

  cmd.beginDebugUtilsLabelEXT(label);
}

void VulkanRenderer::popDebugGroup(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this, "popDebugGroup called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  cmd.endDebugUtilsLabelEXT();
}

RenderCtx VulkanRenderer::ensureRenderingStartedRecording(graphics::vulkan::RecordingFrame &rec) {
  auto info = m_renderingSession->ensureRendering(rec.cmd(), rec.imageIndex);
  return RenderCtx{rec.cmd(), info};
}

void VulkanRenderer::flushQueuedTextureUploads(const FrameCtx &ctx, bool syncModelDescriptors) {
  if (!m_textureUploader || !m_textureManager) {
    return;
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  const bool wasRenderingActive = (m_renderingSession && m_renderingSession->renderingActive());

  // Transfer operations are invalid inside dynamic rendering.
  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  const UploadCtx uploadCtx{ctx.m_recording.ref(), cmd, m_frameCounter};
  m_textureUploader->flushPendingUploads(uploadCtx);

  if (syncModelDescriptors) {
    // Newly resident textures and/or new bindless slot assignments must be visible this frame.
    Assertion(m_bufferManager != nullptr, "flushQueuedTextureUploads requires buffer manager");
    Assertion(m_modelVertexHeapHandle.isValid(), "Model vertex heap handle must be valid");
    vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
    if (static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE) {
      beginModelDescriptorSync(ctx.m_recording.ref(), ctx.m_recording.ref().frameIndex(), vertexHeapBuffer);
    }
  }

  // Restore rendering state if we suspended an active pass; this preserves existing RenderCtx tokens.
  if (wasRenderingActive) {
    (void)ensureRenderingStartedRecording(ctx.m_recording);
  }
}

void VulkanRenderer::beginDeferredLighting(graphics::vulkan::RecordingFrame &rec, bool clearNonColorBufs) {
  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "beginDeferredLighting called with null command buffer");

  // Preserve the current clip scissor across the internal fullscreen copy pass. Model draw paths
  // don't currently set scissor themselves.
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D restoreScissor{};
  restoreScissor.offset = vk::Offset2D{clip.x, clip.y};
  restoreScissor.extent = vk::Extent2D{clip.width, clip.height};

  const bool canCaptureSwapchain =
      (m_vulkanDevice->swapchainUsage() & vk::ImageUsageFlagBits::eTransferSrc) != vk::ImageUsageFlags{};

  const bool sceneHdrTarget = m_renderingSession->targetIsSceneHdr();
  const bool swapchainTarget = m_renderingSession->targetIsSwapchain();

  bool preserveEmissive = false;
  if (sceneHdrTarget) {
    // Scene rendering targets the HDR offscreen image, so preserve pre-deferred content from there (not swapchain).
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSceneHdrToShaderRead(cmd);

    m_renderingSession->requestGBufferEmissiveTarget();
    const auto emissiveRender = ensureRenderingStartedRecording(rec);
    recordPreDeferredSceneHdrCopy(emissiveRender);

    // Restore scissor for subsequent geometry draws.
    cmd.setScissor(0, 1, &restoreScissor);
    preserveEmissive = true;
  } else if (swapchainTarget && canCaptureSwapchain) {
    // End any active swapchain rendering and snapshot the current swapchain image.
    m_renderingSession->captureSwapchainColorToSceneCopy(cmd, rec.imageIndex);

    // Copy the captured scene color into the emissive G-buffer attachment (OpenGL parity).
    m_renderingSession->requestGBufferEmissiveTarget();
    const auto emissiveRender = ensureRenderingStartedRecording(rec);
    recordPreDeferredSceneColorCopy(emissiveRender, rec.imageIndex);

    // Restore scissor for subsequent geometry draws.
    cmd.setScissor(0, 1, &restoreScissor);
    preserveEmissive = true;
  }

  m_renderingSession->beginDeferredPass(clearNonColorBufs, preserveEmissive);
  // Begin dynamic rendering immediately so clears execute even if no geometry draws occur.
  (void)ensureRenderingStartedRecording(rec);
}

void VulkanRenderer::endDeferredGeometry(vk::CommandBuffer cmd) { m_renderingSession->endDeferredGeometry(cmd); }

void VulkanRenderer::setPendingRenderTargetSwapchain() { m_renderingSession->requestSwapchainTarget(); }

void VulkanRenderer::requestMainTargetWithDepth() {
  Assertion(m_renderingSession != nullptr, "requestMainTargetWithDepth called before rendering session initialization");
  if (m_sceneTexture.has_value()) {
    m_renderingSession->requestSceneHdrTarget();
  } else {
    m_renderingSession->requestSwapchainTarget();
  }
}

void VulkanRenderer::beginSceneTexture(const FrameCtx &ctx, bool enableHdrPipeline) {
  Assertion(&ctx.renderer == this, "beginSceneTexture called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "beginSceneTexture called before rendering session initialization");

  // Mirror OpenGL's Scene_framebuffer_in_frame guard: if already active, ignore.
  if (m_sceneTexture.has_value()) {
    return;
  }

  // Boundary state (used by deferred to decide output target).
  m_sceneTexture = SceneTextureState{enableHdrPipeline};

  // Clear the scene target at frame start (color+depth); OpenGL does this in scene_texture_begin.
  m_renderingSession->requestClear();

  // Route subsequent rendering to the scene HDR target; tonemapping becomes passthrough when HDR is disabled.
  m_renderingSession->requestSceneHdrTarget();

  // Begin rendering immediately so the requested clear executes even if the scene draws nothing.
  (void)ensureRenderingStarted(ctx);
}

void VulkanRenderer::copySceneEffectTexture(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this,
            "copySceneEffectTexture called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "copySceneEffectTexture called before rendering session initialization");

  if (!m_sceneTexture.has_value()) {
    // OpenGL is a no-op if not in a scene framebuffer.
    return;
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  m_renderingSession->copySceneHdrToEffect(cmd);
}

void VulkanRenderer::endSceneTexture(const FrameCtx &ctx, bool enablePostProcessing) {
  Assertion(&ctx.renderer == this, "endSceneTexture called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "endSceneTexture called before rendering session initialization");

  if (!m_sceneTexture.has_value()) {
    return;
  }
  const bool hdrEnabled = m_sceneTexture->hdrEnabled;

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  // Preserve scissor across the internal fullscreen passes; UI and some draw paths rely on it.
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D restoreScissor{};
  restoreScissor.offset = vk::Offset2D{clip.x, clip.y};
  restoreScissor.extent = vk::Extent2D{clip.width, clip.height};

  // End any active scene rendering. Post-processing is a chain of fullscreen passes.
  m_renderingSession->suspendRendering();
  m_renderingSession->transitionSceneHdrToShaderRead(cmd);

  VulkanFrame &frame = ctx.frame();

  // Bloom pass (HDR): Scene HDR -> bloom (half-res) -> blur -> additive composite back into Scene HDR.
  if (enablePostProcessing && gr_bloom_intensity() > 0) {
    const auto fullExtent = m_vulkanDevice->swapchainExtent();
    const vk::Extent2D bloomExtent{std::max(1u, fullExtent.width >> 1), std::max(1u, fullExtent.height >> 1)};

    // Bright pass -> bloom[0] mip0 (clear before writing).
    m_renderingSession->requestClear();
    m_renderingSession->requestBloomMipTarget(0, 0);
    auto bright = ensureRenderingStarted(ctx);
    recordBloomBrightPass(bright, frame);

    // Generate mip chain from bright-pass output (required for blur).
    m_renderingSession->suspendRendering();
    generateBloomMipmaps(cmd, 0, bloomExtent);

    // Blur passes: ping-pong between bloom[0] and bloom[1] across mips.
    for (int iteration = 0; iteration < 2; ++iteration) {
      // Vertical: src=0 -> dst=1
      m_renderingSession->transitionBloomToShaderRead(cmd, 0);
      for (uint32_t mip = 0; mip < VulkanRenderTargets::kBloomMipLevels; ++mip) {
        const uint32_t bw = std::max(1u, bloomExtent.width >> mip);
        const uint32_t bh = std::max(1u, bloomExtent.height >> mip);
        m_renderingSession->requestBloomMipTarget(1, mip);
        auto passRender = ensureRenderingStarted(ctx);
        recordBloomBlurPass(passRender, frame, 0, SDR_FLAG_BLUR_VERTICAL, static_cast<int>(mip), bw, bh);
      }
      m_renderingSession->suspendRendering();

      // Horizontal: src=1 -> dst=0
      m_renderingSession->transitionBloomToShaderRead(cmd, 1);
      for (uint32_t mip = 0; mip < VulkanRenderTargets::kBloomMipLevels; ++mip) {
        const uint32_t bw = std::max(1u, bloomExtent.width >> mip);
        const uint32_t bh = std::max(1u, bloomExtent.height >> mip);
        m_renderingSession->requestBloomMipTarget(0, mip);
        auto passRender = ensureRenderingStarted(ctx);
        recordBloomBlurPass(passRender, frame, 1, SDR_FLAG_BLUR_HORIZONTAL, static_cast<int>(mip), bw, bh);
      }
      m_renderingSession->suspendRendering();
    }

    // Composite bloom back into the HDR scene target.
    m_renderingSession->transitionBloomToShaderRead(cmd, 0);
    m_renderingSession->requestSceneHdrNoDepthTarget();
    auto composite = ensureRenderingStarted(ctx);
    recordBloomCompositePass(composite, frame, VulkanRenderTargets::kBloomMipLevels);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSceneHdrToShaderRead(cmd);
  }

  // Tonemapping: Scene HDR -> post LDR (RGBA8).
  m_renderingSession->requestPostLdrTarget();
  auto ldrRender = ensureRenderingStarted(ctx);

  // If the HDR pipeline is disabled, the tonemapper is set to passthrough.
  recordTonemappingToSwapchain(ldrRender, frame, hdrEnabled);

  // End tonemapping pass; subsequent post steps sample the LDR target.
  m_renderingSession->suspendRendering();
  m_renderingSession->transitionPostLdrToShaderRead(cmd);

  vk::DescriptorImageInfo ldrInfo{};
  ldrInfo.sampler = m_renderTargets->postLinearSampler();
  ldrInfo.imageView = m_renderTargets->postLdrView();
  ldrInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
  bool ldrIsSmaaOutput = false;

  // SMAA (LDR): optional edge detection + blending weights + neighborhood blending.
  if (enablePostProcessing && gr_is_smaa_mode(Gr_aa_mode)) {
    Assertion(m_smaaAreaTex.view, "SMAA area texture must be initialized");
    Assertion(m_smaaSearchTex.view, "SMAA search texture must be initialized");

    // Edge detection: postLdr -> smaaEdges
    m_renderingSession->requestSmaaEdgesTarget();
    auto edgeRender = ensureRenderingStarted(ctx);
    recordSmaaEdgePass(edgeRender, frame, ldrInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSmaaEdgesToShaderRead(cmd);

    vk::DescriptorImageInfo edgesInfo{};
    edgesInfo.sampler = m_renderTargets->postLinearSampler();
    edgesInfo.imageView = m_renderTargets->smaaEdgesView();
    edgesInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::DescriptorImageInfo areaInfo{};
    areaInfo.sampler = m_renderTargets->postLinearSampler();
    areaInfo.imageView = m_smaaAreaTex.view.get();
    areaInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    vk::DescriptorImageInfo searchInfo{};
    searchInfo.sampler = m_renderTargets->postLinearSampler();
    searchInfo.imageView = m_smaaSearchTex.view.get();
    searchInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    // Blending weights: edges -> smaaBlend
    m_renderingSession->requestSmaaBlendTarget();
    auto blendRender = ensureRenderingStarted(ctx);
    recordSmaaBlendWeightsPass(blendRender, frame, edgesInfo, areaInfo, searchInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSmaaBlendToShaderRead(cmd);

    vk::DescriptorImageInfo blendInfo{};
    blendInfo.sampler = m_renderTargets->postLinearSampler();
    blendInfo.imageView = m_renderTargets->smaaBlendView();
    blendInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    // Neighborhood blending: postLdr + blend -> smaaOutput
    m_renderingSession->requestSmaaOutputTarget();
    auto nbRender = ensureRenderingStarted(ctx);
    recordSmaaNeighborhoodPass(nbRender, frame, ldrInfo, blendInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionSmaaOutputToShaderRead(cmd);

    // Use SMAA output for final resolve.
    ldrInfo.imageView = m_renderTargets->smaaOutputView();
    ldrIsSmaaOutput = true;
  } else if (enablePostProcessing && gr_is_fxaa_mode(Gr_aa_mode)) {
    // FXAA (LDR): prepass converts RGB -> RGBL, then FXAA writes back into postLdr.
    m_renderingSession->requestPostLuminanceTarget();
    auto pre = ensureRenderingStarted(ctx);
    recordFxaaPrepass(pre, frame, ldrInfo);
    m_renderingSession->suspendRendering();
    m_renderingSession->transitionPostLuminanceToShaderRead(cmd);

    vk::DescriptorImageInfo lumInfo{};
    lumInfo.sampler = m_renderTargets->postLinearSampler();
    lumInfo.imageView = m_renderTargets->postLuminanceView();
    lumInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

    m_renderingSession->requestPostLdrTarget();
    auto fxaa = ensureRenderingStarted(ctx);
    recordFxaaPass(fxaa, frame, lumInfo);

    m_renderingSession->suspendRendering();
    m_renderingSession->transitionPostLdrToShaderRead(cmd);
  }

  // Lightshafts (LDR): additive pass using scene + cockpit depth.
  {
    // These are declared in freespace.cpp and used by OpenGL postprocessing.
    extern float Sun_spot;

    if (enablePostProcessing && !Game_subspace_effect && gr_sunglare_enabled() && gr_lightshafts_enabled() &&
        Sun_spot > 0.0f) {
      const int n_lights = light_get_global_count();
      for (int idx = 0; idx < n_lights; ++idx) {
        vec3d light_dir;
        if (!light_get_global_dir(&light_dir, idx)) {
          continue;
        }
        if (!light_has_glare(idx)) {
          continue;
        }

        const float dot = vm_vec_dot(&light_dir, &Eye_matrix.vec.fvec);
        if (dot <= 0.7f) {
          continue;
        }

        const float x = asinf_safe(vm_vec_dot(&light_dir, &Eye_matrix.vec.rvec)) / PI * 1.5f + 0.5f;
        const float y =
            asinf_safe(vm_vec_dot(&light_dir, &Eye_matrix.vec.uvec)) / PI * 1.5f * gr_screen.clip_aspect + 0.5f;

        graphics::generic_data::lightshaft_data ls{};
        ls.sun_pos.x = x;
        ls.sun_pos.y = y;
        if (graphics::Post_processing_manager != nullptr) {
          const auto &ls_params = graphics::Post_processing_manager->getLightshaftParams();
          ls.density = ls_params.density;
          ls.falloff = ls_params.falloff;
          ls.weight = ls_params.weight;
          ls.intensity = Sun_spot * ls_params.intensity;
          ls.cp_intensity = Sun_spot * ls_params.cpintensity;
          ls.samplenum = ls_params.samplenum;
        } else {
          // Reasonable defaults if the table wasn't loaded.
          ls.density = 0.5f;
          ls.falloff = 1.0f;
          ls.weight = 0.02f;
          ls.intensity = Sun_spot * 0.5f;
          ls.cp_intensity = Sun_spot * 0.5f;
          ls.samplenum = 50;
        }

        // Defensive clamp: avoid pathological stalls/hangs if a table sets an extreme sample count.
        // OpenGL bakes SAMPLE_NUM into the shader at compile time; Vulkan uses a uniform loop bound.
        const int requestedSamples = ls.samplenum;
        CLAMP(ls.samplenum, 1, 128);
        if (ls.samplenum != requestedSamples) {
          mprintf(("Vulkan lightshafts: clamping sample count %d -> %d\n", requestedSamples, ls.samplenum));
        }

        // Depth textures must be shader-readable for sampling.
        m_renderingSession->transitionMainDepthToShaderRead(cmd);
        m_renderingSession->transitionCockpitDepthToShaderRead(cmd);

        // Render into the current LDR buffer (postLdr or SMAA output) with additive blending.
        if (ldrIsSmaaOutput) {
          m_renderingSession->requestSmaaOutputTarget();
        } else {
          m_renderingSession->requestPostLdrTarget();
        }
        auto lsRender = ensureRenderingStarted(ctx);
        recordLightshaftsPass(lsRender, frame, ls);

        m_renderingSession->suspendRendering();
        if (ldrIsSmaaOutput) {
          m_renderingSession->transitionSmaaOutputToShaderRead(cmd);
        } else {
          m_renderingSession->transitionPostLdrToShaderRead(cmd);
        }

        // Only render the first qualifying glare light (OpenGL parity).
        break;
      }
    }
  }

  // Post effects: apply configured post-processing effects to swapchain. If no effects are active, copy the LDR buffer.
  bool doPostEffects = false;
  graphics::generic_data::post_data post{};
  post.timer = i2fl(timer_get_milliseconds() % 100 + 1);
  post.noise_amount = 0.0f;
  // Identity defaults (effects disabled unless explicitly enabled)
  post.saturation = 1.0f;
  post.brightness = 1.0f;
  post.contrast = 1.0f;
  post.film_grain = 0.0f;
  post.tv_stripes = 0.0f;
  post.cutoff = 0.0f;
  post.tint = vmd_zero_vector;
  post.dither = 0.0f;
  post.custom_effect_vec3_a = vmd_zero_vector;
  post.custom_effect_float_a = 0.0f;
  post.custom_effect_vec3_b = vmd_zero_vector;
  post.custom_effect_float_b = 0.0f;

  if (enablePostProcessing && graphics::Post_processing_manager != nullptr) {
    const auto &effects = graphics::Post_processing_manager->getPostEffects();
    for (const auto &eff : effects) {
      // Match OpenGL semantics: effects are only applied when flagged on (always_on OR intensity != default).
      const bool enabled = eff.always_on || eff.intensity != eff.default_intensity;
      if (!enabled) {
        continue;
      }
      doPostEffects = true;

      switch (eff.uniform_type) {
      case graphics::PostEffectUniformType::NoiseAmount:
        post.noise_amount = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Saturation:
        post.saturation = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Brightness:
        post.brightness = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Contrast:
        post.contrast = eff.intensity;
        break;
      case graphics::PostEffectUniformType::FilmGrain:
        post.film_grain = eff.intensity;
        break;
      case graphics::PostEffectUniformType::TvStripes:
        post.tv_stripes = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Cutoff:
        post.cutoff = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Dither:
        post.dither = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Tint:
        post.tint = eff.rgb;
        break;
      case graphics::PostEffectUniformType::CustomEffectVEC3A:
        post.custom_effect_vec3_a = eff.rgb;
        break;
      case graphics::PostEffectUniformType::CustomEffectFloatA:
        post.custom_effect_float_a = eff.intensity;
        break;
      case graphics::PostEffectUniformType::CustomEffectVEC3B:
        post.custom_effect_vec3_b = eff.rgb;
        break;
      case graphics::PostEffectUniformType::CustomEffectFloatB:
        post.custom_effect_float_b = eff.intensity;
        break;
      case graphics::PostEffectUniformType::Invalid:
      default:
        break;
      }
    }
  }

  // Ensure the main scene depth is shader-readable for any custom effects.
  m_renderingSession->transitionMainDepthToShaderRead(cmd);

  vk::DescriptorImageInfo depthInfo{};
  depthInfo.sampler = m_renderTargets->depthSampler();
  depthInfo.imageView = m_renderTargets->depthSampledView();
  depthInfo.imageLayout = m_renderTargets->depthReadLayout();

  // Final: write to swapchain (no depth attachment; post passes may sample depth).
  m_renderingSession->requestSwapchainNoDepthTarget();
  auto swap = ensureRenderingStarted(ctx);
  if (enablePostProcessing && doPostEffects) {
    recordPostEffectsPass(swap, frame, post, ldrInfo, depthInfo);
  } else {
    recordCopyToSwapchain(swap, ldrInfo);
  }

  // Restore clip scissor for any subsequent UI draws.
  cmd.setScissor(0, 1, &restoreScissor);

  // Exit scene texture mode: subsequent UI draws go directly to swapchain.
  m_sceneTexture.reset();
}

DeferredGeometryCtx VulkanRenderer::deferredLightingBegin(graphics::vulkan::RecordingFrame &rec,
                                                          bool clearNonColorBufs) {
  beginDeferredLighting(rec, clearNonColorBufs);
  return DeferredGeometryCtx{m_frameCounter};
}

DeferredLightingCtx VulkanRenderer::deferredLightingEnd(graphics::vulkan::RecordingFrame &rec,
                                                        DeferredGeometryCtx &&geometry) {
  Assertion(geometry.frameIndex == m_frameCounter,
            "deferredLightingEnd called with mismatched frameIndex (got %u, expected %u)", geometry.frameIndex,
            m_frameCounter);
  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "deferredLightingEnd called with null command buffer");

  endDeferredGeometry(cmd);
  if (m_sceneTexture.has_value()) {
    // Deferred lighting output should land in the scene HDR target during scene texture mode.
    m_renderingSession->requestSceneHdrNoDepthTarget();
  }
  return DeferredLightingCtx{m_frameCounter};
}

void VulkanRenderer::deferredLightingFinish(graphics::vulkan::RecordingFrame &rec, DeferredLightingCtx &&lighting,
                                            const vk::Rect2D &restoreScissor) {
  Assertion(lighting.frameIndex == m_frameCounter,
            "deferredLightingFinish called with mismatched frameIndex (got %u, expected %u)", lighting.frameIndex,
            m_frameCounter);

  bindDeferredGlobalDescriptors();
  VulkanFrame &frame = rec.ref();
  vk::Buffer uniformBuffer = frame.uniformBuffer().buffer();

  // Build lights from engine state (boundary: conditionals live here only).
  std::vector<DeferredLight> lights =
      buildDeferredLights(frame, uniformBuffer, gr_view_matrix, gr_projection_matrix, getMinUniformBufferAlignment());

  if (!lights.empty()) {
    // Activate swapchain rendering without depth (target set by endDeferredGeometry).
    auto render = ensureRenderingStartedRecording(rec);
    recordDeferredLighting(render, uniformBuffer, lights);
  }

  vk::CommandBuffer cmd = rec.cmd();
  Assertion(cmd, "deferredLightingFinish called with null command buffer");
  cmd.setScissor(0, 1, &restoreScissor);

  requestMainTargetWithDepth();
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

void VulkanRenderer::recordPreDeferredSceneColorCopy(const RenderCtx &render, uint32_t imageIndex) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordPreDeferredSceneColorCopy called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordPreDeferredSceneColorCopy requires render targets");
  Assertion(m_bufferManager != nullptr, "recordPreDeferredSceneColorCopy requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordPreDeferredSceneColorCopy requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordPreDeferredSceneColorCopy requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  // Fullscreen draw state: independent of current clip/scissor.
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

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_COPY);

  static const vertex_layout copyLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_COPY;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = copyLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, copyLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Push the scene-color snapshot as the per-draw texture (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneColorSampler();
  sceneInfo.imageView = m_renderTargets->sceneColorView(imageIndex);
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.pImageInfo = &sceneInfo;

  std::array<vk::WriteDescriptorSet, 1> writes{write};
  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, writes);

  // Fullscreen triangle (same vertex buffer as deferred ambient).
  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &offset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordPreDeferredSceneHdrCopy(const RenderCtx &render) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordPreDeferredSceneHdrCopy called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordPreDeferredSceneHdrCopy requires render targets");
  Assertion(m_bufferManager != nullptr, "recordPreDeferredSceneHdrCopy requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordPreDeferredSceneHdrCopy requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordPreDeferredSceneHdrCopy requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  // Fullscreen draw state: independent of current clip/scissor.
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

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_COPY);

  static const vertex_layout copyLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_COPY;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = copyLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, copyLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Push the scene HDR color as the per-draw texture (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneHdrSampler();
  sceneInfo.imageView = m_renderTargets->sceneHdrView();
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorCount = 1;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.pImageInfo = &sceneInfo;

  std::array<vk::WriteDescriptorSet, 1> writes{write};
  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, writes);

  // Fullscreen triangle (same vertex buffer as deferred ambient).
  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize offset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &offset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordTonemappingToSwapchain(const RenderCtx &render, VulkanFrame &frame, bool hdrEnabled) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordTonemappingToSwapchain called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordTonemappingToSwapchain requires render targets");
  Assertion(m_bufferManager != nullptr, "recordTonemappingToSwapchain requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordTonemappingToSwapchain requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordTonemappingToSwapchain requires pipeline manager");

  const auto extent = m_vulkanDevice->swapchainExtent();

  // Preserve scissor across the internal fullscreen pass.
  auto clip = getClipScissorFromScreen(gr_screen);
  clip = clampClipScissorToFramebuffer(clip, gr_screen.max_w, gr_screen.max_h);
  vk::Rect2D restoreScissor{};
  restoreScissor.offset = vk::Offset2D{clip.x, clip.y};
  restoreScissor.extent = vk::Extent2D{clip.width, clip.height};

  // Fullscreen draw state.
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

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_TONEMAPPING);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_TONEMAPPING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Build tonemapping uniforms (genericData binding 1).
  graphics::generic_data::tonemapping_data data{};
  if (hdrEnabled) {
    const auto ppc = lighting_profiles::current_piecewise_intermediates();
    data.tonemapper = static_cast<int>(lighting_profiles::current_tonemapper());
    data.sh_B = ppc.sh_B;
    data.sh_lnA = ppc.sh_lnA;
    data.sh_offsetX = ppc.sh_offsetX;
    data.sh_offsetY = ppc.sh_offsetY;
    data.toe_B = ppc.toe_B;
    data.toe_lnA = ppc.toe_lnA;
    data.x0 = ppc.x0;
    data.x1 = ppc.x1;
    data.y0 = ppc.y0;
    data.exposure = lighting_profiles::current_exposure();
  } else {
    // Passthrough: no HDR pipeline => clamp only.
    data.tonemapper = static_cast<int>(lighting_profiles::TonemapperAlgorithm::Linear);
    data.exposure = 1.0f;
  }

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  // Scene HDR sampler (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneHdrSampler();
  sceneInfo.imageView = m_renderTargets->sceneHdrView();
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &sceneInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);

  // Restore clip scissor for any subsequent UI draws.
  cmd.setScissor(0, 1, &restoreScissor);
}

void VulkanRenderer::recordBloomBrightPass(const RenderCtx &render, VulkanFrame & /*frame*/) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordBloomBrightPass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordBloomBrightPass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordBloomBrightPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordBloomBrightPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordBloomBrightPass requires pipeline manager");

  // Bright pass renders at half resolution (mip 0 of bloom texture).
  const auto fullExtent = m_vulkanDevice->swapchainExtent();
  const vk::Extent2D extent{std::max(1u, fullExtent.width >> 1), std::max(1u, fullExtent.height >> 1)};

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_BRIGHTPASS);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_BRIGHTPASS;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Input: scene HDR (binding 2).
  vk::DescriptorImageInfo sceneInfo{};
  sceneInfo.sampler = m_renderTargets->sceneHdrSampler();
  sceneInfo.imageView = m_renderTargets->sceneHdrView();
  sceneInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.descriptorCount = 1;
  write.pImageInfo = &sceneInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, 1, &write);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordBloomBlurPass(const RenderCtx &render, VulkanFrame &frame, uint32_t srcPingPongIndex,
                                         uint32_t variantFlags, int mipLevel, uint32_t bloomWidth,
                                         uint32_t bloomHeight) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordBloomBlurPass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordBloomBlurPass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordBloomBlurPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordBloomBlurPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordBloomBlurPass requires pipeline manager");

  const vk::Extent2D extent{bloomWidth, bloomHeight};

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_BLUR, variantFlags);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_BLUR;
  key.variant_flags = variantFlags;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // genericData (binding 1): blur params.
  graphics::generic_data::blur_data data{};
  if (variantFlags & SDR_FLAG_BLUR_HORIZONTAL) {
    data.texSize = (bloomWidth > 0) ? (1.0f / static_cast<float>(bloomWidth)) : 0.0f;
  } else {
    data.texSize = (bloomHeight > 0) ? (1.0f / static_cast<float>(bloomHeight)) : 0.0f;
  }
  data.level = mipLevel;

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  // Input bloom texture (binding 2): full mip chain view.
  vk::DescriptorImageInfo bloomInfo{};
  bloomInfo.sampler = m_renderTargets->postLinearSampler();
  bloomInfo.imageView = m_renderTargets->bloomView(srcPingPongIndex);
  bloomInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &bloomInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordBloomCompositePass(const RenderCtx &render, VulkanFrame &frame, int mipLevels) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordBloomCompositePass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordBloomCompositePass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordBloomCompositePass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordBloomCompositePass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordBloomCompositePass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_BLOOM_COMP);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_BLOOM_COMP;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Enable blending for the single attachment if supported by dynamic state.
  if (m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable) {
    vk::Bool32 enable = VK_TRUE;
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &enable));
  }

  graphics::generic_data::bloom_composition_data data{};
  data.levels = mipLevels;
  data.bloom_intensity = gr_bloom_intensity() / 100.0f;

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  vk::DescriptorImageInfo bloomInfo{};
  bloomInfo.sampler = m_renderTargets->postLinearSampler();
  bloomInfo.imageView = m_renderTargets->bloomView(0);
  bloomInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &bloomInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);

  // Restore blending disabled for subsequent passes if dynamic state is used.
  if (m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable) {
    vk::Bool32 disable = VK_FALSE;
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &disable));
  }
}

void VulkanRenderer::recordSmaaEdgePass(const RenderCtx &render, VulkanFrame &frame,
                                        const vk::DescriptorImageInfo &colorInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordSmaaEdgePass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordSmaaEdgePass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordSmaaEdgePass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordSmaaEdgePass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_SMAA_EDGE);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_SMAA_EDGE;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::smaa_data data{};
  data.smaa_rt_metrics.x = static_cast<float>(extent.width);
  data.smaa_rt_metrics.y = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &colorInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordSmaaBlendWeightsPass(const RenderCtx &render, VulkanFrame &frame,
                                                const vk::DescriptorImageInfo &edgesInput,
                                                const vk::DescriptorImageInfo &areaTex,
                                                const vk::DescriptorImageInfo &searchTex) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordSmaaBlendWeightsPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordSmaaBlendWeightsPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordSmaaBlendWeightsPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordSmaaBlendWeightsPass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::smaa_data data{};
  data.smaa_rt_metrics.x = static_cast<float>(extent.width);
  data.smaa_rt_metrics.y = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 4> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &edgesInput;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &areaTex;

  writes[3].dstBinding = 4;
  writes[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[3].descriptorCount = 1;
  writes[3].pImageInfo = &searchTex;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordSmaaNeighborhoodPass(const RenderCtx &render, VulkanFrame &frame,
                                                const vk::DescriptorImageInfo &colorInput,
                                                const vk::DescriptorImageInfo &blendTex) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordSmaaNeighborhoodPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordSmaaNeighborhoodPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordSmaaNeighborhoodPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordSmaaNeighborhoodPass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::smaa_data data{};
  data.smaa_rt_metrics.x = static_cast<float>(extent.width);
  data.smaa_rt_metrics.y = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 3> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &colorInput;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &blendTex;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordFxaaPrepass(const RenderCtx &render, VulkanFrame & /*frame*/,
                                       const vk::DescriptorImageInfo &ldrInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordFxaaPrepass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordFxaaPrepass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordFxaaPrepass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordFxaaPrepass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_FXAA_PREPASS);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_FXAA_PREPASS;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.descriptorCount = 1;
  write.pImageInfo = &ldrInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, 1, &write);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordFxaaPass(const RenderCtx &render, VulkanFrame &frame,
                                    const vk::DescriptorImageInfo &luminanceInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordFxaaPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordFxaaPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordFxaaPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordFxaaPass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_FXAA);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_FXAA;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  graphics::generic_data::fxaa_data data{};
  data.rt_w = static_cast<float>(extent.width);
  data.rt_h = static_cast<float>(extent.height);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(data);

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &luminanceInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordLightshaftsPass(const RenderCtx &render, VulkanFrame &frame,
                                           const graphics::generic_data::lightshaft_data &params) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordLightshaftsPass called with null command buffer");
  Assertion(m_renderTargets != nullptr, "recordLightshaftsPass requires render targets");
  Assertion(m_bufferManager != nullptr, "recordLightshaftsPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordLightshaftsPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordLightshaftsPass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_LIGHTSHAFTS);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_LIGHTSHAFTS;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Our pipelines use VK_EXT_extended_dynamic_state3 for colorBlendEnable when available.
  // VulkanRenderingSession::applyDynamicState sets a baseline of blending OFF at pass start,
  // so we must explicitly enable it for additive passes like lightshafts.
  if (m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable) {
    vk::Bool32 enable = VK_TRUE;
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &enable));
  }

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(params), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &params, sizeof(params));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(params);

  vk::DescriptorImageInfo sceneDepth{};
  sceneDepth.sampler = m_renderTargets->depthSampler();
  sceneDepth.imageView = m_renderTargets->depthSampledView();
  sceneDepth.imageLayout = m_renderTargets->depthReadLayout();

  vk::DescriptorImageInfo cockpitDepth{};
  cockpitDepth.sampler = m_renderTargets->depthSampler();
  cockpitDepth.imageView = m_renderTargets->cockpitDepthSampledView();
  cockpitDepth.imageLayout = m_renderTargets->depthReadLayout();

  std::array<vk::WriteDescriptorSet, 3> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &sceneDepth;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &cockpitDepth;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::recordPostEffectsPass(const RenderCtx &render, VulkanFrame &frame,
                                           const graphics::generic_data::post_data &params,
                                           const vk::DescriptorImageInfo &ldrInput,
                                           const vk::DescriptorImageInfo &depthInput) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordPostEffectsPass called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordPostEffectsPass requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordPostEffectsPass requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordPostEffectsPass requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_POST_PROCESS_MAIN);

  static const vertex_layout fsLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_POST_PROCESS_MAIN;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = fsLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, fsLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  const vk::DeviceSize uboAlignment = static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(params), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &params, sizeof(params));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(params);

  std::array<vk::WriteDescriptorSet, 4> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &ldrInput;

  writes[2].dstBinding = 3;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &depthInput;

  // Binding 4 is unused by the built-in shader but reserved for future/custom effects.
  writes[3].dstBinding = 4;
  writes[3].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[3].descriptorCount = 1;
  writes[3].pImageInfo = &depthInput;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0,
                           static_cast<uint32_t>(writes.size()), writes.data());

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

void VulkanRenderer::generateBloomMipmaps(vk::CommandBuffer cmd, uint32_t pingPongIndex, vk::Extent2D baseExtent) {
  Assertion(cmd, "generateBloomMipmaps called with null command buffer");
  Assertion(m_renderTargets != nullptr, "generateBloomMipmaps requires render targets");
  Assertion(pingPongIndex < VulkanRenderTargets::kBloomPingPongCount, "Invalid bloom ping-pong index %u",
            pingPongIndex);

  // Ensure no dynamic rendering is active. Mipmap generation uses blits (transfer ops).
  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  const vk::Image image = m_renderTargets->bloomImage(pingPongIndex);
  const uint32_t mipLevels = VulkanRenderTargets::kBloomMipLevels;

  // Query blit support; fall back to nearest if linear isn't supported for this format.
  vk::Filter filter = vk::Filter::eLinear;
  const auto props = m_vulkanDevice->physicalDevice().getFormatProperties(vk::Format::eR16G16B16A16Sfloat);
  const bool canLinear =
      (props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) != vk::FormatFeatureFlags{};
  if (!canLinear) {
    filter = vk::Filter::eNearest;
  }

  // Transition all mips to TRANSFER_DST (we will move each mip to SRC as we go).
  {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.oldLayout = m_renderTargets->bloomLayout(pingPongIndex);
    barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmd.pipelineBarrier2(dep);

    m_renderTargets->setBloomLayout(pingPongIndex, vk::ImageLayout::eTransferDstOptimal);
  }

  // Transition mip 0 to TRANSFER_SRC.
  {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmd.pipelineBarrier2(dep);
  }

  uint32_t srcW = std::max(1u, baseExtent.width);
  uint32_t srcH = std::max(1u, baseExtent.height);

  for (uint32_t mip = 1; mip < mipLevels; ++mip) {
    const uint32_t dstW = std::max(1u, srcW >> 1);
    const uint32_t dstH = std::max(1u, srcH >> 1);

    vk::ImageBlit blit{};
    blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.srcSubresource.mipLevel = mip - 1;
    blit.srcSubresource.baseArrayLayer = 0;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.srcOffsets[1] = vk::Offset3D{static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};

    blit.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    blit.dstSubresource.mipLevel = mip;
    blit.dstSubresource.baseArrayLayer = 0;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = vk::Offset3D{0, 0, 0};
    blit.dstOffsets[1] = vk::Offset3D{static_cast<int32_t>(dstW), static_cast<int32_t>(dstH), 1};

    // Make sure destination mip is TRANSFER_DST and source mip is TRANSFER_SRC.
    {
      vk::ImageMemoryBarrier2 barriers[2]{};

      barriers[0].srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barriers[0].srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      barriers[0].dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barriers[0].dstAccessMask = vk::AccessFlagBits2::eTransferRead;
      barriers[0].oldLayout = vk::ImageLayout::eTransferDstOptimal;
      barriers[0].newLayout = vk::ImageLayout::eTransferSrcOptimal;
      barriers[0].image = image;
      barriers[0].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      barriers[0].subresourceRange.baseMipLevel = mip - 1;
      barriers[0].subresourceRange.levelCount = 1;
      barriers[0].subresourceRange.baseArrayLayer = 0;
      barriers[0].subresourceRange.layerCount = 1;

      barriers[1].srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barriers[1].srcAccessMask = {};
      barriers[1].dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barriers[1].dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
      barriers[1].oldLayout = vk::ImageLayout::eTransferDstOptimal;
      barriers[1].newLayout = vk::ImageLayout::eTransferDstOptimal;
      barriers[1].image = image;
      barriers[1].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      barriers[1].subresourceRange.baseMipLevel = mip;
      barriers[1].subresourceRange.levelCount = 1;
      barriers[1].subresourceRange.baseArrayLayer = 0;
      barriers[1].subresourceRange.layerCount = 1;

      vk::DependencyInfo dep{};
      dep.imageMemoryBarrierCount = 2;
      dep.pImageMemoryBarriers = barriers;
      cmd.pipelineBarrier2(dep);
    }

    cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal, 1, &blit,
                  filter);

    // Transition destination mip to TRANSFER_SRC so it can serve as source for the next step.
    {
      vk::ImageMemoryBarrier2 barrier{};
      barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
      barrier.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
      barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
      barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      barrier.image = image;
      barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      barrier.subresourceRange.baseMipLevel = mip;
      barrier.subresourceRange.levelCount = 1;
      barrier.subresourceRange.baseArrayLayer = 0;
      barrier.subresourceRange.layerCount = 1;

      vk::DependencyInfo dep{};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &barrier;
      cmd.pipelineBarrier2(dep);
    }

    srcW = dstW;
    srcH = dstH;
  }

  // Transition all mips to shader-read for sampling in blur passes.
  {
    vk::ImageMemoryBarrier2 barrier{};
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead | vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
    barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vk::DependencyInfo dep{};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    cmd.pipelineBarrier2(dep);
  }

  m_renderTargets->setBloomLayout(pingPongIndex, vk::ImageLayout::eShaderReadOnlyOptimal);
}

void VulkanRenderer::recordCopyToSwapchain(const RenderCtx &render, vk::DescriptorImageInfo src) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordCopyToSwapchain called with null command buffer");
  Assertion(m_bufferManager != nullptr, "recordCopyToSwapchain requires buffer manager");
  Assertion(m_shaderManager != nullptr, "recordCopyToSwapchain requires shader manager");
  Assertion(m_pipelineManager != nullptr, "recordCopyToSwapchain requires pipeline manager");

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
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_COPY);

  static const vertex_layout copyLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0);
    return layout;
  }();

  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_COPY;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(render.targetInfo.colorFormat);
  key.depth_format = static_cast<VkFormat>(render.targetInfo.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = render.targetInfo.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_NONE;
  key.layout_hash = copyLayout.hash();

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, copyLayout);
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  vk::WriteDescriptorSet write{};
  write.dstBinding = 2;
  write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
  write.descriptorCount = 1;
  write.pImageInfo = &src;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, m_descriptorLayouts->pipelineLayout(), 0, 1, &write);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &fullscreenVB, &vbOffset);
  cmd.draw(3, 1, 0, 0);
}

vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const {
  Assertion(m_bufferManager != nullptr, "getBuffer called before buffer manager initialization");
  return m_bufferManager->getBuffer(handle);
}

vk::Buffer VulkanRenderer::queryModelVertexHeapBuffer() const {
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

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void *data) {
  Assertion(m_bufferManager != nullptr, "updateBufferData called before buffer manager initialization");
  m_bufferManager->updateBufferData(handle, size, data);
}

void VulkanRenderer::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void *data) {
  Assertion(m_bufferManager != nullptr, "updateBufferDataOffset called before buffer manager initialization");
  m_bufferManager->updateBufferDataOffset(handle, offset, size, data);
}

void *VulkanRenderer::mapBuffer(gr_buffer_handle handle) {
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

vk::DescriptorImageInfo VulkanRenderer::getTextureDescriptor(const FrameCtx &ctx, int bitmapHandle,
                                                             const VulkanTextureManager::SamplerKey &samplerKey) {
  Assertion(m_textureManager != nullptr, "getTextureDescriptor called before texture manager initialization");
  Assertion(&ctx.renderer == this,
            "getTextureDescriptor called with FrameCtx from a different VulkanRenderer instance");
  Assertion(bitmapHandle >= 0, "getTextureDescriptor called with invalid bitmapHandle %d", bitmapHandle);

  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  Assertion(baseFrame >= 0, "Invalid bitmapHandle %d in getTextureDescriptor", bitmapHandle);

  Assertion(m_textureBindings != nullptr, "getTextureDescriptor called before texture bindings initialization");
  const auto id = TextureId::tryFromBaseFrame(baseFrame);
  Assertion(id.has_value(), "Invalid base frame %d in getTextureDescriptor", baseFrame);

  // Fast path: already resident.
  auto info = m_textureManager->tryGetResidentDescriptor(*id, samplerKey);
  if (info.has_value()) {
    m_textureManager->markTextureUsed(*id, m_frameCounter);
    return *info;
  }

  // Slow path: queue the upload and flush immediately so this draw doesn't sample fallback forever
  // (critical for animations that advance every frame).
  m_textureManager->queueTextureUpload(*id, m_frameCounter, samplerKey);
  flushQueuedTextureUploads(ctx, /*syncModelDescriptors=*/false);
  return m_textureBindings->descriptor(*id, m_frameCounter, samplerKey);
}

void VulkanRenderer::beginDecalPass(const FrameCtx &ctx) {
  Assertion(&ctx.renderer == this, "beginDecalPass called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "beginDecalPass called before rendering session initialization");

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  Assertion(cmd, "beginDecalPass called with null command buffer");

  // Decals sample depth; transitions are invalid inside dynamic rendering.
  m_renderingSession->suspendRendering();
  m_renderingSession->transitionMainDepthToShaderRead(cmd);

  // Decal shaders sample the scene depth via the global (set=1) descriptor set.
  bindDeferredGlobalDescriptors();
}

void VulkanRenderer::setViewport(const FrameCtx &ctx, const vk::Viewport &viewport) {
  Assertion(&ctx.renderer == this, "setViewport called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  cmd.setViewport(0, 1, &viewport);
}

void VulkanRenderer::setScissor(const FrameCtx &ctx, const vk::Rect2D &scissor) {
  Assertion(&ctx.renderer == this, "setScissor called with FrameCtx from a different VulkanRenderer instance");
  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }
  cmd.setScissor(0, 1, &scissor);
}

bool VulkanRenderer::createBitmapRenderTarget(int handle, int *width, int *height, int *bpp, int *mm_lvl, int flags) {
  Assertion(m_textureManager != nullptr, "createBitmapRenderTarget called before texture manager initialization");
  if (!width || !height || !bpp || !mm_lvl) {
    return false;
  }
  if (handle < 0) {
    return false;
  }

  uint32_t w = static_cast<uint32_t>(*width);
  uint32_t h = static_cast<uint32_t>(*height);

  // Cubemap faces must be square. Mirror OpenGL behavior: clamp to max dimension.
  if ((flags & BMP_FLAG_CUBEMAP) && (w != h)) {
    const uint32_t mx = (w > h) ? w : h;
    w = mx;
    h = mx;
  }

  // Hard clamp to device limits (fail-fast clamping, no silent overflow).
  const auto &limits = m_vulkanDevice->properties().limits;
  const uint32_t maxDim = (flags & BMP_FLAG_CUBEMAP) ? limits.maxImageDimensionCube : limits.maxImageDimension2D;
  if (w > maxDim) {
    w = maxDim;
  }
  if (h > maxDim) {
    h = maxDim;
  }

  uint32_t mipLevels = 1;
  if (!m_textureManager->createRenderTarget(handle, w, h, flags, &mipLevels)) {
    return false;
  }

  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  // OpenGL parity: report 24bpp even though the underlying image is RGBA8.
  *bpp = 24;
  *mm_lvl = static_cast<int>(mipLevels);
  return true;
}

bool VulkanRenderer::setBitmapRenderTarget(const FrameCtx &ctx, int handle, int face) {
  Assertion(&ctx.renderer == this,
            "setBitmapRenderTarget called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_renderingSession != nullptr, "setBitmapRenderTarget called before rendering session initialization");
  Assertion(m_textureManager != nullptr, "setBitmapRenderTarget called before texture manager initialization");

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return false;
  }

  // bmpman updates gr_screen.rendering_to_texture *after* the graphics API callback returns, so this still reflects
  // the previous target at this point.
  const int prevTarget = gr_screen.rendering_to_texture;

  // Switching targets requires ending any active dynamic rendering scope.
  if (handle < 0) {
    requestMainTargetWithDepth();
  } else {
    if (!m_textureManager->hasRenderTarget(handle)) {
      return false;
    }
    m_renderingSession->requestBitmapTarget(handle, face);
  }

  // Leaving a bitmap render target: transition to shader-read and generate mipmaps if requested.
  // (Skip when switching faces on the same cubemap.)
  if (prevTarget >= 0 && prevTarget != handle) {
    if (m_textureManager->renderTargetMipLevels(prevTarget) > 1) {
      m_textureManager->generateRenderTargetMipmaps(cmd, prevTarget);
    } else {
      m_textureManager->transitionRenderTargetToShaderRead(cmd, prevTarget);
    }
  }

  return true;
}

vk::DescriptorImageInfo
VulkanRenderer::getDefaultTextureDescriptor(const VulkanTextureManager::SamplerKey &samplerKey) {
  Assertion(m_textureManager != nullptr, "getDefaultTextureDescriptor called before texture manager initialization");
  return m_textureManager->defaultBaseDescriptor(samplerKey);
}

uint32_t VulkanRenderer::getBindlessTextureIndex(const FrameCtx &ctx, int bitmapHandle) {
  if (bitmapHandle < 0) {
    return kBindlessTextureSlotFallback;
  }

  Assertion(&ctx.renderer == this,
            "getBindlessTextureIndex called with FrameCtx from a different VulkanRenderer instance");
  Assertion(m_textureBindings != nullptr, "getBindlessTextureIndex called before texture bindings initialization");
  Assertion(m_textureManager != nullptr, "getBindlessTextureIndex called before texture manager initialization");

  const int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
  if (baseFrame < 0) {
    return kBindlessTextureSlotFallback;
  }

  const auto id = TextureId::tryFromBaseFrame(baseFrame);
  if (!id.has_value()) {
    return kBindlessTextureSlotFallback;
  }

  const bool wasResident = m_textureManager->isResident(*id);
  const uint32_t slot = m_textureBindings->bindlessIndex(*id, m_frameCounter);

  if (!wasResident) {
    // Critical for animated textures: upload + re-sync descriptors now so this draw doesn't sample fallback forever.
    flushQueuedTextureUploads(ctx, /*syncModelDescriptors=*/true);
    return m_textureBindings->bindlessIndex(*id, m_frameCounter);
  }

  return slot;
}

void VulkanRenderer::setModelUniformBinding(VulkanFrame &frame, gr_buffer_handle handle, size_t offset, size_t size) {
  const auto alignment = getMinUniformOffsetAlignment();
  Assertion(offset <= std::numeric_limits<uint32_t>::max(), "Model uniform offset %zu exceeds uint32_t range", offset);
  const auto dynOffset = static_cast<uint32_t>(offset);

  Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
  Assertion((dynOffset % alignment) == 0, "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
  Assertion(size >= sizeof(model_uniform_data), "Model uniform size %zu is smaller than sizeof(model_uniform_data) %zu",
            size, sizeof(model_uniform_data));

  Assertion(frame.modelDescriptorSet(), "Model descriptor set must be allocated before binding uniform buffer");
  Assertion(handle.isValid(), "Invalid model uniform buffer handle");
  Assertion(m_bufferManager != nullptr, "setModelUniformBinding requires buffer manager");

  vk::Buffer vkBuffer =
      m_bufferManager->ensureBuffer(handle, static_cast<vk::DeviceSize>(offset + sizeof(model_uniform_data)));
  Assertion(vkBuffer, "Failed to resolve Vulkan buffer for handle %d", handle.value());

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

  frame.modelUniformBinding = DynamicUniformBinding{handle, dynOffset};
}

void VulkanRenderer::setSceneUniformBinding(VulkanFrame &frame, gr_buffer_handle handle, size_t offset, size_t size) {
  // For now, we just track the state in the frame.
  // In the future, this will update a descriptor set for the scene/view block (binding 6).
  // Currently, the engine binds this, but the shaders might not use it via a dedicated set yet.
  // We store it so it's available when we add the descriptor wiring.

  const auto alignment = getMinUniformOffsetAlignment();
  Assertion(offset <= std::numeric_limits<uint32_t>::max(), "Scene uniform offset %zu exceeds uint32_t range", offset);
  const auto dynOffset = static_cast<uint32_t>(offset);

  Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
  Assertion((dynOffset % alignment) == 0, "Scene uniform offset %u is not aligned to %zu", dynOffset, alignment);

  frame.sceneUniformBinding = DynamicUniformBinding{handle, dynOffset};
}

void VulkanRenderer::updateModelDescriptors(uint32_t frameIndex, vk::DescriptorSet set, vk::Buffer vertexHeapBuffer,
                                            vk::Buffer transformBuffer,
                                            const std::vector<std::pair<uint32_t, TextureId>> &textures) {
  std::vector<vk::WriteDescriptorSet> writes;
  writes.reserve(3);

  // Binding 0: Vertex heap SSBO
  Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
            "updateModelDescriptors called with null vertexHeapBuffer");

  vk::DescriptorBufferInfo heapInfo{};
  heapInfo.buffer = vertexHeapBuffer;
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

  // Binding 3: Batched transforms (dynamic SSBO)
  Assertion(static_cast<VkBuffer>(transformBuffer) != VK_NULL_HANDLE,
            "updateModelDescriptors called with null transformBuffer");

  vk::DescriptorBufferInfo transformInfo{};
  transformInfo.buffer = transformBuffer;
  transformInfo.offset = 0;
  transformInfo.range = VK_WHOLE_SIZE;

  vk::WriteDescriptorSet transformWrite{};
  transformWrite.dstSet = set;
  transformWrite.dstBinding = 3;
  transformWrite.dstArrayElement = 0;
  transformWrite.descriptorCount = 1;
  transformWrite.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
  transformWrite.pBufferInfo = &transformInfo;
  writes.push_back(transformWrite);

  // Binding 1: Bindless textures
  // Correctness rule: every slot must always point at a valid descriptor (fallback until resident).
  VulkanTextureManager::SamplerKey samplerKey{};
  samplerKey.address = vk::SamplerAddressMode::eRepeat;
  samplerKey.filter = vk::Filter::eLinear;

  Assertion(m_textureManager != nullptr, "updateModelDescriptors called before texture manager initialization");
  const vk::DescriptorImageInfo fallbackInfo = m_textureManager->fallbackDescriptor(samplerKey);
  const vk::DescriptorImageInfo defaultBaseInfo = m_textureManager->defaultBaseDescriptor(samplerKey);
  const vk::DescriptorImageInfo defaultNormalInfo = m_textureManager->defaultNormalDescriptor(samplerKey);
  const vk::DescriptorImageInfo defaultSpecInfo = m_textureManager->defaultSpecDescriptor(samplerKey);

  std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> desiredInfos{};
  desiredInfos.fill(fallbackInfo);
  desiredInfos[kBindlessTextureSlotDefaultBase] = defaultBaseInfo;
  desiredInfos[kBindlessTextureSlotDefaultNormal] = defaultNormalInfo;
  desiredInfos[kBindlessTextureSlotDefaultSpec] = defaultSpecInfo;

  for (const auto &[arrayIndex, id] : textures) {
    Assertion(arrayIndex < kMaxBindlessTextures, "updateModelDescriptors: slot index %u out of range (max %u)",
              arrayIndex, kMaxBindlessTextures);
    auto info = m_textureManager->tryGetResidentDescriptor(id, samplerKey);
    Assertion(info.has_value(), "updateModelDescriptors requires resident TextureId baseFrame=%d", id.baseFrame());
    desiredInfos[arrayIndex] = *info;
  }

  auto sameInfo = [](const vk::DescriptorImageInfo &a, const vk::DescriptorImageInfo &b) {
    return a.sampler == b.sampler && a.imageView == b.imageView && a.imageLayout == b.imageLayout;
  };

  Assertion(frameIndex < m_modelBindlessCache.size(),
            "updateModelDescriptors called with invalid frameIndex %u (cache size %zu)", frameIndex,
            m_modelBindlessCache.size());
  auto &cache = m_modelBindlessCache[frameIndex];

  if (!cache.initialized) {
    vk::WriteDescriptorSet texturesWrite{};
    texturesWrite.dstSet = set;
    texturesWrite.dstBinding = 1;
    texturesWrite.dstArrayElement = 0;
    texturesWrite.descriptorCount = kMaxBindlessTextures;
    texturesWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
    texturesWrite.pImageInfo = desiredInfos.data();
    writes.push_back(texturesWrite);

    cache.infos = desiredInfos;
    cache.initialized = true;
  } else {
    uint32_t i = 0;
    while (i < kMaxBindlessTextures) {
      if (sameInfo(cache.infos[i], desiredInfos[i])) {
        ++i;
        continue;
      }

      const uint32_t start = i;
      while (i < kMaxBindlessTextures && !sameInfo(cache.infos[i], desiredInfos[i])) {
        cache.infos[i] = desiredInfos[i];
        ++i;
      }
      const uint32_t count = i - start;

      vk::WriteDescriptorSet texturesWrite{};
      texturesWrite.dstSet = set;
      texturesWrite.dstBinding = 1;
      texturesWrite.dstArrayElement = start;
      texturesWrite.descriptorCount = count;
      texturesWrite.descriptorType = vk::DescriptorType::eCombinedImageSampler;
      texturesWrite.pImageInfo = desiredInfos.data() + start;
      writes.push_back(texturesWrite);
    }
  }

  m_vulkanDevice->device().updateDescriptorSets(writes, {});
}

void VulkanRenderer::beginModelDescriptorSync(VulkanFrame &frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer) {
  Assertion(static_cast<VkBuffer>(vertexHeapBuffer) != VK_NULL_HANDLE,
            "beginModelDescriptorSync called with null vertexHeapBuffer");
  Assertion(m_bufferManager != nullptr, "beginModelDescriptorSync requires buffer manager");

  Assertion(frameIndex < kFramesInFlight, "Invalid frame index %u (must be 0..%u)", frameIndex, kFramesInFlight - 1);

  Assertion(frame.modelDescriptorSet(), "Model descriptor set must be allocated at frame construction");

  Assertion(m_textureManager != nullptr, "beginModelDescriptorSync requires texture manager");

  // Binding 0: Vertex heap SSBO; Binding 1: bindless textures; Binding 3: batched transform buffer (dynamic SSBO).
  // We batch the writes to avoid issuing one vkUpdateDescriptorSets call per texture.
  std::vector<std::pair<uint32_t, TextureId>> textures;
  textures.reserve(kMaxBindlessTextures);
  m_textureManager->appendResidentBindlessDescriptors(textures);

  updateModelDescriptors(frameIndex, frame.modelDescriptorSet(), vertexHeapBuffer, frame.vertexBuffer().buffer(),
                         textures);
}

int VulkanRenderer::preloadTexture(int bitmapHandle, bool isAABitmap) {
  if (m_textureManager && bitmapHandle >= 0) {
    return m_textureManager->preloadTexture(bitmapHandle, isAABitmap) ? 1 : 0;
  }
  return 0;
}

void VulkanRenderer::updateTexture(const FrameCtx &ctx, int bitmapHandle, int bpp, const ubyte *data, int width,
                                   int height) {
  if (!m_textureManager || !m_textureUploader) {
    return;
  }
  if (bitmapHandle < 0 || data == nullptr || width <= 0 || height <= 0) {
    return;
  }

  // Transfer operations are invalid inside dynamic rendering.
  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  UploadCtx uploadCtx{ctx.m_recording.ref(), cmd, m_frameCounter};
  (void)m_textureUploader->updateTexture(uploadCtx, bitmapHandle, bpp, data, width, height);
}

void VulkanRenderer::releaseBitmap(int bitmapHandle) {
  if (m_textureManager && bitmapHandle >= 0) {
    m_textureManager->releaseBitmap(bitmapHandle);
  }
}

MovieTextureHandle VulkanRenderer::createMovieTexture(uint32_t width, uint32_t height, MovieColorSpace colorspace,
                                                      MovieColorRange range) {
  if (!m_movieManager || !m_movieManager->isAvailable()) {
    return MovieTextureHandle::Invalid;
  }
  return m_movieManager->createMovieTexture(width, height, colorspace, range);
}

void VulkanRenderer::uploadMovieTexture(const FrameCtx &ctx, MovieTextureHandle handle, const ubyte *y, int yStride,
                                        const ubyte *u, int uStride, const ubyte *v, int vStride) {
  if (!m_movieManager || !m_movieManager->isAvailable() || !gr_is_valid(handle)) {
    return;
  }

  if (m_renderingSession) {
    m_renderingSession->suspendRendering();
  }

  vk::CommandBuffer cmd = ctx.m_recording.cmd();
  if (!cmd) {
    return;
  }

  UploadCtx uploadCtx{ctx.m_recording.ref(), cmd, m_frameCounter};
  m_movieManager->uploadMovieFrame(uploadCtx, handle, y, yStride, u, uStride, v, vStride);
}

void VulkanRenderer::drawMovieTexture(const FrameCtx &ctx, MovieTextureHandle handle, float x1, float y1, float x2,
                                      float y2, float alpha) {
  if (!m_movieManager || !m_movieManager->isAvailable() || !gr_is_valid(handle)) {
    return;
  }

  auto renderCtx = ensureRenderingStarted(ctx);
  incrementPrimDraw();
  m_movieManager->drawMovieTexture(renderCtx, handle, x1, y1, x2, y2, alpha);
}

void VulkanRenderer::releaseMovieTexture(MovieTextureHandle handle) {
  if (!m_movieManager || !gr_is_valid(handle)) {
    return;
  }
  m_movieManager->releaseMovieTexture(handle);
}

void VulkanRenderer::submitInitCommandsAndWait(const InitCtx & /*init*/,
                                               const std::function<void(vk::CommandBuffer)> &recorder) {
  Assertion(m_vulkanDevice != nullptr, "submitInitCommandsAndWait requires VulkanDevice");
  Assertion(m_uploadCommandPool, "submitInitCommandsAndWait requires an upload command pool");
  Assertion(m_submitTimeline, "submitInitCommandsAndWait requires a submit timeline semaphore");

  vk::CommandBufferAllocateInfo allocInfo{};
  allocInfo.level = vk::CommandBufferLevel::ePrimary;
  allocInfo.commandPool = m_uploadCommandPool.get();
  allocInfo.commandBufferCount = 1;

  vk::CommandBuffer cmd{};
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto allocRv = m_vulkanDevice->device().allocateCommandBuffers(allocInfo);
  if (allocRv.result != vk::Result::eSuccess || allocRv.value.empty()) {
    throw std::runtime_error("Failed to allocate init command buffer");
  }
  cmd = allocRv.value[0];
#else
  const auto cmdBuffers = m_vulkanDevice->device().allocateCommandBuffers(allocInfo);
  Assertion(!cmdBuffers.empty(), "Failed to allocate init command buffer");
  cmd = cmdBuffers[0];
#endif

  vk::CommandBufferBeginInfo beginInfo{};
  beginInfo.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto beginResult = cmd.begin(beginInfo);
  if (beginResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to begin init command buffer");
  }
#else
  cmd.begin(beginInfo);
#endif

  recorder(cmd);

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto endResult = cmd.end();
  if (endResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to end init command buffer");
  }
#else
  cmd.end();
#endif

  // Integrate with the renderer's global serial model by signaling the timeline semaphore.
  const uint64_t submitSerial = ++m_submitSerial;

  vk::CommandBufferSubmitInfo cmdInfo{};
  cmdInfo.commandBuffer = cmd;

  vk::SemaphoreSubmitInfo signal{};
  signal.semaphore = m_submitTimeline.get();
  signal.value = submitSerial;
  signal.stageMask = vk::PipelineStageFlagBits2::eAllCommands;

  vk::SubmitInfo2 submitInfo{};
  submitInfo.commandBufferInfoCount = 1;
  submitInfo.pCommandBufferInfos = &cmdInfo;
  submitInfo.signalSemaphoreInfoCount = 1;
  submitInfo.pSignalSemaphoreInfos = &signal;

  m_vulkanDevice->graphicsQueue().submit2(submitInfo, nullptr);

  // Block until the submitted work is complete. This avoids the global stall of queue.waitIdle().
  vk::Semaphore semaphore = m_submitTimeline.get();
  vk::SemaphoreWaitInfo waitInfo{};
  waitInfo.semaphoreCount = 1;
  waitInfo.pSemaphores = &semaphore;
  waitInfo.pValues = &submitSerial;

#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto waitResult = m_vulkanDevice->device().waitSemaphores(waitInfo, std::numeric_limits<uint64_t>::max());
  if (waitResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed waiting for init submit to complete");
  }
#else
  m_vulkanDevice->device().waitSemaphores(waitInfo, std::numeric_limits<uint64_t>::max());
#endif

  m_completedSerial = std::max(m_completedSerial, submitSerial);

  // Safe after wait: recycle command buffer allocations from the init command pool.
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  const auto resetResult = m_vulkanDevice->device().resetCommandPool(m_uploadCommandPool.get());
  if (resetResult != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to reset init upload command pool");
  }
#else
  m_vulkanDevice->device().resetCommandPool(m_uploadCommandPool.get());
#endif
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

int VulkanRenderer::getZbufferMode() const { return static_cast<int>(m_zbufferMode); }

void VulkanRenderer::requestClear() { m_renderingSession->requestClear(); }

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

int VulkanRenderer::saveScreen() {
  if (m_savedScreen.isFrozen()) {
    return -1;
  }

  if (!m_savedScreen.hasHandle()) {
    return -1;
  }

  m_savedScreen.freeze();
  return m_savedScreen.handle();
}

void VulkanRenderer::freeScreen(int handle) {
  if (!m_savedScreen.hasHandle()) {
    return;
  }

  if (handle >= 0) {
    Assertion(handle == m_savedScreen.handle(), "freeScreen called with handle %d but saved screen handle is %d",
              handle, m_savedScreen.handle());
  }

  m_savedScreen.unfreeze();
}

int VulkanRenderer::frozenScreenHandle() const {
  if (!m_savedScreen.isFrozen()) {
    return -1;
  }
  return m_savedScreen.handle();
}

void VulkanRenderer::createDeferredLightingResources() {
  // Fullscreen triangle (covers entire screen with 3 vertices, no clipping)
  // Positions are in clip space: vertex shader passes through directly
  struct FullscreenVertex {
    float x, y, z;
  };
  static const FullscreenVertex fullscreenVerts[] = {{-1.0f, -1.0f, 0.0f}, {3.0f, -1.0f, 0.0f}, {-1.0f, 3.0f, 0.0f}};

  m_fullscreenMesh.vbo = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Static);
  m_bufferManager->updateBufferData(m_fullscreenMesh.vbo, sizeof(fullscreenVerts), fullscreenVerts);
  m_fullscreenMesh.indexCount = 3;

  // Sphere mesh (icosphere-like approximation)
  // Using octahedron subdivided once for reasonable approximation
  std::vector<float> sphereVerts;
  std::vector<uint32_t> sphereIndices;

  // Octahedron base vertices
  const float oct[] = {
      0.0f,  1.0f,  0.0f, // top
      0.0f,  -1.0f, 0.0f, // bottom
      1.0f,  0.0f,  0.0f, // +X
      -1.0f, 0.0f,  0.0f, // -X
      0.0f,  0.0f,  1.0f, // +Z
      0.0f,  0.0f,  -1.0f // -Z
  };

  // Octahedron faces (8 triangles)
  const uint32_t octFaces[] = {
      0, 4, 2, 0, 2, 5, 0, 5, 3, 0, 3, 4, // top half
      1, 2, 4, 1, 5, 2, 1, 3, 5, 1, 4, 3  // bottom half
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
      cylVerts.push_back(cosf(angle)); // x
      cylVerts.push_back(sinf(angle)); // y
      cylVerts.push_back(z);           // z
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

void VulkanRenderer::createSmaaLookupTextures(const InitCtx &init) {
  Assertion(m_vulkanDevice != nullptr, "createSmaaLookupTextures requires VulkanDevice");

  auto createLookupTexture = [&](const uint8_t *pixels, size_t sizeBytes, uint32_t width, uint32_t height,
                                 vk::Format format, SmaaLookupTexture &out) {
    Assertion(pixels != nullptr, "createSmaaLookupTextures pixel pointer must be valid");
    Assertion(width > 0 && height > 0, "createSmaaLookupTextures invalid extent %ux%u", width, height);
    Assertion(sizeBytes > 0, "createSmaaLookupTextures invalid size");

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = format;
    imageInfo.extent = vk::Extent3D(width, height, 1);
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;

    out.image = m_vulkanDevice->device().createImageUnique(imageInfo);

    const auto memReqs = m_vulkanDevice->device().getImageMemoryRequirements(out.image.get());
    vk::MemoryAllocateInfo allocInfo{};
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex =
        m_vulkanDevice->findMemoryType(memReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    out.memory = m_vulkanDevice->device().allocateMemoryUnique(allocInfo);
    m_vulkanDevice->device().bindImageMemory(out.image.get(), out.memory.get(), 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = out.image.get();
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    out.view = m_vulkanDevice->device().createImageViewUnique(viewInfo);

    // Staging buffer (host visible).
    vk::BufferCreateInfo bufInfo{};
    bufInfo.size = sizeBytes;
    bufInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
    bufInfo.sharingMode = vk::SharingMode::eExclusive;
    auto staging = m_vulkanDevice->device().createBufferUnique(bufInfo);

    const auto bufReqs = m_vulkanDevice->device().getBufferMemoryRequirements(staging.get());
    vk::MemoryAllocateInfo bufAlloc{};
    bufAlloc.allocationSize = bufReqs.size;
    bufAlloc.memoryTypeIndex = m_vulkanDevice->findMemoryType(
        bufReqs.memoryTypeBits, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    auto stagingMem = m_vulkanDevice->device().allocateMemoryUnique(bufAlloc);
    m_vulkanDevice->device().bindBufferMemory(staging.get(), stagingMem.get(), 0);

    void *mapped = m_vulkanDevice->device().mapMemory(stagingMem.get(), 0, sizeBytes);
    std::memcpy(mapped, pixels, sizeBytes);
    m_vulkanDevice->device().unmapMemory(stagingMem.get());

    submitInitCommandsAndWait(init, [&](vk::CommandBuffer cmd) {
      // Undefined -> transfer dst
      vk::ImageMemoryBarrier2 toTransfer{};
      toTransfer.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
      toTransfer.srcAccessMask = {};
      toTransfer.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
      toTransfer.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
      toTransfer.oldLayout = vk::ImageLayout::eUndefined;
      toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
      toTransfer.image = out.image.get();
      toTransfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      toTransfer.subresourceRange.levelCount = 1;
      toTransfer.subresourceRange.layerCount = 1;

      vk::DependencyInfo depToTransfer{};
      depToTransfer.imageMemoryBarrierCount = 1;
      depToTransfer.pImageMemoryBarriers = &toTransfer;
      cmd.pipelineBarrier2(depToTransfer);

      vk::BufferImageCopy region{};
      region.bufferOffset = 0;
      region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageExtent = vk::Extent3D(width, height, 1);
      cmd.copyBufferToImage(staging.get(), out.image.get(), vk::ImageLayout::eTransferDstOptimal, 1, &region);

      // Transfer dst -> shader read
      vk::ImageMemoryBarrier2 toShader{};
      toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
      toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
      toShader.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
      toShader.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
      toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
      toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
      toShader.image = out.image.get();
      toShader.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      toShader.subresourceRange.levelCount = 1;
      toShader.subresourceRange.layerCount = 1;

      vk::DependencyInfo depToShader{};
      depToShader.imageMemoryBarrierCount = 1;
      depToShader.pImageMemoryBarriers = &toShader;
      cmd.pipelineBarrier2(depToShader);
    });
  };

  // areaTex: R8G8_UNORM, 160x560
  createLookupTexture(areaTexBytes, AREATEX_SIZE, static_cast<uint32_t>(AREATEX_WIDTH),
                      static_cast<uint32_t>(AREATEX_HEIGHT), vk::Format::eR8G8Unorm, m_smaaAreaTex);

  // searchTex: R8_UNORM, 64x16
  createLookupTexture(searchTexBytes, SEARCHTEX_SIZE, static_cast<uint32_t>(SEARCHTEX_WIDTH),
                      static_cast<uint32_t>(SEARCHTEX_HEIGHT), vk::Format::eR8Unorm, m_smaaSearchTex);
}

void VulkanRenderer::recordDeferredLighting(const RenderCtx &render, vk::Buffer uniformBuffer,
                                            const std::vector<DeferredLight> &lights) {
  vk::CommandBuffer cmd = render.cmd;
  Assertion(cmd, "recordDeferredLighting called with null command buffer");

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

  // Pipelines are cached by VulkanPipelineManager; we still build the key per frame since the render target contract
  // can vary.
  ShaderModules modules = m_shaderManager->getModules(shader_type::SDR_TYPE_DEFERRED_LIGHTING);

  static const vertex_layout deferredLayout = []() {
    vertex_layout layout{};
    layout.add_vertex_component(vertex_format_data::POSITION3, sizeof(float) * 3, 0); // Position only for volume meshes
    return layout;
  }();

  const auto &rt = render.targetInfo;
  PipelineKey key{};
  key.type = shader_type::SDR_TYPE_DEFERRED_LIGHTING;
  key.variant_flags = 0;
  key.color_format = static_cast<VkFormat>(rt.colorFormat);
  key.depth_format = static_cast<VkFormat>(rt.depthFormat);
  key.sample_count = static_cast<VkSampleCountFlagBits>(getSampleCount());
  key.color_attachment_count = rt.colorAttachmentCount;
  key.blend_mode = ALPHA_BLEND_ADDITIVE;
  key.layout_hash = deferredLayout.hash();

  // Ambient pipeline (no blend, overwrites undefined swapchain)
  PipelineKey ambientKey = key;
  ambientKey.blend_mode = ALPHA_BLEND_NONE;

  vk::Pipeline pipeline = m_pipelineManager->getPipeline(key, modules, deferredLayout);
  vk::Pipeline ambientPipeline = m_pipelineManager->getPipeline(ambientKey, modules, deferredLayout);

  DeferredDrawContext ctx{};
  ctx.cmd = cmd;
  ctx.layout = m_descriptorLayouts->deferredPipelineLayout();
  ctx.uniformBuffer = uniformBuffer;
  ctx.pipeline = pipeline;
  ctx.ambientPipeline = ambientPipeline;
  ctx.dynamicBlendEnable =
      m_vulkanDevice->supportsExtendedDynamicState3() && m_vulkanDevice->extDyn3Caps().colorBlendEnable;

  // Bind global (set=1) deferred descriptor set using the *deferred* pipeline layout.
  // Binding via the standard pipeline layout is not descriptor-set compatible because set 0 differs.
  cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, ctx.layout, 1, 1, &m_globalDescriptorSet, 0, nullptr);

  vk::Buffer fullscreenVB = m_bufferManager->getBuffer(m_fullscreenMesh.vbo);
  vk::Buffer sphereVB = m_bufferManager->getBuffer(m_sphereMesh.vbo);
  vk::Buffer sphereIB = m_bufferManager->getBuffer(m_sphereMesh.ibo);
  vk::Buffer cylinderVB = m_bufferManager->getBuffer(m_cylinderMesh.vbo);
  vk::Buffer cylinderIB = m_bufferManager->getBuffer(m_cylinderMesh.ibo);

  for (const auto &light : lights) {
    std::visit(
        [&](const auto &l) {
          using T = std::decay_t<decltype(l)>;
          if constexpr (std::is_same_v<T, FullscreenLight>) {
            l.record(ctx, fullscreenVB);
          } else if constexpr (std::is_same_v<T, SphereLight>) {
            l.record(ctx, sphereVB, sphereIB, m_sphereMesh.indexCount);
          } else if constexpr (std::is_same_v<T, CylinderLight>) {
            l.record(ctx, cylinderVB, cylinderIB, m_cylinderMesh.indexCount);
          }
        },
        light);
  }
  // Note: render pass ends at explicit session boundaries (target changes/frame end).
}

} // namespace vulkan
} // namespace graphics
