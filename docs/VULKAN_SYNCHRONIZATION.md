# Vulkan Synchronization Infrastructure

This document provides a comprehensive analysis of the Vulkan synchronization mechanisms used in the FreeSpace Open engine. It covers frame synchronization, pipeline barriers, image layout transitions, and resource upload synchronization.

---

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Synchronization Files Reference](#synchronization-files-reference)
4. [Frame-in-Flight Configuration](#frame-in-flight-configuration)
5. [Synchronization Primitives](#synchronization-primitives)
6. [Swapchain Acquisition and Presentation](#swapchain-acquisition-and-presentation)
7. [Queue Submission](#queue-submission)
8. [Pipeline Barriers and Image Layout Transitions](#pipeline-barriers-and-image-layout-transitions)
9. [Resource Upload Synchronization](#resource-upload-synchronization)
10. [Queue Family Sharing Mode](#queue-family-sharing-mode)
11. [Device Waits](#device-waits)
12. [Synchronization Diagrams](#synchronization-diagrams)
13. [Debugging Synchronization Issues](#debugging-synchronization-issues)
14. [Known Issues and Recommendations](#known-issues-and-recommendations)

---

## Overview

The Vulkan backend implements a multi-frame-in-flight rendering architecture using Vulkan 1.4 synchronization primitives for fine-grained control over pipeline stages and memory access patterns. The key synchronization mechanisms are:

| Mechanism | Purpose | Primary Location |
|-----------|---------|------------------|
| Binary Semaphores | GPU-GPU sync (acquire/present) | `VulkanFrame.cpp`, `VulkanDevice.cpp` |
| Timeline Semaphore | Serial-based GPU completion tracking | `VulkanRenderer.cpp` |
| Fences | CPU-GPU sync (frame reuse) | `VulkanFrame.cpp` |
| Pipeline Barriers | Execution/memory dependencies | `VulkanRenderingSession.cpp`, `VulkanTextureManager.cpp` |
| Image Layout Transitions | Access pattern changes | `VulkanRenderingSession.cpp` |
| Queue Waits | Immediate synchronization | `VulkanBufferManager.cpp`, `VulkanTextureManager.cpp` |

---

## Prerequisites

This document assumes familiarity with:

- **Vulkan pipeline stages**: The discrete phases of GPU execution (vertex input, fragment shader, color attachment output, etc.)
- **Memory barriers**: Mechanisms to ensure memory writes are visible to subsequent reads
- **Image layouts**: Vulkan's requirement that images be in specific layouts for different operations
- **Semaphores vs. Fences**: Semaphores synchronize GPU-GPU operations; fences synchronize CPU-GPU operations

Key Vulkan 1.4 features used:

- **VK_KHR_synchronization2** (core in 1.3): Provides `vkCmdPipelineBarrier2` with explicit stage/access masks in barrier structures
- **VK_KHR_timeline_semaphore** (core in 1.2): Semaphores with monotonically increasing counter values for serial tracking
- **VK_KHR_dynamic_rendering** (core in 1.3): Renderpass-less rendering with `vkCmdBeginRendering`
- **VK_KHR_push_descriptor** (core in 1.4): Per-draw descriptor updates without pre-allocated pools

---

## Synchronization Files Reference

| File | Key Synchronization Content |
|------|----------------------------|
| `VulkanConstants.h` | `kFramesInFlight = 2` constant |
| `VulkanFrame.h` | Per-frame fence, imageAvailable semaphore, per-frame timeline semaphore (unused) |
| `VulkanFrame.cpp` | Sync primitive creation, `wait_for_gpu()` implementation |
| `VulkanDevice.h/cpp` | Per-swapchain-image renderFinished semaphores, acquire/present, swapchain recreation |
| `VulkanRenderer.cpp` | Global timeline semaphore, frame submission, frame recycling |
| `VulkanRenderingSession.cpp` | `stageAccessForLayout()` helper, all image layout transitions |
| `VulkanBufferManager.cpp` | Buffer upload with pipeline barriers and fence wait |
| `VulkanTextureManager.cpp` | Texture upload barriers, immediate and frame-buffered |

---

## Frame-in-Flight Configuration

**File:** `VulkanConstants.h`
```cpp
constexpr uint32_t kFramesInFlight = 2;
```

The engine uses 2 frames in flight with ring-buffered resources. This configuration allows the CPU to record commands for frame N+1 while the GPU executes frame N, achieving CPU-GPU parallelism.

**Design Rationale:**
- 2 frames provides a good balance between latency and throughput
- Fewer frames reduce input latency; more frames improve GPU utilization
- Ring buffers (uniform, vertex, staging) are sized to accommodate 2 frames of data

**Frame Pool Management:** `VulkanRenderer::createFrames()`
```cpp
void VulkanRenderer::createFrames() {
  const auto& props = m_vulkanDevice->properties();
  m_availableFrames.clear();
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    vk::DescriptorSet globalSet = m_descriptorLayouts->allocateGlobalDescriptorSet();
    vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();

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
      globalSet,
      modelSet);

    m_availableFrames.push_back(AvailableFrame{ m_frames[i].get(), m_completedSerial });
  }
}
```

---

## Synchronization Primitives

### Per-Frame Fence

**File:** `VulkanFrame.cpp`
```cpp
vk::FenceCreateInfo fenceInfo;
fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled; // allow first frame without waiting
m_inflightFence = m_device.createFenceUnique(fenceInfo);
```

The fence is created in **signaled state** to allow the first frame to proceed without blocking. On subsequent frames, `wait_for_gpu()` blocks until the fence signals, ensuring the frame's resources are no longer in use by the GPU before recycling.

**Fence Wait Implementation:**
```cpp
void VulkanFrame::wait_for_gpu()
{
  auto fence = m_inflightFence.get();
  if (!fence) {
    return;
  }

  auto result = m_device.waitForFences(1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
  if (result != vk::Result::eSuccess) {
    Assertion(false, "Fence wait failed for Vulkan frame");
    throw std::runtime_error("Fence wait failed for Vulkan frame");
  }
}
```

### Binary Semaphores

The binary semaphores are split across two locations based on their lifecycle requirements:

**Per-Frame `imageAvailable` Semaphore** (File: `VulkanFrame.cpp`)
```cpp
vk::SemaphoreCreateInfo binaryInfo;
m_imageAvailable = m_device.createSemaphoreUnique(binaryInfo);
```

**Per-Swapchain-Image `renderFinished` Semaphores** (File: `VulkanDevice.cpp`)
```cpp
// Render-finished semaphores are indexed by swapchain image to avoid reuse hazards with presentation.
m_swapchainRenderFinishedSemaphores.reserve(m_swapchainImages.size());
vk::SemaphoreCreateInfo semInfo{};
for (size_t i = 0; i < m_swapchainImages.size(); ++i) {
  m_swapchainRenderFinishedSemaphores.push_back(m_device->createSemaphoreUnique(semInfo));
}
```

| Semaphore | Owned By | Indexed By | Signaled By | Waited By | Purpose |
|-----------|----------|------------|-------------|-----------|---------|
| `imageAvailable` | VulkanFrame | Frame index | `vkAcquireNextImageKHR` | Queue submission | Ensures swapchain image is ready for rendering |
| `renderFinished` | VulkanDevice | Swapchain image index | Queue submission | `vkQueuePresentKHR` | Ensures rendering completes before presentation |

**Design Rationale for Per-Swapchain-Image renderFinished:**

The renderFinished semaphore must be indexed by swapchain image index rather than frame index because:
1. A swapchain image may be acquired before the previous frame using that same image has presented
2. Reusing a semaphore that is still waited on by the presentation engine causes undefined behavior
3. By indexing on swapchain image, the semaphore lifecycle is tied to the image lifecycle, avoiding reuse hazards

### Per-Frame Timeline Semaphore (Reserved for Future Use)

**File:** `VulkanFrame.cpp`
```cpp
vk::SemaphoreTypeCreateInfo timelineType;
timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
timelineType.initialValue = m_timelineValue;

vk::SemaphoreCreateInfo semaphoreInfo;
semaphoreInfo.pNext = &timelineType;
m_timelineSemaphore = m_device.createSemaphoreUnique(semaphoreInfo);
```

**Note:** Each frame creates a timeline semaphore with associated tracking methods (`currentTimelineValue()`, `nextTimelineValue()`, `advanceTimeline()`), but the current implementation uses only the **global** timeline semaphore in `VulkanRenderer`. The per-frame semaphores are reserved for potential future use cases such as per-frame dependency tracking or parallel command buffer submission.

### Global Timeline Semaphore

**File:** `VulkanRenderer.cpp`
```cpp
void VulkanRenderer::createSubmitTimelineSemaphore() {
  vk::SemaphoreTypeCreateInfo timelineType;
  timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
  timelineType.initialValue = 0;

  vk::SemaphoreCreateInfo semaphoreInfo;
  semaphoreInfo.pNext = &timelineType;
  m_submitTimeline = m_vulkanDevice->device().createSemaphoreUnique(semaphoreInfo);
  Assertion(m_submitTimeline, "Failed to create submit timeline semaphore");
}
```

**Purpose:** Tracks GPU completion via monotonically increasing serial numbers. Each frame submission increments `m_submitSerial` and signals the timeline semaphore with that value. Resource managers use this serial to determine when deferred deletions are safe.

**Query Implementation:**
```cpp
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
```

---

## Swapchain Acquisition and Presentation

### Image Acquisition

**File:** `VulkanDevice.cpp`
```cpp
VulkanDevice::AcquireResult VulkanDevice::acquireNextImage(vk::Semaphore imageAvailable) {
  AcquireResult result;
  uint32_t imageIndex = std::numeric_limits<uint32_t>::max();

  vk::Result res = vk::Result::eSuccess;
  try {
    res = m_device->acquireNextImageKHR(
      m_swapchain.get(),
      std::numeric_limits<uint64_t>::max(), // infinite timeout
      imageAvailable,                        // semaphore to signal
      nullptr,                               // no fence
      &imageIndex);
  } catch (const vk::SystemError& err) {
    res = static_cast<vk::Result>(err.code().value());
  }

  if (res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR) {
    result.needsRecreate = true;
    result.success = (res == vk::Result::eSuboptimalKHR);
    result.imageIndex = imageIndex;
    return result;
  }
  // ...
}
```

**Synchronization Flow:**
1. `vkAcquireNextImageKHR` signals `imageAvailable` when the presentation engine releases the image
2. No fence is used (only semaphore-based GPU-GPU sync)
3. Handles `VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` for swapchain recreation

**Important:** The acquire operation may return before the image is actually available. The semaphore ensures the GPU waits for the presentation engine to release the image before writing to it.

### Queue Presentation

**File:** `VulkanDevice.cpp`
```cpp
VulkanDevice::PresentResult VulkanDevice::present(vk::Semaphore renderFinished, uint32_t imageIndex) {
  PresentResult result;

  vk::PresentInfoKHR presentInfo;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pWaitSemaphores = &renderFinished;  // wait for rendering to complete
  presentInfo.swapchainCount = 1;
  auto swapchain = m_swapchain.get();
  presentInfo.pSwapchains = &swapchain;
  presentInfo.pImageIndices = &imageIndex;

  vk::Result presentResult = vk::Result::eSuccess;
  try {
    presentResult = m_presentQueue.presentKHR(presentInfo);
  } catch (const vk::SystemError& err) {
    presentResult = static_cast<vk::Result>(err.code().value());
  }
  // ...
}
```

**Key Points:**
- Presentation waits on `renderFinished` to ensure all rendering commands complete
- Presentation may occur on a different queue than graphics (see Queue Family Sharing Mode)

---

## Queue Submission

### Frame Submission

**File:** `VulkanRenderer.cpp`

The engine uses `vkQueueSubmit2` (Synchronization2) for frame submission:

```cpp
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

  // Wait for swapchain image to be available at COLOR_ATTACHMENT_OUTPUT stage
  vk::SemaphoreSubmitInfo waitSemaphore;
  waitSemaphore.semaphore = frame.imageAvailable();
  waitSemaphore.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

  // renderFinished is per-swapchain-image (not per-frame) to avoid reuse hazards
  vk::Semaphore renderFinished = m_vulkanDevice->swapchainRenderFinishedSemaphore(imageIndex);
  Assertion(renderFinished, "Missing render-finished semaphore for swapchain image %u", imageIndex);

  // Signal both renderFinished (for present) and timeline (for serial tracking)
  vk::SemaphoreSubmitInfo signalSemaphores[2];
  signalSemaphores[0].semaphore = renderFinished;
  signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

  signalSemaphores[1].semaphore = m_submitTimeline.get();
  signalSemaphores[1].value = m_submitSerial + 1;
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

  // Notify resource managers of the new safe-retire serial
  if (m_textureManager) {
    m_textureManager->setSafeRetireSerial(m_submitSerial);
  }
  if (m_bufferManager) {
    m_bufferManager->setSafeRetireSerial(m_submitSerial);
  }

  // Submit with fence for CPU-GPU sync
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);

  // Present immediately after submission
  auto presentResult = m_vulkanDevice->present(renderFinished, imageIndex);
  // ...
}
```

**Semaphore Signal Stages:**
| Semaphore | Signal Stage | Rationale |
|-----------|--------------|-----------|
| `renderFinished` | `eColorAttachmentOutput` | Presentation needs only swapchain writes complete |
| `m_submitTimeline` | `eAllCommands` | Resource release needs all GPU work complete |

### Frame Recycling with Fence Wait

**File:** `VulkanRenderer.cpp`
```cpp
void VulkanRenderer::recycleOneInFlight()
{
  graphics::vulkan::InFlightFrame inflight = std::move(m_inFlightFrames.front());
  m_inFlightFrames.pop_front();

  VulkanFrame& f = inflight.ref();

  // Block CPU until GPU completes this frame
  f.wait_for_gpu();

  // Query timeline for completed serial and update cache
  const uint64_t completed = queryCompletedSerial();
  m_completedSerial = std::max(m_completedSerial, completed);

  Assertion(m_completedSerial >= inflight.submit.serial,
    "Completed serial must be >= recycled submission serial");

  // Release deferred resources and reset frame state
  prepareFrameForReuse(f, m_completedSerial);

  m_availableFrames.push_back(AvailableFrame{ &f, m_completedSerial });
}
```

**Flow:**
1. Pop oldest in-flight frame (FIFO order)
2. Block on fence until GPU signals completion
3. Query timeline semaphore for actual completed serial
4. Trigger deferred resource collection (textures, buffers)
5. Reset command pool and ring buffers
6. Return frame to available pool

---

## Pipeline Barriers and Image Layout Transitions

### stageAccessForLayout() Helper

**File:** `VulkanRenderingSession.cpp`

This utility function maps image layouts to their corresponding pipeline stage and access masks for barrier construction:

```cpp
namespace {
struct StageAccess {
  vk::PipelineStageFlags2 stageMask{};
  vk::AccessFlags2 accessMask{};
};

StageAccess stageAccessForLayout(vk::ImageLayout layout)
{
  StageAccess out{};
  switch (layout) {
  case vk::ImageLayout::eUndefined:
    out.stageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
    out.accessMask = {};
    break;
  case vk::ImageLayout::eColorAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
    out.accessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                     vk::AccessFlagBits2::eColorAttachmentWrite;
    break;
  case vk::ImageLayout::eDepthAttachmentOptimal:
  case vk::ImageLayout::eDepthStencilAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                    vk::PipelineStageFlagBits2::eLateFragmentTests;
    out.accessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                     vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    break;
  case vk::ImageLayout::eShaderReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::eTransferSrcOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferRead;
    break;
  case vk::ImageLayout::eTransferDstOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
    out.accessMask = vk::AccessFlagBits2::eTransferWrite;
    break;
  case vk::ImageLayout::eDepthStencilReadOnlyOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
    out.accessMask = vk::AccessFlagBits2::eShaderRead;
    break;
  case vk::ImageLayout::ePresentSrcKHR:
    // Present is external to the pipeline - use empty masks for release barrier
    out.stageMask = {};
    out.accessMask = {};
    break;
  default:
    // Conservative fallback: synchronize with all stages and access types
    out.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    out.accessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    break;
  }
  return out;
}
} // anonymous namespace
```

### Swapchain Layout Tracking

**File:** `VulkanRenderingSession.cpp`
```cpp
// Constructor initialization
m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
```

The `m_swapchainLayouts` vector tracks the current layout of each swapchain image, enabling correct `oldLayout` specification in barriers. This is essential because:
- Swapchain images start as `eUndefined` after creation or recreation
- Layout must be tracked across frames (images cycle through the swapchain)
- Dynamic rendering requires explicit layout transitions (unlike renderpasses)

### Swapchain to Attachment Transition

**File:** `VulkanRenderingSession.cpp`
```cpp
void VulkanRenderingSession::transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex) {
  Assertion(imageIndex < m_swapchainLayouts.size(),
    "imageIndex out of bounds");

  vk::ImageMemoryBarrier2 toRender{};
  toRender.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
  toRender.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  toRender.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
  toRender.oldLayout = m_swapchainLayouts[imageIndex];
  toRender.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
  toRender.image = m_device.swapchainImage(imageIndex);
  toRender.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toRender.subresourceRange.levelCount = 1;
  toRender.subresourceRange.layerCount = 1;

  vk::DependencyInfo depInfo{};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &toRender;
  cmd.pipelineBarrier2(depInfo);

  m_swapchainLayouts[imageIndex] = vk::ImageLayout::eColorAttachmentOptimal;
}
```

**Key Points:**
- Source stage is `eTopOfPipe` (no prior GPU work to wait for)
- No source access mask (nothing to make visible)
- Destination ensures color attachment writes are synchronized

### Swapchain to Present Transition

**File:** `VulkanRenderingSession.cpp`
```cpp
void VulkanRenderingSession::transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex) {
  Assertion(imageIndex < m_swapchainLayouts.size(),
    "imageIndex out of bounds");

  vk::ImageMemoryBarrier2 toPresent{};
  toPresent.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  toPresent.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
  // Present is external - this is a release barrier
  toPresent.dstStageMask = {};
  toPresent.dstAccessMask = {};
  toPresent.oldLayout = m_swapchainLayouts[imageIndex];
  toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
  toPresent.image = m_device.swapchainImage(imageIndex);
  toPresent.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  toPresent.subresourceRange.levelCount = 1;
  toPresent.subresourceRange.layerCount = 1;

  vk::DependencyInfo depInfo{};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &toPresent;
  cmd.pipelineBarrier2(depInfo);

  m_swapchainLayouts[imageIndex] = vk::ImageLayout::ePresentSrcKHR;
}
```

**Release Barrier Pattern:** The empty destination stage/access masks indicate this is a release barrier. The presentation engine (external to the Vulkan pipeline) will acquire the image; semaphore synchronization handles the cross-queue dependency.

### Depth Attachment Transition

**File:** `VulkanRenderingSession.cpp`
```cpp
void VulkanRenderingSession::transitionDepthToAttachment(vk::CommandBuffer cmd) {
  vk::ImageMemoryBarrier2 toDepth{};
  const auto oldLayout = m_targets.depthLayout();
  const auto newLayout = m_targets.depthAttachmentLayout();
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  toDepth.srcStageMask = src.stageMask;
  toDepth.srcAccessMask = src.accessMask;
  toDepth.dstStageMask = dst.stageMask;
  toDepth.dstAccessMask = dst.accessMask;
  toDepth.oldLayout = oldLayout;
  toDepth.newLayout = newLayout;
  toDepth.image = m_targets.depthImage();
  toDepth.subresourceRange.aspectMask = m_targets.depthAttachmentAspectMask();
  toDepth.subresourceRange.levelCount = 1;
  toDepth.subresourceRange.layerCount = 1;

  vk::DependencyInfo depInfo{};
  depInfo.imageMemoryBarrierCount = 1;
  depInfo.pImageMemoryBarriers = &toDepth;
  cmd.pipelineBarrier2(depInfo);
  m_targets.setDepthLayout(newLayout);
}
```

**Aspect Mask Note:** The depth attachment aspect mask is queried from `VulkanRenderTargets`, which returns `eDepth | eStencil` for combined depth-stencil formats or `eDepth` for depth-only formats.

### G-Buffer Transitions

The deferred rendering pipeline requires transitioning G-buffer attachments between writable (for geometry pass) and readable (for lighting pass) states.

**To Attachment (Writable):** `VulkanRenderingSession.cpp`
```cpp
void VulkanRenderingSession::transitionGBufferToAttachment(vk::CommandBuffer cmd) {
  std::array<vk::ImageMemoryBarrier2, VulkanRenderTargets::kGBufferCount> barriers{};
  for (uint32_t i = 0; i < VulkanRenderTargets::kGBufferCount; ++i) {
    const auto oldLayout = m_targets.gbufferLayout(i);
    const auto newLayout = vk::ImageLayout::eColorAttachmentOptimal;
    const auto src = stageAccessForLayout(oldLayout);
    const auto dst = stageAccessForLayout(newLayout);
    barriers[i].srcStageMask = src.stageMask;
    barriers[i].srcAccessMask = src.accessMask;
    barriers[i].dstStageMask = dst.stageMask;
    barriers[i].dstAccessMask = dst.accessMask;
    barriers[i].oldLayout = oldLayout;
    barriers[i].newLayout = newLayout;
    barriers[i].image = m_targets.gbufferImage(i);
    barriers[i].subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barriers[i].subresourceRange.levelCount = 1;
    barriers[i].subresourceRange.layerCount = 1;
  }

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = VulkanRenderTargets::kGBufferCount;
  dep.pImageMemoryBarriers = barriers.data();
  cmd.pipelineBarrier2(dep);
  // Update layout tracking...
}
```

**Batch Barrier Optimization:** All G-buffer images are transitioned in a single `pipelineBarrier2` call, allowing the driver to optimize the synchronization.

### Post-Processing Target Transitions

The post-processing pipeline involves several intermediate render targets that cycle between attachment and shader-read layouts:

| Target | Layouts Used | Purpose |
|--------|--------------|---------|
| Scene HDR | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | HDR scene rendering and tonemapping input |
| Scene Effect | `eTransferDstOptimal` / `eShaderReadOnlyOptimal` | Distortion/effect snapshot |
| Post LDR | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | Post-tonemap processing |
| Post Luminance | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | Luminance calculation |
| SMAA Edges | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | Edge detection output |
| SMAA Blend | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | Blend weight calculation |
| SMAA Output | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | Final AA output |
| Bloom (ping-pong) | `eColorAttachmentOptimal` / `eShaderReadOnlyOptimal` | Bloom mip chain |

Each target has a dedicated transition method following the same `stageAccessForLayout` pattern.

---

## Resource Upload Synchronization

### Buffer Uploads (Device-Local)

**File:** `VulkanBufferManager.cpp`

The buffer manager uses a **synchronous staging pattern** with immediate fence wait for device-local buffer uploads:

```cpp
void VulkanBufferManager::uploadToDeviceLocal(const VulkanBuffer& buffer,
    vk::DeviceSize dstOffset, vk::DeviceSize size, const void* data)
{
  // 1. Create transient staging buffer (host-visible, coherent)
  // 2. Copy data to staging buffer
  // 3. Record copy command with buffer barrier

  vk::BufferMemoryBarrier2 barrier{};
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  barrier.dstStageMask = dstStage;  // varies by buffer type
  barrier.dstAccessMask = dstAccess;
  barrier.buffer = buffer.buffer.get();
  barrier.offset = dstOffset;
  barrier.size = size;

  vk::DependencyInfo depInfo{};
  depInfo.bufferMemoryBarrierCount = 1;
  depInfo.pBufferMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(depInfo);
  cmd.end();

  // 4. Submit with fence and wait synchronously
  vk::FenceCreateInfo fenceInfo{};
  auto fence = m_device.createFenceUnique(fenceInfo);
  m_transferQueue.submit(submit, fence.get());

  const auto waitResult = m_device.waitForFences(1, &fenceHandle, VK_TRUE,
      std::numeric_limits<uint64_t>::max());
  Assertion(waitResult == vk::Result::eSuccess, "Failed waiting for buffer upload fence");
}
```

**Buffer Type Stage/Access Mapping:**

| Buffer Type | Destination Stage | Destination Access |
|-------------|-------------------|-------------------|
| Vertex | `eVertexInput \| eVertexShader` | `eVertexAttributeRead \| eShaderRead` |
| Index | `eVertexInput` | `eIndexRead` |
| Uniform | `eVertexShader \| eFragmentShader` | `eUniformRead` |

### Texture Uploads (Immediate)

**File:** `VulkanTextureManager.cpp`

Immediate texture uploads (e.g., solid color fallback textures) use queue-level idle wait:

```cpp
// After recording layout transition and copy commands...
vk::SubmitInfo submitInfo;
submitInfo.commandBufferCount = 1;
submitInfo.pCommandBuffers = &cmdBuf;
m_transferQueue.submit(submitInfo, nullptr);
m_transferQueue.waitIdle();  // Simple synchronization for one-time operations
```

**Trade-off:** `waitIdle()` is simpler but stalls all transfer queue work. Acceptable for initialization-time uploads but not for streaming.

### Texture Uploads (Frame-Buffered)

For streaming textures during gameplay, uploads use in-frame barriers recorded in the main command buffer:

```cpp
// Transition to transfer destination
vk::ImageMemoryBarrier2 toTransfer{};
toTransfer.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
toTransfer.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
toTransfer.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
toTransfer.oldLayout = vk::ImageLayout::eUndefined;
toTransfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
// ...
cmd.pipelineBarrier2(depToTransfer);

// Copy from per-frame staging ring buffer
cmd.copyBufferToImage(frame.stagingBuffer().buffer(), /* ... */);

// Transition to shader read
vk::ImageMemoryBarrier2 toShader{};
toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
toShader.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
toShader.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
// ...
cmd.pipelineBarrier2(depToShader);
```

**Advantages:**
- No CPU stall; upload overlaps with other GPU work
- Uses per-frame staging ring buffer (recycled with frame)
- Barriers ensure texture is ready before fragment shader reads

### Upload Phase Context Token

The `UploadCtx` capability token proves that the frame is in the upload phase (after `beginFrame()` but before rendering starts). Texture uploads via `flushPendingUploads(const UploadCtx&)` are constrained to this phase:

```cpp
const UploadCtx uploadCtx{frame, cmd, m_frameCounter};
m_textureUploader->flushPendingUploads(uploadCtx);
```

This token-based design enforces that texture uploads cannot occur during active rendering, preventing mid-render-pass transfer commands.

### Movie Texture Uploads

Movie textures (YUV420 planar format) can upload mid-frame. The renderer must suspend dynamic rendering before issuing transfer commands:

```cpp
// VulkanRenderer::uploadMovieTexture suspends rendering before transfer
m_renderingSession->endActivePass();  // End dynamic rendering if active
// ... record transfer commands ...
```

This ensures transfers occur outside of dynamic rendering passes where they are invalid.

---

## Queue Family Sharing Mode

**File:** `VulkanDevice.cpp`

Swapchain images are configured based on whether graphics and present queues belong to the same queue family:

```cpp
const uint32_t queueFamilyIndices[] = {
    deviceValues.graphicsQueueIndex.index,
    deviceValues.presentQueueIndex.index
};

if (deviceValues.graphicsQueueIndex.index != deviceValues.presentQueueIndex.index) {
  // Different queue families: use concurrent sharing mode
  createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
  createInfo.queueFamilyIndexCount = 2;
  createInfo.pQueueFamilyIndices = queueFamilyIndices;
} else {
  // Same queue family: use exclusive mode
  createInfo.imageSharingMode = vk::SharingMode::eExclusive;
  createInfo.queueFamilyIndexCount = 0;
  createInfo.pQueueFamilyIndices = nullptr;
}
```

**Sharing Mode Implications:**

| Mode | When Used | Barrier Requirements | Performance |
|------|-----------|---------------------|-------------|
| `eExclusive` | Graphics == Present queue | No queue family ownership transfers | Optimal |
| `eConcurrent` | Graphics != Present queue | Automatic ownership handling | Slight overhead |

**Note:** Most desktop GPUs use the same queue family for graphics and present, resulting in exclusive mode. Concurrent mode is a fallback for hardware with separate presentation queues.

---

## Device Waits

The following locations use blocking device/queue waits:

| Location | Wait Type | Purpose |
|----------|-----------|---------|
| `VulkanRenderer.cpp` | `device().waitSemaphores()` (timeline) | `submitInitCommandsAndWait()` - blocking init command completion |
| `VulkanRenderer.cpp` | `device().waitIdle()` | Shutdown synchronization before resource destruction |
| `VulkanDevice.cpp` | `device->waitIdle()` | Pre-destruction cleanup |
| `VulkanDevice.cpp` | `device->waitIdle()` | Pre-swapchain recreation (ensure images not in use) |
| `VulkanBufferManager.cpp` | `waitForFences()` | Buffer upload completion |
| `VulkanTextureManager.cpp` | `transferQueue.waitIdle()` | Solid texture creation |
| `VulkanTextureManager.cpp` | `transferQueue.waitIdle()` | Render target clear |

**Best Practices:**
- Prefer fence waits over `waitIdle()` when possible (more targeted)
- Timeline semaphore waits (`waitSemaphores`) are efficient for serial-based sync
- Queue/device idle waits are acceptable for initialization and shutdown paths

---

## Synchronization Diagrams

### Frame Submission Pipeline

```
CPU Timeline:
  Frame N:   [Record]---[Submit]---------------------------[Recycle]
  Frame N+1:            [Acquire]---[Record]---[Submit]----[...]

GPU Timeline:
  Frame N:              [Execute]---------[Present]
  Frame N+1:                      [Wait]--[Execute]--[Present]

Synchronization Points:
  (1) imageAvailable semaphore -----> Wait at eColorAttachmentOutput
  (2) renderFinished semaphore -----> Signal at eColorAttachmentOutput, wait by Present
  (3) inflightFence ----------------> Signal on submit completion, CPU waits before recycle
  (4) m_submitTimeline -------------> Signal at eAllCommands with serial N
```

### Semaphore and Fence Flow

```
vkAcquireNextImageKHR
         |
         | signals imageAvailable (per-frame)
         v
+------------------+
| Queue Submit     |
|   wait: imageAvailable @ COLOR_ATTACHMENT_OUTPUT
|   signal: renderFinished @ COLOR_ATTACHMENT_OUTPUT (per-swapchain-image)
|   signal: timeline @ ALL_COMMANDS (value = serial)
|   signal: fence (per-frame)
+------------------+
         |
         | signals renderFinished
         v
vkQueuePresentKHR
   wait: renderFinished
         |
         | (next frame)
         v
recycleOneInFlight()
   wait: fence (CPU blocks)
   query: timeline counter
```

### Image Layout State Machine (Swapchain)

```
                    +-------------------+
                    |    eUndefined     |
                    | (creation/resize) |
                    +--------+----------+
                             |
                             v transitionSwapchainToAttachment()
              +-----------------------------+
              | eColorAttachmentOptimal     |<---+
              | (rendering)                 |    |
              +-----------------------------+    |
                             |                   |
                             v transitionSwapchainToPresent()
              +-----------------------------+    | mid-frame
              | ePresentSrcKHR              |    | target switch
              | (presentation)              |    | (back to swapchain)
              +-----------------------------+    |
                             |                   |
                             +-------------------+
                               next frame acquire
```

### Image Layout State Machine (Scene HDR)

```
              +-------------------+
              |    eUndefined     |
              | (creation/resize) |
              +--------+----------+
                       |
                       v transitionSceneHdrToLayout(eColorAttachmentOptimal)
              +-----------------------------+
              | eColorAttachmentOptimal     |<--+
              | (scene rendering)           |   |
              +-----------------------------+   |
                       |                        |
                       v transitionSceneHdrToLayout(eShaderReadOnlyOptimal)
              +-----------------------------+   |
              | eShaderReadOnlyOptimal      |   |
              | (tonemapping input)         |   |
              +-----------------------------+   |
                       |                        |
                       +------------------------+
                         next scene begin
```

### Deferred Rendering Synchronization

```
1. G-Buffer Pass:
   [G-Buffer: eColorAttachmentOptimal] + [Depth: eDepthAttachmentOptimal]
                     |
                     v transitionGBufferToShaderRead()
   [G-Buffer: eShaderReadOnlyOptimal] + [Depth: eDepthStencilReadOnlyOptimal]
                     |
2. Lighting Pass:   v (read G-buffer + depth as textures)
   [Swapchain: eColorAttachmentOptimal] (no depth)
                     |
                     v transitionSwapchainToPresent()
3. Present:
   [Swapchain: ePresentSrcKHR]
```

---

## Debugging Synchronization Issues

### Common Symptoms and Causes

| Symptom | Likely Cause | Investigation |
|---------|--------------|---------------|
| Flickering/corruption | Missing barrier or wrong layout | Enable validation layers; check `oldLayout` matches actual state |
| Torn frames | Semaphore misconfiguration | Verify acquire/present semaphore usage |
| Stalls/hangs | Fence not signaled or wrong fence waited | Check fence creation flags and reset before submit |
| Validation errors about layout | Layout tracking out of sync | Audit all transitions; ensure `setXxxLayout()` called |
| Black screen after resize | Swapchain layouts not reset | Verify `m_swapchainLayouts` cleared on recreation |

### Validation Layer Messages

Enable `VK_LAYER_KHRONOS_validation` to catch synchronization errors. Key message patterns:

- **"SYNC-HAZARD-WRITE-AFTER-READ"**: Missing barrier between read and subsequent write
- **"SYNC-HAZARD-READ-AFTER-WRITE"**: Missing barrier between write and subsequent read
- **"invalid image layout"**: Barrier `oldLayout` doesn't match actual layout
- **"image layout should be X instead of Y"**: Image accessed in wrong layout

### RenderDoc Integration

The engine includes RenderDoc integration (`VulkanDebug.h`, `renderdoc.cpp`) for capture-based debugging:

1. Barriers and layout transitions appear as "Pipeline Barriers" in the event list
2. Use "Resource Inspector" to verify image layouts at each point
3. Timeline view shows synchronization across command buffers

---

## Known Issues and Recommendations

### Issue 1: renderFinished Signal Stage

**Location:** `VulkanRenderer.cpp`
```cpp
signalSemaphores[0].semaphore = renderFinished;
signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
```

**Analysis:** The `renderFinished` semaphore is signaled at `eColorAttachmentOutput`. This is correct because:
1. The final swapchain transition barrier waits on `eColorAttachmentOutput`
2. All rendering to the swapchain must complete before the transition

However, if late-stage compute work (e.g., async compute for next frame) is added in the future, this would need revision.

**Status:** Correct for current architecture. Document if async compute is introduced.

### Issue 2: Identical Preprocessor Branches

**Location:** `VulkanRenderer.cpp`
```cpp
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
#else
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
#endif
```

**Problem:** Both branches execute identical code. The intention was likely to differentiate error handling.

**Recommendation:** Remove the conditional or implement proper differentiation:
```cpp
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  auto result = m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
  if (result != vk::Result::eSuccess) {
    throw std::runtime_error("Queue submission failed");
  }
#else
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);  // Throws on failure
#endif
```

### Issue 3: Unused Per-Frame Timeline Semaphore

**Location:** `VulkanFrame.cpp`

Each frame creates a timeline semaphore that is currently unused. Only the global `m_submitTimeline` is active.

**Options:**
1. **Remove:** Eliminate per-frame timeline semaphores if no future use is planned
2. **Document:** Add comment explaining intended future use (e.g., parallel command buffer submission)
3. **Utilize:** Implement per-frame tracking for more granular resource management

**Current Status:** Kept for potential future per-frame dependency tracking.

### Issue 4: Frame Buffering Trade-off

**Location:** `VulkanConstants.h`

With only 2 frames in flight, CPU may block waiting for GPU completion if frame times vary significantly.

**Trade-offs:**

| Frames | Latency | GPU Utilization | Memory |
|--------|---------|-----------------|--------|
| 2 | Lower | May stall on complex frames | Lower |
| 3 | Higher | Better pipelining | Higher |

**Recommendation:** The current 2-frame configuration is appropriate for a responsive gaming experience. Consider making this configurable if users report stuttering on variable-workload scenes.

### Issue 5: Synchronous Buffer Uploads

**Location:** `VulkanBufferManager.cpp`

Device-local buffer uploads use immediate fence waits, which stall the CPU during the upload.

**Current Behavior:** Acceptable for:
- Initial resource loading
- Infrequent large updates

**Potential Improvement:** For frequently-updated large buffers:
- Queue uploads for execution at frame boundaries
- Use timeline semaphores to track completion
- Double-buffer device-local resources if needed

---

## Summary

The Vulkan synchronization infrastructure implements a robust 2-frame-in-flight model with:

- **Binary semaphores** for GPU-GPU synchronization:
  - Per-frame `imageAvailable` for swapchain acquisition
  - Per-swapchain-image `renderFinished` for presentation (avoids reuse hazards)
- **Per-frame fences** for CPU-GPU synchronization during frame recycling
- **Global timeline semaphore** for serial-based deferred resource release tracking
- **Synchronization2 barriers** (`vkCmdPipelineBarrier2`) for all image layout transitions with explicit stage/access masks
- **Per-image layout tracking** to specify correct source layouts in barriers
- **Immediate staging uploads** with fence-based synchronization for device-local resources
- **Frame-buffered uploads** with in-command-buffer barriers for streaming textures
- **Capability tokens** (`UploadCtx`) to constrain operations to valid frame phases

The implementation uses Vulkan 1.4 as the minimum required version, with `VK_KHR_synchronization2`, `VK_KHR_timeline_semaphore`, `VK_KHR_dynamic_rendering`, and `VK_KHR_push_descriptor` (all core in 1.4). All barriers use the `VkImageMemoryBarrier2` / `VkBufferMemoryBarrier2` structures with explicit pipeline stages, enabling precise dependency tracking and optimal driver scheduling.

---

## Appendix: Quick Reference

### Common Barrier Patterns

**Transfer to Shader Read:**
```cpp
barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
```

**Attachment to Shader Read:**
```cpp
barrier.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
barrier.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
barrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
```

**Shader Read to Attachment:**
```cpp
barrier.srcStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
barrier.srcAccessMask = vk::AccessFlagBits2::eShaderRead;
barrier.dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
barrier.dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
```

### Subresource Range Defaults

```cpp
subresourceRange.baseMipLevel = 0;
subresourceRange.levelCount = 1;       // Or VK_REMAINING_MIP_LEVELS
subresourceRange.baseArrayLayer = 0;
subresourceRange.layerCount = 1;       // Or VK_REMAINING_ARRAY_LAYERS
```

### Fence Lifecycle

```cpp
// Creation (signaled for first use)
vk::FenceCreateInfo info;
info.flags = vk::FenceCreateFlagBits::eSignaled;
auto fence = device.createFenceUnique(info);

// Before submission
device.resetFences(1, &fence);

// Submit with fence
queue.submit2(submitInfo, fence);

// Wait before reuse
device.waitForFences(1, &fence, VK_TRUE, UINT64_MAX);
```

### Timeline Semaphore Usage

```cpp
// Creation
vk::SemaphoreTypeCreateInfo timelineType;
timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
timelineType.initialValue = 0;

vk::SemaphoreCreateInfo semInfo;
semInfo.pNext = &timelineType;
auto timeline = device.createSemaphoreUnique(semInfo);

// Query current value
uint64_t completed = device.getSemaphoreCounterValue(timeline.get());

// Wait for specific value
vk::SemaphoreWaitInfo waitInfo;
waitInfo.semaphoreCount = 1;
waitInfo.pSemaphores = &timeline.get();
waitInfo.pValues = &targetValue;
device.waitSemaphores(waitInfo, UINT64_MAX);
```
