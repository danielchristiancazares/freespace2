# Vulkan Renderer Bug Report

**Date:** 2025-12-12
**Tool:** GPT-5.2 Pro (bug detection), Claude Opus 4.5 (verification)

## Bug #1: Frame Count Constant Mismatch

### Summary

Two different constants define "frames in flight" with conflicting values, causing potential synchronization and resource lifetime bugs.

### Details

| Constant | Value | Location |
|----------|-------|----------|
| `MAX_FRAMES_IN_FLIGHT` | 2 | VulkanRenderer.h:139 |
| `kFramesInFlight` | 3 | VulkanConstants.h:8 |

The renderer's actual frame cycling uses `MAX_FRAMES_IN_FLIGHT = 2`:

```cpp
// VulkanRenderer.cpp:279
m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
```

But texture manager and descriptor code uses `kFramesInFlight = 3`:

```cpp
// VulkanTextureManager.h:56
std::array<bool, kFramesInFlight> descriptorWritten = {false, false, false};

// VulkanTextureManager.cpp:810
m_pendingDestructions.push_back({textureHandle, m_currentFrame + kFramesInFlight});
```

### Impact

1. **Wasted memory**: Resources held one frame longer than necessary before destruction
2. **Unused array slots**: Third element of `descriptorWritten` array never accessed
3. **Inconsistent codebase**: Two constants for the same concept invites future bugs

### Root Cause

The value `3` likely came from confusion with `kGBufferCount = 3` (VulkanRenderTargets.h:13), which defines the number of G-buffer attachments for deferred rendering. This is unrelated to frame-in-flight synchronization.

Another possible source: swapchain image count, which can be 3 (`minImageCount + 1` in VulkanDevice.cpp:649). However, swapchain image count and frames in flight are distinct concepts.

### Recommended Fix

Consolidate to a single constant using the idiomatic Vulkan name:

```cpp
// VulkanConstants.h
constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 2;
constexpr uint32_t kMaxBindlessTextures = 1024;
constexpr uint32_t kModelSetsPerPool = 4096 * MAX_FRAMES_IN_FLIGHT;
```

Remove `MAX_FRAMES_IN_FLIGHT` from VulkanRenderer.h and update all references to `kFramesInFlight` throughout the codebase.

### Files Affected

- `code/graphics/vulkan/VulkanConstants.h`
- `code/graphics/vulkan/VulkanRenderer.h`
- `code/graphics/vulkan/VulkanRenderer.cpp`
- `code/graphics/vulkan/VulkanTextureManager.h`
- `code/graphics/vulkan/VulkanTextureManager.cpp`
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

---

## Pending Analysis

GPT-5.2 Pro identified additional potential issues (not yet verified):

- Ring buffer wrapping mid-frame causing silent data corruption
- Per-draw descriptor sets missing binding2 (dynamic UBO)
- Swapchain out-of-date handling via assertion instead of graceful recovery
- Fallback texture potentially uninitialized
- Descriptor pool exhaustion under heavy load

These require further investigation.
