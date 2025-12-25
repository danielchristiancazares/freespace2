# Vulkan Descriptor Set and Pipeline Management

This document provides a comprehensive analysis of the Vulkan descriptor set system in the FreeSpace 2 Vulkan renderer, covering architecture, allocation strategies, binding patterns, update mechanisms, shader integration, device requirements, and descriptor update safety.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Device Feature Requirements](#2-device-feature-requirements)
3. [Descriptor Set Architecture](#3-descriptor-set-architecture)
4. [Descriptor Pool Management](#4-descriptor-pool-management)
5. [Sampler Ecosystem](#5-sampler-ecosystem)
6. [Descriptor Layout Definitions](#6-descriptor-layout-definitions)
7. [Movie Descriptor System](#7-movie-descriptor-system)
8. [Binding Patterns](#8-binding-patterns)
9. [Update Mechanisms and Batching](#9-update-mechanisms-and-batching)
10. [Shader Integration](#10-shader-integration)
11. [Pipeline Layout Contracts](#11-pipeline-layout-contracts)
12. [Descriptor Update Safety Rules](#12-descriptor-update-safety-rules)
13. [Issues and Improvement Areas](#13-issues-and-improvement-areas)

---

## 1. Overview

The renderer uses a hybrid descriptor strategy combining:

- **Push Descriptors** (VK_KHR_push_descriptor, core in Vulkan 1.4): For per-draw uniform/texture bindings in the standard 2D/UI pipeline
- **Pre-allocated Descriptor Sets**: For bindless model rendering and global G-buffer sampling
- **Dynamic Uniform/Storage Buffers**: For per-draw model data with offset-based indexing
- **Movie Descriptor Sets**: Separate pool for YCbCr video decoding with immutable samplers

### 1.1 Key Constants

Defined in `VulkanConstants.h`:
```cpp
constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxBindlessTextures = 1024;
constexpr uint32_t kBindlessTextureSlotFallback = 0;
constexpr uint32_t kBindlessTextureSlotDefaultBase = 1;
constexpr uint32_t kBindlessTextureSlotDefaultNormal = 2;
constexpr uint32_t kBindlessTextureSlotDefaultSpec = 3;
constexpr uint32_t kBindlessFirstDynamicTextureSlot = 4;
```

### 1.2 Conditional Movie Support

The movie descriptor system is created conditionally based on format support (`VulkanMovieManager.cpp`):
```cpp
bool VulkanMovieManager::initialize(uint32_t maxMovieTextures)
{
    if (!m_vulkanDevice.features11().samplerYcbcrConversion) {
        m_available = false;
        return false;
    }
    if (!queryFormatSupport()) {
        m_available = false;
        return false;
    }
    // ... create YCbCr configs, pool, pipelines
}
```

Callers must check `isAvailable()` before using the movie path.

---

## 2. Device Feature Requirements

The renderer requires specific Vulkan features for bindless model rendering and push descriptors. All validations are **hard requirements** that throw or assert on failure.

### 4.1 Descriptor Indexing Features

Required features checked in `VulkanModelValidation.cpp`:

| Feature | Purpose | Required |
|---------|---------|----------|
| `shaderSampledImageArrayNonUniformIndexing` | Non-uniform indexing into bindless texture array | **Yes** |
| `runtimeDescriptorArray` | Runtime-sized descriptor arrays in shader | **Yes** |
| `descriptorBindingPartiallyBound` | Partially-bound descriptor arrays | **No** (design choice) |

```cpp
bool ValidateModelDescriptorIndexingSupport(const vk::PhysicalDeviceDescriptorIndexingFeatures& features)
{
    if (!features.shaderSampledImageArrayNonUniformIndexing) {
        return false;
    }
    if (!features.runtimeDescriptorArray) {
        return false;
    }
    // descriptorBindingPartiallyBound NOT required:
    // The bindless array is fully written each frame (fallback-filled),
    // so we do not rely on partially-bound descriptors.
    return true;
}
```

### 4.2 Push Descriptor Support

Push descriptors are a **hard requirement** (core in Vulkan 1.4):

```cpp
void EnsurePushDescriptorSupport(const vk::PhysicalDeviceVulkan14Features& features14)
{
    if (!features14.pushDescriptor) {
        throw std::runtime_error("Vulkan: pushDescriptor feature is required but not supported");
    }
}
```

Called at renderer initialization in `VulkanRenderer::createDescriptorResources()`.

### 4.3 Device Limit Validation

Validated in `VulkanDescriptorLayouts.cpp`:

| Limit | Required Value | Purpose |
|-------|----------------|---------|
| `maxDescriptorSetSampledImages` | >= 1024 | Bindless texture array size |
| `maxDescriptorSetStorageBuffers` | >= 1 | Vertex heap SSBO |
| `maxDescriptorSetStorageBuffersDynamic` | >= 1 | Batched transform buffer |

### 4.4 YCbCr Conversion Support (Movie Path)

For video playback, additional features are validated in `VulkanMovieManager.cpp`:

| Feature/Property | Check |
|------------------|-------|
| `samplerYcbcrConversion` | Vulkan 1.1 feature required |
| `G8_B8_R8_3PLANE_420_UNORM` format | Must support `eSampledImage` + `eTransferDst` |
| `combinedImageSamplerDescriptorCount` | Queried for pool sizing (some implementations need > 1) |

---

## 3. Descriptor Set Architecture

The system defines three distinct pipeline layout kinds, each with its own descriptor set organization:

### 4.1 Pipeline Layout Kinds

Defined in `VulkanLayoutContracts.h`:
```cpp
enum class PipelineLayoutKind {
    Standard,  // per-draw push descriptors + global set
    Model,     // model bindless set + push constants
    Deferred   // deferred lighting push descriptors + global (G-buffer) set
};
```

### 4.2 Standard Pipeline Layout

Used for 2D rendering, UI, particles, post-processing, and most non-model shaders.

**Set Organization** (`VulkanDescriptorLayouts.cpp:77-85`):
- **Set 0**: Push descriptor set (per-draw, updated via `vkCmdPushDescriptorSetKHR`)
- **Set 1**: Global descriptor set (G-buffer textures for deferred lighting)

**Set 0 Bindings** (`VulkanDescriptorLayouts.cpp:55-75`):
| Binding | Type | Stage | Purpose |
|---------|------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Matrix UBO |
| 1 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Generic data UBO |
| 2 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Per-draw texture |

### 4.3 Model Pipeline Layout

Used exclusively for `SDR_TYPE_MODEL` shaders with bindless textures and vertex pulling.

**Set Organization** (`VulkanDescriptorLayouts.cpp:160-166`):
- **Set 0**: Model descriptor set (per-frame, pre-allocated)
- **Push Constants**: 64-byte `ModelPushConstants` block

**Set 0 Bindings** (`VulkanDescriptorLayouts.cpp:105-129`):
| Binding | Type | Count | Stage | Purpose |
|---------|------|-------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | 1 | Vertex | Vertex heap SSBO |
| 1 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | 1024 | Fragment | Bindless texture array |
| 2 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` | 1 | Vertex + Fragment | Model uniform data |
| 3 | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC` | 1 | Vertex | Batched transform buffer |

### 4.4 Deferred Pipeline Layout

Used for `SDR_TYPE_DEFERRED_LIGHTING` shader.

**Set Organization** (`VulkanDescriptorLayouts.cpp:233-242`):
- **Set 0**: Push descriptor set (per-light uniforms)
- **Set 1**: Global descriptor set (G-buffer textures)

**Set 0 Bindings** (`VulkanDescriptorLayouts.cpp:216-230`):
| Binding | Type | Stage | Purpose |
|---------|------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Matrix UBO |
| 1 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Light params UBO |

### 4.5 Global Descriptor Set Layout

Shared by Standard and Deferred layouts for G-buffer access.

**Bindings** (`VulkanDescriptorLayouts.cpp:34-47`):
| Binding | Type | Stage | Purpose |
|---------|------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 0 (Color) |
| 1 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 1 (Normal) |
| 2 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 2 (Position) |
| 3 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Depth (sampled) |
| 4 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 3 (Specular) |
| 5 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 4 (Emissive) |

---

## 3. Descriptor Pool Management

### 4.1 Pool Architecture

Two distinct descriptor pools are maintained:

**Global Pool** (`VulkanDescriptorLayouts.cpp:87-97`):
```cpp
poolSizes[0].type = vk::DescriptorType::eCombinedImageSampler;
poolSizes[0].descriptorCount = 6; // G-buffer (5) + depth (1)
poolInfo.maxSets = 1;
```

**Model Pool** (`VulkanDescriptorLayouts.cpp:168-186`):
```cpp
poolSizes[0].type = vk::DescriptorType::eStorageBuffer;
poolSizes[0].descriptorCount = kFramesInFlight;           // vertex heap

poolSizes[1].type = vk::DescriptorType::eStorageBufferDynamic;
poolSizes[1].descriptorCount = kFramesInFlight;           // transform buffer

poolSizes[2].type = vk::DescriptorType::eCombinedImageSampler;
poolSizes[2].descriptorCount = kFramesInFlight * kMaxBindlessTextures; // 2048

poolSizes[3].type = vk::DescriptorType::eUniformBufferDynamic;
poolSizes[3].descriptorCount = kFramesInFlight;           // model data

poolInfo.maxSets = kFramesInFlight; // 2
```

### 4.2 Allocation Strategy

The system uses a **fixed allocation model** with no per-frame descriptor allocation:

**Initialization** (`VulkanRenderer.cpp:100-107`):
```cpp
void VulkanRenderer::createDescriptorResources() {
    VulkanDescriptorLayouts::validateDeviceLimits(m_vulkanDevice->properties().limits);
    EnsurePushDescriptorSupport(m_vulkanDevice->features14());
    m_descriptorLayouts = std::make_unique<VulkanDescriptorLayouts>(m_vulkanDevice->device());
    m_globalDescriptorSet = m_descriptorLayouts->allocateGlobalSet();
}
```

**Frame Creation** (`VulkanRenderer.cpp:109-131`):
```cpp
void VulkanRenderer::createFrames() {
    for (size_t i = 0; i < kFramesInFlight; ++i) {
        vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();
        m_frames[i] = std::make_unique<VulkanFrame>(..., modelSet);
    }
}
```

### 4.3 Device Limit Validation

Before creating layouts, device limits are validated (`VulkanDescriptorLayouts.cpp:12-30`):
```cpp
void VulkanDescriptorLayouts::validateDeviceLimits(const vk::PhysicalDeviceLimits& limits) {
    Assertion(limits.maxDescriptorSetSampledImages >= kMaxBindlessTextures, ...);
    Assertion(limits.maxDescriptorSetStorageBuffers >= 1, ...);
    Assertion(limits.maxDescriptorSetStorageBuffersDynamic >= 1, ...);
}
```

---

## 4. Descriptor Layout Definitions

### 4.1 VulkanDescriptorLayouts Class

**Header** (`VulkanDescriptorLayouts.h:8-44`):
```cpp
class VulkanDescriptorLayouts {
public:
    vk::DescriptorSetLayout globalSetLayout() const;
    vk::DescriptorSetLayout perDrawPushLayout() const;
    vk::PipelineLayout pipelineLayout() const;

    vk::DescriptorSetLayout modelSetLayout() const;
    vk::PipelineLayout modelPipelineLayout() const;
    vk::DescriptorPool modelDescriptorPool() const;

    vk::PipelineLayout deferredPipelineLayout() const;

    vk::DescriptorSet allocateGlobalSet();
    vk::DescriptorSet allocateModelDescriptorSet();
};
```

### 4.2 Model Push Constants

Defined in `VulkanModelTypes.h:21-50`:
```cpp
struct ModelPushConstants {
    // Vertex heap addressing
    uint32_t vertexOffset;       // Byte offset into vertex heap buffer
    uint32_t stride;             // Byte stride between vertices
    uint32_t vertexAttribMask;   // MODEL_ATTRIB_* bits

    // Vertex layout offsets
    uint32_t posOffset;
    uint32_t normalOffset;
    uint32_t texCoordOffset;
    uint32_t tangentOffset;
    uint32_t modelIdOffset;
    uint32_t boneIndicesOffset;
    uint32_t boneWeightsOffset;

    // Material texture indices (bindless array)
    uint32_t baseMapIndex;
    uint32_t glowMapIndex;
    uint32_t normalMapIndex;
    uint32_t specMapIndex;

    uint32_t matrixIndex;        // Reserved for instancing
    uint32_t flags;              // Shader variant flags
};
static_assert(sizeof(ModelPushConstants) == 64);
```

---

## 5. Binding Patterns

### 5.1 Push Descriptor Updates (Standard Pipeline)

Used for per-draw 2D/UI rendering (`VulkanRenderer.cpp:884-901`):
```cpp
vk::WriteDescriptorSet write{};
write.dstBinding = 2;
write.descriptorCount = 1;
write.descriptorType = vk::DescriptorType::eCombinedImageSampler;
write.pImageInfo = &sceneInfo;

std::array<vk::WriteDescriptorSet, 1> writes{write};
cmd.pushDescriptorSetKHR(
    vk::PipelineBindPoint::eGraphics,
    m_descriptorLayouts->pipelineLayout(),
    0,      // set index
    writes);
```

### 5.2 Model Descriptor Binding

Model shaders bind the pre-allocated model descriptor set with dynamic offsets.

**Model Uniform Binding** (`VulkanRenderer.cpp:1309-1352`):
```cpp
void VulkanRenderer::setModelUniformBinding(VulkanFrame& frame,
    gr_buffer_handle handle, size_t offset, size_t size)
{
    // Validate alignment
    Assertion((dynOffset % alignment) == 0, ...);

    // Update descriptor only if buffer handle changed
    if (frame.modelUniformBinding.bufferHandle != handle) {
        vk::WriteDescriptorSet write{};
        write.dstSet = frame.modelDescriptorSet();
        write.dstBinding = 2;
        write.descriptorType = vk::DescriptorType::eUniformBufferDynamic;
        write.descriptorCount = 1;
        write.pBufferInfo = &info;
        m_vulkanDevice->device().updateDescriptorSets(1, &write, 0, nullptr);
    }
    frame.modelUniformBinding = DynamicUniformBinding{ handle, dynOffset };
}
```

### 5.3 Bindless Texture Binding

**Slot Assignment** (`VulkanTextureManager.h:157`):
```cpp
uint32_t getBindlessSlotIndex(int textureHandle);
```

**Reserved Slots** (`VulkanConstants.h:14-18`):
- Slot 0: Fallback (black texture, always valid)
- Slot 1: Default base texture
- Slot 2: Default normal texture
- Slot 3: Default specular texture
- Slots 4+: Dynamic texture assignments

### 5.4 Global Descriptor Binding

For deferred lighting G-buffer access (`VulkanRenderer.cpp:1929-1935`):
```cpp
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
    ctx.layout,         // deferred pipeline layout
    1, 1,               // set 1, count 1
    &m_globalDescriptorSet,
    0, nullptr);        // no dynamic offsets
```

---

## 6. Update Mechanisms and Batching

### 6.1 Model Descriptor Sync Flow

Called at frame start after texture uploads (`VulkanRenderer.cpp:1497-1523`):
```cpp
void VulkanRenderer::beginModelDescriptorSync(VulkanFrame& frame, uint32_t frameIndex,
    vk::Buffer vertexHeapBuffer)
{
    std::vector<std::pair<uint32_t, int>> textures;
    textures.reserve(kMaxBindlessTextures);
    m_textureManager->appendResidentBindlessDescriptors(textures);

    updateModelDescriptors(frameIndex,
        frame.modelDescriptorSet(),
        vertexHeapBuffer,
        frame.vertexBuffer().buffer(),
        textures);
}
```

### 6.2 Batched Descriptor Updates

The `updateModelDescriptors` function batches all writes into a single `vkUpdateDescriptorSets` call (`VulkanRenderer.cpp:1375-1494`):

**Vertex Heap (Binding 0)** - Always updated:
```cpp
vk::WriteDescriptorSet heapWrite{};
heapWrite.dstSet = set;
heapWrite.dstBinding = 0;
heapWrite.descriptorType = vk::DescriptorType::eStorageBuffer;
heapWrite.pBufferInfo = &heapInfo;
writes.push_back(heapWrite);
```

**Transform Buffer (Binding 3)** - Always updated:
```cpp
vk::WriteDescriptorSet transformWrite{};
transformWrite.dstSet = set;
transformWrite.dstBinding = 3;
transformWrite.descriptorType = vk::DescriptorType::eStorageBufferDynamic;
transformWrite.pBufferInfo = &transformInfo;
writes.push_back(transformWrite);
```

**Bindless Textures (Binding 1)** - Optimized with change detection:
```cpp
// First frame: full array write
if (!cache.initialized) {
    texturesWrite.dstArrayElement = 0;
    texturesWrite.descriptorCount = kMaxBindlessTextures;
    cache.initialized = true;
}
// Subsequent frames: only changed contiguous ranges
else {
    while (i < kMaxBindlessTextures) {
        if (sameInfo(cache.infos[i], desiredInfos[i])) { ++i; continue; }
        const uint32_t start = i;
        while (i < kMaxBindlessTextures && !sameInfo(cache.infos[i], desiredInfos[i])) {
            cache.infos[i] = desiredInfos[i];
            ++i;
        }
        // Write contiguous dirty range
        texturesWrite.dstArrayElement = start;
        texturesWrite.descriptorCount = i - start;
        writes.push_back(texturesWrite);
    }
}
```

### 6.3 Bindless Cache Structure

Per-frame cache to track descriptor contents (`VulkanRenderer.h:300-304`):
```cpp
struct ModelBindlessDescriptorCache {
    bool initialized = false;
    std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> infos{};
};
std::array<ModelBindlessDescriptorCache, kFramesInFlight> m_modelBindlessCache;
```

### 6.4 Global Descriptor Updates

Updated before deferred lighting passes (`VulkanRenderer.cpp:743-829`):
```cpp
void VulkanRenderer::bindDeferredGlobalDescriptors() {
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorImageInfo> infos;
    writes.reserve(6);
    infos.reserve(6);

    // G-buffer 0..2
    for (uint32_t i = 0; i < 3; ++i) {
        // ... build info and write ...
    }

    // Depth (binding 3)
    // Specular (binding 4)
    // Emissive (binding 5)

    m_vulkanDevice->device().updateDescriptorSets(writes, {});
}
```

---

## 7. Shader Integration

### 7.1 Shader Module Management

**VulkanShaderManager** (`VulkanShaderManager.h:16-50`) loads SPIR-V modules and caches them by shader type and variant flags:

```cpp
class VulkanShaderManager {
public:
    ShaderModules getModules(shader_type type, uint32_t variantFlags = 0);
    ShaderModules getModulesByFilenames(const SCP_string& vertFilename,
                                        const SCP_string& fragFilename);
private:
    std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_vertexModules;
    std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_fragmentModules;
};
```

### 7.2 Shader Type to Module Mapping

Defined in `VulkanShaderManager.cpp:43-111`:

| Shader Type | Vertex Module | Fragment Module |
|-------------|---------------|-----------------|
| `SDR_TYPE_MODEL` | `model.vert.spv` | `model.frag.spv` |
| `SDR_TYPE_DEFAULT_MATERIAL` | `default-material.vert.spv` | `default-material.frag.spv` |
| `SDR_TYPE_BATCHED_BITMAP` | `batched-bitmap.vert.spv` | `batched-bitmap.frag.spv` |
| `SDR_TYPE_INTERFACE` | `interface.vert.spv` | `interface.frag.spv` |
| `SDR_TYPE_NANOVG` | `nanovg.vert.spv` | `nanovg.frag.spv` |
| `SDR_TYPE_ROCKET_UI` | `rocketui.vert.spv` | `rocketui.frag.spv` |
| `SDR_TYPE_DEFERRED_LIGHTING` | `deferred.vert.spv` | `deferred.frag.spv` |
| `SDR_TYPE_COPY` | `copy.vert.spv` | `copy.frag.spv` |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | `tonemapping.vert.spv` | `tonemapping.frag.spv` |
| `SDR_TYPE_FLAT_COLOR` | `flat-color.vert.spv` | `flat-color.frag.spv` |

### 7.3 Shader Module Loading

Modules are loaded from either embedded data or filesystem (`VulkanShaderManager.cpp:133-177`):
```cpp
vk::UniqueShaderModule VulkanShaderManager::loadModule(const SCP_string& path) {
    // Try embedded file first
    const auto embedded = defaults_get_all();
    for (const auto& df : embedded) {
        if (!stricmp(embeddedName.c_str(), filename.c_str())) {
            // Load from embedded data
        }
    }
    // Fallback to filesystem
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    // ...
}
```

---

## 8. Pipeline Layout Contracts

### 8.1 Layout Specification

Each shader type has a fixed contract (`VulkanLayoutContracts.cpp:28-62`):

```cpp
constexpr std::array<ShaderLayoutSpec, NUM_SHADER_TYPES> buildSpecs() {
    return {
        makeSpec(SDR_TYPE_MODEL, "SDR_TYPE_MODEL", PL::Model, VI::VertexPulling),
        makeSpec(SDR_TYPE_EFFECT_PARTICLE, "SDR_TYPE_EFFECT_PARTICLE", PL::Standard, VI::VertexAttributes),
        makeSpec(SDR_TYPE_DEFERRED_LIGHTING, "SDR_TYPE_DEFERRED_LIGHTING", PL::Deferred, VI::VertexAttributes),
        // ... 30+ shader types
    };
}
```

### 8.2 Vertex Input Mode

Two modes are supported (`VulkanLayoutContracts.h:18-21`):

- **VertexAttributes**: Traditional vertex input via `VkPipelineVertexInputStateCreateInfo`
- **VertexPulling**: No vertex attributes; data fetched from SSBO in vertex shader

### 8.3 Pipeline Creation

The pipeline manager selects the correct layout based on shader type (`VulkanPipelineManager.cpp:480-490`):
```cpp
switch (layoutSpec.pipelineLayout) {
case PipelineLayoutKind::Model:
    pipelineInfo.layout = m_modelPipelineLayout;
    break;
case PipelineLayoutKind::Standard:
    pipelineInfo.layout = m_pipelineLayout;
    break;
case PipelineLayoutKind::Deferred:
    pipelineInfo.layout = m_deferredPipelineLayout;
    break;
}
```

---

## 9. Synchronization and Frame Safety

### 9.1 Frame Ring Buffer

Descriptor sets are rotated with the frame ring buffer:

**Frame Storage** (`VulkanFrame.h:93`):
```cpp
vk::DescriptorSet m_modelDescriptorSet{};
```

**Per-Frame Bindings** (`VulkanFrame.h:59-72`):
```cpp
DynamicUniformBinding modelUniformBinding{ gr_buffer_handle::invalid(), 0 };
DynamicUniformBinding sceneUniformBinding{ gr_buffer_handle::invalid(), 0 };
uint32_t modelTransformDynamicOffset = 0;

void resetPerFrameBindings() {
    modelUniformBinding = { gr_buffer_handle::invalid(), 0 };
    sceneUniformBinding = { gr_buffer_handle::invalid(), 0 };
    modelTransformDynamicOffset = 0;
}
```

### 9.2 Descriptor Update Timing

Updates occur at specific points in the frame:

1. **Frame Start** (`VulkanRenderer.cpp:318-325`):
   - Flush pending texture uploads
   - Sync model descriptors after uploads complete
   ```cpp
   m_textureUploader->flushPendingUploads(uploadCtx);
   beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);
   ```

2. **Per-Draw** (`VulkanRenderer.cpp:1334-1349`):
   - Model uniform binding updated only if buffer handle changes
   - Dynamic offset updated for each draw

3. **Deferred Pass** (`VulkanRenderer.cpp:743-827`):
   - Global G-buffer descriptors updated before lighting

### 9.3 Safe Serial Tracking

Resources track their last-used serial for safe retirement (`VulkanTextureManager.h:116-117`):
```cpp
struct ResidentTexture {
    VulkanTexture gpu;
    uint32_t lastUsedFrame = 0;
    uint64_t lastUsedSerial = 0;
};
```

---

## 10. Issues and Improvement Areas

### 10.1 Fixed Allocation Model

**Observation**: All descriptor sets are pre-allocated at renderer initialization.

**Benefits**:
- Prevents pool exhaustion under normal usage
- Predictable memory usage
- No per-frame allocation overhead

**Limitations**:
- 1024 bindless texture slots x 2 frames = 2048 descriptors allocated regardless of actual texture count
- No dynamic growth if texture count exceeds limit

### 10.2 Global Descriptor Set Sharing

**Observation** (`VulkanRenderer.cpp:743-827`): The global descriptor set is shared across both in-flight frames and updated in-place before deferred lighting.

**Risk**: Concurrent frame access if descriptor updates race with GPU reads.

**Mitigation**: Pipeline barriers ensure G-buffer textures are transitioned to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` before the global descriptors are bound.

### 10.3 Conditional Descriptor Updates

**Observation** (`VulkanRenderer.cpp:1334`): Model uniform descriptor is only updated when the buffer handle changes.
```cpp
if (frame.modelUniformBinding.bufferHandle != handle) {
    // Update descriptor
}
```

**Benefit**: Reduces descriptor write overhead when same buffer is reused.

**Risk**: If the underlying VkBuffer changes without handle change (e.g., buffer resize), stale descriptor could be used.

**Current Safety**: Buffer resizing in `VulkanBufferManager` creates a new VkBuffer, and the handle remains valid but the ensureBuffer call would update the descriptor.

### 10.4 Alignment Validation

Dynamic offset alignment is checked at binding time (`VulkanRenderer.cpp:1318-1320`):
```cpp
Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
Assertion((dynOffset % alignment) == 0, ...);
```

**Gap**: Alignment is validated at bind time, not at allocation time. This could lead to runtime failures if allocations are misaligned.

### 10.5 Partially Bound Descriptors

**Observation** (`VulkanDescriptorLayouts.cpp:131-134`):
```cpp
// Binding flags: none. The model bindless descriptor array is fully written each frame
// (fallback-filled), so we do not rely on partially-bound descriptors.
std::array<vk::DescriptorBindingFlags, 4> bindingFlags{};
bindingFlags.fill({});
```

The system avoids `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` by always filling the entire bindless array with valid descriptors (fallback for unused slots).

### 10.6 Push Descriptor Support

**Requirement** (`VulkanRenderer.cpp:103`):
```cpp
EnsurePushDescriptorSupport(m_vulkanDevice->features14());
```

Push descriptors are a hard requirement. Devices without `VK_KHR_push_descriptor` cannot use this renderer.

### 10.7 Missing Per-Scene Descriptor Set

**Observation** (`VulkanRenderer.cpp:1359-1373`):
```cpp
void VulkanRenderer::setSceneUniformBinding(...) {
    // For now, we just track the state in the frame.
    // In the future, this will update a descriptor set for the scene/view block (binding 6).
    frame.sceneUniformBinding = DynamicUniformBinding{ handle, dynOffset };
}
```

Scene uniforms are tracked but not yet wired to a descriptor set.

---

## File Summary

| File | Purpose | Key Lines |
|------|---------|-----------|
| `VulkanDescriptorLayouts.h` | Layout and pool declarations | 8-44 |
| `VulkanDescriptorLayouts.cpp` | Pool creation, layout definitions, allocation | 12-243 |
| `VulkanConstants.h` | Bindless limits and reserved slots | 8-20 |
| `VulkanFrame.h` | Per-frame descriptor set storage and bindings | 26-98 |
| `VulkanRenderer.h` | Frame array, bindless cache structure | 256-304 |
| `VulkanRenderer.cpp` | Allocation, sync, update logic | 100-131, 743-827, 1309-1523 |
| `VulkanLayoutContracts.h` | Pipeline layout kind enum | 12-16 |
| `VulkanLayoutContracts.cpp` | Shader-to-layout mapping | 28-62, 69-89 |
| `VulkanPipelineManager.h` | Pipeline key and hasher | 17-85 |
| `VulkanPipelineManager.cpp` | Pipeline creation with layout selection | 228-501 |
| `VulkanShaderManager.h` | Shader module caching | 16-50 |
| `VulkanShaderManager.cpp` | Module loading and type mapping | 21-177 |
| `VulkanTextureBindings.h` | Draw-path texture descriptor API | 15-47 |
| `VulkanTextureManager.h` | Bindless slot management | 98-290 |
| `VulkanModelTypes.h` | Push constant structure | 21-50 |
| `VulkanPhaseContexts.h` | Upload/Render context tokens | 15-78 |
| `VulkanDebug.h` | Extended dynamic state caps | 14-19 |

---

## Appendix: Descriptor Type Usage Summary

| Descriptor Type | Count/Set | Sets | Total | Location |
|-----------------|-----------|------|-------|----------|
| `COMBINED_IMAGE_SAMPLER` (global) | 6 | 1 | 6 | Global G-buffer |
| `COMBINED_IMAGE_SAMPLER` (bindless) | 1024 | 2 | 2048 | Model textures |
| `UNIFORM_BUFFER` | 2 | Push | N/A | Standard per-draw |
| `UNIFORM_BUFFER_DYNAMIC` | 1 | 2 | 2 | Model uniform |
| `STORAGE_BUFFER` | 1 | 2 | 2 | Vertex heap |
| `STORAGE_BUFFER_DYNAMIC` | 1 | 2 | 2 | Transform buffer |

Total pre-allocated descriptors: 6 + 2048 + 2 + 2 + 2 = **2060 descriptors** across **3 descriptor sets**.
