
#include "VulkanRenderer.h"
#include "VulkanClip.h"
#include "VulkanConstants.h"
#include "VulkanFrameCaps.h"
#include "VulkanFrameFlow.h"
#include "VulkanGraphics.h"
#include "VulkanMovieManager.h"
#include "VulkanSync2Helpers.h"
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

#include <SDL_video.h>

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

namespace {
vk::Extent2D querySwapchainRecreateExtent(const VulkanDevice &device) {
  auto extent = device.swapchainExtent();
  SDL_Window *window = os::getSDLMainWindow();
  if (!window) {
    return extent;
  }
  int width = 0;
  int height = 0;
  SDL_GetWindowSize(window, &width, &height);
  if (width > 0 && height > 0) {
    extent.width = static_cast<uint32_t>(width);
    extent.height = static_cast<uint32_t>(height);
  }
  return extent;
}
} // namespace

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
    const auto extent = querySwapchainRecreateExtent(*m_vulkanDevice);
    if (!m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
      return std::numeric_limits<uint32_t>::max();
    }
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

void VulkanRenderer::beginFrame(VulkanFrame &frame, uint32_t imageIndex) {
  Assertion(m_renderingSession != nullptr, "beginFrame requires an active rendering session");

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

  vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
  beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);

  // Update the per-frame global descriptor set once at the frame boundary.
  // Do not mutate it again mid-frame (descriptor sets are not UPDATE_AFTER_BIND).
  bindDeferredGlobalDescriptors(frame.globalDescriptorSet());

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

  vk::Semaphore renderFinished = m_vulkanDevice->swapchainRenderFinishedSemaphore(imageIndex);
  Assertion(renderFinished, "Missing render-finished semaphore for swapchain image %u", imageIndex);

  vk::SemaphoreSubmitInfo signalSemaphores[2];
  signalSemaphores[0].semaphore = renderFinished;
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

  auto presentResult = m_vulkanDevice->present(renderFinished, imageIndex);

  if (presentResult.needsRecreate) {
    const auto extent = querySwapchainRecreateExtent(*m_vulkanDevice);
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

std::optional<graphics::vulkan::RecordingFrame> VulkanRenderer::beginRecording() {
  auto af = acquireAvailableFrame();

  const uint32_t imageIndex = acquireImage(*af.frame);
  if (imageIndex == std::numeric_limits<uint32_t>::max()) {
    // Swapchain not ready (minimized/out-of-date) or acquisition failed; keep the frame available.
    m_availableFrames.push_front(af);
    return std::nullopt;
  }
  beginFrame(*af.frame, imageIndex);

  return graphics::vulkan::RecordingFrame{*af.frame, imageIndex};
}

std::optional<graphics::vulkan::RecordingFrame> VulkanRenderer::advanceFrame(graphics::vulkan::RecordingFrame prev) {
  endFrame(prev);
  auto submit = submitRecordedFrame(prev);

  m_inFlightFrames.emplace_back(prev.ref(), submit);

  logFrameCounters();
  m_frameModelDraws = 0;
  m_framePrimDraws = 0;

  return beginRecording();
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

} // namespace vulkan
} // namespace graphics
