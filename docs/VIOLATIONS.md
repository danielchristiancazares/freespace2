# Design Philosophy Violations Report

Based on the principles established in REFACTOR.md, the following violations remain in the codebase.

## 1. Duplicated Recording State

**Principle violated:** Single source of truth

**Location:** `VulkanRenderer.h:191` and `FrameLifecycleTracker.h:19`

VulkanRenderer maintains `m_isRecording` alongside `m_frameLifecycle` (which has its own `m_isRecording`). Both are set to identical values at identical times in `beginFrame()` and `endFrame()`.

```cpp
// VulkanRenderer.h:191
bool m_isRecording = false;
FrameLifecycleTracker m_frameLifecycle;

// VulkanRenderer.cpp:167
m_frameLifecycle.begin(m_currentFrame);
m_isRecording = true;  // Duplicate!

// VulkanRenderer.cpp:207-208
m_frameLifecycle.end();
m_isRecording = false;  // Duplicate!
```

**Fix:** Remove `VulkanRenderer::m_isRecording` and use `m_frameLifecycle.isRecording()` everywhere.

---

## 2. UINT32_MAX Sentinels in VulkanFrame

**Principle violated:** Use std::optional instead of sentinel values

**Location:** `VulkanFrame.h:81-84`

```cpp
uint64_t m_lastSubmitTimeline = 0;
uint32_t m_lastSubmitImageIndex = UINT32_MAX;
uint32_t m_lastSubmitFrameIndex = UINT32_MAX;
uint64_t m_lastSubmitSerial = 0;
bool m_hasSubmitInfo = false;
```

Uses UINT32_MAX sentinel plus a boolean flag to represent "no submit info yet." This is the exact pattern REFACTOR.md eliminates for uniform bindings.

**Fix:** Replace with optional struct:

```cpp
struct SubmitInfo {
    uint32_t imageIndex;
    uint32_t frameIndex;
    uint64_t serial;
    uint64_t timeline;
};
std::optional<SubmitInfo> m_lastSubmitInfo;
```

---

## 3. Silent No-ops Instead of Assertions

**Principle violated:** Assertions for impossible states after initialization

**Location:** `VulkanRenderer.cpp:391-426`

Multiple buffer operations silently do nothing when `m_bufferManager` is null:

```cpp
void VulkanRenderer::deleteBuffer(gr_buffer_handle handle) {
    if (m_bufferManager) {  // Silent no-op if null
        m_bufferManager->deleteBuffer(handle);
    }
}

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void* data) {
    if (m_bufferManager) {  // Silent no-op if null
        m_bufferManager->updateBufferData(handle, size, data);
    }
}
```

Affected methods: `deleteBuffer`, `updateBufferData`, `updateBufferDataOffset`, `flushMappedBuffer`, `resizeBuffer`.

**Fix:** Per REFACTOR.md section 4A, use assertions:

```cpp
void VulkanRenderer::deleteBuffer(gr_buffer_handle handle) {
    Assertion(m_bufferManager != nullptr, "deleteBuffer called before buffer manager initialization");
    m_bufferManager->deleteBuffer(handle);
}
```

---

## 4. Guard Clauses Returning Fallback Values

**Principle violated:** Invalid states should be impossible, not guarded

**Location:** `VulkanRenderer.cpp:361-436`

Several methods return null/empty values when managers don't exist:

```cpp
vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const {
    if (!m_bufferManager) {
        return nullptr;  // Silent fallback masks bugs
    }
    return m_bufferManager->getBuffer(handle);
}

gr_buffer_handle VulkanRenderer::createBuffer(BufferType type, BufferUsageHint usage) {
    if (!m_bufferManager) {
        return gr_buffer_handle::invalid();  // Silent fallback
    }
    return m_bufferManager->createBuffer(type, usage);
}
```

Affected methods: `getBuffer`, `createBuffer`, `mapBuffer`, `getTextureDescriptor`, `queryModelVertexHeapBuffer`.

**Fix:** After initialization completes, these managers always exist. Assert instead of guard:

```cpp
vk::Buffer VulkanRenderer::getBuffer(gr_buffer_handle handle) const {
    Assertion(m_bufferManager != nullptr, "getBuffer called before initialization");
    return m_bufferManager->getBuffer(handle);
}
```

---

## 5. Missing EDS3 Blend State in gr_vulkan_render_model

**Principle violated:** Required pattern for EDS3 devices (REFACTOR.md lines 709-723)

**Location:** `VulkanGraphics.cpp:632-638`

**Severity:** Medium - potential rendering bugs on EDS3 devices

**Complexity:** Not a simple fix. Models can render to G-buffer (3+ attachments) or swapchain (1 attachment). A naive fix using `rt.colorAttachmentCount` with a single value causes undefined behavior when count > 1. Need to either:
1. Create an array of `colorAttachmentCount` blend enable values, or
2. Only apply EDS3 blend state when rendering to swapchain (1 attachment)

Note: The other two draw functions hardcode `1` - this may also be incorrect if they ever render to G-buffer.

The model render function does NOT call `setColorBlendEnableEXT` when EDS3 is supported. The other draw functions implement this correctly:

- `gr_vulkan_render_primitives` (line 914) - correct
- `gr_vulkan_render_primitives_batched` (line 1140) - correct
- `gr_vulkan_render_model` - **missing**

REFACTOR.md explicitly states:

> This applies to: `gr_vulkan_render_model`, `gr_vulkan_render_primitives`, `gr_vulkan_render_primitives_batched`, and any future draw paths.

**Fix:** Add after depth state setup in `gr_vulkan_render_model`:

```cpp
// EDS3 blend state - required for correct blending on EDS3 devices
if (renderer_instance->supportsExtendedDynamicState3()) {
    const auto& caps = renderer_instance->getExtendedDynamicState3Caps();
    if (caps.colorBlendEnable) {
        vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE;
        cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(rt.colorAttachmentCount, &blendEnable));
    }
}
```

---

## Summary

| # | Violation | Severity | Impact |
|---|-----------|----------|--------|
| 1 | Duplicated recording state | Medium | Maintenance burden, potential desync |
| 2 | UINT32_MAX sentinels | Low | Code clarity |
| 3 | Silent no-ops | Medium | Masks initialization bugs |
| 4 | Guard clauses returning fallbacks | Medium | Masks initialization bugs |
| 5 | Missing EDS3 blend state | **High** | Broken blending on EDS3 devices |

Priority order for fixes: 5 > 3 = 4 > 1 > 2
