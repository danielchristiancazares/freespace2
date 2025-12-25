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

---

## 1. Overview

The Vulkan renderer uses a multi-layered error handling strategy:

- **Assertions**: Development-time invariants (disabled in release)
- **Return Codes**: Recoverable errors (swapchain recreation, resource exhaustion)
- **Exceptions**: Unrecoverable errors (device initialization failures)
- **Validation Layers**: Runtime error detection (enabled in debug builds)

**Design Philosophy**: Fail fast during development, degrade gracefully in production.

**Key Files**:
- `code/graphics/vulkan/VulkanDevice.cpp` - Device-level error handling
- `code/graphics/vulkan/VulkanRenderer.cpp` - Renderer-level error handling
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp` - Device limit validation

---

## 2. Error Categories

### 2.1 Recoverable Errors

**Swapchain Recreation**:
- `VK_ERROR_OUT_OF_DATE_KHR`: Swapchain incompatible with surface
- `VK_SUBOPTIMAL_KHR`: Swapchain valid but suboptimal

**Resource Exhaustion**:
- Staging buffer exhausted (texture uploads deferred)
- Descriptor pool exhausted (should not occur with current sizing)
- Bindless slots exhausted (fallback to slot 0)

**Recovery**: Automatic retry or graceful degradation

### 2.2 Unrecoverable Errors

**Device Lost**:
- `VK_ERROR_DEVICE_LOST`: GPU hung or crashed
- `VK_ERROR_DRIVER_FAILED`: Driver error

**Initialization Failures**:
- Device creation fails
- Required extensions unavailable
- Required features unsupported

**Recovery**: None - application should exit gracefully

### 2.3 Validation Errors

**Development-Time Checks**:
- Invalid API usage
- Resource lifetime violations
- Descriptor binding errors

**Recovery**: Assertion failure (debug) or undefined behavior (release)

---

## 3. Swapchain Recreation

### 3.1 Detection

**Acquire Next Image** (`VulkanDevice.cpp:996-1023`):
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
    // ...
}
```

**Present** (`VulkanDevice.cpp:1025-1056`):
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

**Process** (`VulkanDevice.cpp:1058-1171`):

1. **Wait for GPU Idle**:
   ```cpp
   m_device->waitIdle();  // Ensure no in-flight commands
   ```

2. **Destroy Old Resources**:
   ```cpp
   m_swapchainImageViews.clear();
   auto oldSwapchain = std::move(m_swapchain);
   ```

3. **Re-query Surface Capabilities**:
   ```cpp
   m_surfaceCapabilities = m_physicalDevice.getSurfaceCapabilitiesKHR(m_surface.get());
   // Re-query formats and present modes
   ```

4. **Create New Swapchain**:
   ```cpp
   vk::SwapchainCreateInfoKHR createInfo;
   createInfo.oldSwapchain = oldSwapchain.get();  // Preserve old swapchain
   // ... set other parameters ...
   m_swapchain = m_device->createSwapchainKHRUnique(createInfo);
   ```

5. **Recreate Image Views**:
   ```cpp
   auto images = m_device->getSwapchainImagesKHR(m_swapchain.get());
   for (auto image : images) {
       // Create image view for each swapchain image
   }
   ```

**Key Point**: Old swapchain preserved until new one is created to avoid destroying in-use images.

### 3.3 Render Target Recreation

**Function**: `VulkanRenderTargets::resize(vk::Extent2D extent)`

**Called At**: After swapchain recreation (`VulkanRenderer.cpp:241`)

**Process**: Recreates render targets that depend on swapchain size:
- G-buffer attachments
- Post-processing targets (bloom, SMAA, etc.)
- Scene HDR target (if active)

**Key Point**: Render targets must be recreated to match new swapchain size.

### 3.4 Retry After Recreation

**Pattern** (`VulkanRenderer.cpp:231-256`):
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
        return result.imageIndex;
    }
    // ...
}
```

**Key Point**: Always retry acquire after recreation to avoid dropping frames.

---

## 4. Device Lost

### 4.1 Detection

**Current Status**: Device lost errors are not explicitly handled. They manifest as:
- Command buffer submission failures
- Query result failures
- Device wait timeouts

**Future Enhancement**: Add device lost detection and recovery path.

### 4.2 Recovery Strategy (Planned)

**Detection Points**:
- After command buffer submission
- After query operations
- During device wait operations

**Recovery Steps**:
1. Detect device lost (`VK_ERROR_DEVICE_LOST`)
2. Wait for device idle
3. Recreate logical device
4. Recreate all resources
5. Resume rendering

**Challenges**:
- Resource lifetime tracking
- Descriptor set recreation
- Pipeline cache preservation

---

## 5. Resource Exhaustion

### 5.1 Staging Buffer Exhaustion

**Detection**: `VulkanRingBuffer::try_allocate()` returns `std::nullopt`

**Recovery** (`VulkanTextureManager.cpp:775-782`):
```cpp
auto allocOpt = frame.stagingBuffer().try_allocate(static_cast<vk::DeviceSize>(layerSize));
if (!allocOpt) {
    // Staging buffer exhausted - defer to next frame.
    bm_unlock(frameHandle);
    remaining.push_back(baseFrame);
    stagingFailed = true;
    break;
}
```

**Behavior**: Texture uploads deferred to next frame when staging buffer exhausted.

**Mitigation**:
- Increase `STAGING_RING_SIZE` (currently 12 MB per frame)
- Reduce texture sizes
- Preload critical textures

### 5.2 Descriptor Pool Exhaustion

**Current Status**: Should not occur with current pool sizing.

**Pool Sizing**:
- Global set (`VulkanDescriptorLayouts.cpp:93-103`): 1 set, 6 descriptors (G-buffer + depth)
- Model sets (`VulkanDescriptorLayouts.cpp:174-192`): `kFramesInFlight` sets, large bindless array (`kMaxBindlessTextures` samplers)

**Detection**: `vk::Device::allocateDescriptorSets()` throws `vk::OutOfPoolMemoryError`

**Recovery** (if implemented):
- Create additional descriptor pool
- Allocate from new pool
- Track multiple pools for cleanup

### 5.3 Bindless Slot Exhaustion

**Detection**: `getBindlessSlotIndex()` returns slot 0 (fallback)

**Recovery**: Fallback texture used (black texture)

**Mitigation**:
- Increase `kMaxBindlessTextures` (requires device limit check)
- Use texture atlasing
- Delete unused textures

**Current Limit**: 1024 slots (4 reserved + 1020 dynamic)

---

## 6. Validation Errors

### 6.1 Validation Layer Setup

**Enabled In**: Debug builds (`VulkanDevice.cpp`)

**Layers**:
- `VK_LAYER_KHRONOS_validation` (enabled when `FSO_DEBUG` or `Cmdline_graphics_debug_output` is set)

**Output**: Validation messages routed through `vkprintf()` with throttling to avoid log spam from repeated warnings

### 6.2 Common Validation Errors

**Invalid Descriptor Binding**:
- **Cause**: Descriptor set not bound before draw
- **Fix**: Ensure `bindDescriptorSets()` called before draw

**Invalid Image Layout**:
- **Cause**: Image layout mismatch (e.g., sampling from `COLOR_ATTACHMENT`)
- **Fix**: Add pipeline barrier to transition layout

**Invalid Push Constant Range**:
- **Cause**: Push constant size exceeds `maxPushConstantsSize`
- **Fix**: Split push constants or use uniform buffer

**Resource Lifetime Violation**:
- **Cause**: Using destroyed resource
- **Fix**: Track resource lifetime, defer destruction

### 6.3 Assertions

**Usage**: Development-time invariants

**Pattern** (`VulkanRenderer.cpp:285`):
```cpp
Assertion(m_renderingSession != nullptr, 
          "beginFrame requires an active rendering session");
```

**Behavior**:
- Debug builds: Assertion failure stops execution
- Release builds: Assertions disabled (no check)

**Key Point**: Assertions are for bugs, not error conditions. Use return codes for recoverable errors.

---

## 7. Recovery Patterns

### 7.1 Pattern: Swapchain Recreation with Retry

```cpp
uint32_t acquireImageWithRetry(VulkanFrame& frame) {
    auto result = device->acquireNextImage(frame.imageAvailable());
    
    if (result.needsRecreate) {
        if (!recreateSwapchain()) {
            return INVALID_IMAGE_INDEX;  // Failed
        }
        recreateRenderTargets();
        
        // Retry acquire
        result = device->acquireNextImage(frame.imageAvailable());
        if (!result.success) {
            return INVALID_IMAGE_INDEX;
        }
    }
    
    return result.imageIndex;
}
```

### 7.2 Pattern: Graceful Degradation

```cpp
void uploadTextureWithFallback(int textureHandle) {
    if (!tryUploadTexture(textureHandle)) {
        // Upload failed - mark unavailable, use fallback
        markTextureUnavailable(textureHandle);
        // Rendering continues with fallback texture
    }
}
```

### 7.3 Pattern: Resource Validation

```cpp
bool ensureResourceValid(ResourceHandle handle) {
    if (!handle.isValid()) {
        Warning(LOCATION, "Invalid resource handle");
        return false;
    }
    
    if (!resourceExists(handle)) {
        Warning(LOCATION, "Resource does not exist");
        return false;
    }
    
    return true;
}
```

---

## 8. Common Issues

### Issue 1: Swapchain Recreation Fails

**Symptoms**: `recreateSwapchain()` returns false, rendering stops

**Causes**:
- Surface lost (window destroyed)
- Device lost
- Driver bug

**Debugging**:
```cpp
if (!recreateSwapchain(width, height)) {
    Error(LOCATION, "Swapchain recreation failed");
    // Check surface validity, device state
}
```

**Fix**: Handle surface loss, exit gracefully if device lost

### Issue 2: Validation Errors in Release

**Symptoms**: Undefined behavior, crashes

**Causes**:
- Assertions disabled in release
- Validation layers disabled
- Resource lifetime bugs

**Fix**: Enable validation layers in release builds for testing, fix resource lifetime bugs

### Issue 3: Staging Buffer Always Exhausted

**Symptoms**: Textures never upload, always deferred

**Causes**:
- Staging buffer too small
- Too many large textures per frame
- Staging buffer fragmentation

**Debugging**:
```cpp
Warning(LOCATION, "Staging buffer remaining: %zu bytes", 
        frame.stagingBuffer().remaining());
Warning(LOCATION, "Pending uploads: %zu", pendingUploads.size());
```

**Fix**: Increase staging buffer size, reduce texture sizes, preload textures

### Issue 4: Device Lost Not Detected

**Symptoms**: Rendering hangs, no error reported

**Causes**:
- Device lost detection not implemented
- GPU timeout not handled

**Fix**: Add device lost detection after submissions, implement recovery path

---

## Appendix: Error Code Reference

| Error Code | Category | Recovery |
|------------|----------|----------|
| `VK_ERROR_OUT_OF_DATE_KHR` | Swapchain | Recreate swapchain |
| `VK_SUBOPTIMAL_KHR` | Swapchain | Recreate swapchain (optional) |
| `VK_ERROR_DEVICE_LOST` | Device | Recreate device (not implemented) |
| `VK_ERROR_DRIVER_FAILED` | Device | Exit gracefully |
| `VK_ERROR_OUT_OF_POOL_MEMORY` | Descriptor | Create new pool (should not occur) |
| `VK_ERROR_OUT_OF_HOST_MEMORY` | Memory | Exit gracefully |
| `VK_ERROR_OUT_OF_DEVICE_MEMORY` | Memory | Reduce quality, exit if persistent |

---

## References

- `code/graphics/vulkan/VulkanDevice.cpp:996-1023` - `acquireNextImage()` error detection
- `code/graphics/vulkan/VulkanDevice.cpp:1025-1056` - `present()` error detection
- `code/graphics/vulkan/VulkanDevice.cpp:1058-1171` - `recreateSwapchain()` implementation
- `code/graphics/vulkan/VulkanRenderer.cpp:231-256` - Image acquisition with retry
- `code/graphics/vulkan/VulkanTextureManager.cpp:775-782` - Staging buffer exhaustion handling
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp:12-30` - Device limit validation
- Vulkan Specification - Error Handling (runtime behavior)

