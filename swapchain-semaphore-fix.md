# Swapchain Semaphore Reuse Fix (Per-Image Present-Wait Semaphores)

## Goal
Fix Vulkan validation error `VUID-vkQueueSubmit2-semaphore-03868` by making the submit-to-present semaphore
owned per swapchain image (not per frame). The lifetime of the semaphore must match the lifetime of the
swapchain image it protects.

## Additional Correctness Issues (Fix Alongside)

These are code-verified hazards that can still produce "signaled again while pending" semaphore lifetimes if left
unchanged:

1) **SUBOPTIMAL acquire retry can reuse `imageAvailable` without a wait**
   - `VulkanDevice::acquireNextImage(...)` reports `needsRecreate=true` for both `VK_ERROR_OUT_OF_DATE_KHR` and
     `VK_SUBOPTIMAL_KHR` (`code/graphics/vulkan/VulkanDevice.cpp`, `acquireNextImage`).
   - `VulkanRenderer::acquireImageOrThrow(...)` currently recreates on `needsRecreate` and then immediately calls
     `acquireNextImage(frame.imageAvailable())` again (`code/graphics/vulkan/VulkanRenderer.cpp`, `acquireImageOrThrow`).
   - If the first acquire returned SUBOPTIMAL and signaled the semaphore, that immediate reuse is invalid.

2) **Present failures (`success=false`) are ignored**
   - `VulkanDevice::present(...)` can return `success=false` (`code/graphics/vulkan/VulkanDevice.cpp`, `present`).
   - `VulkanRenderer::submitRecordedFrame(...)` currently only checks `needsRecreate` and ignores `success`
     (`code/graphics/vulkan/VulkanRenderer.cpp`, `submitRecordedFrame`).
   - If present fails without consuming the wait semaphore, the next reuse can still hit the VUID.

## Why This Works (State as Location)
- Swapchain images are owned by `VulkanDevice`, so the per-image present-wait semaphores should be owned there too.
- If an image exists at index `i`, then the semaphore at index `i` exists. The container size is the state.
- Recreate the semaphores whenever the swapchain is recreated, because the image count can change.

## Files Changed
- `code/graphics/vulkan/VulkanDevice.h`
- `code/graphics/vulkan/VulkanDevice.cpp`
- `code/graphics/vulkan/VulkanFrame.h`
- `code/graphics/vulkan/VulkanFrame.cpp`
- `code/graphics/vulkan/VulkanRenderer.cpp`
- Docs (see Update Docs section)

---

## Step 1: Fix SUBOPTIMAL Acquire Handling (Do Not Reuse `imageAvailable` Unsafely)

**File:** `code/graphics/vulkan/VulkanRenderer.cpp`

In both `VulkanRenderer::acquireImage(...)` and `VulkanRenderer::acquireImageOrThrow(...)`, treat acquire results as:
- OUT_OF_DATE: `needsRecreate && !success` -> recreate swapchain, resize render targets, then retry acquire
- SUBOPTIMAL: `needsRecreate && success` -> **do not recreate+retry here**; return the `imageIndex` and let the normal
  submit+present path handle recreation (present already reports SUBOPTIMAL as `needsRecreate`)

Rationale: avoids immediately reusing a possibly signaled `imageAvailable` semaphore on a second acquire without any
guaranteed wait in between.

---

## Step 2: Add Per-Image Semaphores To VulkanDevice

**File:** `code/graphics/vulkan/VulkanDevice.h`

1) Add a public accessor in the swapchain access section:

```cpp
// Returns the per-swapchain-image semaphore that submissions should signal and present should wait on.
// Precondition: imageIndex was returned by acquireNextImage for the current swapchain.
vk::Semaphore presentWaitSemaphore(uint32_t imageIndex) const;
```

2) Add a private helper and storage in the swapchain section.
Declare `m_presentWaitSemaphores` in the swapchain section (after `m_device`) so it is destroyed before the device.
Within the swapchain members, the relative order is not semantically important; keep it adjacent to the swapchain
images/views for locality.

```cpp
static SCP_vector<vk::UniqueSemaphore> buildPresentWaitSemaphores(vk::Device device, size_t imageCount);
SCP_vector<vk::UniqueSemaphore> m_presentWaitSemaphores;
```

---

## Step 3: Implement The Accessor + Builder

**File:** `code/graphics/vulkan/VulkanDevice.cpp`

Add these definitions near the other swapchain helpers (e.g., after `swapchainImageCount()`):

```cpp
vk::Semaphore VulkanDevice::presentWaitSemaphore(uint32_t imageIndex) const {
  Assertion(imageIndex < m_presentWaitSemaphores.size(),
            "presentWaitSemaphore: imageIndex %u out of bounds (swapchain has %zu images)", imageIndex,
            m_presentWaitSemaphores.size());
  return m_presentWaitSemaphores[imageIndex].get();
}

SCP_vector<vk::UniqueSemaphore> VulkanDevice::buildPresentWaitSemaphores(vk::Device device, size_t imageCount) {
  SCP_vector<vk::UniqueSemaphore> semaphores;
  semaphores.reserve(imageCount);

  vk::SemaphoreCreateInfo binaryInfo{};
  for (size_t i = 0; i < imageCount; ++i) {
    semaphores.push_back(device.createSemaphoreUnique(binaryInfo));
  }

  return semaphores;
}
```

---

## Step 4: Create Semaphores After Swapchain Creation

**File:** `code/graphics/vulkan/VulkanDevice.cpp`

At the end of `VulkanDevice::createSwapchain(...)`, after image views are created and before `return true;`, add:

```cpp
m_presentWaitSemaphores = buildPresentWaitSemaphores(m_device.get(), m_swapchainImages.size());
```

---

## Step 5: Make Swapchain Recreation Atomic (Including Semaphores)

**File:** `code/graphics/vulkan/VulkanDevice.cpp`

Replace the entire `VulkanDevice::recreateSwapchain(...)` with the version below.
This builds the new swapchain, image views, and semaphores in local variables and only commits if all succeed.

Note on "atomic":
- This snippet commits *swapchain object state* (`m_swapchain`, `m_swapchainImages`, `m_swapchainImageViews`,
  `m_presentWaitSemaphores`, and the format/extent/usage fields) only after all allocations succeed.
- It still updates the cached surface capability members (`m_surfaceCapabilities`, `m_surfaceFormats`, `m_presentModes`)
  up front. That is acceptable because these are just cached query results used for future recreates, not the swapchain
  object itself.
- If you want strict atomic behavior for caches too, query into local temporaries and only commit those caches at the
  same time as the swapchain state.

```cpp
bool VulkanDevice::recreateSwapchain(uint32_t width, uint32_t height) {
  m_device->waitIdle();

  // Re-query surface capabilities
  m_surfaceCapabilities = m_physicalDevice.getSurfaceCapabilitiesKHR(m_surface.get());
  m_surfaceFormats.clear();
  auto formats = m_physicalDevice.getSurfaceFormatsKHR(m_surface.get());
  m_surfaceFormats = SCP_vector<vk::SurfaceFormatKHR>(formats.begin(), formats.end());
  auto modes = m_physicalDevice.getSurfacePresentModesKHR(m_surface.get());
  m_presentModes = SCP_vector<vk::PresentModeKHR>(modes.begin(), modes.end());

  // Build PhysicalDeviceValues from cached data
  PhysicalDeviceValues tempValues;
  tempValues.surfaceCapabilities = m_surfaceCapabilities;
  tempValues.surfaceFormats = std::vector<vk::SurfaceFormatKHR>(m_surfaceFormats.begin(), m_surfaceFormats.end());
  tempValues.presentModes = std::vector<vk::PresentModeKHR>(m_presentModes.begin(), m_presentModes.end());
  tempValues.graphicsQueueIndex.index = m_graphicsQueueIndex;
  tempValues.graphicsQueueIndex.initialized = true;
  tempValues.presentQueueIndex.index = m_presentQueueIndex;
  tempValues.presentQueueIndex.initialized = true;

  uint32_t imageCount = m_surfaceCapabilities.minImageCount + 1;
  if (m_surfaceCapabilities.maxImageCount > 0 && imageCount > m_surfaceCapabilities.maxImageCount) {
    imageCount = m_surfaceCapabilities.maxImageCount;
  }

  vk::SurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(tempValues);

  vk::SwapchainCreateInfoKHR createInfo;
  createInfo.surface = m_surface.get();
  createInfo.minImageCount = imageCount;
  createInfo.imageFormat = surfaceFormat.format;
  createInfo.imageColorSpace = surfaceFormat.colorSpace;
  createInfo.imageExtent = chooseSwapExtent(tempValues, width, height);
  createInfo.imageArrayLayers = 1;
  {
    const auto supported = m_surfaceCapabilities.supportedUsageFlags;
    const auto requested = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
    createInfo.imageUsage = requested & supported;
    Assertion((createInfo.imageUsage & vk::ImageUsageFlagBits::eColorAttachment) != vk::ImageUsageFlags{},
              "Surface does not support swapchain images as color attachments (supportedUsageFlags=0x%x)",
              static_cast<uint32_t>(supported));
    if ((createInfo.imageUsage & vk::ImageUsageFlagBits::eTransferSrc) == vk::ImageUsageFlags{}) {
      vkprintf("VulkanDevice: swapchain does not support TRANSFER_SRC usage; "
               "pre-deferred scene capture will be disabled.\n");
    }
  }

  const uint32_t queueFamilyIndices[] = {m_graphicsQueueIndex, m_presentQueueIndex};
  if (m_graphicsQueueIndex != m_presentQueueIndex) {
    createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
    createInfo.queueFamilyIndexCount = 2;
    createInfo.pQueueFamilyIndices = queueFamilyIndices;
  } else {
    createInfo.imageSharingMode = vk::SharingMode::eExclusive;
    createInfo.queueFamilyIndexCount = 0;
    createInfo.pQueueFamilyIndices = nullptr;
  }

  createInfo.preTransform = m_surfaceCapabilities.currentTransform;
  createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
  createInfo.presentMode = choosePresentMode(tempValues);
  createInfo.clipped = true;
  createInfo.oldSwapchain = m_swapchain.get();

  vk::UniqueSwapchainKHR newSwapchain;
  try {
    newSwapchain = m_device->createSwapchainKHRUnique(createInfo);
  } catch (const vk::SystemError &) {
    return false;
  }

  std::vector<vk::Image> swapchainImages = m_device->getSwapchainImagesKHR(newSwapchain.get());
  SCP_vector<vk::Image> newImages(swapchainImages.begin(), swapchainImages.end());

  SCP_vector<vk::UniqueImageView> newImageViews;
  newImageViews.reserve(newImages.size());
  for (const auto &image : newImages) {
    vk::ImageViewCreateInfo viewCreateInfo;
    viewCreateInfo.image = image;
    viewCreateInfo.viewType = vk::ImageViewType::e2D;
    viewCreateInfo.format = surfaceFormat.format;

    viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
    viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;

    viewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    viewCreateInfo.subresourceRange.baseMipLevel = 0;
    viewCreateInfo.subresourceRange.levelCount = 1;
    viewCreateInfo.subresourceRange.baseArrayLayer = 0;
    viewCreateInfo.subresourceRange.layerCount = 1;

    newImageViews.push_back(m_device->createImageViewUnique(viewCreateInfo));
  }

  SCP_vector<vk::UniqueSemaphore> newSemaphores = buildPresentWaitSemaphores(m_device.get(), newImages.size());

  // Commit only after all allocations succeed.
  m_swapchain = std::move(newSwapchain);
  m_swapchainImages = std::move(newImages);
  m_swapchainImageViews = std::move(newImageViews);
  m_swapchainFormat = surfaceFormat.format;
  m_swapchainExtent = createInfo.imageExtent;
  m_swapchainUsage = createInfo.imageUsage;
  m_presentWaitSemaphores = std::move(newSemaphores);

  return true;
}
```

---

## Step 6: Use The Per-Image Semaphore In Submit + Present

(Do this before removing `VulkanFrame::renderFinished()` so the change stays buildable.)

**File:** `code/graphics/vulkan/VulkanRenderer.cpp`

In `VulkanRenderer::submitRecordedFrame(...)`, replace the per-frame semaphore usage (see Step 8 for additional
handling after present).

Replace:

```cpp
signalSemaphores[0].semaphore = frame.renderFinished();
...
auto presentResult = m_vulkanDevice->present(frame.renderFinished(), imageIndex);
```

With:

```cpp
vk::Semaphore presentWait = m_vulkanDevice->presentWaitSemaphore(imageIndex);
signalSemaphores[0].semaphore = presentWait;
...
auto presentResult = m_vulkanDevice->present(presentWait, imageIndex);
```

Note: If you later add a skip-present path (minimized window, zero extent, etc.), make sure that path
also skips signaling `presentWait` so you do not create a signaled-but-never-waited semaphore.

---

## Step 7: Remove The Per-Frame Render-Finished Semaphore

**File:** `code/graphics/vulkan/VulkanFrame.h`

Remove:

```cpp
vk::Semaphore renderFinished() const { return m_renderFinished.get(); }
vk::UniqueSemaphore m_renderFinished;
```

**File:** `code/graphics/vulkan/VulkanFrame.cpp`

Remove this line from the constructor:

```cpp
m_renderFinished = m_device.createSemaphoreUnique(binaryInfo);
```

---

## Step 8: Harden Present Error Handling (Don’t Continue After `success=false`)

**File:** `code/graphics/vulkan/VulkanRenderer.cpp`

In `VulkanRenderer::submitRecordedFrame(...)`, after calling `present(...)`:
- If `presentResult.needsRecreate`, call `recreateSwapchain(...)` (this covers both OUT_OF_DATE and SUBOPTIMAL); if
  recreation succeeds, also resize swapchain-dependent targets (e.g. `m_renderTargets->resize(m_vulkanDevice->swapchainExtent())`);
  if recreation fails, throw/abort.
- Else if `!presentResult.success`, throw/abort the Vulkan backend (do not continue rendering with potentially
  signaled-but-never-waited semaphores).

If you need better diagnostics, extend `VulkanDevice::PresentResult` to include the underlying `vk::Result` so
callers can log/throw with the actual error code.

---

## Step 9: Update Docs

Update the Vulkan docs to match the new ownership in `VulkanDevice`:
- `docs/VULKAN_SYNCHRONIZATION.md`
- `docs/VULKAN_FRAME_LIFECYCLE.md`
- `docs/VULKAN_SWAPCHAIN.md`
- `docs/VULKAN_ARCHITECTURE_OVERVIEW.md`

---

## Quick Sanity Checks

```sh
rg -n "renderFinished" code/graphics/vulkan
rg -n "presentWaitSemaphore" code/graphics/vulkan
```

Expected:
- No remaining `renderFinished` references.
- Only the new `presentWaitSemaphore` accessor and usage.

---

## Build + Run (Optional)

```powershell
./build.ps1 -Target code
```

Run with validation layers and reproduce the old failure path (resize, alt-tab, minimize). The
`VUID-vkQueueSubmit2-semaphore-03868` error should no longer appear.
