# Vulkan Texture Binding Architecture

This document provides comprehensive documentation of the Vulkan texture binding system in the FreeSpace 2 engine, covering texture management, sampler creation, descriptor set integration, format handling, YCbCr conversion, and caching strategies.

**Intended Audience**: Engine developers working on the Vulkan rendering backend, maintainers debugging texture-related issues, and contributors extending the texture system.

**Prerequisites**: Familiarity with Vulkan concepts (descriptors, image layouts, samplers) and the FreeSpace 2 bmpman (bitmap manager) subsystem.

---

## Table of Contents

1. [Texture Binding Architecture Overview](#1-texture-binding-architecture-overview)
2. [Sampler Creation and Management](#2-sampler-creation-and-management)
3. [Descriptor Set Integration](#3-descriptor-set-integration)
4. [Texture Format Handling and Conversions](#4-texture-format-handling-and-conversions)
5. [YCbCr Sampler Creation and Hardware Conversion](#5-ycbcr-sampler-creation-and-hardware-conversion)
6. [Texture Caching and Reuse Patterns](#6-texture-caching-and-reuse-patterns)
7. [Mipmap and Array Texture Handling](#7-mipmap-and-array-texture-handling)
8. [Builtin Textures](#8-builtin-textures)
9. [Image Layout Transitions](#9-image-layout-transitions)
10. [Error Handling and Unavailability](#10-error-handling-and-unavailability)
11. [Thread Safety Considerations](#11-thread-safety-considerations)
12. [Debugging and Troubleshooting](#12-debugging-and-troubleshooting)
13. [Key Files Reference](#key-files-reference)
14. [Glossary](#glossary)

---

## 1. Texture Binding Architecture Overview

### Core Components

The texture binding system consists of several interconnected components that separate concerns between draw-time operations (descriptor access) and upload-time operations (GPU transfers):

| Component | File | Purpose |
|-----------|------|---------|
| `VulkanTextureManager` | `VulkanTextureManager.h:98-295` | Central texture lifecycle manager; owns GPU resources |
| `VulkanTextureBindings` | `VulkanTextureBindings.h:15-47` | Draw-path API; returns descriptors and queues uploads |
| `VulkanTextureUploader` | `VulkanTextureBindings.h:49-66` | Upload-phase API; records GPU transfer commands |
| `TextureId` | `VulkanTextureId.h:18-48` | Strong-typed texture identity wrapper (non-negative bmpman base frame) |
| `VulkanMovieManager` | `VulkanMovieManager.h:19-131` | YCbCr movie texture handling with hardware color conversion |

### Binding Model

The system employs a hybrid binding strategy optimized for different rendering paths:

**1. Bindless Textures (Model Rendering)**

A fixed-size array of up to `kMaxBindlessTextures` (1024) texture slots bound via a descriptor set. Shaders index into this array using per-vertex or per-instance indices, enabling efficient batched rendering without per-draw descriptor updates.

**2. Push Descriptors (2D/HUD Rendering)**

Per-draw immediate descriptor updates via `vkCmdPushDescriptorSetKHR`, bypassing the bindless array for simpler 2D paths where texture locality is unpredictable.

```cpp
// VulkanConstants.h:9-21
constexpr uint32_t kMaxBindlessTextures = 1024;
constexpr uint32_t kBindlessTextureSlotFallback = 0;     // Black fallback (always valid)
constexpr uint32_t kBindlessTextureSlotDefaultBase = 1;  // White default
constexpr uint32_t kBindlessTextureSlotDefaultNormal = 2; // Flat normal (0,0,1)
constexpr uint32_t kBindlessTextureSlotDefaultSpec = 3;   // Dielectric F0
constexpr uint32_t kBindlessFirstDynamicTextureSlot = 4;  // Dynamic range start
```

**Design Rationale**: Slots 0-3 are reserved for builtin textures, ensuring shaders never need conditional "absent texture" sentinel routing. When a texture is unavailable, its slot descriptor points to fallback, producing correct (black) sampling without shader-side branching.

### State Tracking Model

Texture state is tracked by container membership rather than explicit state fields. This approach eliminates state synchronization bugs and makes invariants explicit:

```cpp
// VulkanTextureManager.h:253-265
// State as location:
// - presence in m_residentTextures   => resident (GPU resources valid)
// - presence in m_pendingUploads     => queued for upload
// - presence in m_unavailableTextures => permanently unavailable (non-retriable)
// - presence in m_bindlessSlots      => has a bindless slot assigned
// - presence in m_pendingBindlessSlots => waiting for slot assignment
// - presence in m_renderTargets      => is a render target (pinned)

std::unordered_map<int, ResidentTexture> m_residentTextures;      // keyed by bmpman base frame
std::unordered_map<int, UnavailableTexture> m_unavailableTextures;
std::unordered_map<int, uint32_t> m_bindlessSlots;
std::unordered_set<int> m_pendingBindlessSlots;
std::unordered_set<int> m_pendingRetirements;
std::vector<int> m_pendingUploads;
```

### Texture Identity

The `TextureId` class provides type-safe wrapping of bmpman base-frame handles:

```cpp
// VulkanTextureId.h:18-48
class TextureId {
public:
    // Factory method validates handle before construction
    [[nodiscard]] static std::optional<TextureId> tryFromBaseFrame(int baseFrame)
    {
        if (baseFrame < 0) {
            return std::nullopt;
        }
        return TextureId(baseFrame);
    }

    int baseFrame() const { return m_baseFrame; }

private:
    explicit TextureId(int baseFrame) : m_baseFrame(baseFrame) {}
    int m_baseFrame = 0;
};
```

**Key Property**: Builtin fallback/default textures are not represented as synthetic negative handles. They have explicit descriptor APIs on `VulkanTextureManager`, eliminating handle collision risks.

---

## 2. Sampler Creation and Management

### Default Sampler Configuration

The texture manager creates a default sampler at initialization with standard filtering and addressing suitable for most game textures:

```cpp
// VulkanTextureManager.cpp (createDefaultSampler)
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
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;  // Allows all mip levels
    m_defaultSampler = m_device.createSamplerUnique(samplerInfo);
}
```

### Sampler Caching System

The `SamplerKey` structure enables sampler reuse through a hash-based cache, minimizing Vulkan object creation:

```cpp
// VulkanTextureManager.h:123-131
struct SamplerKey {
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode address = vk::SamplerAddressMode::eRepeat;

    bool operator==(const SamplerKey& other) const {
        return filter == other.filter && address == other.address;
    }
};
```

Sampler lookup and creation occurs on-demand with lazy population:

```cpp
// VulkanTextureManager.cpp (getOrCreateSampler - conceptual)
vk::Sampler VulkanTextureManager::getOrCreateSampler(const SamplerKey& key) const
{
    // Hash combines filter and address mode for O(1) lookup
    const size_t hash = (static_cast<size_t>(key.filter) << 4) ^
                         static_cast<size_t>(key.address);

    auto it = m_samplerCache.find(hash);
    if (it != m_samplerCache.end()) {
        return it->second.get();  // Cache hit
    }

    // Cache miss: create new sampler with requested configuration
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo.magFilter = key.filter;
    samplerInfo.minFilter = key.filter;
    samplerInfo.addressModeU = key.address;
    samplerInfo.addressModeV = key.address;
    samplerInfo.addressModeW = key.address;
    // Remaining fields match default sampler configuration

    auto sampler = m_device.createSamplerUnique(samplerInfo);
    vk::Sampler handle = sampler.get();
    m_samplerCache.emplace(hash, std::move(sampler));
    return handle;
}
```

### Supported Sampler Configurations

| Filter Mode | Address Mode | Use Case |
|-------------|--------------|----------|
| `eLinear` | `eRepeat` | Default for model textures (diffuse, normal, specular) |
| `eLinear` | `eClampToEdge` | UI/HUD elements, environment maps, skyboxes |
| `eNearest` | `eRepeat` | Pixel-art style or when bilinear filtering causes artifacts |
| `eNearest` | `eClampToEdge` | HUD icons, text glyphs, lookup tables |

**Note**: The sampler cache currently supports `filter` and `address` mode variations. Anisotropic filtering and border colors use fixed defaults. Extending the cache requires adding fields to `SamplerKey` and updating the hash function.

---

## 3. Descriptor Set Integration

### Bindless Descriptor Architecture

Model rendering uses a pre-allocated descriptor set with a large combined-image-sampler array. This enables batch rendering where each vertex/instance carries a texture index rather than requiring per-draw descriptor switches:

```cpp
// VulkanRenderer.cpp (bindless descriptor update - conceptual)
// Correctness rule: every slot must always point at a valid descriptor.
// Shader code can sample any slot without defensive null checks.

const vk::DescriptorImageInfo fallbackInfo = m_textureManager->fallbackDescriptor(samplerKey);
const vk::DescriptorImageInfo defaultBaseInfo = m_textureManager->defaultBaseDescriptor(samplerKey);
const vk::DescriptorImageInfo defaultNormalInfo = m_textureManager->defaultNormalDescriptor(samplerKey);
const vk::DescriptorImageInfo defaultSpecInfo = m_textureManager->defaultSpecDescriptor(samplerKey);

std::array<vk::DescriptorImageInfo, kMaxBindlessTextures> desiredInfos{};
desiredInfos.fill(fallbackInfo);  // All slots default to fallback
desiredInfos[kBindlessTextureSlotDefaultBase] = defaultBaseInfo;
desiredInfos[kBindlessTextureSlotDefaultNormal] = defaultNormalInfo;
desiredInfos[kBindlessTextureSlotDefaultSpec] = defaultSpecInfo;

// Overlay resident textures onto their assigned slots
for (const auto& [arrayIndex, handle] : textures) {
    vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(handle, samplerKey);
    desiredInfos[arrayIndex] = info;
}
```

### Descriptor Update Caching

The renderer maintains a per-frame cache to minimize redundant descriptor updates. Only slots whose descriptors have changed since the previous frame are written:

```cpp
// VulkanRenderer.cpp (differential descriptor updates - conceptual)
auto& cache = m_modelBindlessCache[frameIndex];

if (!cache.initialized) {
    // First frame: write all slots
    vk::WriteDescriptorSet texturesWrite{};
    texturesWrite.dstBinding = 1;
    texturesWrite.descriptorCount = kMaxBindlessTextures;
    // ... full update
    cache.infos = desiredInfos;
    cache.initialized = true;
} else {
    // Subsequent frames: only write changed slots
    uint32_t i = 0;
    while (i < kMaxBindlessTextures) {
        if (sameInfo(cache.infos[i], desiredInfos[i])) {
            ++i;
            continue;
        }
        // Find contiguous run of changed slots for batch update
        const uint32_t start = i;
        while (i < kMaxBindlessTextures && !sameInfo(cache.infos[i], desiredInfos[i])) {
            cache.infos[i] = desiredInfos[i];
            ++i;
        }
        // Batch write the changed range [start, i)
        writes.push_back(/* range write */);
    }
}
```

**Performance Note**: The differential update algorithm coalesces adjacent changed slots into single write operations, reducing Vulkan API overhead when only a few textures change between frames.

### Push Descriptor Usage (2D Rendering)

For HUD and 2D rendering, push descriptors (`VK_KHR_push_descriptor`) provide per-draw texture binding without pre-allocation. Push descriptors are used in multiple rendering paths:

| Rendering Path | Description |
|----------------|-------------|
| Primitive Rendering | Per-draw uniform and texture binding for primitives, models, and complex geometry |
| Bitmap Rendering | Immediate descriptor update for HUD bitmaps |
| String/Font Rendering | Per-string uniforms and font texture descriptors for NanoVG and VFNT |
| Model Rendering | Batched transform and material descriptors for model instances |

**Example (Bitmap Rendering)**:
```cpp
// VulkanGraphics.cpp (bitmap rendering - conceptual)
// Get descriptor for the current batch texture
auto texDescriptor = m_textureManager->getTextureDescriptor(bitmapHandle);

// Immediate descriptor update without pre-allocation
cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,
                         layout, 0, 1, &writeDescriptor);
```

### Descriptor Binding Layout

The model descriptor set layout uses the following bindings:

| Binding | Type | Count | Description |
|---------|------|-------|-------------|
| 0 | `eStorageBuffer` | 1 | Vertex heap SSBO (GPU-side vertex data) |
| 1 | `eCombinedImageSampler` | 1024 | Bindless texture array |
| 2 | `eUniformBufferDynamic` | 1 | Per-model uniforms (matrices, material params) |
| 3 | `eStorageBufferDynamic` | 1 | Batched transforms (instance data) |

---

## 4. Texture Format Handling and Conversions

### Format Selection

Format selection is based on bitmap flags and bit depth, determined at upload time:

```cpp
// VulkanTextureManager.cpp (selectFormat - conceptual)
vk::Format selectFormat(const bitmap& bmp)
{
    // Block-compressed formats (DDS files)
    if (bmp.flags & BMP_TEX_DXT1) return vk::Format::eBc1RgbaUnormBlock;
    if (bmp.flags & BMP_TEX_DXT3) return vk::Format::eBc2UnormBlock;
    if (bmp.flags & BMP_TEX_DXT5) return vk::Format::eBc3UnormBlock;
    if (bmp.flags & BMP_TEX_BC7)  return vk::Format::eBc7UnormBlock;

    // Single-channel formats (fonts, alpha masks)
    if ((bmp.flags & BMP_AABITMAP) || bmp.bpp == 8) {
        return vk::Format::eR8Unorm;
    }

    // Standard color formats - bmpman stores as BGRA
    return vk::Format::eB8G8R8A8Unorm;
}
```

### Supported Format Matrix

| Source Format | Vulkan Format | Bytes/Pixel | Block Size | Notes |
|---------------|---------------|-------------|------------|-------|
| DXT1/BC1 | `eBc1RgbaUnormBlock` | 0.5 (avg) | 4x4, 8 bytes | 1-bit alpha |
| DXT3/BC2 | `eBc2UnormBlock` | 1 (avg) | 4x4, 16 bytes | Explicit 4-bit alpha |
| DXT5/BC3 | `eBc3UnormBlock` | 1 (avg) | 4x4, 16 bytes | Interpolated alpha |
| BC7 | `eBc7UnormBlock` | 1 (avg) | 4x4, 16 bytes | High-quality compression |
| 8bpp/AABITMAP | `eR8Unorm` | 1 | N/A | Single channel (fonts) |
| 16bpp | `eB8G8R8A8Unorm` | 4 | N/A | Expanded during upload |
| 24bpp | `eB8G8R8A8Unorm` | 4 | N/A | Expanded during upload |
| 32bpp | `eB8G8R8A8Unorm` | 4 | N/A | Direct copy |

### Block-Compressed Format Detection

```cpp
// VulkanTextureManager.h:32-43
inline bool isBlockCompressedFormat(vk::Format format)
{
    switch (format) {
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc7UnormBlock:
        return true;
    default:
        return false;
    }
}
```

### Pixel Format Conversions

Non-32bpp uncompressed formats are expanded to BGRA8 during staging buffer population:

**16bpp A1R5G5B5 Expansion**:
```cpp
// VulkanTextureManager.cpp (16bpp conversion - conceptual)
// 16bpp uses A1R5G5B5 packing (see Gr_t_* masks in code/graphics/2d.cpp)
auto* src = reinterpret_cast<uint16_t*>(frameBmp->data);
auto* dst = static_cast<uint8_t*>(alloc.mapped);
for (uint32_t i = 0; i < width * height; ++i) {
    const uint16_t pixel = src[i];
    const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
    const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
    const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
    const uint8_t a = (pixel & 0x8000) ? 255u : 0u;  // 1-bit alpha

    dst[i * 4 + 0] = b;
    dst[i * 4 + 1] = g;
    dst[i * 4 + 2] = r;
    dst[i * 4 + 3] = a;
}
```

**24bpp RGB Expansion**:
```cpp
// VulkanTextureManager.cpp (24bpp conversion - conceptual)
auto* src = reinterpret_cast<uint8_t*>(frameBmp->data);
auto* dst = static_cast<uint8_t*>(alloc.mapped);
for (uint32_t i = 0; i < width * height; ++i) {
    dst[i * 4 + 0] = src[i * 3 + 0];  // B
    dst[i * 4 + 1] = src[i * 3 + 1];  // G
    dst[i * 4 + 2] = src[i * 3 + 2];  // R
    dst[i * 4 + 3] = 255;              // A (opaque)
}
```

### Block-Compressed Size Calculation

```cpp
// VulkanTextureManager.h:24-30
inline size_t calculateCompressedSize(uint32_t w, uint32_t h, vk::Format format)
{
    // BC1 uses 8 bytes per 4x4 block; BC2/BC3/BC7 use 16 bytes
    const size_t blockSize = (format == vk::Format::eBc1RgbaUnormBlock) ? 8 : 16;
    const size_t blocksWide = (w + 3) / 4;  // Round up to block boundary
    const size_t blocksHigh = (h + 3) / 4;
    return blocksWide * blocksHigh * blockSize;
}
```

### Layer Size Calculation

For staging buffer allocation, layer size depends on format:

```cpp
// VulkanTextureManager.h:45-55
inline size_t calculateLayerSize(uint32_t w, uint32_t h, vk::Format format)
{
    if (isBlockCompressedFormat(format)) {
        return calculateCompressedSize(w, h, format);
    }
    if (format == vk::Format::eR8Unorm) {
        return static_cast<size_t>(w) * h;  // 1 byte per pixel
    }
    // Non-compressed uploads are expanded to 4 bytes/pixel in the upload path
    return static_cast<size_t>(w) * h * 4;
}
```

---

## 5. YCbCr Sampler Creation and Hardware Conversion

The `VulkanMovieManager` implements hardware-accelerated YCbCr-to-RGB conversion for video playback, offloading color space conversion from the CPU to the GPU sampling hardware.

### Feature Detection

YCbCr conversion requires Vulkan 1.1 and the `samplerYcbcrConversion` feature:

```cpp
// VulkanMovieManager.cpp (initialize - conceptual)
bool VulkanMovieManager::initialize(uint32_t maxMovieTextures)
{
    // Requires Vulkan 1.1 samplerYcbcrConversion feature
    if (!m_vulkanDevice.features11().samplerYcbcrConversion) {
        vkprintf("VulkanMovieManager: samplerYcbcrConversion not supported\n");
        m_available = false;
        return false;
    }
    // ... continue initialization
}
```

### Format Support Query

The manager queries physical device format properties to determine supported sampling features:

```cpp
// VulkanMovieManager.cpp (queryFormatSupport - conceptual)
bool VulkanMovieManager::queryFormatSupport()
{
    const vk::Format format = vk::Format::eG8B8R83Plane420Unorm;  // YUV420P (I420)

    vk::FormatProperties2 formatProps{};
    m_physicalDevice.getFormatProperties2(format, &formatProps);

    const auto features = formatProps.formatProperties.optimalTilingFeatures;

    // Check required features for movie playback
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eTransferDst;
    if ((features & required) != required) {
        return false;
    }

    // Determine chroma location support (affects reconstruction quality)
    if (features & vk::FormatFeatureFlagBits::eMidpointChromaSamples) {
        m_chromaLocation = vk::ChromaLocation::eMidpoint;
    } else if (features & vk::FormatFeatureFlagBits::eCositedChromaSamples) {
        m_chromaLocation = vk::ChromaLocation::eCositedEven;
    }

    // Determine chroma filter support (linear preferred for quality)
    if (features & vk::FormatFeatureFlagBits::eSampledImageYcbcrConversionLinearFilter) {
        m_movieChromaFilter = vk::Filter::eLinear;
    } else {
        m_movieChromaFilter = vk::Filter::eNearest;
    }

    return true;
}
```

### YCbCr Configuration Matrix

Four configurations are created to support BT.601/BT.709 colorspaces with full/narrow ranges:

```cpp
// VulkanMovieManager.cpp (createMovieYcbcrConfigs - conceptual)
void VulkanMovieManager::createMovieYcbcrConfigs()
{
    for (uint32_t i = 0; i < MOVIE_YCBCR_CONFIG_COUNT; ++i) {  // 4 configs
        const auto colorspace = static_cast<MovieColorSpace>(i / 2u);
        const auto range = static_cast<MovieColorRange>(i % 2u);

        vk::SamplerYcbcrConversionCreateInfo convInfo{};
        convInfo.format = vk::Format::eG8B8R83Plane420Unorm;

        // Select color matrix (defines Y'CbCr to R'G'B' transform coefficients)
        convInfo.ycbcrModel = (colorspace == MovieColorSpace::BT709)
            ? vk::SamplerYcbcrModelConversion::eYcbcr709
            : vk::SamplerYcbcrModelConversion::eYcbcr601;

        // Select value range (narrow: 16-235/16-240, full: 0-255)
        convInfo.ycbcrRange = (range == MovieColorRange::Full)
            ? vk::SamplerYcbcrRange::eItuFull
            : vk::SamplerYcbcrRange::eItuNarrow;

        convInfo.components = { eIdentity, eIdentity, eIdentity, eIdentity };
        convInfo.xChromaOffset = m_chromaLocation;
        convInfo.yChromaOffset = m_chromaLocation;
        convInfo.chromaFilter = m_movieChromaFilter;
        convInfo.forceExplicitReconstruction = VK_FALSE;

        cfg.conversion = m_device.createSamplerYcbcrConversionUnique(convInfo);

        // Create immutable sampler referencing the conversion
        vk::SamplerYcbcrConversionInfo samplerConvInfo{};
        samplerConvInfo.conversion = cfg.conversion.get();

        vk::SamplerCreateInfo samplerInfo{};
        samplerInfo.pNext = &samplerConvInfo;  // Chain YCbCr info
        samplerInfo.magFilter = m_movieChromaFilter;
        samplerInfo.minFilter = m_movieChromaFilter;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        // ...
        cfg.sampler = m_device.createSamplerUnique(samplerInfo);
    }
}
```

### YCbCr Configuration Index Mapping

```cpp
// VulkanMovieManager.cpp:99
uint32_t VulkanMovieManager::ycbcrIndex(MovieColorSpace colorspace, MovieColorRange range) const
{
    return static_cast<uint32_t>(colorspace) * 2u + static_cast<uint32_t>(range);
}
```

| Index | Colorspace | Range | Matrix | Typical Use |
|-------|------------|-------|--------|-------------|
| 0 | BT.601 | Narrow | ITU-R BT.601 (16-235) | SD video, legacy content |
| 1 | BT.601 | Full | ITU-R BT.601 (0-255) | SD video, computer graphics |
| 2 | BT.709 | Narrow | ITU-R BT.709 (16-235) | HD video (broadcast) |
| 3 | BT.709 | Full | ITU-R BT.709 (0-255) | HD video, modern content |

### Multi-Planar Image Upload

Movie frames are uploaded as three separate planes (Y, U, V) with 4:2:0 chroma subsampling:

```cpp
// VulkanMovieManager.cpp (uploadMovieFrame - conceptual)
std::array<vk::BufferImageCopy, 3> copies{};

// Y plane (full resolution: width x height)
copies[0].bufferOffset = alloc.offset + tex.yOffset;
copies[0].bufferRowLength = tex.uploadYStride;
copies[0].imageSubresource.aspectMask = vk::ImageAspectFlagBits::ePlane0;
copies[0].imageExtent = vk::Extent3D{tex.width, tex.height, 1};

// U plane (half resolution: width/2 x height/2)
copies[1].bufferOffset = alloc.offset + tex.uOffset;
copies[1].bufferRowLength = tex.uploadUVStride;
copies[1].imageSubresource.aspectMask = vk::ImageAspectFlagBits::ePlane1;
copies[1].imageExtent = vk::Extent3D{uvW, uvH, 1};

// V plane (half resolution: width/2 x height/2)
copies[2].bufferOffset = alloc.offset + tex.vOffset;
copies[2].bufferRowLength = tex.uploadUVStride;
copies[2].imageSubresource.aspectMask = vk::ImageAspectFlagBits::ePlane2;
copies[2].imageExtent = vk::Extent3D{uvW, uvH, 1};

cmd.copyBufferToImage(stagingBuffer, tex.image.get(),
                      vk::ImageLayout::eTransferDstOptimal, copies);
```

**Memory Layout**: The three planes are laid out contiguously in the staging buffer with appropriate alignment. The YCbCr sampler reconstructs full-resolution color by upsampling chroma planes during texture sampling.

---

## 6. Texture Caching and Reuse Patterns

### Residency Lifecycle

```
Upload Request (queueTextureUpload)
     |
     v
+------------------+     flushPendingUploads()     +--------------------+
| m_pendingUploads | --------------------------->  | m_residentTextures |
+------------------+                                +--------------------+
     |                                                       |
     | Upload Failed                                         | deleteTexture() / eviction
     v                                                       v
+------------------------+                         +----------------------+
| m_unavailableTextures  |                         | DeferredReleaseQueue |
+------------------------+                         +----------------------+
                                                             |
                                                             | collect(completedSerial)
                                                             v
                                                   [GPU resources destroyed]
```

### LRU Eviction Strategy

When bindless slots are exhausted, the manager evicts the least recently used texture to make room for new allocations:

```cpp
// VulkanTextureManager.cpp (tryAssignBindlessSlot - eviction logic, conceptual)
if (m_freeBindlessSlots.empty() && allowResidentEvict) {
    // Only evict textures whose last use has completed on the GPU
    uint32_t oldestFrame = UINT32_MAX;
    int oldestHandle = -1;

    for (const auto& [handle, slot] : m_bindlessSlots) {
        // Skip render targets (pinned, never evictable)
        if (m_renderTargets.find(handle) != m_renderTargets.end()) {
            continue;
        }

        auto residentIt = m_residentTextures.find(handle);
        if (residentIt == m_residentTextures.end()) {
            continue;
        }

        const auto& other = residentIt->second;
        // Only evict if GPU is done with this texture (safety check)
        if (other.lastUsedSerial <= m_completedSerial) {
            if (other.lastUsedFrame < oldestFrame) {
                oldestFrame = other.lastUsedFrame;
                oldestHandle = handle;
            }
        }
    }

    if (oldestHandle >= 0) {
        retireTexture(oldestHandle, m_safeRetireSerial);
    }
}
```

**Eviction Constraints**:
- Render targets are pinned and never evicted
- Only textures whose GPU usage has completed can be evicted
- Frame counter provides LRU ordering

### Resident Texture Tracking

```cpp
// VulkanTextureManager.h:113-117
struct ResidentTexture {
    VulkanTexture gpu;              // GPU resources (image, memory, view)
    uint32_t lastUsedFrame = 0;     // CPU frame counter for LRU ordering
    uint64_t lastUsedSerial = 0;    // GPU submission serial for safety
};
```

Frame and serial tracking is updated on texture access:

```cpp
// VulkanTextureManager.cpp (markTextureUsedBaseFrame - conceptual)
auto& record = residentIt->second;
record.lastUsedFrame = m_currentFrameIndex;      // LRU bookkeeping
record.lastUsedSerial = m_safeRetireSerial;       // GPU safety tracking
```

### Deferred Resource Release

GPU resources are released only after all referencing command buffers complete execution:

```cpp
// VulkanDeferredRelease.h:57-84
class DeferredReleaseQueue {
    void collect(uint64_t completedSerial)
    {
        size_t writeIdx = 0;
        for (auto& e : m_entries) {
            if (e.retireSerial <= completedSerial) {
                e.release();  // GPU done with this resource, safe to destroy
            } else {
                m_entries[writeIdx++] = std::move(e);  // Keep waiting
            }
        }
        m_entries.resize(writeIdx);
    }
};
```

**Serial Tracking**: Each enqueued resource carries a `retireSerial` representing the GPU submission that last referenced it. When `collect()` is called with the most recently completed serial, all resources with earlier serials are released.

### Staging Buffer Management

Texture uploads share a per-frame staging buffer with budget tracking:

```cpp
// VulkanTextureManager.cpp (flushPendingUploads - staging allocation, conceptual)
vk::DeviceSize stagingBudget = frame.stagingBuffer().size();
vk::DeviceSize stagingUsed = 0;

for (int baseFrame : m_pendingUploads) {
    // Calculate required staging space
    size_t totalUploadSize = /* calculated per-texture based on dimensions and format */;

    // Textures too large for staging buffer are permanently unavailable
    if (totalUploadSize > stagingBudget) {
        markUnavailable(baseFrame, UnavailableReason::TooLargeForStaging);
        continue;
    }

    // Defer to next frame if budget exhausted for this frame
    if (stagingUsed + totalUploadSize > stagingBudget) {
        remaining.push_back(baseFrame);
        continue;
    }

    // Allocate from per-frame staging buffer
    auto allocOpt = frame.stagingBuffer().try_allocate(layerSize);
    if (!allocOpt) {
        remaining.push_back(baseFrame);
        break;  // No more staging space this frame
    }

    stagingUsed += totalUploadSize;
    // ... perform upload
}
```

**Budget Overflow Behavior**: When the staging buffer is exhausted, remaining uploads are deferred to subsequent frames. This prevents frame stalls from excessive texture streaming.

---

## 7. Mipmap and Array Texture Handling

### Texture Array Detection and Creation

```cpp
// VulkanTextureManager.cpp (upload path - conceptual)
int numFrames = 1;
const int resolvedBase = bm_get_base_frame(baseFrame, &numFrames);
const bool isArray = bm_is_texture_array(baseFrame);
const uint32_t layers = isArray ? static_cast<uint32_t>(numFrames) : 1u;

// Validate array frames have matching dimensions
if (isArray) {
    for (int i = 0; i < numFrames; ++i) {
        ushort f = 0;
        int fw = 0, fh = 0;
        bm_get_info(baseFrame + i, &fw, &fh, &f, nullptr, nullptr);
        if (static_cast<uint32_t>(fw) != width ||
            static_cast<uint32_t>(fh) != height ||
            (f & BMP_TEX_COMP) != (flags & BMP_TEX_COMP)) {
            validArray = false;  // Dimension/format mismatch
            break;
        }
    }
}
```

**Array Validation**: All frames in a texture array must have identical dimensions and compression format. Mismatched arrays are marked as `InvalidArray` and become unavailable.

### Image View Type

All textures (single or array) use `e2DArray` view type for uniform shader access:

```cpp
// VulkanTextureManager.cpp (createImageView - conceptual)
vk::ImageViewCreateInfo viewInfo;
viewInfo.image = record.gpu.image.get();
viewInfo.viewType = vk::ImageViewType::e2DArray;  // Uniform for all textures
viewInfo.format = format;
viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
viewInfo.subresourceRange.levelCount = mipLevels;
viewInfo.subresourceRange.layerCount = layers;
record.gpu.imageView = m_device.createImageViewUnique(viewInfo);
```

**Rationale**: Using `e2DArray` uniformly allows shaders to use the same sampler type for all textures. Single textures have `layerCount = 1` and are accessed with layer index 0.

### Immediate Upload Layout

```cpp
// VulkanTextureManager.h:62-83
struct ImmediateUploadLayout {
    size_t layerSize = 0;          // Size of each layer in bytes
    size_t totalSize = 0;          // Total staging buffer requirement
    std::vector<size_t> layerOffsets;  // Per-layer offsets in staging buffer
};

inline ImmediateUploadLayout buildImmediateUploadLayout(
    uint32_t w, uint32_t h, vk::Format format, uint32_t layers)
{
    ImmediateUploadLayout layout;
    layout.layerSize = calculateLayerSize(w, h, format);
    layout.layerOffsets.reserve(layers);

    constexpr size_t kCopyOffsetAlignment = 4;  // Vulkan spec requirement
    size_t offset = 0;
    for (uint32_t layer = 0; layer < layers; ++layer) {
        offset = alignUp(offset, kCopyOffsetAlignment);
        layout.layerOffsets.push_back(offset);
        offset += layout.layerSize;
    }
    layout.totalSize = alignUp(offset, kCopyOffsetAlignment);
    return layout;
}
```

### Render Target Mipmap Generation

Render targets support automatic mipmap generation via GPU blit operations:

```cpp
// VulkanTextureManager.cpp (mipLevelsForExtent)
uint32_t mipLevelsForExtent(uint32_t w, uint32_t h)
{
    uint32_t levels = 1;
    uint32_t size = (w > h) ? w : h;  // Largest dimension
    while (size > 1) {
        size >>= 1;
        ++levels;
    }
    return levels;
}
```

**Mipmap Generation Algorithm**:

```cpp
// VulkanTextureManager.cpp (generateRenderTargetMipmaps - conceptual)
for (uint32_t level = 1; level < tex.mipLevels; ++level) {
    const int32_t nextW = (mipW > 1) ? (mipW / 2) : 1;
    const int32_t nextH = (mipH > 1) ? (mipH / 2) : 1;

    // Transition destination mip level to transfer dst
    vk::ImageMemoryBarrier2 toDst{};
    toDst.oldLayout = vk::ImageLayout::eUndefined;
    toDst.newLayout = vk::ImageLayout::eTransferDstOptimal;
    toDst.subresourceRange.baseMipLevel = level;
    // ...

    // Blit from previous mip to current (downscale)
    vk::ImageBlit blit{};
    blit.srcSubresource.mipLevel = level - 1;
    blit.srcOffsets[1] = vk::Offset3D(mipW, mipH, 1);
    blit.dstSubresource.mipLevel = level;
    blit.dstOffsets[1] = vk::Offset3D(nextW, nextH, 1);

    cmd.blitImage(tex.image.get(), eTransferSrcOptimal,
                  tex.image.get(), eTransferDstOptimal,
                  1, &blit, vk::Filter::eLinear);

    // Transition current mip to transfer src for next iteration
    // ...

    mipW = nextW;
    mipH = nextH;
}
```

### Cubemap Render Targets

Cubemaps are created with 6 array layers and individual per-face attachment views for rendering:

```cpp
// VulkanTextureManager.cpp (createRenderTarget - cubemap handling, conceptual)
const bool isCubemap = (flags & BMP_FLAG_CUBEMAP) != 0;
const uint32_t layers = isCubemap ? 6u : 1u;

vk::ImageCreateInfo imageInfo{};
imageInfo.flags = isCubemap ? vk::ImageCreateFlagBits::eCubeCompatible : vk::ImageCreateFlags{};
imageInfo.arrayLayers = layers;

// Create per-face attachment views for framebuffer attachment
const uint32_t faceCount = isCubemap ? 6u : 1u;
for (uint32_t face = 0; face < faceCount; ++face) {
    vk::ImageViewCreateInfo faceViewInfo{};
    faceViewInfo.viewType = vk::ImageViewType::e2D;  // Single-face view
    faceViewInfo.subresourceRange.baseArrayLayer = face;
    faceViewInfo.subresourceRange.layerCount = 1;
    rt.faceViews[face] = m_device.createImageViewUnique(faceViewInfo);
}
```

**Cubemap Face Order**: Follows Vulkan/OpenGL convention: +X, -X, +Y, -Y, +Z, -Z (indices 0-5).

---

## 8. Builtin Textures

Four builtin textures provide fallback and default values, ensuring shaders always sample valid data:

| Texture | Slot | Color (RGBA) | Purpose |
|---------|------|--------------|---------|
| Fallback | 0 | (0, 0, 0, 255) | Black; sampled when texture unavailable or not yet loaded |
| Default Base | 1 | (255, 255, 255, 255) | White; untextured surfaces (allows material color through) |
| Default Normal | 2 | (128, 128, 255, 255) | Flat tangent-space normal encoding (0, 0, 1) |
| Default Spec | 3 | (10, 10, 10, 0) | Dielectric F0 (~0.04); reasonable default for non-metals |

```cpp
// VulkanTextureManager.cpp (builtin texture creation - conceptual)
void VulkanTextureManager::createFallbackTexture() {
    const uint8_t black[4] = {0, 0, 0, 255};
    m_builtins.fallback = createSolidTexture(black);
}

void VulkanTextureManager::createDefaultTexture() {
    const uint8_t white[4] = {255, 255, 255, 255};
    m_builtins.defaultBase = createSolidTexture(white);
}

void VulkanTextureManager::createDefaultNormalTexture() {
    // Flat tangent-space normal: (0.5, 0.5, 1.0) encoded as (128, 128, 255)
    // Shader decodes: n = normalize(texel.xyz * 2.0 - 1.0) = (0, 0, 1)
    const uint8_t flatNormal[4] = {128, 128, 255, 255};
    m_builtins.defaultNormal = createSolidTexture(flatNormal);
}

void VulkanTextureManager::createDefaultSpecTexture() {
    // Default dielectric F0 (~0.04) for Fresnel calculations
    // 10/255 = 0.039, close to typical dielectric reflectance
    const uint8_t dielectricF0[4] = {10, 10, 10, 0};
    m_builtins.defaultSpec = createSolidTexture(dielectricF0);
}
```

**Descriptor Access**:
```cpp
// VulkanTextureManager.h:166-169
vk::DescriptorImageInfo fallbackDescriptor(const SamplerKey& samplerKey) const;
vk::DescriptorImageInfo defaultBaseDescriptor(const SamplerKey& samplerKey) const;
vk::DescriptorImageInfo defaultNormalDescriptor(const SamplerKey& samplerKey) const;
vk::DescriptorImageInfo defaultSpecDescriptor(const SamplerKey& samplerKey) const;
```

---

## 9. Image Layout Transitions

The texture manager tracks and transitions image layouts using Vulkan 1.3 synchronization (VK_KHR_synchronization2):

```cpp
// VulkanTextureManager.cpp (stageAccessForLayout - conceptual)
struct StageAccess {
    vk::PipelineStageFlags2 stageMask{};
    vk::AccessFlags2 accessMask{};
};

StageAccess stageAccessForLayout(vk::ImageLayout layout)
{
    StageAccess out{};
    switch (layout) {
    case vk::ImageLayout::eUndefined:
        out.stageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
        out.accessMask = {};  // No prior access
        break;

    case vk::ImageLayout::eColorAttachmentOptimal:
        out.stageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        out.accessMask = vk::AccessFlagBits2::eColorAttachmentRead |
                         vk::AccessFlagBits2::eColorAttachmentWrite;
        break;

    case vk::ImageLayout::eShaderReadOnlyOptimal:
        out.stageMask = vk::PipelineStageFlagBits2::eFragmentShader;
        out.accessMask = vk::AccessFlagBits2::eShaderRead;
        break;

    case vk::ImageLayout::eTransferSrcOptimal:
        out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
        out.accessMask = vk::AccessFlagBits2::eTransferRead;
        break;

    case vk::ImageLayout::eTransferDstOptimal:
        out.stageMask = vk::PipelineStageFlagBits2::eTransfer;
        out.accessMask = vk::AccessFlagBits2::eTransferWrite;
        break;

    default:
        // Conservative fallback for unknown layouts
        out.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
        out.accessMask = vk::AccessFlagBits2::eMemoryRead |
                         vk::AccessFlagBits2::eMemoryWrite;
        break;
    }
    return out;
}
```

**Common Transition Patterns**:

| From | To | Use Case |
|------|----|----------|
| `eUndefined` | `eTransferDstOptimal` | Initial upload |
| `eTransferDstOptimal` | `eShaderReadOnlyOptimal` | Post-upload ready for sampling |
| `eShaderReadOnlyOptimal` | `eColorAttachmentOptimal` | Render target rendering |
| `eColorAttachmentOptimal` | `eShaderReadOnlyOptimal` | Post-render sampling |
| `eShaderReadOnlyOptimal` | `eTransferSrcOptimal` | Mipmap generation source |

---

## 10. Error Handling and Unavailability

Textures can become unavailable for several reasons, tracked by the `UnavailableReason` enum:

```cpp
// VulkanTextureManager.h:105-111
enum class UnavailableReason {
    InvalidHandle,       // bmpman handle invalid or released
    InvalidArray,        // Array frames have mismatched dimensions/formats
    BmpLockFailed,       // Could not lock bitmap for reading (bmpman error)
    TooLargeForStaging,  // Exceeds per-frame staging buffer size
    UnsupportedFormat,   // Format not supported by Vulkan implementation
};
```

Unavailable textures are tracked separately and **never retried** (non-retriable failure):

```cpp
// VulkanTextureManager.cpp (markUnavailable - conceptual)
auto markUnavailable = [&](int baseFrame, UnavailableReason reason) {
    m_unavailableTextures.insert_or_assign(baseFrame, UnavailableTexture{reason});
    m_pendingBindlessSlots.erase(baseFrame);  // Remove from retry queue

    // Release any assigned bindless slot back to free pool
    auto slotIt = m_bindlessSlots.find(baseFrame);
    if (slotIt != m_bindlessSlots.end()) {
        const uint32_t slot = slotIt->second;
        m_bindlessSlots.erase(slotIt);
        if (isDynamicBindlessSlot(slot)) {  // Don't free reserved slots 0-3
            m_freeBindlessSlots.push_back(slot);
        }
    }
};
```

**Recovery**: Unavailable textures sample the fallback (black) texture. The only recovery path is releasing the bmpman handle entirely and re-loading the asset.

### Bitmap Handle Release

When bmpman releases a handle (slot becomes `BM_TYPE_NONE`), the texture manager must immediately drop GPU mappings:

```cpp
// VulkanTextureManager.h:146-148
// Called by bmpman when a bitmap handle is being released.
// This must drop any GPU mapping immediately so handle reuse cannot collide.
void releaseBitmap(int bitmapHandle);
```

This prevents handle reuse collisions where a new bitmap might be assigned the same handle as a previously-loaded texture.

---

## 11. Thread Safety Considerations

**Current Threading Model**: The texture binding system is designed for single-threaded access from the main render thread. Key constraints:

| Operation | Thread Safety |
|-----------|---------------|
| `queueTextureUpload()` | Main thread only |
| `flushPendingUploads()` | Main thread only (upload phase) |
| `getBindlessSlotIndex()` | Main thread only (draw phase) |
| `getTextureDescriptorInfo()` | Main thread only |
| `collect()` | Main thread only (frame end) |
| bmpman callbacks (`releaseBitmap`) | Main thread only |

**No Internal Synchronization**: The manager does not use mutexes or atomics. All access must be serialized externally by the caller.

**Future Considerations**: If background texture streaming is implemented:
1. Upload queueing could be made thread-safe with a concurrent queue
2. Residency maps would need read-write locks
3. Bindless slot assignment would require atomic slot allocation

---

## 12. Debugging and Troubleshooting

### Common Issues

**Symptom**: Black textures appear in-game
- **Cause 1**: Texture marked unavailable (check `m_unavailableTextures`)
- **Cause 2**: Upload not yet complete (texture in `m_pendingUploads`)
- **Cause 3**: Staging buffer exhausted, upload deferred
- **Diagnosis**: Enable `vkprintf` logging to see upload status and unavailability reasons

**Symptom**: Texture corruption or visual artifacts
- **Cause 1**: Format mismatch between bmpman and Vulkan
- **Cause 2**: Incorrect staging buffer layout (alignment)
- **Cause 3**: Missing layout transition barrier
- **Diagnosis**: Use RenderDoc to capture frame and inspect image contents

**Symptom**: Crash on texture access
- **Cause 1**: Accessing destroyed resource (serial tracking bug)
- **Cause 2**: Invalid bindless slot index
- **Cause 3**: Handle reuse collision
- **Diagnosis**: Enable Vulkan validation layers; check `m_completedSerial` vs `lastUsedSerial`

**Symptom**: High VRAM usage
- **Cause 1**: LRU eviction not triggering (all textures "in use")
- **Cause 2**: Deferred release queue not draining
- **Cause 3**: Too many render targets pinned
- **Diagnosis**: Log `m_residentTextures.size()` and `m_deferredReleases.size()`

### Diagnostic APIs

```cpp
// Query texture state
bool hasBindlessSlot(int baseFrameHandle) const;  // Is slot assigned?
bool hasRenderTarget(int baseFrameHandle) const;  // Is this a render target?

// Query collections (for debugging)
size_t residentCount = m_residentTextures.size();
size_t pendingCount = m_pendingUploads.size();
size_t unavailableCount = m_unavailableTextures.size();
size_t deferredCount = m_deferredReleases.size();
```

### Validation Layer Messages

Enable `VK_LAYER_KHRONOS_validation` to catch:
- Use-after-free on image/imageView/sampler
- Invalid image layout transitions
- Descriptor set update errors
- Push descriptor binding mismatches

---

## Key Files Reference

| File | Lines | Description |
|------|-------|-------------|
| `VulkanTextureManager.h` | 1-296 | Texture manager class definition, helper functions |
| `VulkanTextureManager.cpp` | ~2000 | Texture manager implementation |
| `VulkanTextureBindings.h` | 1-70 | Draw-path and upload-path facade APIs |
| `VulkanTextureId.h` | 1-52 | Strong-typed texture identity (base frame wrapper) |
| `VulkanMovieManager.h` | 1-131 | Movie texture manager definition |
| `VulkanMovieManager.cpp` | ~750 | YCbCr movie texture implementation |
| `VulkanConstants.h` | 1-24 | Bindless slot constants, frames-in-flight |
| `VulkanDeferredRelease.h` | 1-102 | GPU lifetime management (serial-gated destruction) |

---

## Glossary

| Term | Definition |
|------|------------|
| **Base Frame** | The first frame index of a bitmap animation or texture array in bmpman |
| **Bindless** | Descriptor indexing pattern where shaders access textures via array index rather than per-draw bindings |
| **bmpman** | FreeSpace 2's bitmap manager subsystem; handles loading, caching, and lifecycle of image assets |
| **Descriptor** | Vulkan object that binds a resource (buffer, image) to a shader binding point |
| **Eviction** | Removing a texture from the bindless array to make room for another |
| **Immutable Sampler** | Sampler baked into descriptor set layout (required for YCbCr) |
| **Push Descriptor** | Vulkan extension for immediate per-draw descriptor updates without pre-allocation |
| **Resident** | A texture with valid GPU resources (image, memory, view) ready for sampling |
| **Serial** | Monotonic counter tracking GPU command buffer submissions for lifetime management |
| **Staging Buffer** | Host-visible GPU memory used to transfer data from CPU to device-local memory |
| **YCbCr** | Color space used in video encoding; Y=luminance, Cb/Cr=chrominance |

---

*Last updated: Document reflects code state as of the Vulkan rendering backend implementation.*
