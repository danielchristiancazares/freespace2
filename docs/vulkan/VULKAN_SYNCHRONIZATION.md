# Vulkan Synchronization Infrastructure

This document provides a comprehensive analysis of the Vulkan synchronization mechanisms used in the FreeSpace Open engine. It covers frame synchronization, pipeline barriers, image layout transitions, and resource upload synchronization.

---

## Table of Contents

1. [Overview](#overview)
2. [Synchronization Files Reference](#synchronization-files-reference)
3. [Frame-in-Flight Configuration](#frame-in-flight-configuration)
4. [Synchronization Primitives](#synchronization-primitives)
5. [Swapchain Acquisition and Presentation](#swapchain-acquisition-and-presentation)
6. [Queue Submission](#queue-submission)
7. [Pipeline Barriers and Image Layout Transitions](#pipeline-barriers-and-image-layout-transitions)
8. [Resource Upload Synchronization](#resource-upload-synchronization)
9. [Queue Family Sharing Mode](#queue-family-sharing-mode)
10. [Device Waits](#device-waits)
11. [Synchronization Diagrams](#synchronization-diagrams)
12. [Known Issues and Recommendations](#known-issues-and-recommendations)

---

## Overview

The Vulkan backend implements a multi-frame-in-flight rendering architecture with the following key synchronization mechanisms:

| Mechanism | Purpose | Location |
|-----------|---------|----------|
| Binary Semaphores | GPU-GPU sync (acquire/present) | `VulkanFrame.cpp:60-62` |
| Timeline Semaphore | Serial-based completion tracking | `VulkanFrame.cpp:52-58`, `VulkanRenderer.cpp:152-160` |
| Fences | CPU-GPU sync (frame reuse) | `VulkanFrame.cpp:48-50` |
| Pipeline Barriers | Execution/memory dependencies | `VulkanRenderingSession.cpp:678-825` |
| Image Layout Transitions | Access pattern changes | `VulkanRenderingSession.cpp:20-68` |
| Queue Waits | Immediate synchronization | `VulkanBufferManager.cpp:164`, `VulkanTextureManager.cpp:289,549` |

---

## Synchronization Files Reference

| File | Key Synchronization Content |
|------|---------------------------|
| `VulkanConstants.h:8` | `kFramesInFlight = 2` constant |
| `VulkanFrame.h:48-52` | Semaphore/fence accessors |
| `VulkanFrame.cpp:48-62` | Sync primitive creation |
| `VulkanRenderer.cpp:152-160` | Global timeline semaphore creation |
| `VulkanRenderer.cpp:340-405` | Frame submission with semaphores |
| `VulkanRenderer.cpp:431-449` | Frame recycling with fence wait |
| `VulkanDevice.cpp:996-1023` | Swapchain image acquisition |
| `VulkanDevice.cpp:1025-1056` | Queue presentation |
| `VulkanDevice.cpp:783-791` | Queue family sharing mode |
| `VulkanRenderingSession.cpp:20-68` | `stageAccessForLayout()` helper |
| `VulkanRenderingSession.cpp:78-79` | Swapchain layout tracking |
| `VulkanRenderingSession.cpp:678-825` | Image layout transitions |
| `VulkanBufferManager.cpp:71-169` | Buffer upload with barriers |
| `VulkanTextureManager.cpp:238-289` | Texture upload barriers |

---

## Frame-in-Flight Configuration

**File:** `VulkanConstants.h:8`
```cpp
constexpr uint32_t kFramesInFlight = 2;
```

The engine uses 2 frames in flight with ring-buffered resources. This allows the CPU to record frame N+1 while the GPU executes frame N.

**Frame Pool Management:** `VulkanRenderer.cpp:109-132`
```cpp
void VulkanRenderer::createFrames() {
  for (size_t i = 0; i < kFramesInFlight; ++i) {
    m_frames[i] = std::make_unique<VulkanFrame>(/* ... */);
    m_availableFrames.push_back(AvailableFrame{ m_frames[i].get(), m_completedSerial });
  }
}
```

---

## Synchronization Primitives

### Per-Frame Fence

**File:** `VulkanFrame.cpp:48-50`
```cpp
vk::FenceCreateInfo fenceInfo;
fenceInfo.flags = vk::FenceCreateFlagBits::eSignaled; // allow first frame without waiting
m_inflightFence = m_device.createFenceUnique(fenceInfo);
```

The fence is created in signaled state to allow the first frame to proceed without blocking. Fence wait occurs in `VulkanRenderer::recycleOneInFlight()` before frame reuse.

### Per-Frame Timeline Semaphore (Unused)

**File:** `VulkanFrame.cpp:52-58`
```cpp
vk::SemaphoreTypeCreateInfo timelineType;
timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
timelineType.initialValue = m_timelineValue;

vk::SemaphoreCreateInfo semaphoreInfo;
semaphoreInfo.pNext = &timelineType;
m_timelineSemaphore = m_device.createSemaphoreUnique(semaphoreInfo);
```

**Note:** This per-frame timeline semaphore is created but **never used** in the current implementation. The global timeline semaphore (`VulkanRenderer::m_submitTimeline`) is used instead for serial tracking.

### Binary Semaphores

**File:** `VulkanFrame.cpp:60-62`
```cpp
vk::SemaphoreCreateInfo binaryInfo;
m_imageAvailable = m_device.createSemaphoreUnique(binaryInfo);
m_renderFinished = m_device.createSemaphoreUnique(binaryInfo);
```

| Semaphore | Signaled By | Waited By |
|-----------|-------------|-----------|
| `imageAvailable` | `vkAcquireNextImageKHR` | Queue submission |
| `renderFinished` | Queue submission | `vkQueuePresentKHR` |

### Global Timeline Semaphore

**File:** `VulkanRenderer.cpp:152-160`
```cpp
void VulkanRenderer::createSubmitTimelineSemaphore() {
  vk::SemaphoreTypeCreateInfo timelineType;
  timelineType.semaphoreType = vk::SemaphoreType::eTimeline;
  timelineType.initialValue = 0;

  vk::SemaphoreCreateInfo semaphoreInfo;
  semaphoreInfo.pNext = &timelineType;
  m_submitTimeline = m_vulkanDevice->device().createSemaphoreUnique(semaphoreInfo);
}
```

Used for tracking GPU completion serials. Queried via `getSemaphoreCounterValue()` at `VulkanRenderer.cpp:163-178`.

---

## Swapchain Acquisition and Presentation

### Image Acquisition

**File:** `VulkanDevice.cpp:996-1023`
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

### Queue Presentation

**File:** `VulkanDevice.cpp:1025-1056`
```cpp
VulkanDevice::PresentResult VulkanDevice::present(vk::Semaphore renderFinished, uint32_t imageIndex) {
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

---

## Queue Submission

### Frame Submission

**File:** `VulkanRenderer.cpp:340-405`
```cpp
graphics::vulkan::SubmitInfo VulkanRenderer::submitRecordedFrame(graphics::vulkan::RecordingFrame& rec) {
  // Reset fence before submission
  vk::Fence fence = frame.inflightFence();
  auto resetResult = m_vulkanDevice->device().resetFences(1, &fence);

  // Wait semaphore: imageAvailable at COLOR_ATTACHMENT_OUTPUT stage
  vk::SemaphoreSubmitInfo waitSemaphore;
  waitSemaphore.semaphore = frame.imageAvailable();
  waitSemaphore.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;

  // Signal semaphores: renderFinished + timeline
  vk::SemaphoreSubmitInfo signalSemaphores[2];
  signalSemaphores[0].semaphore = frame.renderFinished();
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

  // Submit with fence
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
  // ...
}
```

### Redundant Submit2 Calls (Bug)

**File:** `VulkanRenderer.cpp:383-387`
```cpp
#if defined(VULKAN_HPP_NO_EXCEPTIONS)
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
#else
  m_vulkanDevice->graphicsQueue().submit2(submitInfo, fence);
#endif
```

**Issue:** Lines 384 and 386 contain identical `submit2()` calls. The preprocessor conditional should differentiate between exception and no-exception handling, but both branches execute the same code. This is functionally harmless but represents dead code duplication.

### Frame Recycling with Fence Wait

**File:** `VulkanRenderer.cpp:431-449`
```cpp
void VulkanRenderer::recycleOneInFlight()
{
  graphics::vulkan::InFlightFrame inflight = std::move(m_inFlightFrames.front());
  m_inFlightFrames.pop_front();

  VulkanFrame& f = inflight.ref();

  f.wait_for_gpu();  // Blocks until fence signals
  const uint64_t completed = queryCompletedSerial();
  m_completedSerial = std::max(m_completedSerial, completed);
  prepareFrameForReuse(f, m_completedSerial);

  m_availableFrames.push_back(AvailableFrame{ &f, m_completedSerial });
}
```

**File:** `VulkanFrame.cpp:65-77`
```cpp
void VulkanFrame::wait_for_gpu()
{
  auto fence = m_inflightFence.get();
  if (!fence) {
    return;
  }

  auto result = m_device.waitForFences(1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
  if (result != vk::Result::eSuccess) {
    throw std::runtime_error("Fence wait failed for Vulkan frame");
  }
}
```

---

## Pipeline Barriers and Image Layout Transitions

### stageAccessForLayout() Helper

**File:** `VulkanRenderingSession.cpp:20-68`

This utility function maps image layouts to their corresponding pipeline stage and access masks:

```cpp
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
    out.accessMask = vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
    break;
  case vk::ImageLayout::eDepthAttachmentOptimal:
  case vk::ImageLayout::eDepthStencilAttachmentOptimal:
    out.stageMask = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    out.accessMask = vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
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
    // Present is external to the pipeline - release barrier
    out.stageMask = {};
    out.accessMask = {};
    break;
  default:
    out.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
    out.accessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    break;
  }
  return out;
}
```

### Swapchain Layout Tracking

**File:** `VulkanRenderingSession.cpp:78-79`, `VulkanRenderingSession.h:224-225`
```cpp
// Constructor
m_swapchainLayouts.assign(m_device.swapchainImageCount(), vk::ImageLayout::eUndefined);
```

The `m_swapchainLayouts` vector tracks the current layout of each swapchain image, enabling correct barrier source layout specification.

### Swapchain to Attachment Transition

**File:** `VulkanRenderingSession.cpp:678-699`
```cpp
void VulkanRenderingSession::transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex) {
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

### Swapchain to Present Transition

**File:** `VulkanRenderingSession.cpp:725-748`
```cpp
void VulkanRenderingSession::transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex) {
  vk::ImageMemoryBarrier2 toPresent{};
  toPresent.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
  toPresent.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
  // Present is external - release barrier (dstStage/Access = 0)
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

### Depth Attachment Transition

**File:** `VulkanRenderingSession.cpp:701-723`
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

### G-Buffer Transitions

**File:** `VulkanRenderingSession.cpp:750-777` (to attachment)
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

**File:** `VulkanRenderingSession.cpp:779-825` (to shader read)
Transitions G-buffer attachments and depth to shader-readable layouts for deferred lighting.

### Scene HDR Transitions

**File:** `VulkanRenderingSession.cpp:860-887`
```cpp
void VulkanRenderingSession::transitionSceneHdrToLayout(vk::CommandBuffer cmd, vk::ImageLayout newLayout)
{
  const auto oldLayout = m_targets.sceneHdrLayout();
  if (oldLayout == newLayout) {
    return; // Early-out optimization
  }

  vk::ImageMemoryBarrier2 barrier{};
  const auto src = stageAccessForLayout(oldLayout);
  const auto dst = stageAccessForLayout(newLayout);
  barrier.srcStageMask = src.stageMask;
  barrier.srcAccessMask = src.accessMask;
  barrier.dstStageMask = dst.stageMask;
  barrier.dstAccessMask = dst.accessMask;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.image = m_targets.sceneHdrImage();
  barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;

  vk::DependencyInfo dep{};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(dep);

  m_targets.setSceneHdrLayout(newLayout);
}
```

---

## Resource Upload Synchronization

### Buffer Uploads (Device-Local)

**File:** `VulkanBufferManager.cpp:71-169`

The buffer manager uses a synchronous staging pattern with immediate fence wait:

```cpp
void VulkanBufferManager::uploadToDeviceLocal(const VulkanBuffer& buffer,
    vk::DeviceSize dstOffset, vk::DeviceSize size, const void* data)
{
  // 1. Create transient staging buffer
  auto stagingBuffer = m_device.createBufferUnique(stagingInfo);
  // ... allocate and map ...

  // 2. Copy data to staging
  void* mapped = m_device.mapMemory(stagingMemory.get(), 0, size);
  std::memcpy(mapped, data, static_cast<size_t>(size));
  m_device.unmapMemory(stagingMemory.get());

  // 3. Record copy command
  cmd.begin(beginInfo);
  cmd.copyBuffer(stagingBuffer.get(), buffer.buffer.get(), 1, &copy);

  // 4. Buffer memory barrier for visibility
  vk::BufferMemoryBarrier2 barrier{};
  barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
  barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
  barrier.dstStageMask = dstStage;   // Depends on buffer type
  barrier.dstAccessMask = dstAccess; // Depends on buffer type
  barrier.buffer = buffer.buffer.get();
  barrier.offset = dstOffset;
  barrier.size = size;

  vk::DependencyInfo depInfo{};
  depInfo.bufferMemoryBarrierCount = 1;
  depInfo.pBufferMemoryBarriers = &barrier;
  cmd.pipelineBarrier2(depInfo);
  cmd.end();

  // 5. Submit with fence and wait
  vk::FenceCreateInfo fenceInfo{};
  auto fence = m_device.createFenceUnique(fenceInfo);

  vk::SubmitInfo submit{};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  m_transferQueue.submit(submit, fence.get());

  // 6. Blocking wait
  const auto waitResult = m_device.waitForFences(1, &fenceHandle, VK_TRUE,
      std::numeric_limits<uint64_t>::max());
}
```

**Buffer Type to Stage/Access Mapping:**
| Buffer Type | Destination Stage | Destination Access |
|-------------|------------------|-------------------|
| Vertex | `eVertexInput \| eVertexShader` | `eVertexAttributeRead \| eShaderRead` |
| Index | `eVertexInput` | `eIndexRead` |
| Uniform | `eVertexShader \| eFragmentShader` | `eUniformRead` |

### Texture Uploads (Immediate)

**File:** `VulkanTextureManager.cpp:285-289`, `VulkanTextureManager.cpp:545-549`

Immediate texture uploads use `waitIdle()` for simplicity:

```cpp
// After recording barriers and copy commands...
vk::SubmitInfo submitInfo;
submitInfo.commandBufferCount = 1;
submitInfo.pCommandBuffers = &cmdBuf;
m_transferQueue.submit(submitInfo, nullptr);
m_transferQueue.waitIdle();  // Blocking synchronization
```

### Texture Uploads (Frame-Buffered)

**File:** `VulkanTextureManager.cpp:870-905`

In-frame texture uploads use pipeline barriers within the main command buffer:

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

// Copy from staging ring buffer
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

### Movie Texture Uploads

**File:** `VulkanMovieManager.cpp:661-693`

Movie textures use in-frame barriers similar to other textures:

```cpp
// Transition YUV planar image to transfer destination
vk::ImageMemoryBarrier2 barrier{};
barrier.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
barrier.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
barrier.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
// ... (handles 3-plane YUV420 layout)
cmd.pipelineBarrier2(dep);

// After plane copies...
// Transition to shader read for YCbCr sampling
cmd.pipelineBarrier2(dep);
```

---

## Queue Family Sharing Mode

**File:** `VulkanDevice.cpp:783-791`

Swapchain images are configured based on queue family indices:

```cpp
const uint32_t queueFamilyIndices[] = {
    deviceValues.graphicsQueueIndex.index,
    deviceValues.presentQueueIndex.index
};

if (deviceValues.graphicsQueueIndex.index != deviceValues.presentQueueIndex.index) {
  // Different queue families: use concurrent sharing
  createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
  createInfo.queueFamilyIndexCount = 2;
  createInfo.pQueueFamilyIndices = queueFamilyIndices;
} else {
  // Same queue family: use exclusive mode (no ownership transfers needed)
  createInfo.imageSharingMode = vk::SharingMode::eExclusive;
  createInfo.queueFamilyIndexCount = 0;
  createInfo.pQueueFamilyIndices = nullptr;
}
```

**Sharing Mode Implications:**
| Mode | When Used | Barrier Requirements |
|------|-----------|---------------------|
| `eExclusive` | Graphics == Present queue | No queue family ownership transfers |
| `eConcurrent` | Graphics != Present queue | Automatic ownership; no explicit transfers |

---

## Device Waits

The following locations use blocking device/queue waits:

| Location | Wait Type | Purpose |
|----------|-----------|---------|
| `VulkanRenderer.cpp:1647` | `graphicsQueue().waitIdle()` | `immediateSubmit()` completion |
| `VulkanRenderer.cpp:1655` | `device().waitIdle()` | Shutdown synchronization |
| `VulkanDevice.cpp:418` | `device->waitIdle()` | Pre-shutdown cleanup |
| `VulkanDevice.cpp:1059` | `device->waitIdle()` | Pre-swapchain recreation |
| `VulkanBufferManager.cpp:164` | `waitForFences()` | Buffer upload completion |
| `VulkanTextureManager.cpp:289` | `transferQueue.waitIdle()` | Solid texture creation |
| `VulkanTextureManager.cpp:549` | `transferQueue.waitIdle()` | Immediate texture upload |
| `VulkanTextureManager.cpp:1780` | `transferQueue.waitIdle()` | Render target clear |

---

## Synchronization Diagrams

### Frame Submission Pipeline

```
CPU Timeline:
  Frame N: [Record]---[Submit]---------------------------[Recycle]
  Frame N+1:          [Acquire]---[Record]---[Submit]----[...]

GPU Timeline:
  Frame N:            [Execute]---------[Present]
  Frame N+1:                    [Wait]--[Execute]--[Present]

Synchronization Points:
  |--- imageAvailable semaphore (GPU-GPU) ---|
  |------------- inflightFence (CPU-GPU) -----------|
  |--- renderFinished semaphore (GPU-GPU) ---|
```

### Image Layout State Machine (Swapchain)

```
                    +-------------------+
                    |    eUndefined     |
                    | (initial/resize)  |
                    +--------+----------+
                             |
                             v beginFrame()
              +-----------------------------+
              | eColorAttachmentOptimal     |<---+
              | (rendering)                 |    |
              +-----------------------------+    |
                             |                   |
                             v endFrame()        | mid-frame
              +-----------------------------+    | target switch
              | ePresentSrcKHR              |    |
              | (presentation)              |    |
              +-----------------------------+    |
                             |                   |
                             +-------------------+
                               next frame acquire
```

### Image Layout State Machine (Scene HDR)

```
              +-------------------+
              |    eUndefined     |
              +--------+----------+
                       |
                       v requestSceneHdrTarget()
              +-----------------------------+
              | eColorAttachmentOptimal     |<--+
              | (scene rendering)           |   |
              +-----------------------------+   |
                       |                        |
                       v transitionToShaderRead |
              +-----------------------------+   |
              | eShaderReadOnlyOptimal      |   |
              | (tonemapping input)         |   |
              +-----------------------------+   |
                       |                        |
                       +------------------------+
                         next scene begin
```

---

## Known Issues and Recommendations

### Issue #1: renderFinished Signal Stage

**Location:** `VulkanRenderer.cpp:358-359`
```cpp
signalSemaphores[0].semaphore = frame.renderFinished();
signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
```

**Problem:** The `renderFinished` semaphore is signaled at `eColorAttachmentOutput`, but post-processing (tonemapping, bloom, FXAA), deferred lighting, and HUD rendering may occur after this stage.

**Risk:** Presentation may begin before all rendering completes, causing visual artifacts.

**Recommendation:** Signal at `eBottomOfPipe` or `eAllCommands` to ensure all work is complete:
```cpp
signalSemaphores[0].stageMask = vk::PipelineStageFlagBits2::eAllCommands;
```

### Issue #2: Redundant submit2 Calls

**Location:** `VulkanRenderer.cpp:383-387`

Both branches of the preprocessor conditional execute identical code.

**Recommendation:** Remove one branch or implement proper exception handling differentiation.

### Issue #3: Unused Per-Frame Timeline Semaphore

**Location:** `VulkanFrame.cpp:52-58`

Each frame creates a timeline semaphore that is never used; only the global `m_submitTimeline` in VulkanRenderer is active.

**Recommendation:** Either remove the per-frame timeline semaphore or document its intended future use.

### Issue #4: Tight Frame Synchronization

**Location:** `VulkanConstants.h:8`

With only 2 frames in flight, CPU may block waiting for GPU completion if frame time varies.

**Recommendation:** Consider 3-frame buffering for more consistent pipelining, or document the tradeoff between memory usage and latency.

### Issue #5: Wait Stage Optimization

**Location:** `VulkanRenderer.cpp:354-355`
```cpp
waitSemaphore.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
```

Waiting at `eColorAttachmentOutput` is correct for swapchain writes, but depth-only pre-passes could potentially start earlier.

**Recommendation:** Consider `eTopOfPipe` if early-z or depth pre-pass is implemented.

---

## Summary

The Vulkan synchronization infrastructure implements a robust 2-frame-in-flight model with:

- **Binary semaphores** for GPU-GPU synchronization between acquire, render, and present
- **Per-frame fences** for CPU-GPU synchronization during frame recycling
- **Global timeline semaphore** for deferred resource release tracking
- **Synchronization2 barriers** (`vkCmdPipelineBarrier2`) for all image layout transitions
- **Per-image layout tracking** to specify correct source layouts in barriers
- **Immediate staging uploads** with fence-based synchronization for device-local resources
- **Frame-buffered uploads** with in-command-buffer barriers for streaming textures

The implementation correctly uses the Vulkan 1.3+ synchronization2 extension for fine-grained control over pipeline stages and memory access patterns.
