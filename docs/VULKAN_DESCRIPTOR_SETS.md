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

// Bindless model texture slots:
// - Slot 0 is always valid fallback (black) so bindless sampling never touches destroyed images.
// - Slots 1..3 are well-known defaults so shaders never need "absent texture" sentinel routing.
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

### 2.1 Descriptor Indexing Features

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

### 2.2 Push Descriptor Support

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

### 2.3 Device Limit Validation

Validated in `VulkanDescriptorLayouts.cpp`:

| Limit | Required Value | Purpose |
|-------|----------------|---------|
| `maxDescriptorSetSampledImages` | >= 1024 | Bindless texture array size |
| `maxDescriptorSetStorageBuffers` | >= 1 | Vertex heap SSBO |
| `maxDescriptorSetStorageBuffersDynamic` | >= 1 | Batched transform buffer |

```cpp
void VulkanDescriptorLayouts::validateDeviceLimits(const vk::PhysicalDeviceLimits& limits)
{
    // Hard assert - no silent clamping
    Assertion(limits.maxDescriptorSetSampledImages >= kMaxBindlessTextures,
              "Device maxDescriptorSetSampledImages (%u) < required %u",
              limits.maxDescriptorSetSampledImages, kMaxBindlessTextures);

    Assertion(limits.maxDescriptorSetStorageBuffers >= 1,
              "Device maxDescriptorSetStorageBuffers (%u) < required 1",
              limits.maxDescriptorSetStorageBuffers);
    Assertion(limits.maxDescriptorSetStorageBuffersDynamic >= 1,
              "Device maxDescriptorSetStorageBuffersDynamic (%u) < required 1",
              limits.maxDescriptorSetStorageBuffersDynamic);
}
```

### 2.4 YCbCr Conversion Support (Movie Path)

For video playback, additional features are validated in `VulkanMovieManager.cpp`:

| Feature/Property | Check |
|------------------|-------|
| `samplerYcbcrConversion` | Vulkan 1.1 feature required |
| `G8_B8_R8_3PLANE_420_UNORM` format | Must support `eSampledImage` + `eTransferDst` |
| `combinedImageSamplerDescriptorCount` | Queried for pool sizing (some implementations need > 1) |

---

## 3. Descriptor Set Architecture

The system defines three distinct pipeline layout kinds, each with its own descriptor set organization. All pre-allocated descriptor sets (global and model) are per-frame, with one set per frame-in-flight (2 total for each type).

### 3.1 Pipeline Layout Kinds

Defined in `VulkanLayoutContracts.h`:
```cpp
enum class PipelineLayoutKind {
    Standard,  // per-draw push descriptors + global set
    Model,     // model bindless set + push constants
    Deferred   // deferred lighting push descriptors + global (G-buffer) set
};
```

### 3.2 Standard Pipeline Layout

Used for 2D rendering, UI, particles, post-processing, and most non-model shaders.

**Set Organization**:
- **Set 0**: Push descriptor set (per-draw, updated via `vkCmdPushDescriptorSetKHR`)
- **Set 1**: Global descriptor set (G-buffer textures for deferred lighting)

**Set 0 Bindings**:
| Binding | Type | Stage | Purpose |
|---------|------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Matrix UBO |
| 1 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Generic data UBO |
| 2 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Texture sampler 0 (primary) |
| 3 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Texture sampler 1 (secondary) |
| 4 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Texture sampler 2 (post-processing) |
| 5 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Texture sampler 3 (post-processing) |

### 3.3 Model Pipeline Layout

Used exclusively for `SDR_TYPE_MODEL` shaders with bindless textures and vertex pulling.

**Set Organization**:
- **Set 0**: Model descriptor set (per-frame, pre-allocated)
- **Push Constants**: 64-byte `ModelPushConstants` block

**Set 0 Bindings**:
| Binding | Type | Count | Stage | Purpose |
|---------|------|-------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | 1 | Vertex | Vertex heap SSBO |
| 1 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | 1024 | Fragment | Bindless texture array |
| 2 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` | 1 | Vertex + Fragment | Model uniform data |
| 3 | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC` | 1 | Vertex | Batched transform buffer |

### 3.4 Deferred Pipeline Layout

Used for `SDR_TYPE_DEFERRED_LIGHTING` shader.

**Set Organization**:
- **Set 0**: Push descriptor set (per-light uniforms)
- **Set 1**: Global descriptor set (G-buffer textures)

**Set 0 Bindings**:
| Binding | Type | Stage | Purpose |
|---------|------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Matrix UBO |
| 1 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | Vertex + Fragment | Light params UBO |

### 3.5 Global Descriptor Set Layout

Shared by Standard and Deferred layouts for G-buffer access.

**Bindings**:
| Binding | Type | Stage | Purpose |
|---------|------|-------|---------|
| 0 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 0 (Color) |
| 1 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 1 (Normal) |
| 2 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 2 (Position) |
| 3 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | Depth (sampled) |
| 4 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 3 (Specular) |
| 5 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | Fragment | G-buffer 4 (Emissive) |

---

## 4. Descriptor Pool Management

### 4.1 Pool Architecture

Three distinct descriptor pools are maintained:

**Global Pool** (for G-buffer descriptors, per-frame):
```cpp
poolSizes[0].type = vk::DescriptorType::eCombinedImageSampler;
poolSizes[0].descriptorCount = 6 * kFramesInFlight; // G-buffer (5) + depth (1), per frame
poolInfo.maxSets = kFramesInFlight; // 2 sets (one per frame-in-flight)
```

**Model Pool** (for bindless model rendering):
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

**Movie Pool** (for YCbCr video textures):
```cpp
poolSize.type = vk::DescriptorType::eCombinedImageSampler;
poolSize.descriptorCount = maxMovieTextures * m_movieCombinedImageSamplerDescriptorCount;
poolInfo.maxSets = maxMovieTextures;
```

### 4.2 Allocation Strategy

The system uses a **fixed per-frame allocation model** with no runtime descriptor allocation:

**Initialization** (descriptor layouts and pools):
```cpp
void VulkanRenderer::createDescriptorResources() {
    VulkanDescriptorLayouts::validateDeviceLimits(m_vulkanDevice->properties().limits);
    EnsurePushDescriptorSupport(m_vulkanDevice->features14());
    m_descriptorLayouts = std::make_unique<VulkanDescriptorLayouts>(m_vulkanDevice->device());
}
```

**Frame Creation** (per-frame descriptor sets):
```cpp
void VulkanRenderer::createFrames() {
    for (size_t i = 0; i < kFramesInFlight; ++i) {
        vk::DescriptorSet globalSet = m_descriptorLayouts->allocateGlobalSet();
        vk::DescriptorSet modelSet = m_descriptorLayouts->allocateModelDescriptorSet();
        m_frames[i] = std::make_unique<VulkanFrame>(..., globalSet, modelSet);
    }
}
```

Each frame-in-flight has its own global and model descriptor sets, enabling safe concurrent GPU access without synchronization between frames.

### 4.3 Pool Flags

All pools use `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`:
- Not strictly needed for fixed ring allocation
- Enables explicit set freeing for movie textures (which have dynamic lifetimes)

---

## 5. Sampler Ecosystem

The renderer uses a cached sampler approach to reduce object creation overhead and ensure consistent sampler configurations.

### 5.1 Sampler Key Structure

Samplers are keyed by filter mode and address mode (`VulkanTextureManager.h`):
```cpp
struct SamplerKey {
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

    bool operator==(const SamplerKey& other) const {
        return filter == other.filter && address == other.address;
    }
};
```

### 5.2 Default Sampler Configuration

Created at texture manager initialization (`VulkanTextureManager.cpp`):
```cpp
void VulkanTextureManager::createDefaultSampler()
{
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = vk::CompareOp::eAlways;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    // Allow sampling all mip levels present in the bound image view.
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;

    m_defaultSampler = m_device.createSamplerUnique(samplerInfo);
}
```

### 5.3 Sampler Cache

Samplers are cached by a hash of their key properties:
```cpp
vk::Sampler VulkanTextureManager::getOrCreateSampler(const SamplerKey& key) const
{
    const size_t hash = (static_cast<size_t>(key.filter) << 4) ^ static_cast<size_t>(key.address);
    auto it = m_samplerCache.find(hash);
    if (it != m_samplerCache.end()) {
        return it->second.get();
    }

    vk::SamplerCreateInfo samplerInfo;
    // ... configure sampler from key ...
    auto sampler = m_device.createSamplerUnique(samplerInfo);
    vk::Sampler handle = sampler.get();
    m_samplerCache.emplace(hash, std::move(sampler));
    return handle;
}
```

### 5.4 YCbCr Samplers (Movie Path)

Movie textures use immutable samplers with YCbCr conversion. Four configurations are created for all combinations of:
- **Color space**: BT.601 or BT.709
- **Color range**: Full or Narrow (ITU)

```cpp
convInfo.ycbcrModel = (colorspace == MovieColorSpace::BT709)
    ? vk::SamplerYcbcrModelConversion::eYcbcr709
    : vk::SamplerYcbcrModelConversion::eYcbcr601;
convInfo.ycbcrRange = (range == MovieColorRange::Full)
    ? vk::SamplerYcbcrRange::eItuFull
    : vk::SamplerYcbcrRange::eItuNarrow;
```

The sampler is embedded as an immutable sampler in the descriptor set layout:
```cpp
binding.pImmutableSamplers = &immutableSampler;
```

---

## 6. Descriptor Layout Definitions

### 6.1 VulkanDescriptorLayouts Class

**Header** (`VulkanDescriptorLayouts.h`):
```cpp
class VulkanDescriptorLayouts {
public:
    explicit VulkanDescriptorLayouts(vk::Device device);

    // Validate device limits before creating layouts - hard assert on failure
    static void validateDeviceLimits(const vk::PhysicalDeviceLimits& limits);

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

### 6.2 Model Push Constants

Defined in `VulkanModelTypes.h`:
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

Push constant size is validated against the Vulkan 1.4 guaranteed minimum:
```cpp
static_assert(sizeof(ModelPushConstants) <= 256,
              "ModelPushConstants exceeds guaranteed minimum push constant size");
static_assert(sizeof(ModelPushConstants) % 4 == 0,
              "ModelPushConstants size must be multiple of 4");
```

---

## 7. Movie Descriptor System

The movie descriptor system provides a separate pipeline for YCbCr video playback, isolated from the main rendering paths.

### 7.1 Architecture Overview

Each movie texture has:
- A dedicated descriptor set (allocated from the movie pool)
- An immutable YCbCr sampler embedded in the descriptor set layout
- A dedicated pipeline and pipeline layout per YCbCr configuration

### 7.2 YCbCr Configuration Matrix

Four configurations are created for all color space/range combinations:
```cpp
static constexpr uint32_t MOVIE_YCBCR_CONFIG_COUNT = 4;

uint32_t ycbcrIndex(MovieColorSpace colorspace, MovieColorRange range) const {
    return static_cast<uint32_t>(colorspace) * 2u + static_cast<uint32_t>(range);
}
```

| Index | Color Space | Range |
|-------|-------------|-------|
| 0 | BT.601 | Narrow |
| 1 | BT.601 | Full |
| 2 | BT.709 | Narrow |
| 3 | BT.709 | Full |

### 7.3 Per-Configuration Resources

Each configuration contains:
```cpp
struct YcbcrConfig {
    vk::UniqueSamplerYcbcrConversion conversion;
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout setLayout;
    vk::UniquePipelineLayout pipelineLayout;
    vk::UniquePipeline pipeline;
};
```

### 7.4 Movie Push Constants

```cpp
struct MoviePushConstants {
    float screenSize[2];
    float rectMin[2];
    float rectMax[2];
    float alpha;
    float pad;
};
static_assert(sizeof(MoviePushConstants) == 32);
```

### 7.5 Descriptor Pool Sizing

The movie pool accounts for implementations that require multiple descriptors per YCbCr combined image sampler:
```cpp
void VulkanMovieManager::createMovieDescriptorPool(uint32_t maxMovieTextures)
{
    vk::DescriptorPoolSize poolSize{};
    poolSize.type = vk::DescriptorType::eCombinedImageSampler;
    poolSize.descriptorCount = maxMovieTextures * m_movieCombinedImageSamplerDescriptorCount;

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = maxMovieTextures;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;

    m_movieDescriptorPool = m_device.createDescriptorPoolUnique(poolInfo);
}
```

### 7.6 Deferred Release

Movie textures use deferred release to ensure GPU is done with the descriptor before freeing:
```cpp
const uint64_t retireSerial = std::max(m_safeRetireSerial, tex.lastUsedSerial);
m_deferredReleases.enqueue(retireSerial, [t = std::move(tex), pool, dev]() mutable {
    if (t.descriptorSet) {
        dev.freeDescriptorSets(pool, t.descriptorSet);
        t.descriptorSet = VK_NULL_HANDLE;
    }
});
```

---

## 8. Binding Patterns

### 8.1 Push Descriptor Updates (Standard Pipeline)

Used for per-draw 2D/UI rendering. Each push updates only the bindings that differ from defaults:

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

**Important**: Push descriptors are additive within a command buffer. A binding retains its last-pushed value until overwritten. Callers must ensure all required bindings are pushed before each draw. In practice, most draw paths push the full set of 6 bindings (2 UBOs + 4 samplers) to avoid stale descriptor reads.

### 8.2 Model Descriptor Binding

Model shaders bind the pre-allocated model descriptor set with dynamic offsets.

**Model Uniform Binding**:
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

### 8.3 Draw-Path Texture Binding Abstraction

The renderer separates draw-path and upload-path texture access via two wrapper classes:

**VulkanTextureBindings** (draw-path, no GPU work):
```cpp
class VulkanTextureBindings {
public:
    // Returns valid descriptor (fallback if not resident); queues upload if needed.
    vk::DescriptorImageInfo descriptor(TextureId id, uint32_t currentFrameIndex,
                                       const SamplerKey& samplerKey);

    // Returns stable bindless slot index (fallback=0 if not resident/assigned).
    uint32_t bindlessIndex(TextureId id, uint32_t currentFrameIndex);
};
```

**VulkanTextureUploader** (upload-phase only, records GPU work):
```cpp
class VulkanTextureUploader {
public:
    void flushPendingUploads(const UploadCtx& ctx);
    bool updateTexture(const UploadCtx& ctx, int bitmapHandle, ...);
};
```

This separation ensures draw-path code cannot accidentally trigger GPU work or violate phase constraints.

### 8.4 Bindless Slot Management

**Slot Assignment**:
```cpp
void requestBindlessSlot(TextureId id);
std::optional<uint32_t> tryGetBindlessSlot(TextureId id) const;
```

**Reserved Slots** (`VulkanConstants.h`):
- Slot 0: Fallback (black texture, always valid)
- Slot 1: Default base texture
- Slot 2: Default normal texture
- Slot 3: Default specular texture
- Slots 4+: Dynamic texture assignments

### 8.5 Global Descriptor Binding

For deferred lighting G-buffer access:
```cpp
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
    ctx.layout,         // deferred pipeline layout
    1, 1,               // set 1, count 1
    &m_globalDescriptorSet,
    0, nullptr);        // no dynamic offsets
```

---

## 9. Update Mechanisms and Batching

### 9.1 Model Descriptor Sync Flow

Called at frame start after texture uploads:
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

### 9.2 Batched Descriptor Updates

The `updateModelDescriptors` function batches all writes into a single `vkUpdateDescriptorSets` call:

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

### 9.3 Bindless Cache Structure

Per-frame cache to track descriptor contents (`VulkanRenderer.h`):
```cpp
struct ModelBindlessDescriptorCache {
    bool initialized = false;
    std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> infos{};
};
std::array<ModelBindlessDescriptorCache, kFramesInFlight> m_modelBindlessCache;
```

### 9.4 Global Descriptor Updates

Updated before deferred lighting passes or decal rendering. Each frame's global set is updated independently:

```cpp
void VulkanRenderer::bindDeferredGlobalDescriptors(vk::DescriptorSet dstSet) {
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorImageInfo> infos;
    writes.reserve(6);
    infos.reserve(6);

    // G-buffer 0..2 (Color, Normal, Position)
    for (uint32_t i = 0; i < 3; ++i) {
        // ... build info and write ...
    }

    // Depth (binding 3) - uses nearest-filter sampler
    // Specular (binding 4) - G-buffer attachment 3
    // Emissive (binding 5) - G-buffer attachment 4

    m_vulkanDevice->device().updateDescriptorSets(writes, {});
}
```

Called from:
- `deferredLightingFinish()` - before lighting pass
- `beginDecalPass()` - decals sample scene depth

---

## 10. Shader Integration

### 10.1 Shader Module Management

**VulkanShaderManager** (`VulkanShaderManager.h`) loads SPIR-V modules and caches them by shader type and variant flags:

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

### 10.2 Shader Type to Module Mapping

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
| (Movie path) | `movie.vert.spv` | `movie.frag.spv` |

### 10.3 Shader Module Loading

Modules are loaded from either embedded data or filesystem:
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

## 11. Pipeline Layout Contracts

### 11.1 Layout Specification

Each shader type has a fixed contract (`VulkanLayoutContracts.cpp`):

```cpp
constexpr std::array<ShaderLayoutSpec, NUM_SHADER_TYPES> buildSpecs() {
    using PL = PipelineLayoutKind;
    using VI = VertexInputMode;

    return {
        makeSpec(SDR_TYPE_MODEL, "SDR_TYPE_MODEL", PL::Model, VI::VertexPulling),
        makeSpec(SDR_TYPE_EFFECT_PARTICLE, "SDR_TYPE_EFFECT_PARTICLE", PL::Standard, VI::VertexAttributes),
        makeSpec(SDR_TYPE_DEFERRED_LIGHTING, "SDR_TYPE_DEFERRED_LIGHTING", PL::Deferred, VI::VertexAttributes),
        // ... 33 shader types total
    };
}
```

### 11.2 Vertex Input Mode

Two modes are supported (`VulkanLayoutContracts.h`):

- **VertexAttributes**: Traditional vertex input via `VkPipelineVertexInputStateCreateInfo`
- **VertexPulling**: No vertex attributes; data fetched from SSBO in vertex shader

```cpp
enum class VertexInputMode {
    VertexAttributes, // traditional vertex attributes from vertex_layout
    VertexPulling     // no vertex attributes; fetch from buffers in shader
};
```

### 11.3 Complete Shader Layout Table

| Shader Type | Pipeline Layout | Vertex Input |
|-------------|-----------------|--------------|
| `SDR_TYPE_MODEL` | Model | VertexPulling |
| `SDR_TYPE_EFFECT_PARTICLE` | Standard | VertexAttributes |
| `SDR_TYPE_EFFECT_DISTORTION` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_MAIN` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_BLUR` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_BRIGHTPASS` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_FXAA` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_FXAA_PREPASS` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | Standard | VertexAttributes |
| `SDR_TYPE_DEFERRED_LIGHTING` | Deferred | VertexAttributes |
| `SDR_TYPE_DEFERRED_CLEAR` | Standard | VertexAttributes |
| `SDR_TYPE_VIDEO_PROCESS` | Standard | VertexAttributes |
| `SDR_TYPE_PASSTHROUGH_RENDER` | Standard | VertexAttributes |
| `SDR_TYPE_SHIELD_DECAL` | Standard | VertexAttributes |
| `SDR_TYPE_BATCHED_BITMAP` | Standard | VertexAttributes |
| `SDR_TYPE_DEFAULT_MATERIAL` | Standard | VertexAttributes |
| `SDR_TYPE_INTERFACE` | Standard | VertexAttributes |
| `SDR_TYPE_NANOVG` | Standard | VertexAttributes |
| `SDR_TYPE_DECAL` | Standard | VertexAttributes |
| `SDR_TYPE_SCENE_FOG` | Standard | VertexAttributes |
| `SDR_TYPE_VOLUMETRIC_FOG` | Standard | VertexAttributes |
| `SDR_TYPE_ROCKET_UI` | Standard | VertexAttributes |
| `SDR_TYPE_COPY` | Standard | VertexAttributes |
| `SDR_TYPE_COPY_WORLD` | Standard | VertexAttributes |
| `SDR_TYPE_MSAA_RESOLVE` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_SMAA_EDGE` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT` | Standard | VertexAttributes |
| `SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING` | Standard | VertexAttributes |
| `SDR_TYPE_ENVMAP_SPHERE_WARP` | Standard | VertexAttributes |
| `SDR_TYPE_IRRADIANCE_MAP_GEN` | Standard | VertexAttributes |
| `SDR_TYPE_FLAT_COLOR` | Standard | VertexAttributes |

### 11.4 Pipeline Creation

The pipeline manager selects the correct layout based on shader type:
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

## 12. Descriptor Update Safety Rules

### 12.1 Frame Ring Buffer

Each frame-in-flight has its own descriptor sets, enabling safe concurrent GPU access:

**Frame Storage** (`VulkanFrame.h`):
```cpp
vk::DescriptorSet m_globalDescriptorSet{}; // G-buffer bindings for deferred/decal passes
vk::DescriptorSet m_modelDescriptorSet{};  // Bindless textures + vertex heap + uniforms
```

**Per-Frame Bindings** (`VulkanFrame.h`):
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

### 12.2 Descriptor Update Timing

Updates occur at specific points in the frame:

1. **Frame Start**:
   - Flush pending texture uploads
   - Sync model descriptors after uploads complete
   ```cpp
   m_textureUploader->flushPendingUploads(uploadCtx);
   beginModelDescriptorSync(frame, frame.frameIndex(), vertexHeapBuffer);
   ```

2. **Per-Draw**:
   - Model uniform binding updated only if buffer handle changes
   - Dynamic offset updated for each draw

3. **Deferred Pass**:
   - Global G-buffer descriptors updated before lighting

### 12.3 Safe Serial Tracking

Resources track their last-used serial for safe retirement:
```cpp
struct ResidentTexture {
    VulkanTexture gpu;
    uint32_t lastUsedFrame = 0;
    uint64_t lastUsedSerial = 0; // Serial of most recent submission that may reference this texture
};
```

### 12.4 Global Descriptor Set Safety

**Design**: Each frame-in-flight has its own global descriptor set, eliminating race conditions between frames.

**Update Pattern**: The global descriptor set is updated in-place before deferred lighting or decal passes via `bindDeferredGlobalDescriptors()`. Updates occur at a safe point in the frame when:
1. The previous pass (geometry or main rendering) has completed
2. G-buffer textures have been transitioned to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
3. No concurrent GPU reads are possible on the current frame's descriptor set

**Timing**: Since each frame has its own set, the only constraint is within a single frame's command buffer recording.

### 12.5 Bindless Array Safety

The bindless texture array avoids partially-bound descriptors by:
1. Always writing all 1024 slots
2. Filling unused slots with the fallback texture descriptor
3. Ensuring fallback texture is always valid

```cpp
// Binding flags: none. The model bindless descriptor array is fully written each frame
// (fallback-filled), so we do not rely on partially-bound descriptors.
std::array<vk::DescriptorBindingFlags, 4> bindingFlags{};
bindingFlags.fill({});
```

### 12.6 Movie Descriptor Safety

Movie textures use explicit deferred release to prevent use-after-free:
```cpp
const uint64_t retireSerial = std::max(m_safeRetireSerial, tex.lastUsedSerial);
m_deferredReleases.enqueue(retireSerial, [...] {
    dev.freeDescriptorSets(pool, t.descriptorSet);
});
```

---

## 13. Issues and Improvement Areas

### 13.1 Fixed Allocation Model

**Observation**: All descriptor sets are pre-allocated at renderer initialization.

**Benefits**:
- Prevents pool exhaustion under normal usage
- Predictable memory usage
- No per-frame allocation overhead

**Limitations**:
- 1024 bindless texture slots x 2 frames = 2048 descriptors allocated regardless of actual texture count
- No dynamic growth if texture count exceeds limit

### 13.2 Per-Frame Global Descriptor Sets

**Design**: Each frame-in-flight has its own global descriptor set, allocated at frame creation and stored in `VulkanFrame`.

**Benefit**: Eliminates synchronization concerns between frames. Each frame's global set can be updated independently during command buffer recording without affecting other in-flight frames.

**Usage Pattern**: Updated via `bindDeferredGlobalDescriptors()` before deferred lighting passes or decal rendering, writing G-buffer views and depth sampler references.

### 13.3 Conditional Descriptor Updates

**Observation**: Model uniform descriptor is only updated when the buffer handle changes.
```cpp
if (frame.modelUniformBinding.bufferHandle != handle) {
    // Update descriptor
}
```

**Benefit**: Reduces descriptor write overhead when same buffer is reused.

**Risk**: If the underlying VkBuffer changes without handle change (e.g., buffer resize), stale descriptor could be used.

**Current Safety**: Buffer resizing in `VulkanBufferManager` creates a new VkBuffer, and the handle remains valid but the ensureBuffer call would update the descriptor.

### 13.4 Alignment Validation

Dynamic offset alignment is checked at binding time:
```cpp
Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
Assertion((dynOffset % alignment) == 0, ...);
```

**Gap**: Alignment is validated at bind time, not at allocation time. This could lead to runtime failures if allocations are misaligned.

### 13.5 Partially Bound Descriptors

**Observation**:
```cpp
// Binding flags: none. The model bindless descriptor array is fully written each frame
// (fallback-filled), so we do not rely on partially-bound descriptors.
std::array<vk::DescriptorBindingFlags, 4> bindingFlags{};
bindingFlags.fill({});
```

The system avoids `VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` by always filling the entire bindless array with valid descriptors (fallback for unused slots).

### 13.6 Push Descriptor Support

**Requirement**:
```cpp
EnsurePushDescriptorSupport(m_vulkanDevice->features14());
```

Push descriptors are a hard requirement. Devices without `VK_KHR_push_descriptor` cannot use this renderer.

### 13.7 Missing Per-Scene Descriptor Set

**Observation**:
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

| File | Purpose |
|------|---------|
| `VulkanDescriptorLayouts.h` | Layout and pool declarations |
| `VulkanDescriptorLayouts.cpp` | Pool creation, layout definitions, allocation |
| `VulkanConstants.h` | Bindless limits and reserved slots |
| `VulkanFrame.h` | Per-frame descriptor set storage and bindings |
| `VulkanRenderer.h` | Frame array, bindless cache structure |
| `VulkanRendererLifecycle.cpp` | Descriptor resource creation |
| `VulkanRendererResources.cpp` | Model descriptor sync and update logic |
| `VulkanLayoutContracts.h` | Pipeline layout kind enum, vertex input mode |
| `VulkanLayoutContracts.cpp` | Shader-to-layout mapping (33 shader types) |
| `VulkanPipelineManager.h` | Pipeline key and hasher |
| `VulkanPipelineManager.cpp` | Pipeline creation with layout selection |
| `VulkanShaderManager.h` | Shader module caching |
| `VulkanShaderManager.cpp` | Module loading and type mapping |
| `VulkanTextureBindings.h` | Draw-path texture descriptor API (VulkanTextureBindings, VulkanTextureUploader) |
| `VulkanTextureManager.h` | Texture residency, bindless slot management, sampler cache |
| `VulkanTextureManager.cpp` | Sampler creation, texture upload |
| `VulkanModelTypes.h` | Push constant structure |
| `VulkanMovieManager.h` | YCbCr configuration, movie texture types |
| `VulkanMovieManager.cpp` | Movie descriptor pool, pipeline creation |
| `VulkanPhaseContexts.h` | Upload/Render context tokens |
| `VulkanDebug.h` | Extended dynamic state caps |

---

## Appendix: Descriptor Type Usage Summary

| Descriptor Type | Count/Set | Sets | Total | Location |
|-----------------|-----------|------|-------|----------|
| `COMBINED_IMAGE_SAMPLER` (global) | 6 | 2 | 12 | Global G-buffer (per-frame) |
| `COMBINED_IMAGE_SAMPLER` (bindless) | 1024 | 2 | 2048 | Model textures (per-frame) |
| `COMBINED_IMAGE_SAMPLER` (movie) | Variable | N | Variable | Movie textures |
| `UNIFORM_BUFFER` | 2 | Push | N/A | Standard per-draw |
| `UNIFORM_BUFFER_DYNAMIC` | 1 | 2 | 2 | Model uniform (per-frame) |
| `STORAGE_BUFFER` | 1 | 2 | 2 | Vertex heap (per-frame) |
| `STORAGE_BUFFER_DYNAMIC` | 1 | 2 | 2 | Transform buffer (per-frame) |

Total pre-allocated descriptors (excluding movie): 12 + 2048 + 2 + 2 + 2 = **2066 descriptors** across **4 descriptor sets** (2 global + 2 model).
