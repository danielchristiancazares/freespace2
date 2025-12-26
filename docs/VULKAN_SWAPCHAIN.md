# Vulkan Swapchain Management

This document describes the FSO Vulkan backend's swapchain implementation in `VulkanDevice` and related acquisition/presentation logic in `VulkanRenderer`.

## Swapchain Creation

Swapchain creation occurs in `VulkanDevice::createSwapchain()` during device initialization and `recreateSwapchain()` during resize.

### Image Count

```cpp
uint32_t imageCount = surfaceCapabilities.minImageCount + 1;
if (surfaceCapabilities.maxImageCount > 0 &&
    imageCount > surfaceCapabilities.maxImageCount) {
    imageCount = surfaceCapabilities.maxImageCount;
}
```

Requests one more image than the minimum to avoid stalling on driver synchronization. Most systems get 3 images (double-buffered minimum + 1).

### Image Usage

```cpp
const auto requested = vk::ImageUsageFlagBits::eColorAttachment |
                       vk::ImageUsageFlagBits::eTransferSrc;
createInfo.imageUsage = requested & surfaceCapabilities.supportedUsageFlags;
```

- `COLOR_ATTACHMENT`: Required for rendering to the swapchain
- `TRANSFER_SRC`: Optional; enables pre-deferred scene capture for screen save/restore. If unsupported, the feature is disabled with a warning.

### Queue Sharing Mode

```cpp
if (graphicsQueueIndex != presentQueueIndex) {
    createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
    // indices: graphics, present
} else {
    createInfo.imageSharingMode = vk::SharingMode::eExclusive;
}
```

Uses exclusive mode when graphics and present queues are the same family (common case). Falls back to concurrent sharing on systems with separate queue families.

## Format Selection

`VulkanDevice::chooseSurfaceFormat()` selects the swapchain format:

```cpp
// Preference order:
// 1. B8G8R8A8_SRGB + SRGB_NONLINEAR (ideal for gamma-correct rendering)
// 2. First available format (fallback)
```

The selected format is exposed via `swapchainFormat()`. All render targets and attachments must be compatible with this format.

## Present Mode Selection

`VulkanDevice::choosePresentMode()` selects based on the `Gr_enable_vsync` global:

| VSync Enabled | Preferred Mode | Fallback |
|---------------|----------------|----------|
| Yes           | MAILBOX        | FIFO     |
| No            | IMMEDIATE      | FIFO     |

- `MAILBOX`: Triple-buffered; replaces queued frames to reduce latency while maintaining VSync
- `IMMEDIATE`: No synchronization; lowest latency but causes tearing
- `FIFO`: Standard double-buffered VSync; always supported

## Image Acquisition

### Low-Level: VulkanDevice::acquireNextImage()

```cpp
struct AcquireResult {
    uint32_t imageIndex = 0;
    bool needsRecreate = false;  // VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR
    bool success = false;
};
AcquireResult acquireNextImage(vk::Semaphore imageAvailable);
```

- Blocks until an image is available (infinite timeout)
- Signals the provided `imageAvailable` semaphore upon completion
- On `VK_SUBOPTIMAL_KHR`: returns success=true with needsRecreate=true
- On `VK_ERROR_OUT_OF_DATE_KHR`: returns success=false with needsRecreate=true

### High-Level: VulkanRenderer::acquireImage()

```cpp
uint32_t VulkanRenderer::acquireImage(VulkanFrame& frame) {
    auto result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());

    if (result.needsRecreate) {
        // Recreate swapchain and resize render targets
        m_vulkanDevice->recreateSwapchain(extent.width, extent.height);
        m_renderTargets->resize(m_vulkanDevice->swapchainExtent());

        // Retry acquisition after recreation
        result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());
    }

    return result.success ? result.imageIndex : UINT32_MAX;
}
```

Returns `UINT32_MAX` on failure. The throwing variant `acquireImageOrThrow()` raises `std::runtime_error` instead.

## Presentation

### VulkanDevice::present()

```cpp
struct PresentResult {
    bool needsRecreate = false;
    bool success = false;
};
PresentResult present(vk::Semaphore renderFinished, uint32_t imageIndex);
```

Submits the present operation to `m_presentQueue`:

```cpp
vk::PresentInfoKHR presentInfo;
presentInfo.waitSemaphoreCount = 1;
presentInfo.pWaitSemaphores = &renderFinished;
presentInfo.swapchainCount = 1;
presentInfo.pSwapchains = &swapchain;
presentInfo.pImageIndices = &imageIndex;

presentResult = m_presentQueue.presentKHR(presentInfo);
```

### Renderer Submit and Present Flow

`VulkanRenderer::submitRecordedFrame()` executes the complete frame submission:

1. Reset the frame's in-flight fence
2. Submit command buffer to graphics queue:
   - Wait on `imageAvailable` at COLOR_ATTACHMENT_OUTPUT stage
   - Signal `renderFinished` at COLOR_ATTACHMENT_OUTPUT stage
   - Signal timeline semaphore for resource tracking
3. Call `present()` with `renderFinished` semaphore
4. Handle swapchain recreation if `needsRecreate` is set

## Swapchain Recreation

`VulkanDevice::recreateSwapchain()` handles window resize and recovery from OUT_OF_DATE:

```cpp
bool recreateSwapchain(uint32_t width, uint32_t height) {
    m_device->waitIdle();

    // Destroy old resources
    m_swapchainImageViews.clear();
    auto oldSwapchain = std::move(m_swapchain);

    // Re-query surface capabilities (dimensions may have changed)
    m_surfaceCapabilities = m_physicalDevice.getSurfaceCapabilitiesKHR(m_surface.get());

    // Create new swapchain referencing old for resource recycling
    createInfo.oldSwapchain = oldSwapchain.get();
    m_swapchain = m_device->createSwapchainKHRUnique(createInfo);

    // Destroy old swapchain after new one is created
    oldSwapchain.reset();

    // Retrieve new images and create views
    // ...
}
```

Key behaviors:
- Waits for device idle before destroying old swapchain
- Re-queries surface capabilities to get correct dimensions
- Passes old swapchain handle to enable driver-level resource recycling
- Creates new image views for the new swapchain images
- Returns false on failure; caller should retry or abort

## Swapchain Images vs VulkanFrame

The swapchain images and VulkanFrame serve different purposes:

### Swapchain Images (owned by VulkanDevice)
- `m_swapchainImages`: Raw VkImage handles retrieved from the swapchain
- `m_swapchainImageViews`: Image views for rendering to swapchain images
- Indexed by the `imageIndex` returned from `acquireNextImage()`
- Number varies by system (typically 2-3)

### VulkanFrame (owned by VulkanRenderer)
- Represents a "frame in flight" with its own resources
- Contains per-frame synchronization primitives (semaphores, fence)
- Contains per-frame ring buffers (uniform, vertex, staging)
- Number is fixed (typically matches MAX_FRAMES_IN_FLIGHT)

The relationship:
```
acquireNextImage(frame.imageAvailable())
       |
       v
   imageIndex -----> m_swapchainImages[imageIndex]
                     m_swapchainImageViews[imageIndex]
       |
       v
   VulkanFrame provides command buffer, fence, semaphores for rendering
```

Multiple frames can be "in flight" (submitted but not presented), but each acquired swapchain image is used by exactly one frame at a time.

## Semaphore Synchronization

Each VulkanFrame owns two binary semaphores for swapchain synchronization:

```cpp
vk::UniqueSemaphore m_imageAvailable;   // Acquisition -> Submit
vk::UniqueSemaphore m_renderFinished;   // Submit -> Present
```

### Synchronization Flow

```
                    GPU Timeline
                    ============

vkAcquireNextImageKHR
    |
    | signals imageAvailable
    v
[imageAvailable semaphore] ----+
                               |
                               v
                    vkQueueSubmit2
                        waits on imageAvailable
                        (at COLOR_ATTACHMENT_OUTPUT)
                               |
                               | executes command buffer
                               |
                               | signals renderFinished
                               v
                    [renderFinished semaphore] ----+
                                                   |
                                                   v
                                        vkQueuePresentKHR
                                            waits on renderFinished
```

This ensures:
1. The GPU does not start writing to the swapchain image until acquisition completes
2. The presentation engine does not display the image until rendering completes

## Error Handling

### VK_ERROR_OUT_OF_DATE_KHR

Returned when the swapchain is incompatible with the surface (window resized, minimized, etc.):

- **During acquire**: Cannot use the swapchain; must recreate before retrying
- **During present**: The presented frame may not display correctly; recreate for next frame

Both `acquireImage()` and `submitRecordedFrame()` handle this by calling `recreateSwapchain()` and `VulkanRenderTargets::resize()`.

### VK_SUBOPTIMAL_KHR

Returned when the swapchain still works but is not optimal for the surface:

- **During acquire**: `success=true`, `needsRecreate=true` - image is usable but recreation is recommended
- **During present**: `success=true`, `needsRecreate=true` - frame was presented but recreation is recommended

FSO treats suboptimal as a signal to recreate at the next convenient opportunity, but continues rendering.

## Swapchain Accessors

VulkanDevice provides these accessors for swapchain state:

| Method | Returns |
|--------|---------|
| `swapchain()` | The VkSwapchainKHR handle |
| `swapchainFormat()` | VkFormat of swapchain images |
| `swapchainExtent()` | Current width/height |
| `swapchainImage(index)` | VkImage at index |
| `swapchainImageView(index)` | VkImageView at index |
| `swapchainImageCount()` | Number of swapchain images |
| `swapchainUsage()` | Image usage flags |

## Related Files

- `code/graphics/vulkan/VulkanDevice.h` - Swapchain member declarations and API
- `code/graphics/vulkan/VulkanDevice.cpp` - Swapchain creation, acquisition, presentation
- `code/graphics/vulkan/VulkanRenderer.cpp` - High-level acquire/present flow
- `code/graphics/vulkan/VulkanFrame.h` - Per-frame semaphores and resources
- `code/graphics/vulkan/VulkanRenderTargets.cpp` - Resize handling triggered by swapchain recreation
