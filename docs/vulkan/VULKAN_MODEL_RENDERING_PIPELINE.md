# Vulkan Model Rendering End-to-End Pipeline

This document describes the complete model rendering pipeline in the Vulkan renderer, from model data registration through vertex heap allocation, descriptor synchronization, and draw calls.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Complete Flow Diagram](#2-complete-flow-diagram)
3. [Model Data Registration](#3-model-data-registration)
4. [Vertex Heap System](#4-vertex-heap-system)
5. [Uniform Buffer Management](#5-uniform-buffer-management)
6. [Descriptor Synchronization](#6-descriptor-synchronization)
7. [Draw Call Execution](#7-draw-call-execution)
8. [Push Constants](#8-push-constants)
9. [Common Patterns](#9-common-patterns)
10. [Common Issues](#10-common-issues)

---

## 1. Overview

Model rendering in the Vulkan renderer uses a **vertex pulling** architecture with bindless textures:

- **Vertex Pulling**: Shader fetches vertex data from SSBO (no vertex attributes)
- **Bindless Textures**: Textures accessed via array index (no per-draw descriptor updates)
- **Dynamic Uniform Buffers**: Per-model data with offset-based indexing
- **Vertex Heap**: Large SSBO containing all model vertex data

**Key Files**:
- `code/graphics/vulkan/VulkanRenderer.cpp` - Model rendering orchestration
- `code/graphics/vulkan/VulkanModelTypes.h` - Push constants and types
- `code/graphics/vulkan/VulkanGraphics.cpp` - Engine integration
- `code/graphics/shaders/model.vert` / `model.frag` - Model shaders

**Pipeline**: `SDR_TYPE_MODEL` with Model pipeline layout

---

## 2. Complete Flow Diagram

### High-Level Sequence

```
[Model Registration]
    │
    ├── Register model with engine (bmpman, model system)
    │
    ├── [Frame Start]
    │   ├── setModelVertexHeapHandle(handle)  ← Called when heap created
    │   ├── beginModelDescriptorSync(frame, frameIndex, vertexHeapBuffer)
    │   │   ├── Update vertex heap SSBO descriptor (binding 0)
    │   │   ├── Update bindless texture array (binding 1)
    │   │   ├── Update transform buffer SSBO (binding 3)
    │   │   └── Update model uniform buffer descriptor (binding 2)
    │   └── Descriptors ready for draw
    │
    ├── [Per-Model Setup]
    │   ├── Upload vertex data to vertex heap
    │   │   └── Track offset in heap
    │   ├── Upload uniform data to uniform buffer
    │   │   └── Track offset (aligned to minUniformBufferOffsetAlignment)
    │   ├── Get bindless texture slots
    │   │   ├── baseMapIndex = getBindlessTextureIndex(baseHandle)
    │   │   ├── normalMapIndex = getBindlessTextureIndex(normalHandle)
    │   │   └── specMapIndex = getBindlessTextureIndex(specHandle)
    │   └── Build ModelPushConstants
    │
    ├── [Draw Call]
    │   ├── Bind model pipeline
    │   ├── Bind model descriptor set (with dynamic offsets)
    │   ├── Push ModelPushConstants
    │   ├── Bind index buffer
    │   └── DrawIndexed (vertex data from SSBO via push constants)
    │
    └── [Frame End]
        └── Vertex heap and uniform buffer reset for next frame
```

### Detailed Data Flow

```
Engine Model Data
    │
    ├── Vertex Data → Vertex Heap SSBO (per-frame ring buffer)
    │   └── Offset tracked per model
    │
    ├── Uniform Data → Model Uniform Buffer (dynamic, per-frame)
    │   └── Offset aligned to device alignment (256 bytes typical)
    │
    ├── Texture Handles → Bindless Slot Assignment
    │   ├── getBindlessTextureIndex(handle)
    │   └── Slot index stored in push constants
    │
    └── Transform Data → Transform Buffer SSBO (batched, per-frame)
        └── Dynamic offset per draw
```

---

## 3. Model Data Registration

### 3.1 Vertex Heap Registration

**Function**: `VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle)`

**Called At**: When `GPUMemoryHeap` creates the ModelVertex heap

**Location**: `VulkanGraphics.cpp:2090`

**Process** (`VulkanRenderer.cpp:2680-2685`):
```cpp
void VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle) {
    // Only store the handle - VkBuffer will be looked up lazily when needed.
    // At registration time, the buffer doesn't exist yet (VulkanBufferManager::createBuffer
    // defers actual VkBuffer creation until updateBufferData is called).
    m_modelVertexHeapHandle = handle;
}
```

**Key Point**: Handle is registered, but `VkBuffer` is created lazily on first use. No validation is performed at registration time since the buffer does not exist yet.

### 3.2 Buffer Lazy Creation

**Function**: `VulkanBufferManager::ensureBuffer(gr_buffer_handle handle, vk::DeviceSize size)`

**Called At**: Before descriptor sync, ensures buffer exists

**Process** (`VulkanRenderer.cpp:332-334`):
```cpp
// Ensure vertex heap buffer exists and sync descriptors
vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);
```

**Lazy Semantics**: Buffer created on first `ensureBuffer()` call if it doesn't exist.

---

## 4. Vertex Heap System

### 4.1 Vertex Heap Structure

**Type**: Storage Buffer (`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`)

**Binding**: Set 0, Binding 0 (Model pipeline layout)

**Usage**: Vertex pulling - shader reads vertex data directly from SSBO

**Memory**: Per-frame ring buffer or persistent managed buffer (depends on usage hint)

### 4.2 Vertex Data Upload

**Pattern**: Engine uploads vertex data to heap, tracks offset

**Example** (`VulkanGraphics.cpp:949-956`):
```cpp
// Upload vertex data
gr_update_buffer_data(vertexHeapHandle, vertexData, vertexSize);

// Track offset for this model
uint32_t vertexOffset = /* offset in heap */;
Assertion(vertexOffset <= UINT32_MAX, "Vertex offset exceeds uint32 range");
```

**Vertex Layout**: Variable format per model (specified in push constants)

### 4.3 Vertex Pulling

**Shader**: `model.vert` uses SSBO for vertex data

**Push Constants** (`VulkanModelTypes.h:21-50`):
```cpp
struct ModelPushConstants {
    uint32_t vertexOffset;      // Byte offset into vertex heap
    uint32_t stride;            // Byte stride between vertices
    uint32_t vertexAttribMask;  // Which attributes are present
    uint32_t posOffset;         // Offset of position within vertex
    uint32_t normalOffset;      // Offset of normal within vertex
    // ... more offsets ...
};
```

**Shader Logic** (`model.vert`):
```glsl
layout(set = 0, binding = 0) readonly buffer VertexBuffer {
    float data[];
} vertexBuffer;

// Helper to calculate word index from byte offsets
uint wordIndex(uint byteBase, uint byteOffset) {
    return (byteBase + byteOffset) >> 2; // divide by 4
}

vec3 loadVec3(uint byteBase, uint offset) {
    uint idx = wordIndex(byteBase, offset);
    return vec3(vertexBuffer.data[idx], vertexBuffer.data[idx + 1], vertexBuffer.data[idx + 2]);
}

void main() {
    uint byteBase = pcs.vertexOffset + uint(gl_VertexIndex) * pcs.stride;

    vec3 position = hasAttrib(MODEL_ATTRIB_POS) ? loadVec3(byteBase, pcs.posOffset) : vec3(0.0);
    vec3 normal = hasAttrib(MODEL_ATTRIB_NORMAL) ? loadVec3(byteBase, pcs.normalOffset) : vec3(0.0, 0.0, 1.0);
    // ... read other attributes based on vertexAttribMask ...
}
```

**Note**: The vertex data is stored as floats (not raw bytes), so byte offsets are converted to float indices by dividing by 4.

---

## 5. Uniform Buffer Management

### 5.1 Model Uniform Buffer

**Type**: Dynamic Uniform Buffer (`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`)

**Binding**: Set 0, Binding 2 (Model pipeline layout)

**Structure**: `model_uniform_data` from `uniform_structs.h`

**Size**: Variable, aligned to `minUniformBufferOffsetAlignment` (typically 256 bytes)

### 5.2 Uniform Data Upload

**Function**: `VulkanRenderer::setModelUniformBinding(VulkanFrame& frame, gr_buffer_handle handle, size_t offset, size_t size)`

**Process** (`VulkanRenderer.cpp:2907-2950`):

1. **Validate Alignment**:
   ```cpp
   const auto alignment = getMinUniformOffsetAlignment();
   Assertion((offset % alignment) == 0, "Offset not aligned");
   ```

2. **Update Descriptor** (if buffer handle changed):
   ```cpp
   if (frame.modelUniformBinding.bufferHandle != handle) {
       vk::WriteDescriptorSet write{};
       write.dstSet = frame.modelDescriptorSet();
       write.dstBinding = 2;
       write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
       write.pBufferInfo = &bufferInfo;
       device.updateDescriptorSets(1, &write, 0, nullptr);
   }
   ```

3. **Store Binding State**:
   ```cpp
   frame.modelUniformBinding = DynamicUniformBinding{ handle, dynOffset };
   ```

**Key Point**: Descriptor updated only when buffer handle changes. Dynamic offset applied at bind time.

### 5.3 Uniform Data Structure

**Struct**: `model_uniform_data` (`uniform_structs.h:79-145`)

**Contents**:
- Matrices: `modelViewMatrix`, `modelMatrix`, `viewMatrix`, `projMatrix`, `textureMatrix`
- Shadow matrices: `shadow_mv_matrix`, `shadow_proj_matrix[4]`
- Material: `color`, lights array, factors (ambient, diffuse, emission)
- Textures: Bindless slot indices (`sBasemapIndex`, `sNormalmapIndex`, etc.)
- Flags: Shader variant flags, effect numbers

**Alignment**: Must be 16-byte aligned (std140) AND device-aligned (typically 256 bytes)

---

## 6. Descriptor Synchronization

### 6.1 Frame Start Sync

**Function**: `VulkanRenderer::beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex, vk::Buffer vertexHeapBuffer)`

**Called At**: Frame start, after texture uploads (`VulkanRenderer.cpp:329-334`)

**Process** (`VulkanRenderer.cpp:3095-3121`):

1. **Collect Resident Textures**:
   ```cpp
   std::vector<std::pair<uint32_t, int>> textures;
   m_textureManager->appendResidentBindlessDescriptors(textures);
   // Returns (slotIndex, baseFrameHandle) pairs
   ```

2. **Update Descriptors**:
   ```cpp
   updateModelDescriptors(
       frameIndex,
       frame.modelDescriptorSet(),
       vertexHeapBuffer,
       frame.vertexBuffer().buffer(),  // Transform buffer
       textures);
   ```

### 6.2 Descriptor Update Details

**Function**: `VulkanRenderer::updateModelDescriptors(...)`

**Process** (`VulkanRenderer.cpp:2973-3093`):

1. **Binding 0: Vertex Heap SSBO** (always updated):
   ```cpp
   vk::WriteDescriptorSet heapWrite{};
   heapWrite.dstBinding = 0;
   heapWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
   heapWrite.pBufferInfo = &heapInfo;  // vertexHeapBuffer
   ```

2. **Binding 1: Bindless Texture Array** (optimized with change detection):
   ```cpp
   // First frame: full array write
   if (!cache.initialized) {
       texturesWrite.dstArrayElement = 0;
       texturesWrite.descriptorCount = kMaxBindlessTextures;
       // Write all 1024 slots
   }
   // Subsequent frames: only changed ranges
   else {
       // Detect contiguous dirty ranges
       // Write only changed slots
   }
   ```

3. **Binding 2: Model Uniform Buffer** (updated per-draw via `setModelUniformBinding()`)

4. **Binding 3: Transform Buffer SSBO** (always updated):
   ```cpp
   vk::WriteDescriptorSet transformWrite{};
   transformWrite.dstBinding = 3;
   transformWrite.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
   transformWrite.pBufferInfo = &transformInfo;  // Batched transform data
   ```

### 6.3 Bindless Cache

**Structure** (`VulkanRenderer.h:335-339`):
```cpp
struct ModelBindlessDescriptorCache {
    bool initialized = false;
    std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> infos{};
};
std::array<ModelBindlessDescriptorCache, kFramesInFlight> m_modelBindlessCache;
```

**Optimization**: Tracks previous frame's descriptor contents to minimize updates.

**Change Detection**: Compares `DescriptorImageInfo` to detect which slots changed.

---

## 7. Draw Call Execution

### 7.1 Draw Setup

**Pipeline**: Model pipeline (`SDR_TYPE_MODEL`)

**Pipeline Key**:
```cpp
PipelineKey key{};
key.type = shader_type::SDR_TYPE_MODEL;
key.color_format = /* render target format */;
key.depth_format = /* render target depth format */;
key.color_attachment_count = /* 1 for swapchain, 5 for G-buffer */;
key.blend_mode = /* blend mode */;
// layout_hash ignored (vertex pulling)
```

**Pipeline Binding**:
```cpp
vk::Pipeline pipeline = pipelineManager->getPipeline(key, modules, emptyLayout);
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
```

### 7.2 Descriptor Set Binding

**Binding Pattern** (`VulkanGraphics.cpp:837-854` in `issueModelDraw`):
```cpp
// Set model descriptor set + dynamic UBO offset
const auto modelSet = ctx.bound.modelSet;
const auto modelDynamicOffset = ctx.bound.modelUbo.dynamicOffset;
const auto transformDynamicOffset = ctx.bound.transformDynamicOffset;
std::array<uint32_t, 2> dynamicOffsets = { modelDynamicOffset, transformDynamicOffset };

cmd.bindDescriptorSets(
    vk::PipelineBindPoint::eGraphics,
    ctx.pipelineLayout,
    0,  // firstSet
    1,  // setCount
    &modelSet,
    static_cast<uint32_t>(dynamicOffsets.size()),
    dynamicOffsets.data());
```

**Dynamic Offsets**:
- Offset 0: Model uniform buffer offset (per-model data)
- Offset 1: Transform buffer offset (batched transforms, if used)

### 7.3 Push Constants

**Structure**: `ModelPushConstants` (64 bytes)

**Push Pattern**:
```cpp
ModelPushConstants pushConstants{};
pushConstants.vertexOffset = /* offset in vertex heap */;
pushConstants.stride = /* vertex stride */;
pushConstants.vertexAttribMask = /* MODEL_ATTRIB_* bits */;
pushConstants.posOffset = /* position offset within vertex */;
// ... set other offsets ...
pushConstants.baseMapIndex = /* bindless slot */;
pushConstants.normalMapIndex = /* bindless slot */;
pushConstants.specMapIndex = /* bindless slot */;
pushConstants.flags = /* shader variant flags */;

cmd.pushConstants(
    modelPipelineLayout,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
    0,
    sizeof(ModelPushConstants),
    &pushConstants);
```

### 7.4 Draw Call

**Pattern**: No vertex buffer bound (vertex pulling), but index buffer IS bound.

**Actual draw call** (`VulkanGraphics.cpp:867-890` in `issueModelDraw`):
```cpp
// Get and bind index buffer
vk::Buffer indexBuffer = ctx.bound.ctx.renderer.getBuffer(ctx.vertSource.Ibuffer_handle);

// Select index type based on VB_FLAG_LARGE_INDEX
const bool use32BitIndices = (batch.flags & VB_FLAG_LARGE_INDEX) != 0;
const vk::IndexType indexType = use32BitIndices ? vk::IndexType::eUint32
                                                : vk::IndexType::eUint16;

// Index data offset in buffer
const vk::DeviceSize indexOffsetBytes =
    static_cast<vk::DeviceSize>(ctx.vertSource.Index_offset + batch.index_offset);

cmd.bindIndexBuffer(indexBuffer, indexOffsetBytes, indexType);

const uint32_t indexCount = static_cast<uint32_t>(batch.n_verts);

cmd.drawIndexed(
    indexCount,
    1,  // instanceCount
    0,  // firstIndex (byte offset already baked above)
    0,  // vertexOffset (vertex pulling handles the base)
    0   // firstInstance
);
```

**Key Point**: Vertex data comes from SSBO via push constant offset, not from vertex buffers. Index buffers are still used normally.

---

## 8. Push Constants

### 8.1 Structure

**Definition**: `VulkanModelTypes.h:21-50`

**Size**: 64 bytes (matches GLSL `layout(push_constant)` block)

**Fields**:

| Field | Type | Purpose |
|-------|------|---------|
| `vertexOffset` | `uint32_t` | Byte offset into vertex heap |
| `stride` | `uint32_t` | Byte stride between vertices |
| `vertexAttribMask` | `uint32_t` | Bitmask of present attributes |
| `posOffset` | `uint32_t` | Position offset within vertex |
| `normalOffset` | `uint32_t` | Normal offset within vertex |
| `texCoordOffset` | `uint32_t` | Texture coordinate offset |
| `tangentOffset` | `uint32_t` | Tangent offset |
| `modelIdOffset` | `uint32_t` | Model ID offset (batching) |
| `boneIndicesOffset` | `uint32_t` | Bone indices offset |
| `boneWeightsOffset` | `uint32_t` | Bone weights offset |
| `baseMapIndex` | `uint32_t` | Bindless texture slot (base map) |
| `glowMapIndex` | `uint32_t` | Bindless texture slot (glow map) |
| `normalMapIndex` | `uint32_t` | Bindless texture slot (normal map) |
| `specMapIndex` | `uint32_t` | Bindless texture slot (specular map) |
| `matrixIndex` | `uint32_t` | Reserved for instancing |
| `flags` | `uint32_t` | Shader variant flags |

### 8.2 Vertex Attribute Mask

**Bits** (`VulkanModelTypes.h:10-16`):
- `MODEL_ATTRIB_POS` (bit 0): Position present
- `MODEL_ATTRIB_NORMAL` (bit 1): Normal present
- `MODEL_ATTRIB_TEXCOORD` (bit 2): Texture coordinate present
- `MODEL_ATTRIB_TANGENT` (bit 3): Tangent present
- `MODEL_ATTRIB_BONEINDICES` (bit 4): Bone indices present
- `MODEL_ATTRIB_BONEWEIGHTS` (bit 5): Bone weights present
- `MODEL_ATTRIB_MODEL_ID` (bit 6): Model ID present

**Usage**: Shader checks bits to determine which attributes to read from vertex heap.

---

## 9. Common Patterns

### 9.1 Pattern: Single Model Draw

```cpp
void drawModel(const FrameCtx& frameCtx, const ModelData& model) {
    // 1. Get model-bound frame (validates bindings)
    auto modelBound = requireModelBound(frameCtx);
    
    // 2. Get render context
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    
    // 3. Build push constants
    ModelPushConstants push{};
    push.vertexOffset = model.vertexHeapOffset;
    push.stride = model.vertexStride;
    push.vertexAttribMask = model.attribMask;
    push.posOffset = model.posOffset;
    // ... set other offsets ...
    push.baseMapIndex = frameCtx.renderer.getBindlessTextureIndex(frameCtx, model.baseTex);
    push.normalMapIndex = frameCtx.renderer.getBindlessTextureIndex(frameCtx, model.normalTex);
    push.flags = model.shaderFlags;
    
    // 4. Bind pipeline
    PipelineKey key = buildModelPipelineKey(renderCtx.targetInfo);
    vk::Pipeline pipeline = getPipeline(key);
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    
    // 5. Bind descriptor set with dynamic offsets
    uint32_t offsets[2] = { modelBound.modelUbo.dynamicOffset, modelBound.transformDynamicOffset };
    renderCtx.cmd.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        modelPipelineLayout,
        0, 1, &modelBound.modelSet,
        2, offsets);
    
    // 6. Push constants
    renderCtx.cmd.pushConstants(
        modelPipelineLayout,
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        0, sizeof(push), &push);
    
    // 7. Draw
    renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
}
```

### 9.2 Pattern: Batched Model Draws

```cpp
void drawBatchedModels(const FrameCtx& frameCtx, const std::vector<ModelData>& models) {
    auto modelBound = requireModelBound(frameCtx);
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    
    // Bind pipeline once (all models use same pipeline)
    PipelineKey key = buildModelPipelineKey(renderCtx.targetInfo);
    vk::Pipeline pipeline = getPipeline(key);
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    
    // Draw each model (rebind descriptors and push constants per model)
    for (const auto& model : models) {
        // Set uniform binding (updates descriptor if buffer changed)
        frameCtx.renderer.setModelUniformBinding(
            frameCtx.frame(),
            model.uniformBufferHandle,
            model.uniformOffset,
            model.uniformSize);
        
        // Bind descriptors with dynamic offsets
        uint32_t offsets[2] = { model.uniformOffset, model.transformOffset };
        renderCtx.cmd.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            modelPipelineLayout,
            0, 1, &modelBound.modelSet,
            2, offsets);
        
        // Push constants
        ModelPushConstants push = buildPushConstants(model);
        renderCtx.cmd.pushConstants(
            modelPipelineLayout,
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            0, sizeof(push), &push);
        
        // Draw
        renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
    }
}
```

### 9.3 Pattern: G-Buffer Model Rendering

```cpp
void drawModelToGBuffer(const FrameCtx& frameCtx, const ModelData& model) {
    // G-buffer target already set by deferred lighting begin
    auto modelBound = requireModelBound(frameCtx);
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);
    
    // Pipeline key must match G-buffer format
    PipelineKey key{};
    key.type = shader_type::SDR_TYPE_MODEL;
    key.color_format = VK_FORMAT_R16G16B16A16_SFLOAT;  // G-buffer format
    key.depth_format = VK_FORMAT_D32_SFLOAT;
    key.color_attachment_count = 5;  // G-buffer has 5 attachments
    
    // ... rest of draw setup same as single model ...
}
```

---

## 10. Common Issues

### Issue 1: Vertex Heap Buffer Not Created

**Symptoms**: Descriptor sync fails, validation errors about null buffer

**Causes**:
- `setModelVertexHeapHandle()` not called
- Buffer handle invalid
- `ensureBuffer()` not called before descriptor sync

**Fix**: Ensure vertex heap handle is set when heap is created, verify `ensureBuffer()` called

### Issue 2: Uniform Offset Not Aligned

**Symptoms**: Validation error: "VUID-VkDescriptorBufferInfo-offset-00340"

**Causes**:
- Uniform offset not aligned to `minUniformBufferOffsetAlignment`
- Using struct size instead of aligned size

**Fix**: Always align offsets:
```cpp
const size_t alignment = renderer.getMinUniformOffsetAlignment();
size_t offset = align_up(baseOffset, alignment);
```

### Issue 3: Bindless Slot Returns Fallback

**Symptoms**: All models use fallback texture (black)

**Causes**:
- Texture not resident
- Bindless slots exhausted
- Texture handle invalid

**Debugging**:
```cpp
uint32_t slot = getBindlessTextureIndex(frameCtx, textureHandle);
if (slot == 0) {
    Warning(LOCATION, "Texture %d returned fallback slot", textureHandle);
    // Check texture residency
}
```

### Issue 4: Push Constants Wrong Size

**Symptoms**: Shader reads garbage values

**Causes**:
- `ModelPushConstants` size mismatch (not 64 bytes)
- Push constant data not initialized
- Wrong offset in `pushConstants()`

**Fix**: Verify `static_assert(sizeof(ModelPushConstants) == 64)` passes

### Issue 5: Descriptor Set Not Bound

**Symptoms**: Validation error: "Descriptor set not bound"

**Causes**:
- Wrong pipeline layout (Standard vs Model)
- Descriptor set not bound before draw
- Dynamic offsets not provided

**Fix**: Ensure model pipeline layout used, bind descriptor set with correct offsets

---

## Appendix: Descriptor Set Layout Reference

**Model Pipeline Layout** (`VulkanDescriptorLayouts.cpp:109-172` in `createModelLayouts()`):

| Binding | Type | Count | Stage | Purpose |
|---------|------|-------|-------|---------|
| 0 | `STORAGE_BUFFER` | 1 | Vertex | Vertex heap SSBO |
| 1 | `COMBINED_IMAGE_SAMPLER` | 1024 | Fragment | Bindless texture array |
| 2 | `UNIFORM_BUFFER_DYNAMIC` | 1 | Vertex + Fragment | Model uniform data |
| 3 | `STORAGE_BUFFER_DYNAMIC` | 1 | Vertex | Batched transform buffer |

**Dynamic Offsets**:
- Offset 0: Binding 2 (uniform buffer)
- Offset 1: Binding 3 (transform buffer)

---

## References

- `code/graphics/vulkan/VulkanRenderer.cpp:2907-2950` - `setModelUniformBinding()` - Uniform binding per-draw
- `code/graphics/vulkan/VulkanRenderer.cpp:2973-3093` - `updateModelDescriptors()` - Bindless texture sync
- `code/graphics/vulkan/VulkanRenderer.cpp:3095-3121` - `beginModelDescriptorSync()` - Frame start sync
- `code/graphics/vulkan/VulkanModelTypes.h` - Push constants structure and attribute bits
- `code/graphics/vulkan/VulkanGraphics.cpp:837-893` - `issueModelDraw()` - Draw call execution
- `code/graphics/vulkan/VulkanGraphics.cpp:895-1083` - `gr_vulkan_render_model()` - Model rendering entry point
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp:109-172` - Model descriptor set layout
- `code/graphics/shaders/model.vert` - Vertex pulling shader
- `code/graphics/shaders/model.frag` - Fragment shader with bindless textures
- `docs/vulkan/VULKAN_DESCRIPTOR_SETS.md` - Descriptor set architecture
- `docs/vulkan/VULKAN_TEXTURE_RESIDENCY.md` - Bindless texture system

