# Vulkan Error Handling and Recovery

This document describes error handling strategies, recovery mechanisms, and validation patterns in the Vulkan renderer.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Error Categories](#2-error-categories)
3. [Swapchain Recreation](#3-swapchain-recreation)
4. [Device Lost](#4-device-lost)
5. [Resource Exhaustion](#5-resource-exhaustion)
6. [Validation Errors](#6-validation-errors)
7. [Recovery Patterns](#7-recovery-patterns)
8. [Common Issues](#8-common-issues)
9. [Appendix: Error Code Reference](#appendix-error-code-reference)
10. [References](#references)

---

## 1. Overview

The Vulkan renderer uses a multi-layered error handling strategy:

| Layer | Purpose | Build Mode |
|-------|---------|------------|
| **Assertions** | Development-time invariants | Debug only |
| **Return Codes** | Recoverable errors (swapchain recreation, resource exhaustion) | All builds |
| **Exceptions** | Unrecoverable errors (device init, memory allocation) | All builds |
| **Validation Layers** | Runtime error detection | Debug or `Cmdline_graphics_debug_output` |

**Design Philosophy**: Fail fast during development, degrade gracefully in production.

**Key Files**:
- `code/graphics/vulkan/VulkanDevice.cpp` - Device-level error handling, swapchain recreation
- `code/graphics/vulkan/VulkanRenderer.cpp` - Renderer-level error handling, frame acquisition
- `code/graphics/vulkan/VulkanFrame.cpp` - Per-frame synchronization and fence handling
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp` - Device limit validation
- `code/graphics/vulkan/VulkanRingBuffer.cpp` - Ring buffer allocation failures

---

## 2. Error Categories

### 2.1 Recoverable Errors

These errors can be handled without terminating the application.

**Swapchain Recreation**:
| Error Code | Meaning | Action |
|------------|---------|--------|
| `VK_ERROR_OUT_OF_DATE_KHR` | Swapchain incompatible with surface (e.g., window resized) | Recreate swapchain |
| `VK_SUBOPTIMAL_KHR` | Swapchain still valid but suboptimal for current surface | Recreate swapchain (optional but recommended) |

**Resource Exhaustion**:
| Resource | Detection | Recovery |
|----------|-----------|----------|
| Staging buffer | `VulkanRingBuffer::try_allocate()` returns `std::nullopt` | Defer upload to next frame |
| Descriptor pool | `vk::OutOfPoolMemoryError` thrown | Create new pool (should not occur with current sizing) |
| Bindless slots | All 1020 dynamic slots consumed | Fallback to slot 0 (black texture) |

### 2.2 Unrecoverable Errors

These errors require application termination or significant recovery logic.

**Device Errors**:
| Error Code | Cause | Recovery |
|------------|-------|----------|
| `VK_ERROR_DEVICE_LOST` | GPU hung, crashed, or driver reset | Recreate device (not currently implemented) |
| `VK_ERROR_DRIVER_FAILED` | Driver internal error | Exit gracefully |

**Initialization Failures**:
| Failure | Detection | Behavior |
|---------|-----------|----------|
| Device creation fails | Exception from `createDevice()` | Application exits |
| Required extension unavailable | Checked before device creation | Application exits |
| Required feature unsupported | Checked before device creation | Application exits |
| No suitable memory type | `findMemoryType()` throws `std::runtime_error` | Application exits |

### 2.3 Validation Errors

**Development-Time Checks** (detected by validation layers):
- Invalid API usage (wrong parameters, invalid handles)
- Resource lifetime violations (using destroyed resources)
- Descriptor binding errors (unbound descriptors, wrong types)
- Image layout mismatches (sampling from wrong layout)
- Synchronization hazards (missing barriers)

**Runtime Behavior**:
- Debug builds: Assertion failure stops execution
- Release builds: Assertions disabled; undefined behavior possible

---

## 3. Swapchain Recreation

### 3.1 Detection

Swapchain recreation is triggered when `acquireNextImage` or `present` returns an out-of-date or suboptimal status.

**Acquire Next Image** (`VulkanDevice.cpp`):
```cpp
VulkanDevice::AcquireResult VulkanDevice::acquireNextImage(vk::Semaphore imageAvailable) {
    AcquireResult result;
    uint32_t imageIndex = std::numeric_limits<uint32_t>::max();

    vk::Result res = vk::Result::eSuccess;
    try {
        res = m_device->acquireNextImageKHR(
            m_swapchain.get(), std::numeric_limits<uint64_t>::max(),
            imageAvailable, nullptr, &imageIndex);
    } catch (const vk::SystemError& err) {
        res = static_cast<vk::Result>(err.code().value());
    }

    if (res == vk::Result::eErrorOutOfDateKHR || res == vk::Result::eSuboptimalKHR) {
        result.needsRecreate = true;
        result.success = (res == vk::Result::eSuboptimalKHR);
        result.imageIndex = imageIndex;
        return result;
    }
    // ... handle other errors ...
}
```

**Present** (`VulkanDevice.cpp`):
```cpp
VulkanDevice::PresentResult VulkanDevice::present(vk::Semaphore renderFinished, uint32_t imageIndex) {
    PresentResult result;
    // ... setup presentInfo ...

    vk::Result presentResult = vk::Result::eSuccess;
    try {
        presentResult = m_presentQueue.presentKHR(presentInfo);
    } catch (const vk::SystemError& err) {
        presentResult = static_cast<vk::Result>(err.code().value());
    }

    if (presentResult == vk::Result::eErrorOutOfDateKHR ||
        presentResult == vk::Result::eSuboptimalKHR) {
        result.needsRecreate = true;
        result.success = (presentResult == vk::Result::eSuboptimalKHR);
        return result;
    }
    // ...
}
```

### 3.2 Recreation Process

**Function**: `VulkanDevice::recreateSwapchain(uint32_t width, uint32_t height)`

**Location**: `VulkanDevice.cpp`

**Process**:

1. **Wait for GPU Idle**: Ensure no in-flight commands reference swapchain resources.
   ```cpp
   m_device->waitIdle();
   ```

2. **Destroy Old Resources**: Clear image views, preserve old swapchain handle.
   ```cpp
   m_swapchainImageViews.clear();
   auto oldSwapchain = std::move(m_swapchain);
   ```

3. **Re-query Surface Capabilities**: Surface properties may have changed.
   ```cpp
   m_surfaceCapabilities = m_physicalDevice.getSurfaceCapabilitiesKHR(m_surface.get());
   // Re-query formats and present modes
   ```

4. **Create New Swapchain**: Pass old swapchain for resource recycling.
   ```cpp
   vk::SwapchainCreateInfoKHR createInfo;
   createInfo.oldSwapchain = oldSwapchain.get();  // Enables resource recycling
   // ... set other parameters ...
   try {
       m_swapchain = m_device->createSwapchainKHRUnique(createInfo);
   } catch (const vk::SystemError&) {
       m_swapchain = std::move(oldSwapchain);  // Restore old swapchain on failure
       return false;
   }
   ```

5. **Destroy Old Swapchain**: Only after new one is successfully created.
   ```cpp
   oldSwapchain.reset();
   ```

6. **Recreate Image Views**: Create views for each new swapchain image.
   ```cpp
   std::vector<vk::Image> swapchainImages = m_device->getSwapchainImagesKHR(m_swapchain.get());
   for (const auto& image : swapchainImages) {
       // Create image view for each swapchain image
   }
   ```

**Key Points**:
- Old swapchain preserved during creation to avoid destroying in-use images
- On failure, old swapchain is restored and function returns `false`
- Surface capabilities must be re-queried as they may have changed

### 3.3 Render Target Recreation

**Function**: `VulkanRenderTargets::resize(vk::Extent2D extent)`

**Called At**: After swapchain recreation (`VulkanRenderer.cpp`)

**Recreated Resources**:
- G-buffer attachments (color, normal, position, specular, emissive)
- Post-processing targets (bloom, SMAA, etc.)
- Scene HDR target (if active)

**Requirement**: Render targets must match swapchain extent for correct rendering.

### 3.4 Acquisition Strategies

The renderer provides two acquisition functions with different error handling approaches.

**Graceful Degradation** (`VulkanRenderer::acquireImage`) - Returns invalid index on failure:
```cpp
uint32_t VulkanRenderer::acquireImage(VulkanFrame& frame) {
    auto result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());

    if (result.needsRecreate) {
        const auto extent = m_vulkanDevice->swapchainExtent();
        if (!m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
            return std::numeric_limits<uint32_t>::max();  // Failed
        }
        m_renderTargets->resize(m_vulkanDevice->swapchainExtent());

        // Retry acquire after successful recreation
        result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());
        if (!result.success) {
            return std::numeric_limits<uint32_t>::max();
        }
    }
    return result.imageIndex;
}
```

**Exception-Based** (`VulkanRenderer::acquireImageOrThrow`) - Throws on failure:
```cpp
uint32_t VulkanRenderer::acquireImageOrThrow(VulkanFrame& frame) {
    auto result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());

    if (result.needsRecreate) {
        const auto extent = m_vulkanDevice->swapchainExtent();
        if (!m_vulkanDevice->recreateSwapchain(extent.width, extent.height)) {
            throw std::runtime_error("acquireImageOrThrow: swapchain recreation failed");
        }
        m_renderTargets->resize(m_vulkanDevice->swapchainExtent());

        result = m_vulkanDevice->acquireNextImage(frame.imageAvailable());
        if (!result.success) {
            throw std::runtime_error("acquireImageOrThrow: acquire failed after swapchain recreation");
        }
    }
    return result.imageIndex;
}
```

**Usage Guidelines**:
- Use `acquireImage` when frame drops are acceptable
- Use `acquireImageOrThrow` when acquisition must succeed (e.g., critical renders)

---

## 4. Device Lost

### 4.1 Detection

**Current Status**: Device lost errors are detected but not fully recovered. They manifest as:
- Command buffer submission failures (`vk::Result::eErrorDeviceLost`)
- Query result failures
- Fence wait failures (timeout or device lost)

**Fence Wait Failure Handling** (`VulkanFrame.cpp`):
```cpp
void VulkanFrame::wait_for_gpu() {
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

### 4.2 Recovery Strategy (Planned)

**Detection Points**:
- After `vkQueueSubmit` calls
- After `vkWaitForFences` calls
- After `vkGetQueryPoolResults` calls

**Proposed Recovery Steps**:
1. Detect device lost (`VK_ERROR_DEVICE_LOST`)
2. Wait for any remaining device operations (may timeout)
3. Destroy all device-dependent resources
4. Recreate logical device
5. Recreate all resources (pipelines, buffers, textures)
6. Restore application state
7. Resume rendering

**Implementation Challenges**:
- Resource lifetime tracking across device recreation
- Descriptor set recreation with same bindings
- Pipeline cache preservation and restoration
- Application state serialization

---

## 5. Resource Exhaustion

### 5.1 Staging Buffer Exhaustion

The staging ring buffer is used for CPU-to-GPU transfers (texture uploads, buffer updates).

**Configuration** (`VulkanRenderer.h`):
```cpp
static constexpr vk::DeviceSize STAGING_RING_SIZE = 12 * 1024 * 1024; // 12 MiB per frame
```

**Detection**: `VulkanRingBuffer::try_allocate()` returns `std::nullopt` when insufficient space remains.

**Recovery Pattern** (`VulkanTextureManager.cpp`):
```cpp
auto allocOpt = frame.stagingBuffer().try_allocate(static_cast<vk::DeviceSize>(layerSize));
if (!allocOpt) {
    // Staging buffer exhausted - defer to next frame
    bm_unlock(frameHandle);
    remaining.push_back(baseFrame);
    stagingFailed = true;
    break;
}
```

**Behavior**: Texture uploads deferred to next frame when staging buffer exhausted. Rendering continues with previous textures or fallbacks.

**Mitigation Strategies**:
| Strategy | Trade-off |
|----------|-----------|
| Increase `STAGING_RING_SIZE` | Higher memory usage |
| Reduce texture sizes | Lower visual quality |
| Preload critical textures | Longer load times |
| Use texture streaming | Implementation complexity |

### 5.2 Descriptor Pool Exhaustion

**Current Status**: Should not occur with current pool sizing (pools are sized for fixed usage patterns).

**Pool Sizing** (`VulkanDescriptorLayouts.cpp`):

*Global Descriptor Pool*:
| Type | Count | Purpose |
|------|-------|---------|
| `CombinedImageSampler` | 6 | G-buffer (5) + depth (1) |
| Max Sets | 1 | Single global set |

*Model Descriptor Pool*:
| Type | Count | Purpose |
|------|-------|---------|
| `StorageBuffer` | `kFramesInFlight` | Vertex heap (1 per frame) |
| `StorageBufferDynamic` | `kFramesInFlight` | Transform buffer (1 per frame) |
| `CombinedImageSampler` | `kFramesInFlight * kMaxBindlessTextures` | Bindless textures |
| `UniformBufferDynamic` | `kFramesInFlight` | Model data (1 per frame) |
| Max Sets | `kFramesInFlight` | One set per frame-in-flight |

**Detection**: `vk::Device::allocateDescriptorSets()` throws `vk::OutOfPoolMemoryError`.

**Potential Recovery** (not currently implemented):
```cpp
// Pseudocode for dynamic pool allocation
try {
    set = device.allocateDescriptorSets(allocInfo);
} catch (const vk::OutOfPoolMemoryError&) {
    auto newPool = createDescriptorPool();
    m_pools.push_back(std::move(newPool));
    allocInfo.descriptorPool = m_pools.back().get();
    set = device.allocateDescriptorSets(allocInfo);
}
```

### 5.3 Bindless Slot Exhaustion

**Slot Layout** (defined in `VulkanConstants.h`):
| Slot | Purpose |
|------|---------|
| 0 | Fallback (black texture) - always valid |
| 1 | Default base texture |
| 2 | Default normal map |
| 3 | Default specular map |
| 4-1023 | Dynamic texture slots (1020 available) |

```cpp
constexpr uint32_t kMaxBindlessTextures = 1024;
constexpr uint32_t kBindlessTextureSlotFallback = 0;
constexpr uint32_t kBindlessTextureSlotDefaultBase = 1;
constexpr uint32_t kBindlessTextureSlotDefaultNormal = 2;
constexpr uint32_t kBindlessTextureSlotDefaultSpec = 3;
constexpr uint32_t kBindlessFirstDynamicTextureSlot = 4;
```

**Detection**: When dynamic slots (4-1023) are exhausted, new texture requests fall back to slot 0.

**Recovery**: Fallback texture (black) used - rendering continues but textures appear black.

**Mitigation Strategies**:
| Strategy | Consideration |
|----------|---------------|
| Increase `kMaxBindlessTextures` | Requires device limit check (`maxDescriptorSetSampledImages`) |
| Use texture atlasing | Reduces unique texture count |
| Implement texture streaming | Load/unload based on visibility |
| Delete unused textures | Free slots for reuse |

---

## 6. Validation Errors

### 6.1 Validation Layer Setup

**Enabled When**: Debug builds (`FSO_DEBUG`) or `Cmdline_graphics_debug_output` command-line flag.

**Layers Enabled**:
- `VK_LAYER_KHRONOS_validation` - Comprehensive validation layer

**Debug Callback** (`VulkanDevice.cpp`):

The validation callback includes throttling to prevent log spam from repeated errors:

```cpp
VkBool32 VKAPI_PTR debugReportCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageTypes,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/)
{
    // Track message frequency with hash-based deduplication
    static std::mutex s_mutex;
    static std::unordered_map<uint64_t, uint32_t> s_counts;

    // ... compute message hash ...

    // Throttling behavior:
    // - First occurrence: always logged
    // - 2nd-10th occurrences: logged normally
    // - 11th occurrence: logged with "suppressing further duplicates" notice
    // - 12th+ occurrences: suppressed (periodic reminder every N occurrences)
}
```

**Output Format**:
```
Validation[ERROR] [VALIDATION] id=123 name=VUID-xxx: <message>
  object[0]: type=Image handle=0x1234 name=GBuffer0
  CmdBufLabel[0]: RenderGBuffer
```

### 6.2 Device Limit Validation

Before creating descriptor layouts, device limits are validated (`VulkanDescriptorLayouts.cpp`):

```cpp
void VulkanDescriptorLayouts::validateDeviceLimits(const vk::PhysicalDeviceLimits& limits) {
    // Hard assert - no silent clamping
    Assertion(limits.maxDescriptorSetSampledImages >= kMaxBindlessTextures,
              "Device maxDescriptorSetSampledImages (%u) < required %u. "
              "Vulkan model rendering not supported on this device.",
              limits.maxDescriptorSetSampledImages, kMaxBindlessTextures);

    Assertion(limits.maxDescriptorSetStorageBuffers >= 1,
              "Device maxDescriptorSetStorageBuffers (%u) < required 1",
              limits.maxDescriptorSetStorageBuffers);

    Assertion(limits.maxDescriptorSetStorageBuffersDynamic >= 1,
              "Device maxDescriptorSetStorageBuffersDynamic (%u) < required 1",
              limits.maxDescriptorSetStorageBuffersDynamic);
}
```

**Push Constant Validation** (`VulkanDescriptorLayouts.cpp`):
```cpp
// Static assert: push constants within spec minimum (256 bytes for Vulkan 1.4)
static_assert(sizeof(ModelPushConstants) <= 256,
              "ModelPushConstants exceeds guaranteed minimum push constant size");
static_assert(sizeof(ModelPushConstants) % 4 == 0,
              "ModelPushConstants size must be multiple of 4");
```

### 6.3 Common Validation Errors

| Error | Cause | Fix |
|-------|-------|-----|
| **Invalid Descriptor Binding** | Descriptor set not bound before draw | Ensure `bindDescriptorSets()` called before draw |
| **Invalid Image Layout** | Image layout mismatch (e.g., sampling from `COLOR_ATTACHMENT`) | Add pipeline barrier to transition layout |
| **Invalid Push Constant Range** | Push constant size exceeds `maxPushConstantsSize` | Split push constants or use uniform buffer |
| **Resource Lifetime Violation** | Using destroyed resource | Track resource lifetime, defer destruction |
| **Missing Barrier** | Read-after-write without synchronization | Insert appropriate memory/pipeline barrier |
| **Incompatible Render Pass** | Framebuffer/render pass attachment mismatch | Verify attachment formats and counts match |

### 6.4 Assertions

**Usage**: Development-time invariants that should never fail in correct code.

**Pattern** (`VulkanRenderer.cpp`):
```cpp
Assertion(m_renderingSession != nullptr,
          "beginFrame requires an active rendering session");
```

**Behavior**:
| Build | Behavior |
|-------|----------|
| Debug | Assertion failure stops execution with message |
| Release | Assertions disabled (no runtime check) |

**Guidelines**:
- Use assertions for programmer errors (invariant violations)
- Use return codes/exceptions for runtime errors (recoverable conditions)
- Never use assertions for user input validation

---

## 7. Recovery Patterns

### 7.1 Pattern: Swapchain Recreation with Retry

```cpp
uint32_t acquireImageWithRetry(VulkanFrame& frame) {
    auto result = device->acquireNextImage(frame.imageAvailable());

    if (result.needsRecreate) {
        if (!recreateSwapchain()) {
            return INVALID_IMAGE_INDEX;  // Unrecoverable
        }
        recreateRenderTargets();

        // Retry acquire after recreation
        result = device->acquireNextImage(frame.imageAvailable());
        if (!result.success) {
            return INVALID_IMAGE_INDEX;
        }
    }

    return result.imageIndex;
}
```

### 7.2 Pattern: Graceful Degradation with Fallback

```cpp
void uploadTextureWithFallback(int textureHandle) {
    auto allocOpt = stagingBuffer.try_allocate(textureSize);
    if (!allocOpt) {
        // Staging exhausted - defer to next frame
        pendingUploads.push_back(textureHandle);
        return;
    }

    // Proceed with upload
    performUpload(*allocOpt, textureHandle);
}
```

### 7.3 Pattern: Resource Validation Guard

```cpp
bool ensureResourceValid(ResourceHandle handle, const char* context) {
    if (!handle.isValid()) {
        Warning(LOCATION, "%s: Invalid resource handle", context);
        return false;
    }

    if (!resourceExists(handle)) {
        Warning(LOCATION, "%s: Resource does not exist", context);
        return false;
    }

    return true;
}
```

### 7.4 Pattern: Exception-to-Return-Code Conversion

```cpp
bool tryOperation() {
    try {
        performVulkanOperation();
        return true;
    } catch (const vk::SystemError& err) {
        vkprintf("Operation failed: %s\n", err.what());
        return false;
    }
}
```

### 7.5 Pattern: Frame Synchronization with Timeout

```cpp
void waitForFrameComplete(VulkanFrame& frame, uint64_t timeoutNs) {
    auto result = device.waitForFences(1, &frame.fence(), VK_TRUE, timeoutNs);

    if (result == vk::Result::eTimeout) {
        Warning(LOCATION, "Frame fence wait timed out - GPU may be stalled");
        // Consider device lost recovery
    } else if (result != vk::Result::eSuccess) {
        throw std::runtime_error("Fence wait failed");
    }
}
```

---

## 8. Common Issues

### Issue 1: Swapchain Recreation Fails

**Symptoms**: `recreateSwapchain()` returns false, rendering stops

**Possible Causes**:
| Cause | Diagnostic | Fix |
|-------|------------|-----|
| Surface lost (window destroyed) | Check if surface handle is valid | Exit gracefully |
| Device lost | Check for `VK_ERROR_DEVICE_LOST` | Recreate device (not implemented) |
| Invalid dimensions | Check if width/height are 0 | Wait for valid dimensions |
| Driver bug | Check driver version | Update GPU drivers |

**Debugging**:
```cpp
if (!recreateSwapchain(width, height)) {
    vkprintf("Swapchain recreation failed: width=%u height=%u\n", width, height);
    // Log surface capabilities
    auto caps = physicalDevice.getSurfaceCapabilitiesKHR(surface);
    vkprintf("Surface extent: %ux%u\n",
             caps.currentExtent.width, caps.currentExtent.height);
}
```

### Issue 2: Validation Errors in Release Builds

**Symptoms**: Crashes, rendering corruption, undefined behavior

**Causes**:
- Validation layers disabled in release
- Assertions disabled in release
- Resource lifetime bugs masked by debug timing

**Diagnosis**:
1. Enable validation in release: Set `Cmdline_graphics_debug_output` flag
2. Run with `VK_LAYER_KHRONOS_validation` enabled
3. Check for use-after-free with address sanitizers

**Prevention**:
- Test release builds with validation enabled periodically
- Use deferred destruction for all GPU resources
- Implement explicit resource lifetime tracking

### Issue 3: Staging Buffer Always Exhausted

**Symptoms**: Textures never upload, always deferred, visual artifacts

**Causes**:
| Cause | Diagnostic |
|-------|------------|
| Buffer too small | Log `stagingBuffer().remaining()` |
| Too many large textures per frame | Count pending uploads |
| Uploads never completing | Check if staging buffer resets each frame |

**Debugging**:
```cpp
vkprintf("Staging buffer: %zu / %zu bytes remaining\n",
         frame.stagingBuffer().remaining(),
         frame.stagingBuffer().size());
vkprintf("Pending texture uploads: %zu\n", pendingUploads.size());
```

**Fix**:
1. Increase `STAGING_RING_SIZE` in `VulkanRenderer.h`
2. Reduce texture resolution or compression
3. Spread texture loading across multiple frames
4. Preload textures during loading screens

### Issue 4: Fence Wait Timeout

**Symptoms**: Rendering hangs, frame rate drops to 0

**Causes**:
| Cause | Diagnostic |
|-------|------------|
| GPU hang | Check GPU utilization |
| Infinite shader loop | Review shader code |
| Excessive draw calls | Profile GPU workload |
| Device lost | Check for driver crash |

**Debugging**:
```cpp
// Use finite timeout to detect hangs
auto result = device.waitForFences(1, &fence, VK_TRUE, 5'000'000'000ull); // 5 seconds
if (result == vk::Result::eTimeout) {
    vkprintf("GPU appears hung - fence not signaled after 5 seconds\n");
    // Trigger device lost recovery
}
```

### Issue 5: Bindless Texture Corruption

**Symptoms**: Wrong textures displayed, textures flickering

**Causes**:
| Cause | Fix |
|-------|-----|
| Slot reused before GPU finished | Defer slot reuse to next frame |
| Descriptor not updated | Verify descriptor write occurred |
| Wrong slot index in shader | Check push constant data |

**Prevention**:
- Slot 0 always contains valid fallback texture
- Slots 1-3 contain well-known defaults
- Shader samples from slot 0 for invalid indices

---

## Appendix: Error Code Reference

| Error Code | Category | Recovery | Notes |
|------------|----------|----------|-------|
| `VK_ERROR_OUT_OF_DATE_KHR` | Swapchain | Recreate swapchain | Common on window resize |
| `VK_SUBOPTIMAL_KHR` | Swapchain | Recreate swapchain (optional) | Rendering still possible |
| `VK_ERROR_DEVICE_LOST` | Device | Recreate device (not implemented) | GPU crash or driver reset |
| `VK_ERROR_DRIVER_FAILED` | Device | Exit gracefully | Driver internal error |
| `VK_ERROR_OUT_OF_POOL_MEMORY` | Descriptor | Create new pool | Should not occur with current sizing |
| `VK_ERROR_OUT_OF_HOST_MEMORY` | Memory | Exit gracefully | System RAM exhausted |
| `VK_ERROR_OUT_OF_DEVICE_MEMORY` | Memory | Reduce quality, exit if persistent | GPU VRAM exhausted |
| `VK_ERROR_SURFACE_LOST_KHR` | Surface | Exit gracefully | Window destroyed |
| `VK_TIMEOUT` | Synchronization | Retry or abort | Fence/semaphore wait exceeded |
| `VK_ERROR_INITIALIZATION_FAILED` | Init | Exit gracefully | Vulkan cannot initialize |

---

## References

**Source Files**:
- `code/graphics/vulkan/VulkanDevice.cpp` - Error detection (`acquireNextImage`, `present`), swapchain recreation, validation callback
- `code/graphics/vulkan/VulkanRenderer.cpp` - Image acquisition with retry logic
- `code/graphics/vulkan/VulkanFrame.cpp` - Fence wait error handling
- `code/graphics/vulkan/VulkanTextureManager.cpp` - Staging buffer exhaustion handling
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp` - Device limit validation
- `code/graphics/vulkan/VulkanConstants.h` - Bindless texture slot definitions

**External Documentation**:
- [Vulkan Specification - Error Handling](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#fundamentals-errorcodes)
- [Vulkan Specification - Synchronization](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#synchronization)
- [Vulkan Best Practices - Error Handling](https://github.com/KhronosGroup/Vulkan-Guide/blob/main/chapters/error_handling.adoc)
