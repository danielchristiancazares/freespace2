# Vulkan Renderer State Elimination Redesign

## Overview

This redesign eliminates invalid states by construction rather than by guards. The core principle: **the object that begins the render pass is the only place that knows the render target contract (formats + attachment count), and it hands that contract to pipeline creation directly**.

---

## What is Deleted (By Construction, Not By Guards)

1. **Pending vs Active dual state** - Replace with polymorphic "target state" object. There is only one truth.

2. **Render-pass-active bool + cleanup guards** - Replace with RAII `ActivePass` value. "A pass is active" means the value exists. Ending is destruction.

3. **UINT32_MAX sentinel for "unset" uniform offsets** - Replace with `std::optional<ModelUniformBinding>` + fallback uniform buffer bound at frame start.

4. **Handle exists but VkBuffer doesn't** - Replace with `VulkanBufferManager::ensureBuffer(handle, minSize)` that makes the buffer exist at first use.

5. **Pipeline key built from "current" queried state + later asserted** - Replace with: call `ensureRenderingStarted()` first, receive the actual active target info, build the key from that.

---

## Code: Architectural Rewrite

### 1) VulkanRenderingSession - Target States + RAII ActivePass

#### VulkanRenderingSession.h

```cpp
#pragma once

#include "VulkanClip.h"
#include "VulkanDescriptorLayouts.h"
#include "VulkanDevice.h"
#include "VulkanRenderTargets.h"

#include <vulkan/vulkan.hpp>

#include <memory>
#include <optional>

namespace graphics::vulkan {

class VulkanRenderingSession {
public:
    struct RenderTargetInfo {
        vk::Format colorFormat = vk::Format::eUndefined;
        uint32_t   colorAttachmentCount = 1;
        vk::Format depthFormat = vk::Format::eUndefined; // eUndefined => no depth attachment
    };

    VulkanRenderingSession(VulkanDevice& device,
                           VulkanRenderTargets& targets,
                           VulkanDescriptorLayouts& descriptorLayouts);

    void beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex, vk::DescriptorSet globalDescriptorSet);
    void endFrame(vk::CommandBuffer cmd, uint32_t imageIndex);

    // Starts dynamic rendering for the *current target* if needed; returns the *actual* target contract.
    const RenderTargetInfo& ensureRenderingActive(vk::CommandBuffer cmd, uint32_t imageIndex);

    // Boundary-facing state transitions (no "pending", no dual state)
    void requestSwapchainTarget();                  // swapchain + depth
    void beginDeferredPass(bool clearNonColorBufs); // selects gbuffer target
    void endDeferredGeometry(vk::CommandBuffer cmd);// transitions gbuffer -> shader read, selects swapchain-no-depth

    void requestClear();
    void setClearColor(float r, float g, float b, float a);

    void setClipRect(const ClipRect& clip);

private:
    class RenderTargetState {
    public:
        virtual ~RenderTargetState() = default;
        virtual RenderTargetInfo info(const VulkanDevice&, const VulkanRenderTargets&) const = 0;
        virtual void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) = 0;
    };

    class SwapchainWithDepthTarget final : public RenderTargetState {
    public:
        RenderTargetInfo info(const VulkanDevice&, const VulkanRenderTargets&) const override;
        void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
    };

    class DeferredGBufferTarget final : public RenderTargetState {
    public:
        RenderTargetInfo info(const VulkanDevice&, const VulkanRenderTargets&) const override;
        void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
    };

    class SwapchainNoDepthTarget final : public RenderTargetState {
    public:
        RenderTargetInfo info(const VulkanDevice&, const VulkanRenderTargets&) const override;
        void begin(VulkanRenderingSession& session, vk::CommandBuffer cmd, uint32_t imageIndex) override;
    };

    struct ActivePass {
        vk::CommandBuffer cmd{};
        explicit ActivePass(vk::CommandBuffer c) : cmd(c) {}
        ~ActivePass() { cmd.endRendering(); }

        ActivePass(const ActivePass&) = delete;
        ActivePass& operator=(const ActivePass&) = delete;
        ActivePass(ActivePass&&) = default;
        ActivePass& operator=(ActivePass&&) = default;
    };

    void endActivePass();
    void applyDynamicState(vk::CommandBuffer cmd);

    void beginSwapchainRenderingInternal(vk::CommandBuffer cmd, uint32_t imageIndex);
    void beginGBufferRenderingInternal(vk::CommandBuffer cmd);
    void beginSwapchainRenderingNoDepthInternal(vk::CommandBuffer cmd, uint32_t imageIndex);

    void transitionSwapchainToAttachment(vk::CommandBuffer cmd, uint32_t imageIndex);
    void transitionDepthToAttachment(vk::CommandBuffer cmd);
    void transitionSwapchainToPresent(vk::CommandBuffer cmd, uint32_t imageIndex);
    void transitionGBufferToAttachment(vk::CommandBuffer cmd);
    void transitionGBufferToShaderRead(vk::CommandBuffer cmd);

private:
    VulkanDevice& m_device;
    VulkanRenderTargets& m_targets;
    VulkanDescriptorLayouts& m_descriptorLayouts;

    vk::DescriptorSet m_globalDescriptorSet{};

    std::unique_ptr<RenderTargetState> m_target;
    std::optional<ActivePass> m_activePass;
    RenderTargetInfo m_activeInfo{};

    bool m_shouldClearColor = true;
    bool m_shouldClearDepth = true;
    vk::ClearColorValue m_clearColor{};
};

} // namespace graphics::vulkan
```

#### VulkanRenderingSession.cpp (key sections)

```cpp
#include "VulkanRenderingSession.h"

namespace graphics::vulkan {

VulkanRenderingSession::VulkanRenderingSession(VulkanDevice& device,
                                               VulkanRenderTargets& targets,
                                               VulkanDescriptorLayouts& descriptorLayouts)
    : m_device(device), m_targets(targets), m_descriptorLayouts(descriptorLayouts)
{
    m_target = std::make_unique<SwapchainWithDepthTarget>();
}

void VulkanRenderingSession::beginFrame(vk::CommandBuffer cmd, uint32_t imageIndex, vk::DescriptorSet globalDescriptorSet)
{
    m_globalDescriptorSet = globalDescriptorSet;

    endActivePass();
    m_target = std::make_unique<SwapchainWithDepthTarget>();

    m_shouldClearColor = true;
    m_shouldClearDepth = true;

    transitionSwapchainToAttachment(cmd, imageIndex);
    transitionDepthToAttachment(cmd);
}

void VulkanRenderingSession::endFrame(vk::CommandBuffer cmd, uint32_t imageIndex)
{
    endActivePass();
    transitionSwapchainToPresent(cmd, imageIndex);
}

const VulkanRenderingSession::RenderTargetInfo&
VulkanRenderingSession::ensureRenderingActive(vk::CommandBuffer cmd, uint32_t imageIndex)
{
    if (!m_activePass) {
        m_activeInfo = m_target->info(m_device, m_targets);
        m_target->begin(*this, cmd, imageIndex);
        m_activePass.emplace(cmd);
        applyDynamicState(cmd);
    }
    return m_activeInfo;
}

void VulkanRenderingSession::requestSwapchainTarget()
{
    endActivePass();
    m_target = std::make_unique<SwapchainWithDepthTarget>();
}

void VulkanRenderingSession::beginDeferredPass(bool /*clearNonColorBufs*/)
{
    endActivePass();
    m_shouldClearColor = true;
    m_shouldClearDepth = true;
    m_target = std::make_unique<DeferredGBufferTarget>();
}

void VulkanRenderingSession::endDeferredGeometry(vk::CommandBuffer cmd)
{
    Assertion(dynamic_cast<DeferredGBufferTarget*>(m_target.get()) != nullptr,
              "endDeferredGeometry called when not in deferred gbuffer target");

    endActivePass();
    transitionGBufferToShaderRead(cmd);
    m_target = std::make_unique<SwapchainNoDepthTarget>();
}

void VulkanRenderingSession::endActivePass()
{
    if (m_activePass) {
        m_activePass.reset(); // calls cmd.endRendering() exactly once
    }
}

// ---- Target types ----

VulkanRenderingSession::RenderTargetInfo
VulkanRenderingSession::SwapchainWithDepthTarget::info(const VulkanDevice& device, const VulkanRenderTargets&) const
{
    RenderTargetInfo out{};
    out.colorFormat = device.swapchainFormat();
    out.colorAttachmentCount = 1;
    out.depthFormat = device.depthFormat();
    return out;
}

void VulkanRenderingSession::SwapchainWithDepthTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t imageIndex)
{
    s.beginSwapchainRenderingInternal(cmd, imageIndex);
}

VulkanRenderingSession::RenderTargetInfo
VulkanRenderingSession::DeferredGBufferTarget::info(const VulkanDevice&, const VulkanRenderTargets& targets) const
{
    RenderTargetInfo out{};
    out.colorFormat = targets.gbufferFormat();
    out.colorAttachmentCount = VulkanRenderTargets::kGBufferCount;
    out.depthFormat = targets.depthFormat();
    return out;
}

void VulkanRenderingSession::DeferredGBufferTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t)
{
    s.beginGBufferRenderingInternal(cmd);
}

VulkanRenderingSession::RenderTargetInfo
VulkanRenderingSession::SwapchainNoDepthTarget::info(const VulkanDevice& device, const VulkanRenderTargets&) const
{
    RenderTargetInfo out{};
    out.colorFormat = device.swapchainFormat();
    out.colorAttachmentCount = 1;
    out.depthFormat = vk::Format::eUndefined;
    return out;
}

void VulkanRenderingSession::SwapchainNoDepthTarget::begin(VulkanRenderingSession& s, vk::CommandBuffer cmd, uint32_t imageIndex)
{
    s.beginSwapchainRenderingNoDepthInternal(cmd, imageIndex);
}

// ---- applyDynamicState: attachment-count comes from *active pass info*, not "pending mode" ----
//
// BLEND STATE ARCHITECTURE:
// - Pipeline static blend: mode-dependent (VK_FALSE for ALPHA_BLEND_NONE, VK_TRUE + factors for others)
// - Dynamic state baseline: VK_FALSE (blending disabled by default)
// - Per-material draw: paths that need blending call setColorBlendEnableEXT(VK_TRUE)
//
// This establishes "blending off unless requested". Draw paths (gr_vulkan_render_primitives,
// gr_vulkan_render_model, etc.) must explicitly enable blending when material blend mode
// != ALPHA_BLEND_NONE, otherwise EDS3 devices will have blending disabled even if the
// pipeline was created with blendEnable=VK_TRUE.

void VulkanRenderingSession::applyDynamicState(vk::CommandBuffer cmd)
{
    const uint32_t attachmentCount = m_activeInfo.colorAttachmentCount;

    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);

    if (m_device.supportsExtendedDynamicState3()) {
        // Baseline: blending OFF. Draw paths must enable per-material.
        cmd.setColorBlendEnableEXT(0, attachmentCount, std::vector<VkBool32>(attachmentCount, VK_FALSE).data());
        cmd.setColorBlendEquationEXT(
            0, attachmentCount,
            std::vector<vk::ColorBlendEquationEXT>(attachmentCount, vk::ColorBlendEquationEXT{
                vk::BlendFactor::eSrcAlpha,
                vk::BlendFactor::eOneMinusSrcAlpha,
                vk::BlendOp::eAdd,
                vk::BlendFactor::eOne,
                vk::BlendFactor::eOneMinusSrcAlpha,
                vk::BlendOp::eAdd,
            }).data()
        );
        cmd.setColorWriteMaskEXT(0, attachmentCount, std::vector<vk::ColorComponentFlags>(
            attachmentCount,
            vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        ).data());
    }

    cmd.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        m_descriptorLayouts.pipelineLayout(),
        1, 1,
        &m_globalDescriptorSet,
        0, nullptr
    );
}

} // namespace graphics::vulkan
```

---

### 2) VulkanFrame - Descriptor Set Required, Uniforms Optional + Fallback

#### VulkanFrame.h

```cpp
#pragma once

#include "VulkanRingBuffer.h"
#include "graphics/2d.h"

#include <vulkan/vulkan.hpp>
#include <optional>

namespace graphics::vulkan {

struct DynamicUniformBinding {
    gr_buffer_handle bufferHandle{};
    uint32_t dynamicOffset = 0;
};

class VulkanFrame {
public:
    VulkanFrame(VulkanDevice& device,
                size_t uniformRingSize,
                size_t vertexRingSize,
                size_t stagingRingSize,
                vk::DescriptorSet modelSet);

    vk::CommandBuffer commandBuffer() const { return m_cmd; }

    VulkanRingBuffer& uniformBuffer() { return m_uniformRing; }
    VulkanRingBuffer& vertexBuffer()  { return m_vertexRing; }
    VulkanRingBuffer& stagingBuffer() { return m_stagingRing; }

    // Always valid by construction
    vk::DescriptorSet modelDescriptorSet() const { return m_modelDescriptorSet; }

    // Absence => fallback uniform bound at frame start, offset 0
    std::optional<DynamicUniformBinding> modelUniformBinding;
    std::optional<DynamicUniformBinding> sceneUniformBinding;

    void resetPerFrameBindings()
    {
        modelUniformBinding.reset();
        sceneUniformBinding.reset();
    }

private:
    vk::CommandBuffer m_cmd{};
    VulkanRingBuffer m_uniformRing;
    VulkanRingBuffer m_vertexRing;
    VulkanRingBuffer m_stagingRing;

    vk::DescriptorSet m_modelDescriptorSet{};
};

} // namespace graphics::vulkan
```

#### VulkanFrame.cpp

```cpp
#include "VulkanFrame.h"

namespace graphics::vulkan {

VulkanFrame::VulkanFrame(VulkanDevice& device,
                         size_t uniformRingSize,
                         size_t vertexRingSize,
                         size_t stagingRingSize,
                         vk::DescriptorSet modelSet)
    : m_uniformRing(device, uniformRingSize, vk::BufferUsageFlagBits::eUniformBuffer),
      m_vertexRing(device, vertexRingSize,  vk::BufferUsageFlagBits::eVertexBuffer),
      m_stagingRing(device, stagingRingSize, vk::BufferUsageFlagBits::eTransferSrc),
      m_modelDescriptorSet(modelSet)
{
    Assertion(m_modelDescriptorSet, "VulkanFrame requires a valid model descriptor set");
}

} // namespace graphics::vulkan
```

---

### 3) VulkanBufferManager - ensureBuffer Makes Buffer Existence Inevitable

#### VulkanBufferManager.h (add ensureBuffer)

```cpp
// Add to existing class:

// If you need to use the VkBuffer, you get a VkBuffer that exists.
vk::Buffer ensureBuffer(gr_buffer_handle handle, vk::DeviceSize minSize);
```

#### VulkanBufferManager.cpp

```cpp
vk::Buffer VulkanBufferManager::ensureBuffer(gr_buffer_handle handle, vk::DeviceSize minSize)
{
    Assertion(handle.isValid() && static_cast<size_t>(handle.value()) < m_buffers.size(),
              "Invalid buffer handle %d in ensureBuffer", handle.value());
    Assertion(minSize > 0, "ensureBuffer requires minSize > 0");

    auto& buffer = m_buffers[handle.value()];

    if (!buffer.buffer || buffer.size < minSize) {
        resizeBuffer(handle, static_cast<size_t>(minSize));
    }

    return buffer.buffer.get();
}

void VulkanBufferManager::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
    Assertion(size > 0, "Buffer size must be > 0 in updateBufferData");
    ensureBuffer(handle, static_cast<vk::DeviceSize>(size));

    auto& buffer = m_buffers[handle.value()];
    std::memcpy(buffer.mapped, data, size);
}

void VulkanBufferManager::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data)
{
    Assertion(size > 0, "Buffer size must be > 0 in updateBufferDataOffset");
    const vk::DeviceSize required = static_cast<vk::DeviceSize>(offset + size);
    ensureBuffer(handle, required);

    auto& buffer = m_buffers[handle.value()];
    std::memcpy(static_cast<char*>(buffer.mapped) + offset, data, size);
}
```

---

### 4) VulkanRenderer - Frames Own Descriptor Sets; Descriptor Sync Unconditional; Fallback Uniform

#### A) Remove "factory might fail" routing

```cpp
gr_buffer_handle VulkanRenderer::createBuffer(BufferType type, BufferUsageHint usage)
{
    Assertion(m_bufferManager != nullptr, "createBuffer called before buffer manager initialization");
    return m_bufferManager->createBuffer(type, usage);
}

void VulkanRenderer::updateBufferData(gr_buffer_handle handle, size_t size, const void* data)
{
    Assertion(m_bufferManager != nullptr, "updateBufferData called before buffer manager initialization");
    m_bufferManager->updateBufferData(handle, size, data);
}
```

#### B) Allocate model descriptor sets at frame construction

```cpp
void VulkanRenderer::createFrames()
{
    Assertion(m_descriptorLayouts != nullptr, "DescriptorLayouts must exist before frames are created");

    m_frames.clear();
    m_frames.reserve(kFramesInFlight);

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();
        Assertion(modelSet, "Failed to allocate model descriptor set for frame %u", i);

        m_frames.emplace_back(VulkanFrame(
            *m_vulkanDevice,
            m_uniformRingSize,
            m_vertexRingSize,
            m_stagingRingSize,
            modelSet
        ));
    }
}
```

#### C) Create fallback resources once

```cpp
// VulkanRenderer.h (add members)
gr_buffer_handle m_fallbackModelUniformHandle = gr_buffer_handle::invalid();
gr_buffer_handle m_fallbackModelVertexHeapHandle = gr_buffer_handle::invalid();
```

```cpp
// VulkanRenderer.cpp
#include "graphics/util/uniform_structs.h"

void VulkanRenderer::createFallbackResources()
{
    Assertion(m_bufferManager != nullptr, "Fallback resources require buffer manager");

    m_fallbackModelUniformHandle = m_bufferManager->createBuffer(BufferType::Uniform, BufferUsageHint::Dynamic);
    model_uniform_data zeroModel{};
    m_bufferManager->updateBufferData(m_fallbackModelUniformHandle, sizeof(zeroModel), &zeroModel);

    m_fallbackModelVertexHeapHandle = m_bufferManager->createBuffer(BufferType::Vertex, BufferUsageHint::Dynamic);
    uint32_t dummy = 0;
    m_bufferManager->updateBufferData(m_fallbackModelVertexHeapHandle, sizeof(dummy), &dummy);

    m_modelVertexHeapHandle = m_fallbackModelVertexHeapHandle;
}
```

#### D) beginFrame: descriptor sync unconditional

```cpp
void VulkanRenderer::beginFrame(VulkanFrame& frame, uint32_t imageIndex)
{
    // ... existing beginFrame logic ...

    frame.resetPerFrameBindings();

    Assertion(m_bufferManager != nullptr, "beginFrame requires buffer manager");
    Assertion(m_modelVertexHeapHandle.isValid(), "Model vertex heap handle must be valid");

    vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);

    beginModelDescriptorSync(frame, m_currentFrame, vertexHeapBuffer);

    m_renderingSession->beginFrame(frame.commandBuffer(), imageIndex, m_globalDescriptorSet);
}
```

#### E) beginModelDescriptorSync: no lazy allocation

```cpp
#include "graphics/util/uniform_structs.h"

void VulkanRenderer::beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer)
{
    Assertion(frameIndex < kFramesInFlight, "Invalid frameIndex in beginModelDescriptorSync");

    vk::DescriptorSet set = frame.modelDescriptorSet();
    Assertion(set, "Frame must own a valid model descriptor set");

    vk::DescriptorBufferInfo vertexInfo{};
    vertexInfo.buffer = vertexHeapBuffer;
    vertexInfo.offset = 0;
    vertexInfo.range  = VK_WHOLE_SIZE;

    vk::WriteDescriptorSet writeVertex{};
    writeVertex.dstSet = set;
    writeVertex.dstBinding = 0;
    writeVertex.descriptorType = vk::DescriptorType::eStorageBuffer;
    writeVertex.descriptorCount = 1;
    writeVertex.pBufferInfo = &vertexInfo;

    vk::Buffer fallbackUbo = m_bufferManager->ensureBuffer(m_fallbackModelUniformHandle, sizeof(model_uniform_data));
    vk::DescriptorBufferInfo modelUboInfo{};
    modelUboInfo.buffer = fallbackUbo;
    modelUboInfo.offset = 0;
    modelUboInfo.range  = sizeof(model_uniform_data);

    vk::WriteDescriptorSet writeUbo{};
    writeUbo.dstSet = set;
    writeUbo.dstBinding = 2;
    writeUbo.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    writeUbo.descriptorCount = 1;
    writeUbo.pBufferInfo = &modelUboInfo;

    std::array<vk::WriteDescriptorSet, 2> writes = { writeVertex, writeUbo };
    m_vulkanDevice->device().updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}
```

#### F) setModelUniformBinding: always valid

```cpp
#include "graphics/util/uniform_structs.h"

void VulkanRenderer::setModelUniformBinding(VulkanFrame& frame, gr_buffer_handle handle, size_t offset, size_t size)
{
    const auto alignment = getMinUniformOffsetAlignment();
    Assertion(offset <= std::numeric_limits<uint32_t>::max(), "Model uniform offset out of range");
    const uint32_t dynOffset = static_cast<uint32_t>(offset);

    Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
    Assertion((dynOffset % alignment) == 0, "Model uniform offset not aligned");
    Assertion(size >= sizeof(model_uniform_data), "Model uniform size smaller than model_uniform_data");
    Assertion(handle.isValid(), "Invalid model uniform buffer handle");

    Assertion(m_bufferManager != nullptr, "setModelUniformBinding requires buffer manager");
    const vk::DeviceSize required = static_cast<vk::DeviceSize>(offset + sizeof(model_uniform_data));
    vk::Buffer vkBuffer = m_bufferManager->ensureBuffer(handle, required);

    vk::DescriptorBufferInfo info{};
    info.buffer = vkBuffer;
    info.offset = 0;
    info.range  = sizeof(model_uniform_data);

    vk::WriteDescriptorSet write{};
    write.dstSet = frame.modelDescriptorSet();
    write.dstBinding = 2;
    write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
    write.descriptorCount = 1;
    write.pBufferInfo = &info;

    m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);

    frame.modelUniformBinding = DynamicUniformBinding{ handle, dynOffset };
}
```

---

### 5) VulkanRenderer.ensureRenderingStarted Returns Target Contract

```cpp
// VulkanRenderer.h
const VulkanRenderingSession::RenderTargetInfo& ensureRenderingStarted(vk::CommandBuffer cmd);
```

```cpp
// VulkanRenderer.cpp
const VulkanRenderingSession::RenderTargetInfo& VulkanRenderer::ensureRenderingStarted(vk::CommandBuffer cmd)
{
    Assertion(m_renderingSession != nullptr, "Rendering session not initialized");
    return m_renderingSession->ensureRenderingActive(cmd, m_recordingImage);
}
```

---

## Code: Fix Pipeline Creation to Use Active Target Info

### VulkanGraphics.cpp - gr_vulkan_render_model

```cpp
VulkanFrame& frame = *g_currentFrame;
vk::CommandBuffer cmd = frame.commandBuffer();

// Start rendering first and get the *actual* target contract
const auto& rt = renderer_instance->ensureRenderingStarted(cmd);

// Build pipeline key from the active render target contract (no mismatch possible)
PipelineKey key{};
key.type                   = SDR_TYPE_MODEL;
key.variant_flags          = material_info->get_shader_flags();
key.color_format           = static_cast<VkFormat>(rt.colorFormat);
key.depth_format           = static_cast<VkFormat>(rt.depthFormat);
key.sample_count           = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
key.color_attachment_count = rt.colorAttachmentCount;
key.blend_mode             = material_info->get_blend_mode();

vk::Pipeline pipeline = renderer_instance->getPipeline(key, modules, emptyLayout);
vk::PipelineLayout layout = renderer_instance->getModelPipelineLayout();

vk::DescriptorSet modelSet = frame.modelDescriptorSet();

// Dynamic offset: absence => fallback bound at frame start, offset 0
uint32_t dynOffset = frame.modelUniformBinding ? frame.modelUniformBinding->dynamicOffset : 0;

ModelDrawContext ctx{
    frame, cmd, pipeline, layout, modelSet, dynOffset, pcs, *vert_source, *bufferp, texi
};

issueModelDraw(ctx);
```

### VulkanGraphics.cpp - gr_vulkan_render_primitives_batched

```cpp
const auto& rt = renderer_instance->ensureRenderingStarted(cmd);

PipelineKey pipelineKey{};
pipelineKey.type = shaderType;
pipelineKey.variant_flags = material_info->get_shader_flags();
pipelineKey.color_format = static_cast<VkFormat>(rt.colorFormat);
pipelineKey.depth_format = static_cast<VkFormat>(rt.depthFormat);
pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
pipelineKey.color_attachment_count = rt.colorAttachmentCount;
pipelineKey.blend_mode = material_info->get_blend_mode();
pipelineKey.layout_hash = layout->hash();
```

---

## What This Redesign Guarantees (Without Guards)

- There is no constructible state where `pending != active`, because there is no pair of modes.
- There is no "render pass active bool" to clean up repeatedly; "active pass" is the presence of `ActivePass`.
- There is no "unset" uniform offset sentinel. Absence is represented by `std::optional` and rendered safely via fallback.
- Descriptor sync is not conditional on a maybe-existing VkBuffer anymore. If the renderer needs it, it exists because `ensureBuffer()` makes it exist.

---

## Critical Implementation Detail: EDS3 Blend State

When Extended Dynamic State 3 is supported, pipelines include `VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT`. The session baseline sets blend OFF via `setColorBlendEnableEXT(..., VK_FALSE)`.

**Consequence:** Any draw path that relies on pipeline static blending (`blendEnable=VK_TRUE` in `PipelineColorBlendAttachmentState`) but does NOT call `setColorBlendEnableEXT(VK_TRUE)` will render with blending disabled on EDS3 devices.

**Required pattern for draw paths:**
```cpp
if (renderer_instance->supportsExtendedDynamicState3()) {
    vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE;
    cmd.setColorBlendEnableEXT(0, attachmentCount, &blendEnable);
}
```

This applies to: `gr_vulkan_render_model`, `gr_vulkan_render_primitives`, `gr_vulkan_render_primitives_batched`, and any future draw paths.

---

## Implementation Order

1. **VulkanBufferManager::ensureBuffer()** - Foundation, no dependencies
2. **VulkanFrame refactor** - Add optional binding, constructor change
3. **VulkanRenderer fallback resources** - Depends on ensureBuffer
4. **VulkanRenderingSession refactor** - Largest change, depends on nothing
5. **VulkanRenderer descriptor sync** - Depends on steps 1-4
6. **VulkanGraphics pipeline key** - Final integration
