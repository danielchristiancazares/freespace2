# Vulkan Model Rendering End-to-End Pipeline

This document describes the complete model rendering pipeline in the Vulkan renderer, from model data registration through vertex heap allocation, descriptor synchronization, and draw calls.

---

## Table of Contents

1. [Overview](#1-overview)
   - [1.1 Architecture Summary](#11-architecture-summary)
   - [1.2 Why Vertex Pulling?](#12-why-vertex-pulling)
   - [1.3 Key Files](#13-key-files)
   - [1.4 Pipeline Type](#14-pipeline-type)
   - [1.5 Pipeline Layout Contracts](#15-pipeline-layout-contracts)
   - [1.6 Dynamic State](#16-dynamic-state)
2. [Prerequisites](#2-prerequisites)
3. [Glossary](#3-glossary)
4. [Complete Flow Diagram](#4-complete-flow-diagram)
5. [Model Data Registration](#5-model-data-registration)
6. [Vertex Heap System](#6-vertex-heap-system)
7. [Uniform Buffer Management](#7-uniform-buffer-management)
8. [Descriptor Synchronization](#8-descriptor-synchronization)
9. [Draw Call Execution](#9-draw-call-execution)
   - [9.2 Pipeline Selection and VulkanPipelineManager](#92-pipeline-selection-and-vulkanpipelinemanager)
10. [Push Constants](#10-push-constants)
11. [Common Patterns](#11-common-patterns)
12. [Performance Considerations](#12-performance-considerations)
13. [Debugging and Validation](#13-debugging-and-validation)
14. [Common Issues](#14-common-issues)
15. [Appendix: Descriptor Set Layout Reference](#appendix-descriptor-set-layout-reference)
16. [References](#references)

---

## 1. Overview

Model rendering in the Vulkan renderer uses a **vertex pulling** architecture with bindless textures. This design minimizes CPU overhead by eliminating per-draw descriptor updates and vertex buffer bindings.

### 1.1 Architecture Summary

| Component | Description |
|-----------|-------------|
| **Vertex Pulling** | Shader fetches vertex data directly from an SSBO using `gl_VertexIndex`. No traditional vertex attributes are bound. |
| **Bindless Textures** | Textures accessed via array index from a 1024-slot descriptor array. Slot indices passed through push constants. |
| **Dynamic Uniform Buffers** | Per-model data stored in a single buffer with offset-based indexing at bind time. |
| **Vertex Heap** | Large SSBO containing all model vertex data for the frame. Models reference their data via byte offsets. |

### 1.2 Why Vertex Pulling?

Traditional vertex attribute binding requires:
- Setting up vertex input state per-pipeline variant
- Binding vertex buffers before each draw
- Managing multiple vertex buffer bindings for different layouts

Vertex pulling eliminates this overhead:
- One pipeline for all vertex layouts (attributes described in push constants)
- Zero vertex buffer binds (single SSBO bound once per frame)
- Flexible vertex formats without pipeline recompilation

### 1.3 Key Files

| File | Purpose |
|------|---------|
| `code/graphics/vulkan/VulkanRendererResources.cpp` | Model rendering orchestration, descriptor sync |
| `code/graphics/vulkan/VulkanModelTypes.h` | Push constants structure and attribute bits |
| `code/graphics/vulkan/VulkanGraphics.cpp` | Engine integration, `gr_vulkan_render_model()` |
| `code/graphics/vulkan/VulkanPipelineManager.h` | Pipeline caching, `PipelineKey` structure, vertex input modes |
| `code/graphics/vulkan/VulkanPipelineManager.cpp` | Pipeline creation, vertex layout conversion |
| `code/graphics/vulkan/VulkanLayoutContracts.h` | Shader-to-pipeline-layout mapping, `VertexInputMode` enum |
| `code/graphics/vulkan/VulkanLayoutContracts.cpp` | Layout specifications for all shader types |
| `code/graphics/vulkan/VulkanDescriptorLayouts.cpp` | Model pipeline layout and descriptor set layout |
| `code/graphics/shaders/model.vert` | Vertex shader with vertex pulling logic |
| `code/graphics/shaders/model.frag` | Fragment shader with bindless texture sampling |
| `code/graphics/util/uniform_structs.h` | `model_uniform_data` structure definition |

### 1.4 Pipeline Type

- **Shader Type**: `SDR_TYPE_MODEL`
- **Pipeline Layout**: Model-specific layout with vertex heap SSBO, bindless textures, dynamic UBO, and transform SSBO

### 1.5 Pipeline Layout Contracts

The Vulkan renderer uses a contract system (`VulkanLayoutContracts.h`) to map shader types to pipeline configurations:

```cpp
enum class PipelineLayoutKind {
  Standard, // per-draw push descriptors + global set
  Model,    // model bindless set + push constants
  Deferred  // deferred lighting push descriptors + global (G-buffer) set
};

enum class VertexInputMode {
  VertexAttributes, // traditional vertex attributes from vertex_layout
  VertexPulling     // no vertex attributes; fetch from buffers in shader
};

struct ShaderLayoutSpec {
  shader_type type;
  const char *name;
  PipelineLayoutKind pipelineLayout;
  VertexInputMode vertexInput;
};
```

**Model Shader Contract**:
- **Pipeline Layout**: `PipelineLayoutKind::Model`
- **Vertex Input**: `VertexInputMode::VertexPulling`

This is the **only** shader type that uses vertex pulling. All other shaders use `VertexInputMode::VertexAttributes`.

### 1.6 Dynamic State

The renderer targets Vulkan 1.4 and uses extensive dynamic state to minimize pipeline permutations:

**Core Dynamic States** (Extended Dynamic State 1/2, always available):

| State | Set Via |
|-------|---------|
| Viewport | `vkCmdSetViewport` |
| Scissor | `vkCmdSetScissor` |
| Line Width | `vkCmdSetLineWidth` |
| Cull Mode | `vkCmdSetCullMode` |
| Front Face | `vkCmdSetFrontFace` |
| Primitive Topology | `vkCmdSetPrimitiveTopology` |
| Depth Test Enable | `vkCmdSetDepthTestEnable` |
| Depth Write Enable | `vkCmdSetDepthWriteEnable` |
| Depth Compare Op | `vkCmdSetDepthCompareOp` |
| Stencil Test Enable | `vkCmdSetStencilTestEnable` |

**Optional Dynamic States** (Extended Dynamic State 3, capability-gated):

| State | Capability Flag |
|-------|-----------------|
| Color Blend Enable | `caps.colorBlendEnable` |
| Color Write Mask | `caps.colorWriteMask` |
| Polygon Mode | `caps.polygonMode` |
| Rasterization Samples | `caps.rasterizationSamples` |

**Baked Pipeline State** (not dynamic):

- Stencil compare/write masks
- Stencil operations (fail/depth-fail/pass for front and back faces)
- Stencil reference value
- Blend factors and equations

---

## 2. Prerequisites

Before reading this document, you should be familiar with:

| Document | Topics Covered |
|----------|----------------|
| `VULKAN_ARCHITECTURE_OVERVIEW.md` | Overall Vulkan renderer structure |
| `VULKAN_DESCRIPTOR_SETS.md` | Descriptor set organization and binding strategy |
| `VULKAN_TEXTURE_RESIDENCY.md` | Bindless texture system and residency management |
| `VULKAN_UNIFORM_BINDINGS.md` | Uniform buffer layout and alignment |
| `VULKAN_SYNCHRONIZATION.md` | Frame synchronization and resource lifetime |

You should also understand:
- Vulkan descriptor sets and dynamic offsets
- GLSL storage buffers (SSBOs) and uniform buffers
- std140 layout rules for uniform blocks

---

## 3. Glossary

| Term | Definition |
|------|------------|
| **Vertex Pulling** | Technique where shaders read vertex data from a storage buffer using `gl_VertexIndex`, rather than through fixed-function vertex attribute binding. |
| **Vertex Attributes** | Traditional vertex input mode where the pipeline binds vertex buffers and the fixed-function unit provides data to shader inputs. |
| **Bindless Textures** | Array-based texture access where textures are indexed by integer slot rather than bound individually per draw. |
| **SSBO** | Shader Storage Buffer Object. A buffer accessible in shaders with read/write capability and flexible size. |
| **Dynamic Offset** | An offset applied at descriptor set bind time, allowing multiple data ranges to share a single descriptor binding. |
| **Vertex Heap** | The SSBO containing all model vertex data. Each model's vertices occupy a contiguous range at a known byte offset. |
| **Fallback Texture** | A default texture (typically black or pink) used when the requested texture is not resident. |
| **Residency** | Whether a texture's data is currently uploaded to GPU memory and available for sampling. |
| **Push Constants** | Small, fast uniform data pushed directly into the command buffer. Limited to 256 bytes minimum (spec guarantee). |
| **PipelineKey** | Composite key identifying a unique graphics pipeline configuration. Used by `VulkanPipelineManager` for caching. |
| **Extended Dynamic State (EDS)** | Vulkan extensions allowing pipeline state to be set dynamically at command buffer time. EDS1/EDS2 are core in Vulkan 1.4; EDS3 is optional. |
| **Layout Contract** | Mapping from `shader_type` to its required `PipelineLayoutKind` and `VertexInputMode`, defined in `VulkanLayoutContracts.h`. |

---

## 4. Complete Flow Diagram

### 4.1 High-Level Sequence

```
[Model Registration]
    |
    +-- Register model with engine (bmpman, model system)
    |
    +-- [Frame Start]
    |   +-- setModelVertexHeapHandle(handle)       <- Called when heap created
    |   +-- beginModelDescriptorSync(frame, frameIndex, vertexHeapBuffer)
    |   |   +-- Update vertex heap SSBO descriptor      (binding 0)
    |   |   +-- Update bindless texture array           (binding 1)
    |   |   +-- Update transform buffer SSBO            (binding 3)
    |   |   +-- Model uniform buffer updated per-draw   (binding 2)
    |   +-- Descriptors ready for model draws
    |
    +-- [Per-Model Setup]
    |   +-- Upload vertex data to vertex heap
    |   |   +-- Track byte offset in heap
    |   +-- Upload uniform data to uniform buffer
    |   |   +-- Track offset (aligned to minUniformBufferOffsetAlignment)
    |   +-- Resolve bindless texture slots
    |   |   +-- baseMapIndex = getBindlessTextureIndex(baseHandle)
    |   |   +-- normalMapIndex = getBindlessTextureIndex(normalHandle)
    |   |   +-- specMapIndex = getBindlessTextureIndex(specHandle)
    |   +-- Build ModelPushConstants
    |
    +-- [Draw Call]
    |   +-- Build PipelineKey from material and render target
    |   +-- Get/create pipeline via VulkanPipelineManager::getPipeline()
    |   |   +-- Model shaders use VertexPulling mode (no vertex input state)
    |   +-- Set dynamic state (cull mode, depth test/write, stencil test, etc.)
    |   +-- Bind model pipeline
    |   +-- Bind model descriptor set (with dynamic offsets for UBO and transform buffer)
    |   +-- Push ModelPushConstants (vertex layout + texture slots)
    |   +-- Bind index buffer
    |   +-- vkCmdDrawIndexed (vertex data from SSBO via push constants)
    |
    +-- [Frame End]
        +-- Vertex heap and uniform buffer reset for next frame
```

### 4.2 Detailed Data Flow

```
Engine Model Data
    |
    +-- Vertex Data --> Vertex Heap SSBO (per-frame ring buffer)
    |   +-- Engine calls gr_update_buffer_data()
    |   +-- Byte offset tracked per model in indexed_vertex_source
    |
    +-- Uniform Data --> Model Uniform Buffer (dynamic, per-frame)
    |   +-- Size: sizeof(model_uniform_data) = 1216 bytes
    |   +-- Offset aligned to device limit (typically 256 bytes)
    |   +-- Contains matrices, lights, material properties
    |
    +-- Texture Handles --> Bindless Slot Assignment
    |   +-- getBindlessTextureIndex(handle) -> slot 0-1023
    |   +-- Slot 0 reserved for fallback (black)
    |   +-- Slots 1-3 reserved for default base/normal/spec
    |   +-- Slot stored in push constants (baseMapIndex, etc.)
    |
    +-- Transform Data --> Transform Buffer SSBO (batched, per-frame)
        +-- Used for batched model rendering (multiple transforms in one draw)
        +-- Dynamic offset per draw for batch subset
```

---

## 5. Model Data Registration

### 5.1 Vertex Heap Registration

**Function**: `VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle)`

**Location**: `VulkanRendererResources.cpp`

**Called At**: When `GPUMemoryHeap` creates the ModelVertex heap during engine initialization.

**Implementation**:
```cpp
void VulkanRenderer::setModelVertexHeapHandle(gr_buffer_handle handle) {
    // Only store the handle - VkBuffer will be looked up lazily when needed.
    // At registration time, the buffer doesn't exist yet (VulkanBufferManager::createBuffer
    // defers actual VkBuffer creation until updateBufferData is called).
    m_modelVertexHeapHandle = handle;
}
```

**Key Points**:
- Handle is stored, but `VkBuffer` creation is deferred
- No validation at registration time (buffer does not exist yet)
- Buffer is created on first use via `ensureBuffer()`

### 5.2 Buffer Lazy Creation

**Function**: `VulkanBufferManager::ensureBuffer(gr_buffer_handle handle, vk::DeviceSize minSize)`

**Purpose**: Ensures the VkBuffer backing a handle exists and meets minimum size requirements.

**Called At**: Before descriptor sync at frame start

**Example** (from `VulkanRendererFrameFlow.cpp`):
```cpp
// Ensure vertex heap buffer exists and sync descriptors
vk::Buffer vertexHeapBuffer = m_bufferManager->ensureBuffer(m_modelVertexHeapHandle, 1);
beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);
```

**Behavior**:
- If buffer does not exist, creates it with the specified minimum size
- If buffer exists but is smaller than `minSize`, may resize (implementation-dependent)
- Returns the `VkBuffer` handle for immediate use

---

## 6. Vertex Heap System

### 6.1 Vertex Heap Structure

| Property | Value |
|----------|-------|
| **Descriptor Type** | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` |
| **Binding** | Set 0, Binding 0 |
| **Memory** | Device-local or host-visible depending on usage pattern |
| **Lifetime** | Per-frame (ring buffer across frames-in-flight) |

**Purpose**: Contains all model vertex data for the current frame. Each model occupies a contiguous byte range.

### 6.2 Vertex Data Upload

Vertex data flows from the engine to the GPU through these steps:

1. **Engine allocates space** in vertex heap via `GPUMemoryHeap`
2. **Engine uploads data** via `gr_update_buffer_data()` or `gr_update_buffer_data_offset()`
3. **Offset recorded** in `indexed_vertex_source::Vertex_offset`
4. **Offset passed to shader** via `ModelPushConstants::vertexOffset`

**Important**: The offset is a **byte offset**, not a vertex index. The shader converts this to float indices.

### 6.3 Vertex Data Layout

Unlike traditional vertex buffers with fixed layouts, the vertex heap stores vertices with variable formats. The push constants describe the layout:

| Push Constant Field | Purpose |
|---------------------|---------|
| `vertexOffset` | Byte offset of this model's vertices in the heap |
| `stride` | Byte stride between consecutive vertices |
| `vertexAttribMask` | Bitmask indicating which attributes are present |
| `posOffset` | Byte offset of position (vec3) within each vertex |
| `normalOffset` | Byte offset of normal (vec3) within each vertex |
| `texCoordOffset` | Byte offset of texcoord (vec2) within each vertex |
| `tangentOffset` | Byte offset of tangent (vec4) within each vertex |

### 6.4 Vertex Pulling in Shader

**Shader**: `model.vert`

The shader reads vertex data from the SSBO using helper functions:

```glsl
layout(set = 0, binding = 0) readonly buffer VertexBuffer {
    float data[];
} vertexBuffer;

// Convert byte offset to float index (divide by 4)
uint wordIndex(uint byteBase, uint byteOffset) {
    return (byteBase + byteOffset) >> 2;
}

vec3 loadVec3(uint byteBase, uint offset) {
    uint idx = wordIndex(byteBase, offset);
    return vec3(vertexBuffer.data[idx],
                vertexBuffer.data[idx + 1],
                vertexBuffer.data[idx + 2]);
}

void main() {
    // Calculate byte base for this vertex
    uint byteBase = pcs.vertexOffset + uint(gl_VertexIndex) * pcs.stride;

    // Load attributes conditionally based on mask
    vec3 position = hasAttrib(MODEL_ATTRIB_POS)
        ? loadVec3(byteBase, pcs.posOffset)
        : vec3(0.0);

    vec3 normal = hasAttrib(MODEL_ATTRIB_NORMAL)
        ? loadVec3(byteBase, pcs.normalOffset)
        : vec3(0.0, 0.0, 1.0);

    // ... remaining vertex processing ...
}
```

**Key Implementation Detail**: The vertex data is stored as floats (not raw bytes), so byte offsets are converted to float indices by right-shifting by 2 (dividing by 4).

---

## 7. Uniform Buffer Management

### 7.1 Model Uniform Buffer

| Property | Value |
|----------|-------|
| **Descriptor Type** | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` |
| **Binding** | Set 0, Binding 2 |
| **Structure** | `model_uniform_data` (1216 bytes) |
| **Alignment** | `minUniformBufferOffsetAlignment` (typically 256 bytes) |

### 7.2 Uniform Data Structure

**Definition**: `code/graphics/util/uniform_structs.h`

The `model_uniform_data` structure contains:

| Category | Fields |
|----------|--------|
| **Matrices** | `modelViewMatrix`, `modelMatrix`, `viewMatrix`, `projMatrix`, `textureMatrix` |
| **Shadow** | `shadow_mv_matrix`, `shadow_proj_matrix[4]` |
| **Color/Material** | `color`, `outlineWidth`, `defaultGloss`, `alphaGloss`, `alphaMult` |
| **Lighting** | `lights[8]` (array of `model_light`), `n_lights`, `ambientFactor`, `diffuseFactor`, `emissionFactor` |
| **Fog** | `fogColor`, `fogStart`, `fogScale` |
| **Textures** | `sBasemapIndex`, `sGlowmapIndex`, `sSpecmapIndex`, `sNormalmapIndex`, `sAmbientmapIndex`, `sMiscmapIndex` |
| **Misc** | `effect_num`, `flags`, `clip_equation`, `use_clip_plane`, `buffer_matrix_offset`, `thruster_scale` |

**Size**: 1216 bytes (must be 16-byte aligned per std140, device-aligned for dynamic offsets)

### 7.3 Uniform Buffer Update

**Function**: `VulkanRenderer::setModelUniformBinding()`

**Location**: `VulkanRendererResources.cpp`

**Process**:

1. **Validate alignment**:
   ```cpp
   const auto alignment = getMinUniformOffsetAlignment();
   Assertion((dynOffset % alignment) == 0,
       "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
   ```

2. **Update descriptor if buffer handle changed**:
   ```cpp
   if (frame.modelUniformBinding.bufferHandle != handle) {
       vk::WriteDescriptorSet write{};
       write.dstSet = frame.modelDescriptorSet();
       write.dstBinding = 2;
       write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
       // ... write descriptor ...
       m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
   }
   ```

3. **Store binding state for draw time**:
   ```cpp
   frame.modelUniformBinding = DynamicUniformBinding{ handle, dynOffset };
   ```

**Optimization**: Descriptor is only updated when the buffer handle changes. The dynamic offset is applied at `vkCmdBindDescriptorSets` time.

---

## 8. Descriptor Synchronization

### 8.1 Frame Start Sync

**Function**: `VulkanRenderer::beginModelDescriptorSync()`

**Location**: `VulkanRendererResources.cpp`

**Called At**: Frame start, after texture uploads complete

**Purpose**: Updates the model descriptor set for the current frame-in-flight with:
- Current vertex heap buffer (binding 0)
- All resident bindless textures (binding 1)
- Current transform buffer (binding 3)

**Implementation**:
```cpp
void VulkanRenderer::beginModelDescriptorSync(VulkanFrame& frame,
                                              uint32_t frameIndex,
                                              vk::Buffer vertexHeapBuffer) {
    // Collect all resident textures with their bindless slots
    std::vector<std::pair<uint32_t, int>> textures;
    textures.reserve(kMaxBindlessTextures);
    m_textureManager->appendResidentBindlessDescriptors(textures);

    // Update all bindings in one call
    updateModelDescriptors(frameIndex,
        frame.modelDescriptorSet(),
        vertexHeapBuffer,
        frame.vertexBuffer().buffer(),
        textures);
}
```

### 8.2 Descriptor Update Details

**Function**: `VulkanRenderer::updateModelDescriptors()`

**Location**: `VulkanRendererResources.cpp`

This function batches all descriptor writes for efficiency:

**Binding 0 - Vertex Heap SSBO** (always updated):
```cpp
vk::DescriptorBufferInfo heapInfo{};
heapInfo.buffer = vertexHeapBuffer;
heapInfo.offset = 0;
heapInfo.range = VK_WHOLE_SIZE;

vk::WriteDescriptorSet heapWrite{};
heapWrite.dstBinding = 0;
heapWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
heapWrite.pBufferInfo = &heapInfo;
```

**Binding 1 - Bindless Texture Array** (optimized with change detection):
```cpp
// First frame: write all 1024 slots
if (!cache.initialized) {
    texturesWrite.dstArrayElement = 0;
    texturesWrite.descriptorCount = kMaxBindlessTextures;
    // Write fallback + resident textures
}
// Subsequent frames: only write changed ranges
else {
    // Detect contiguous dirty ranges
    // Write only slots that differ from cached state
}
```

**Binding 2 - Model Uniform Buffer**: Updated per-draw via `setModelUniformBinding()`, not here.

**Binding 3 - Transform Buffer SSBO** (always updated):
```cpp
vk::DescriptorBufferInfo transformInfo{};
transformInfo.buffer = transformBuffer;
transformInfo.offset = 0;
transformInfo.range = VK_WHOLE_SIZE;

vk::WriteDescriptorSet transformWrite{};
transformWrite.dstBinding = 3;
transformWrite.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
```

### 8.3 Bindless Texture Cache

**Structure** (`VulkanRenderer.h`):
```cpp
struct ModelBindlessDescriptorCache {
    bool initialized = false;
    std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> infos{};
};
std::array<ModelBindlessDescriptorCache, kFramesInFlight> m_modelBindlessCache;
```

**Optimization Strategy**:
- Cache stores the previous frame's descriptor contents
- Each frame compares desired state against cache
- Only contiguous changed ranges are written
- Reduces `vkUpdateDescriptorSets` overhead significantly

**Reserved Slots**:
| Slot | Purpose |
|------|---------|
| 0 | Fallback texture (sampling missing texture returns this) |
| 1 | Default base texture (white) |
| 2 | Default normal texture (flat normal) |
| 3 | Default specular texture |

---

## 9. Draw Call Execution

### 9.1 Entry Point

**Function**: `gr_vulkan_render_model()`

**Location**: `VulkanGraphics.cpp`

**Parameters**:
- `material_info`: Material properties, shader flags, blend mode
- `vert_source`: Vertex and index buffer handles, offsets
- `bufferp`: Vertex buffer metadata
- `texi`: Texture batch index

### 9.2 Pipeline Selection and VulkanPipelineManager

The `VulkanPipelineManager` handles pipeline creation and caching. It supports two vertex input modes:

| Mode | Description | Used By |
|------|-------------|---------|
| **VertexPulling** | No vertex input attributes; shader fetches from storage buffers via `gl_VertexIndex` | `SDR_TYPE_MODEL` |
| **VertexAttributes** | Traditional vertex input with bindings and attributes from `vertex_layout` | All other shaders |

The mode is determined by `VulkanLayoutContracts.h`:

```cpp
// From VulkanLayoutContracts.h
enum class VertexInputMode {
  VertexAttributes, // traditional vertex attributes from vertex_layout
  VertexPulling     // no vertex attributes; fetch from buffers in shader
};

// Model shaders use vertex pulling
bool usesVertexPulling(shader_type type);  // Returns true for SDR_TYPE_MODEL
```

**PipelineKey Structure** (`VulkanPipelineManager.h`):

```cpp
struct PipelineKey {
  // Shader identification
  shader_type type = SDR_TYPE_NONE;
  uint32_t variant_flags = 0;
  size_t shader_hash = 0;  // Computed internally by getPipeline()

  // Render target configuration
  VkFormat color_format = VK_FORMAT_UNDEFINED;
  VkFormat depth_format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT;
  uint32_t color_attachment_count = 1;

  // Blend and color state
  gr_alpha_blend blend_mode = ALPHA_BLEND_NONE;
  uint32_t color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  // Stencil configuration (baked into pipeline)
  bool stencil_test_enable = false;
  VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;
  uint32_t stencil_compare_mask = 0xFF;
  uint32_t stencil_write_mask = 0xFF;
  uint32_t stencil_reference = 0;

  // Stencil operations (front/back faces)
  VkStencilOp front_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp front_depth_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp front_pass_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_depth_fail_op = VK_STENCIL_OP_KEEP;
  VkStencilOp back_pass_op = VK_STENCIL_OP_KEEP;

  // Vertex layout hash (IGNORED for vertex pulling shaders)
  size_t layout_hash = 0;
};
```

**Building the Pipeline Key** (from `gr_vulkan_render_model()`):

```cpp
// Build pipeline key from render target and material
PipelineKey key{};
key.type                   = SDR_TYPE_MODEL;
key.variant_flags          = material_info->get_shader_flags();
key.color_format           = static_cast<VkFormat>(rt.colorFormat);
key.depth_format           = static_cast<VkFormat>(rt.depthFormat);
key.sample_count           = static_cast<VkSampleCountFlagBits>(renderer.getSampleCount());
key.color_attachment_count = rt.colorAttachmentCount;
key.blend_mode             = material_info->get_blend_mode();
key.color_write_mask       = convertColorWriteMask(material_info->get_color_mask());

// Stencil configuration
key.stencil_test_enable    = material_info->is_stencil_enabled();
key.stencil_compare_op     = static_cast<VkCompareOp>(convertComparisionFunction(stencilFunc.compare));
key.stencil_compare_mask   = stencilFunc.mask;
key.stencil_reference      = static_cast<uint32_t>(stencilFunc.ref);
key.stencil_write_mask     = material_info->get_stencil_mask();

// Front/back stencil operations from material
key.front_fail_op          = static_cast<VkStencilOp>(convertStencilOperation(frontOp.stencilFailOperation));
key.front_depth_fail_op    = static_cast<VkStencilOp>(convertStencilOperation(frontOp.depthFailOperation));
key.front_pass_op          = static_cast<VkStencilOp>(convertStencilOperation(frontOp.successOperation));
// ... similar for back_*_op ...

// Model shaders ignore layout_hash (vertex pulling mode)

// Get or create pipeline (pass empty layout for vertex pulling)
vertex_layout emptyLayout;
vk::Pipeline pipeline = renderer.getPipeline(key, modules, emptyLayout);
```

**Key Points**:

- `layout_hash` is **ignored** for model shaders because vertex pulling eliminates vertex input state
- `shader_hash` is computed internally from the shader modules, not set by the caller
- Stencil operations are baked into the pipeline (not dynamic state)
- The pipeline manager caches pipelines by `PipelineKey` for reuse across frames

### 9.3 Descriptor Set Binding

**Function**: `issueModelDraw()`

**Location**: `VulkanGraphics.cpp`

```cpp
const auto modelSet = ctx.bound.modelSet;
const auto modelDynamicOffset = ctx.bound.modelUbo.dynamicOffset;
const auto transformDynamicOffset = ctx.bound.transformDynamicOffset;
std::array<uint32_t, 2> dynamicOffsets = { modelDynamicOffset, transformDynamicOffset };

cmd.bindDescriptorSets(
    vk::PipelineBindPoint::eGraphics,
    ctx.pipelineLayout,
    0,      // firstSet
    1,      // setCount
    &modelSet,
    static_cast<uint32_t>(dynamicOffsets.size()),
    dynamicOffsets.data());
```

**Dynamic Offsets Order**:
1. Offset 0 → Binding 2 (model uniform buffer)
2. Offset 1 → Binding 3 (transform buffer)

Order matches the order dynamic descriptors appear in the layout (by binding number).

### 9.4 Push Constants

```cpp
cmd.pushConstants(
    ctx.pipelineLayout,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
    0,
    sizeof(ModelPushConstants),
    &ctx.pcs);
```

Push constants are visible to both vertex and fragment stages (texture indices used in fragment shader, vertex layout used in vertex shader).

### 9.5 Index Buffer and Draw

```cpp
// Select index type (16-bit or 32-bit)
const bool use32BitIndices = (batch.flags & VB_FLAG_LARGE_INDEX) != 0;
const vk::IndexType indexType = use32BitIndices
    ? vk::IndexType::eUint32
    : vk::IndexType::eUint16;

// Calculate byte offset into index buffer
const vk::DeviceSize indexOffsetBytes =
    static_cast<vk::DeviceSize>(ctx.vertSource.Index_offset + batch.index_offset);

cmd.bindIndexBuffer(indexBuffer, indexOffsetBytes, indexType);

cmd.drawIndexed(
    indexCount,
    1,  // instanceCount
    0,  // firstIndex (offset baked into bindIndexBuffer)
    0,  // vertexOffset (vertex pulling handles base via push constant)
    0   // firstInstance
);
```

**Key Points**:
- **No vertex buffer bound**: Vertex data comes from SSBO via push constant offset
- **Index buffer is still used normally**: Provides triangle connectivity
- **Index type varies**: 16-bit for small models, 32-bit for large models (>65535 vertices)

---

## 10. Push Constants

### 10.1 Structure Definition

**Location**: `VulkanModelTypes.h`

**Size**: 64 bytes (16 fields x 4 bytes, verified by `static_assert`)

```cpp
// Push constant block for model rendering with vertex pulling and bindless textures.
// Layout must exactly match the GLSL declaration in model.vert and model.frag.
struct ModelPushConstants {
    // Vertex heap addressing
    uint32_t vertexOffset;      // Byte offset into vertex heap buffer for this draw
    uint32_t stride;            // Byte stride between vertices

    // Vertex attribute presence mask (MODEL_ATTRIB_* bits)
    uint32_t vertexAttribMask;

    // Vertex layout offsets (byte offsets within a vertex; ignored if not present in vertexAttribMask)
    uint32_t posOffset;         // Position (vec3)
    uint32_t normalOffset;      // Normal (vec3)
    uint32_t texCoordOffset;    // Texture coordinate (vec2)
    uint32_t tangentOffset;     // Tangent (vec4)
    uint32_t modelIdOffset;     // Model id (float; used for batched transforms)
    uint32_t boneIndicesOffset; // Bone indices (ivec4)
    uint32_t boneWeightsOffset; // Bone weights (vec4)

    // Material texture indices (into bindless texture array). Always valid.
    uint32_t baseMapIndex;
    uint32_t glowMapIndex;
    uint32_t normalMapIndex;
    uint32_t specMapIndex;

    // Instancing (reserved for future use)
    uint32_t matrixIndex;

    // Shader variant flags
    uint32_t flags;
};
static_assert(sizeof(ModelPushConstants) == 64, "ModelPushConstants must be 64 bytes to match GLSL layout");
```

### 10.2 Vertex Attribute Mask Bits

**Location**: `VulkanModelTypes.h`

| Bit | Constant | Meaning |
|-----|----------|---------|
| 0 | `MODEL_ATTRIB_POS` | Position (vec3) present |
| 1 | `MODEL_ATTRIB_NORMAL` | Normal (vec3) present |
| 2 | `MODEL_ATTRIB_TEXCOORD` | Texture coordinate (vec2) present |
| 3 | `MODEL_ATTRIB_TANGENT` | Tangent (vec4) present |
| 4 | `MODEL_ATTRIB_BONEINDICES` | Bone indices (ivec4) present |
| 5 | `MODEL_ATTRIB_BONEWEIGHTS` | Bone weights (vec4) present |
| 6 | `MODEL_ATTRIB_MODEL_ID` | Model ID (float) present for batching |

**Shader Usage**:
```glsl
bool hasAttrib(uint bit) {
    return (pcs.vertexAttribMask & bit) != 0u;
}
```

### 10.3 Flags Field

The `flags` field contains shader variant flags from `MODEL_SDR_FLAG_*` constants defined in `model_shader_flags.h`. Examples:
- `MODEL_SDR_FLAG_TRANSFORM`: Use transform buffer for batched rendering
- `MODEL_SDR_FLAG_THRUSTER`: Apply thruster scaling to vertex positions

---

## 11. Common Patterns

### 11.1 Single Model Draw

```cpp
void drawModel(const FrameCtx& frameCtx, const ModelData& model) {
    // 1. Validate model bindings
    auto modelBound = requireModelBound(frameCtx);

    // 2. Start rendering pass
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);

    // 3. Build push constants
    ModelPushConstants push{};
    push.vertexOffset = model.vertexHeapOffset;
    push.stride = model.vertexStride;
    push.vertexAttribMask = model.attribMask;
    push.posOffset = model.posOffset;
    push.normalOffset = model.normalOffset;
    // ... other offsets ...
    push.baseMapIndex = frameCtx.renderer.getBindlessTextureIndex(frameCtx, model.baseTex);
    push.normalMapIndex = frameCtx.renderer.getBindlessTextureIndex(frameCtx, model.normalTex);
    push.specMapIndex = frameCtx.renderer.getBindlessTextureIndex(frameCtx, model.specTex);
    push.flags = model.shaderFlags;

    // 4. Bind pipeline
    vk::Pipeline pipeline = getPipeline(buildModelPipelineKey(renderCtx.targetInfo));
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

    // 5. Bind descriptor set with dynamic offsets
    uint32_t offsets[2] = { modelBound.modelUbo.dynamicOffset,
                           modelBound.transformDynamicOffset };
    renderCtx.cmd.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        modelPipelineLayout, 0, 1,
        &modelBound.modelSet, 2, offsets);

    // 6. Push constants
    renderCtx.cmd.pushConstants(
        modelPipelineLayout,
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        0, sizeof(push), &push);

    // 7. Bind index buffer and draw
    renderCtx.cmd.bindIndexBuffer(model.indexBuffer, model.indexOffset, model.indexType);
    renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
}
```

### 11.2 Batched Model Draws

For multiple models sharing the same pipeline:

```cpp
void drawBatchedModels(const FrameCtx& frameCtx, const std::vector<ModelData>& models) {
    auto modelBound = requireModelBound(frameCtx);
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);

    // Bind pipeline ONCE
    vk::Pipeline pipeline = getPipeline(buildModelPipelineKey(renderCtx.targetInfo));
    renderCtx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

    // Draw each model (only descriptors and push constants change)
    for (const auto& model : models) {
        // Update uniform binding if buffer changed
        frameCtx.renderer.setModelUniformBinding(
            frameCtx.frame(),
            model.uniformBufferHandle,
            model.uniformOffset,
            model.uniformSize);

        // Bind descriptors with this model's dynamic offsets
        uint32_t offsets[2] = { model.uniformOffset, model.transformOffset };
        renderCtx.cmd.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            modelPipelineLayout, 0, 1,
            &modelBound.modelSet, 2, offsets);

        // Push constants for this model
        ModelPushConstants push = buildPushConstants(model);
        renderCtx.cmd.pushConstants(
            modelPipelineLayout,
            vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
            0, sizeof(push), &push);

        // Draw
        renderCtx.cmd.bindIndexBuffer(model.indexBuffer, model.indexOffset, model.indexType);
        renderCtx.cmd.drawIndexed(model.indexCount, 1, 0, 0, 0);
    }
}
```

### 11.3 G-Buffer Model Rendering

When rendering to the deferred G-buffer:

```cpp
void drawModelToGBuffer(const FrameCtx& frameCtx, const ModelData& model) {
    // G-buffer target set by gr_deferred_lighting_begin()
    auto renderCtx = frameCtx.renderer.ensureRenderingStarted(frameCtx);

    // Pipeline key must match G-buffer format
    PipelineKey key{};
    key.type = SDR_TYPE_MODEL;
    key.color_format = VK_FORMAT_R16G16B16A16_SFLOAT;  // G-buffer uses float formats
    key.depth_format = VK_FORMAT_D32_SFLOAT;
    key.color_attachment_count = 5;  // G-buffer: diffuse, normal, position, specular, emissive
    key.blend_mode = ALPHA_BLEND_NONE;  // G-buffer writes are not blended

    // Rest of draw is identical to single model draw
    // ...
}
```

---

## 12. Performance Considerations

### 12.1 Pipeline Caching

`VulkanPipelineManager` caches all created pipelines by `PipelineKey`. Benefits:

- First frame: Pipelines created on-demand (may cause brief hitches)
- Subsequent frames: Cached pipelines returned immediately
- Vulkan pipeline cache (`VkPipelineCache`) accelerates SPIR-V compilation across sessions

**Model shader advantage**: Because vertex pulling eliminates vertex input state variations, model shaders have fewer pipeline permutations than traditional vertex attribute shaders. The key differences are:
- Stencil configuration changes
- Blend mode changes
- Render target format changes (forward vs deferred)
- Shader variant flags

### 12.2 Minimizing State Changes

| Operation | Cost | Frequency |
|-----------|------|-----------|
| Pipeline bind | High | Once per material/blend mode/stencil change |
| Descriptor set bind | Medium | Once per model (dynamic offset changes) |
| Push constants | Low | Every draw call |
| Index buffer bind | Low | Every draw call |

**Optimization**: Sort draws by `PipelineKey` to minimize pipeline binds. Key fields that force new pipelines:
- `stencil_test_enable` and stencil operations (if different)
- `blend_mode` (when EDS3 colorBlendEnable is not available)
- `color_write_mask` (when EDS3 colorWriteMask is not available)
- Render target format changes

### 12.3 Descriptor Update Batching

The bindless cache (`m_modelBindlessCache`) ensures:
- First frame: All 1024 texture slots written once
- Subsequent frames: Only changed slots written
- Typical case: 0-50 texture changes per frame

### 12.4 Memory Access Patterns

**Vertex Pulling Trade-offs**:
- **Pro**: Single SSBO for all vertex data simplifies binding
- **Con**: Shader loads are not as cache-efficient as fixed-function vertex fetch
- **Mitigation**: Modern GPUs handle SSBO loads efficiently; performance is typically GPU-bound on fragment shading

**Recommendation**: Profile with RenderDoc or Nsight to identify actual bottlenecks.

### 12.5 Dynamic Offset Alignment

Ensure uniform buffer offsets respect `minUniformBufferOffsetAlignment`:
```cpp
size_t alignedOffset = (baseOffset + alignment - 1) & ~(alignment - 1);
```

Typical alignment: 256 bytes. Failing to align causes validation errors and undefined behavior.

---

## 13. Debugging and Validation

### 13.1 Validation Layer Messages

| Error | Likely Cause |
|-------|--------------|
| `VUID-VkDescriptorBufferInfo-offset-00340` | Uniform buffer offset not aligned |
| `VUID-vkCmdDrawIndexed-None-02699` | Descriptor set not bound before draw |
| `VUID-vkCmdDrawIndexed-None-02721` | Push constants not set before draw |
| `VUID-VkWriteDescriptorSet-descriptorType-00325` | Writing to binding with wrong descriptor type |

### 13.2 RenderDoc Debugging

**To verify vertex pulling**:
1. Capture frame with model rendering
2. Select a `vkCmdDrawIndexed` call
3. Check Pipeline State → Descriptor Sets → Set 0 Binding 0
4. Verify SSBO contains expected vertex data
5. Check VS Input/Output to see fetched vertex values

**To verify bindless textures**:
1. Check Pipeline State → Descriptor Sets → Set 0 Binding 1
2. Verify texture array contains expected textures
3. Check push constants for texture slot indices
4. Verify fragment shader samples correct slot

### 13.3 Common Debug Assertions

```cpp
// Verify vertex heap handle is valid
Assertion(m_modelVertexHeapHandle.isValid(),
    "Model vertex heap handle not set");

// Verify uniform offset alignment
Assertion((dynOffset % alignment) == 0,
    "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);

// Verify descriptor set allocated
Assertion(frame.modelDescriptorSet(),
    "Model descriptor set not allocated");

// Verify push constant size
static_assert(sizeof(ModelPushConstants) == 64,
    "ModelPushConstants must be 64 bytes");
```

---

## 14. Common Issues

### Issue 1: Vertex Heap Buffer Not Created

**Symptoms**: Descriptor sync fails, validation errors about null buffer

**Causes**:
- `setModelVertexHeapHandle()` not called during initialization
- Buffer handle invalid or expired
- `ensureBuffer()` not called before descriptor sync

**Solution**:
1. Verify `setModelVertexHeapHandle()` called when heap created
2. Check `m_modelVertexHeapHandle.isValid()` before use
3. Call `ensureBuffer()` to create buffer if needed

### Issue 2: Uniform Offset Not Aligned

**Symptoms**: Validation error `VUID-VkDescriptorBufferInfo-offset-00340`

**Causes**:
- Uniform offset not aligned to `minUniformBufferOffsetAlignment`
- Using `sizeof(model_uniform_data)` directly without alignment padding

**Solution**:
```cpp
const size_t alignment = renderer.getMinUniformOffsetAlignment();
size_t alignedSize = (sizeof(model_uniform_data) + alignment - 1) & ~(alignment - 1);
size_t offset = modelIndex * alignedSize;  // Each model at aligned offset
```

### Issue 3: Bindless Slot Returns Fallback

**Symptoms**: All models render with fallback texture (black or pink)

**Causes**:
- Texture not uploaded/resident when slot requested
- Bindless slots exhausted (>1024 textures)
- Texture handle invalid

**Debugging**:
```cpp
uint32_t slot = getBindlessTextureIndex(frameCtx, textureHandle);
if (slot == kBindlessTextureSlotFallback) {
    Warning(LOCATION, "Texture handle %d returned fallback slot", textureHandle);
    // Check: Is texture resident? Is handle valid?
}
```

### Issue 4: Push Constants Wrong Size

**Symptoms**: Shader reads garbage values, visual corruption

**Causes**:
- C++ struct size does not match GLSL `push_constant` block
- Misaligned fields between CPU and GPU

**Solution**:
1. Verify `static_assert(sizeof(ModelPushConstants) == 64)` compiles
2. Compare C++ struct layout against GLSL declaration field-by-field
3. Ensure both use the same field order and sizes

### Issue 5: Descriptor Set Not Bound

**Symptoms**: Validation error `VUID-vkCmdDrawIndexed-None-02699`

**Causes**:
- Draw issued before `vkCmdBindDescriptorSets`
- Wrong pipeline layout (e.g., using Standard layout with Model pipeline)
- Dynamic offsets not provided when binding

**Solution**:
1. Ensure `bindDescriptorSets` called before every `drawIndexed`
2. Verify pipeline layout matches descriptor set layout
3. Provide correct number of dynamic offsets (2 for model pipeline)

### Issue 6: Index Buffer Offset Incorrect

**Symptoms**: Triangles render incorrectly, vertices from wrong model

**Causes**:
- `Index_offset` not added to `batch.index_offset`
- Wrong index type (16-bit vs 32-bit) selected

**Solution**:
```cpp
// Correct offset calculation
vk::DeviceSize indexOffsetBytes =
    static_cast<vk::DeviceSize>(vertSource.Index_offset + batch.index_offset);

// Correct index type selection
bool use32Bit = (batch.flags & VB_FLAG_LARGE_INDEX) != 0;
vk::IndexType indexType = use32Bit ? vk::IndexType::eUint32 : vk::IndexType::eUint16;
```

### Issue 7: Layout Hash Mismatch (Non-Model Shaders)

**Symptoms**: `std::runtime_error` thrown with message "PipelineKey.layout_hash mismatches provided vertex_layout"

**Causes**:
- `PipelineKey.layout_hash` was not set to match the `vertex_layout` passed to `getPipeline()`
- The layout was modified between computing the hash and calling `getPipeline()`

**Solution**:
```cpp
// For shaders using VertexAttributes mode, set layout_hash from the vertex_layout
PipelineKey key{};
key.type = SDR_TYPE_INTERFACE;  // or other non-model shader
key.layout_hash = myLayout.hash();  // Must match layout passed to getPipeline()

// Then call getPipeline with the same layout
vk::Pipeline pipeline = renderer.getPipeline(key, modules, myLayout);
```

**Note**: This error only applies to shaders using `VertexInputMode::VertexAttributes`. Model shaders (`SDR_TYPE_MODEL`) use `VertexInputMode::VertexPulling` and ignore `layout_hash` entirely.

### Issue 8: Stencil Enabled Without Stencil Format

**Symptoms**: `std::runtime_error` thrown with message "Stencil test enabled but render target depth format has no stencil component"

**Causes**:
- `PipelineKey.stencil_test_enable = true` but depth format lacks stencil (e.g., `VK_FORMAT_D32_SFLOAT`)

**Solution**:
- Use a depth/stencil format like `VK_FORMAT_D32_SFLOAT_S8_UINT` or `VK_FORMAT_D24_UNORM_S8_UINT`
- Or disable stencil testing if stencil is not needed

---

## Appendix: Descriptor Set Layout Reference

### Model Pipeline Layout

**Location**: `VulkanDescriptorLayouts.cpp`

| Binding | Type | Count | Stage | Purpose |
|---------|------|-------|-------|---------|
| 0 | `STORAGE_BUFFER` | 1 | Vertex | Vertex heap SSBO |
| 1 | `COMBINED_IMAGE_SAMPLER` | 1024 | Fragment | Bindless texture array |
| 2 | `UNIFORM_BUFFER_DYNAMIC` | 1 | Vertex + Fragment | Model uniform data |
| 3 | `STORAGE_BUFFER_DYNAMIC` | 1 | Vertex | Batched transform buffer |

### Dynamic Offset Order

When calling `vkCmdBindDescriptorSets`, dynamic offsets are provided in binding order:
1. Offset 0 → Binding 2 (uniform buffer)
2. Offset 1 → Binding 3 (transform buffer)

### Push Constant Range

- **Stage Flags**: `VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT`
- **Offset**: 0
- **Size**: 64 bytes (`sizeof(ModelPushConstants)`)

### Descriptor Pool Sizes

Per frame-in-flight:
- 1 × Storage Buffer (vertex heap)
- 1 × Storage Buffer Dynamic (transform buffer)
- 1024 × Combined Image Sampler (bindless textures)
- 1 × Uniform Buffer Dynamic (model uniforms)

---

## References

### Source Files

| File | Function/Section |
|------|------------------|
| `VulkanRendererResources.cpp` | `setModelVertexHeapHandle()`, `setModelUniformBinding()` |
| `VulkanRendererResources.cpp` | `updateModelDescriptors()`, `beginModelDescriptorSync()` |
| `VulkanGraphics.cpp` | `issueModelDraw()`, `gr_vulkan_render_model()` |
| `VulkanPipelineManager.h` | `PipelineKey`, `PipelineKeyHasher`, `VertexInputState` |
| `VulkanPipelineManager.cpp` | `getPipeline()`, `createPipeline()`, `convertVertexLayoutToVulkan()` |
| `VulkanLayoutContracts.h` | `PipelineLayoutKind`, `VertexInputMode`, `ShaderLayoutSpec` |
| `VulkanLayoutContracts.cpp` | `getShaderLayoutSpec()`, `usesVertexPulling()` |
| `VulkanDescriptorLayouts.cpp` | `createModelLayouts()` |
| `VulkanModelTypes.h` | `ModelPushConstants`, `MODEL_ATTRIB_*` constants |
| `uniform_structs.h` | `model_uniform_data` structure |
| `model.vert` | Vertex pulling shader |
| `model.frag` | Fragment shader with bindless textures |

### Related Documentation

| Document | Topics |
|----------|--------|
| `VULKAN_ARCHITECTURE_OVERVIEW.md` | Overall Vulkan renderer structure |
| `VULKAN_DESCRIPTOR_SETS.md` | Overall descriptor set architecture |
| `VULKAN_TEXTURE_RESIDENCY.md` | Bindless texture residency system |
| `VULKAN_UNIFORM_BINDINGS.md` | Uniform buffer organization |
| `VULKAN_UNIFORM_ALIGNMENT.md` | std140 alignment rules |
| `VULKAN_PIPELINE_MANAGEMENT.md` | Pipeline caching and creation |
| `VULKAN_SYNCHRONIZATION.md` | Frame synchronization |
| `VULKAN_DEFERRED_LIGHTING_FLOW.md` | G-buffer rendering context |
