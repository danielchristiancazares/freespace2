# Vulkan Pipeline Management

This document defines the architecture, lifecycle, and usage contracts for Vulkan pipeline objects in the FSO renderer. It covers pipeline creation and caching, the division between baked and dynamic state, shader variant selection, vertex input handling, and the rules governing descriptor updates and buffer management.

**Audience**: Developers working on the Vulkan rendering backend or integrating new rendering features.

**Prerequisites**: Familiarity with Vulkan concepts (pipelines, descriptor sets, render passes) and the FSO graphics abstraction layer.

---

## Table of Contents

1. [Pipeline Architecture Overview](#1-pipeline-architecture-overview)
2. [Baked vs Dynamic State Matrix](#2-baked-vs-dynamic-state-matrix)
3. [Pipeline Cache Key Structure](#3-pipeline-cache-key-structure)
4. [Shader Variant Flags](#4-shader-variant-flags)
5. [Vertex Input State Conversion](#5-vertex-input-state-conversion)
6. [Vertex Pulling vs Vertex Attributes](#6-vertex-pulling-vs-vertex-attributes)
7. [Dynamic Rendering Compatibility](#7-dynamic-rendering-compatibility)
8. [Pipeline Lifecycle](#8-pipeline-lifecycle)
9. [Descriptor Update Safety Rules](#9-descriptor-update-safety-rules)
10. [Buffer Orphaning and Descriptor Caching](#10-buffer-orphaning-and-descriptor-caching)
11. [Feature and Extension Requirements](#11-feature-and-extension-requirements)
12. [Failure Modes and Fallbacks](#12-failure-modes-and-fallbacks)
13. [Debugging and Validation](#13-debugging-and-validation)
14. [Related Documents](#14-related-documents)

---

## 1. Pipeline Architecture Overview

The Vulkan renderer uses **dynamic state extensively** to minimize pipeline object count while maintaining correct rendering. Pipelines are cached in-memory and reused based on a composite key that captures all state affecting pipeline identity.

### Key Design Decisions

| Aspect | Approach | Rationale |
|--------|----------|-----------|
| Render Pass | `VK_KHR_dynamic_rendering` | Eliminates `VkRenderPass`/`VkFramebuffer` management overhead |
| State Model | Maximally dynamic | Fewer pipeline variants, late binding of per-draw state |
| Cache Strategy | In-memory only | Disk persistence not yet implemented |
| Warm-up | None (lazy compile) | Pipelines created on first use; may cause hitches |

### Pipeline Layout Kinds

Three distinct pipeline layouts serve different rendering paths. Each layout defines the descriptor set bindings and push constant ranges available to shaders:

| Layout Kind | Use Case | Descriptor Strategy | Source |
|-------------|----------|---------------------|--------|
| `Standard` | 2D, UI, particles, post-process | Push descriptors (set 0) + global set | Most shaders |
| `Model` | 3D model rendering | Bindless textures (set 0) + dynamic UBO (set 1) | `SDR_TYPE_MODEL` only |
| `Deferred` | Deferred lighting pass | Push descriptors + G-buffer global set | `SDR_TYPE_DEFERRED_LIGHTING` only |

### Complete Shader Type to Layout Mapping

The mapping from `shader_type` to pipeline layout and vertex input mode is defined in `VulkanLayoutContracts.cpp`. This table shows the complete assignment:

| Shader Type | Pipeline Layout | Vertex Input | Notes |
|-------------|----------------|--------------|-------|
| `SDR_TYPE_MODEL` | Model | VertexPulling | Only shader using vertex pulling |
| `SDR_TYPE_DEFERRED_LIGHTING` | Deferred | VertexAttributes | Only shader using deferred layout |
| `SDR_TYPE_EFFECT_PARTICLE` | Standard | VertexAttributes | Point sprite particles |
| `SDR_TYPE_EFFECT_DISTORTION` | Standard | VertexAttributes | Heat/distortion effects |
| `SDR_TYPE_POST_PROCESS_MAIN` | Standard | VertexAttributes | Main post-process pass |
| `SDR_TYPE_POST_PROCESS_BLUR` | Standard | VertexAttributes | Bloom blur (H/V variants) |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | Standard | VertexAttributes | Bloom composite |
| `SDR_TYPE_POST_PROCESS_BRIGHTPASS` | Standard | VertexAttributes | Bloom threshold/downsample |
| `SDR_TYPE_POST_PROCESS_FXAA` | Standard | VertexAttributes | FXAA anti-aliasing |
| `SDR_TYPE_POST_PROCESS_FXAA_PREPASS` | Standard | VertexAttributes | FXAA luma prepass |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | Standard | VertexAttributes | God rays |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | Standard | VertexAttributes | HDR to LDR conversion |
| `SDR_TYPE_POST_PROCESS_SMAA_EDGE` | Standard | VertexAttributes | SMAA edge detection |
| `SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT` | Standard | VertexAttributes | SMAA blending weights |
| `SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING` | Standard | VertexAttributes | SMAA final blend |
| `SDR_TYPE_DEFERRED_CLEAR` | Standard | VertexAttributes | G-buffer clear |
| `SDR_TYPE_VIDEO_PROCESS` | Standard | VertexAttributes | Movie playback |
| `SDR_TYPE_PASSTHROUGH_RENDER` | Standard | VertexAttributes | Fixed-function fallback |
| `SDR_TYPE_SHIELD_DECAL` | Standard | VertexAttributes | Shield impact effects |
| `SDR_TYPE_BATCHED_BITMAP` | Standard | VertexAttributes | Batched 2D sprites |
| `SDR_TYPE_DEFAULT_MATERIAL` | Standard | VertexAttributes | Default material fallback |
| `SDR_TYPE_INTERFACE` | Standard | VertexAttributes | 2D UI rendering |
| `SDR_TYPE_NANOVG` | Standard | VertexAttributes | Vector graphics |
| `SDR_TYPE_DECAL` | Standard | VertexAttributes | Projected decals |
| `SDR_TYPE_SCENE_FOG` | Standard | VertexAttributes | Scene fog pass |
| `SDR_TYPE_VOLUMETRIC_FOG` | Standard | VertexAttributes | Volumetric fog |
| `SDR_TYPE_ROCKET_UI` | Standard | VertexAttributes | libRocket UI |
| `SDR_TYPE_COPY` | Standard | VertexAttributes | Texture copy |
| `SDR_TYPE_COPY_WORLD` | Standard | VertexAttributes | World-space copy |
| `SDR_TYPE_MSAA_RESOLVE` | Standard | VertexAttributes | Custom MSAA resolve |
| `SDR_TYPE_ENVMAP_SPHERE_WARP` | Standard | VertexAttributes | Environment map warping |
| `SDR_TYPE_IRRADIANCE_MAP_GEN` | Standard | VertexAttributes | Irradiance map generation |
| `SDR_TYPE_FLAT_COLOR` | Standard | VertexAttributes | Position-only solid color |

### Pipeline Layout Selection

Pipeline layout selection is determined by shader type via the `VulkanLayoutContracts` API:

```cpp
// VulkanLayoutContracts.h
PipelineLayoutKind pipelineLayoutForShader(shader_type type);
bool usesVertexPulling(shader_type type);
const ShaderLayoutSpec& getShaderLayoutSpec(shader_type type);
```

**Key Constraint**: The layout is **baked into the pipeline** at creation time. Binding a pipeline with one layout while using descriptor sets from a different layout results in undefined behavior (typically validation errors or GPU hangs).

**Compatibility Example**: When switching between pipeline families, descriptor sets must be rebound with the matching layout:

```cpp
// Standard pipeline path
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, standardPipeline);
cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,
                         standardPipelineLayout, 0, descriptorWrites);

// Model pipeline path (different layout - must rebind)
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, modelPipeline);
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
                       modelPipelineLayout, 0, modelDescriptorSet,
                       dynamicOffsets);
```

---

## 2. Baked vs Dynamic State Matrix

This section defines which pipeline state is baked at creation time (affecting the cache key) versus set dynamically per-draw (no cache key impact).

### Core Baked State

These values are immutable once the pipeline is created:

| State | Cache Key Field | Notes |
|-------|-----------------|-------|
| Shader modules | `type` + `variant_flags` | Vertex + fragment modules selected by type/variant |
| Pipeline layout | Derived from `type` | Determined by `pipelineLayoutForShader()` |
| Vertex input layout | `layout_hash` | Attribute bindings and formats (unless vertex pulling) |
| Color attachment format | `color_format` | From render target |
| Depth attachment format | `depth_format` | From render target |
| Sample count | `sample_count` | MSAA samples |
| Color attachment count | `color_attachment_count` | Number of color outputs |
| Blend mode | `blend_mode` | See blend mode mapping below |
| Color write mask | `color_write_mask` | RGBA write enable bits |
| Stencil state | Multiple fields | See stencil section below |

### Dynamic State (Vulkan 1.3 Core)

These states are set per-draw via command buffer commands and do not affect pipeline identity:

| State | Command | Default Baked Value |
|-------|---------|---------------------|
| Viewport | `setViewport()` | Count = 1 |
| Scissor | `setScissor()` | Count = 1 |
| Line width | `setLineWidth()` | 1.0 |
| Cull mode | `setCullMode()` | `eBack` |
| Front face | `setFrontFace()` | `eCounterClockwise` |
| Primitive topology | `setPrimitiveTopology()` | `eTriangleList` |
| Depth test enable | `setDepthTestEnable()` | Depends on depth format |
| Depth write enable | `setDepthWriteEnable()` | Depends on depth format |
| Depth compare op | `setDepthCompareOp()` | `eLessOrEqual` |
| Stencil test enable | `setStencilTestEnable()` | From `stencil_test_enable` |

### Extended Dynamic State 3 (Optional Extension)

These features require `VK_EXT_extended_dynamic_state3` and are tracked via `ExtendedDynamicState3Caps`:

| State | Dynamic Command | Fallback if Unavailable |
|-------|-----------------|------------------------|
| Color blend enable | `setColorBlendEnableEXT()` | Baked from `blend_mode` |
| Color write mask | `setColorWriteMaskEXT()` | Baked from `color_write_mask` |
| Polygon mode | `setPolygonModeEXT()` | Baked as `eFill` |
| Rasterization samples | `setRasterizationSamplesEXT()` | Baked from `sample_count` |

The `ExtendedDynamicState3Caps` structure tracks per-feature availability:

```cpp
// VulkanDebug.h
struct ExtendedDynamicState3Caps {
    bool colorBlendEnable = false;
    bool colorWriteMask = false;
    bool polygonMode = false;
    bool rasterizationSamples = false;
};
```

### Blend Mode to Pipeline State Mapping

Blend modes are converted to Vulkan blend attachment state via `buildBlendAttachment()`:

| `gr_alpha_blend` | Enable | Src Color | Dst Color | Src Alpha | Dst Alpha | Op |
|------------------|:------:|-----------|-----------|-----------|-----------|:--:|
| `ALPHA_BLEND_NONE` | No | - | - | - | - | - |
| `ALPHA_BLEND_ADDITIVE` | Yes | `ONE` | `ONE` | `ONE` | `ONE` | `ADD` |
| `ALPHA_BLEND_ALPHA_ADDITIVE` | Yes | `SRC_ALPHA` | `ONE` | `SRC_ALPHA` | `ONE` | `ADD` |
| `ALPHA_BLEND_ALPHA_BLEND_ALPHA` | Yes | `SRC_ALPHA` | `ONE_MINUS_SRC_ALPHA` | `SRC_ALPHA` | `ONE_MINUS_SRC_ALPHA` | `ADD` |
| `ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR` | Yes | `SRC_ALPHA` | `ONE_MINUS_SRC_COLOR` | `SRC_ALPHA` | `ONE_MINUS_SRC_COLOR` | `ADD` |
| `ALPHA_BLEND_PREMULTIPLIED` | Yes | `ONE` | `ONE_MINUS_SRC_ALPHA` | `ONE` | `ONE_MINUS_SRC_ALPHA` | `ADD` |

All blend modes use `VK_BLEND_OP_ADD`. The distinction lies in the blend factors.

### Stencil State

Stencil parameters are baked into the pipeline (all affect the cache key):

| State | Cache Key Field | Notes |
|-------|-----------------|-------|
| Stencil test enable | `stencil_test_enable` | Also available as dynamic state |
| Compare op | `stencil_compare_op` | Applied to both front and back |
| Compare mask | `stencil_compare_mask` | Default: `0xFF` |
| Write mask | `stencil_write_mask` | Default: `0xFF` |
| Reference | `stencil_reference` | Default: `0` |
| Front fail/depth-fail/pass ops | `front_*_op` | Default: `VK_STENCIL_OP_KEEP` |
| Back fail/depth-fail/pass ops | `back_*_op` | Default: `VK_STENCIL_OP_KEEP` |

**Current Usage**: Stencil state is primarily used for shield decals (`SDR_TYPE_SHIELD_DECAL`). Most pipelines use default stencil state (test disabled).

---

## 3. Pipeline Cache Key Structure

The pipeline cache key captures all state that affects pipeline identity. Defined in `VulkanPipelineManager.h`:

```cpp
struct PipelineKey {
    // Shader identity
    shader_type type = SDR_TYPE_NONE;
    uint32_t variant_flags = 0;

    // Render target format
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT;
    uint32_t color_attachment_count = 1;

    // Fixed-function state
    gr_alpha_blend blend_mode = ALPHA_BLEND_NONE;
    size_t layout_hash = 0;  // Hash of vertex_layout (ignored for vertex pulling)
    uint32_t color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Stencil state
    bool stencil_test_enable = false;
    VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;
    uint32_t stencil_compare_mask = 0xFF;
    uint32_t stencil_write_mask = 0xFF;
    uint32_t stencil_reference = 0;
    VkStencilOp front_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp front_depth_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp front_pass_op = VK_STENCIL_OP_KEEP;
    VkStencilOp back_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp back_depth_fail_op = VK_STENCIL_OP_KEEP;
    VkStencilOp back_pass_op = VK_STENCIL_OP_KEEP;
};
```

### Hash Computation

The key is hashed for map lookup using a boost-style hash combine pattern. Each field is combined with a magic constant (`0x9e3779b9`, derived from the golden ratio) to distribute hash values:

```cpp
size_t PipelineKeyHasher::operator()(const PipelineKey& key) const
{
    size_t h = static_cast<size_t>(key.type);
    h ^= static_cast<size_t>(key.variant_flags + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.color_format + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.depth_format + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.sample_count + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.color_attachment_count + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.blend_mode + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.color_write_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.stencil_test_enable + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.stencil_compare_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.stencil_compare_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.stencil_write_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.stencil_reference + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.front_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.front_depth_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.front_pass_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.back_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.back_depth_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<size_t>(key.back_pass_op + 0x9e3779b9 + (h << 6) + (h >> 2));

    // Only include layout_hash for shaders using vertex attributes
    if (!usesVertexPulling(key.type)) {
        h ^= key.layout_hash;
    }
    return h;
}
```

### Key Equality

The `operator==` implementation mirrors the hash, with special handling for vertex pulling shaders:

```cpp
bool PipelineKey::operator==(const PipelineKey& other) const
{
    if (type != other.type) return false;

    const bool ignoreLayout = usesVertexPulling(type);

    return variant_flags == other.variant_flags &&
           color_format == other.color_format &&
           depth_format == other.depth_format &&
           sample_count == other.sample_count &&
           color_attachment_count == other.color_attachment_count &&
           blend_mode == other.blend_mode &&
           color_write_mask == other.color_write_mask &&
           stencil_test_enable == other.stencil_test_enable &&
           stencil_compare_op == other.stencil_compare_op &&
           // ... all stencil fields ...
           (ignoreLayout || layout_hash == other.layout_hash);
}
```

---

## 4. Shader Variant Flags

Shader variant flags allow a single shader type to have multiple SPIR-V implementations selected at runtime. Variants are encoded in `PipelineKey::variant_flags` and affect shader module file selection.

### Variant Flag Definitions

Variant flags are defined in `code/graphics/2d.h`:

| Shader Type | Flag | Value | Purpose |
|-------------|------|-------|---------|
| `SDR_TYPE_EFFECT_PARTICLE` | `SDR_FLAG_PARTICLE_POINT_GEN` | `1<<0` | Point sprite generation |
| `SDR_TYPE_POST_PROCESS_BLUR` | `SDR_FLAG_BLUR_HORIZONTAL` | `1<<0` | Horizontal blur pass |
| `SDR_TYPE_POST_PROCESS_BLUR` | `SDR_FLAG_BLUR_VERTICAL` | `1<<1` | Vertical blur pass |
| `SDR_TYPE_NANOVG` | `SDR_FLAG_NANOVG_EDGE_AA` | `1<<0` | Edge anti-aliasing |
| `SDR_TYPE_DECAL` | `SDR_FLAG_DECAL_USE_NORMAL_MAP` | `1<<0` | Normal map enabled |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_4` | `1<<0` | 4x MSAA resolve |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_8` | `1<<1` | 8x MSAA resolve |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_16` | `1<<2` | 16x MSAA resolve |
| `SDR_TYPE_VOLUMETRIC_FOG` | `SDR_FLAG_VOLUMETRICS_DO_EDGE_SMOOTHING` | `1<<0` | Edge smoothing |
| `SDR_TYPE_VOLUMETRIC_FOG` | `SDR_FLAG_VOLUMETRICS_NOISE` | `1<<1` | Noise enabled |
| `SDR_TYPE_COPY` | `SDR_FLAG_COPY_FROM_ARRAY` | `1<<0` | Copy from texture array |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | `SDR_FLAG_TONEMAPPING_LINEAR_OUT` | `1<<0` | Linear output (skip sRGB) |
| `SDR_TYPE_ENVMAP_SPHERE_WARP` | `SDR_FLAG_ENV_MAP` | `1<<0` | Environment map enabled |

### Shader Module Selection

Variant flags affect SPIR-V filename selection in `VulkanShaderManager::getModules()`:

```cpp
case shader_type::SDR_TYPE_POST_PROCESS_BLUR:
    const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
    const auto fragPath =
        (variantFlags & SDR_FLAG_BLUR_HORIZONTAL)
            ? (fs::path(m_shaderRoot) / "blur_h.frag.spv")
            : (fs::path(m_shaderRoot) / "blur_v.frag.spv");
    break;
```

### Shader Module Caching

Shader modules are cached separately from pipelines by `(shader_type, variantFlags)` key:

```cpp
// VulkanShaderManager.h
struct Key {
    shader_type type;
    uint32_t flags;
    bool operator==(const Key& other) const;
};

std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_vertexModules;
std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_fragmentModules;
```

Different variant flags create different cached modules and different pipelines (since `variant_flags` is part of `PipelineKey`).

### Filename-Based Module Loading

For Vulkan-only shaders that do not map to a `shader_type` (e.g., movie playback), use the filename-based API:

```cpp
ShaderModules modules = shaderManager->getModulesByFilenames(
    "movie.vert.spv", "movie.frag.spv");
```

### Variant Flag Usage Pattern

```cpp
// Create pipeline key with variant flags
PipelineKey key{};
key.type = SDR_TYPE_POST_PROCESS_BLUR;
key.variant_flags = SDR_FLAG_BLUR_HORIZONTAL;
key.color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
// ... other fields ...

// Get shader modules (selects blur_h.frag.spv based on variant flag)
ShaderModules modules = shaderManager->getModules(key.type, key.variant_flags);

// Get or create pipeline (cached by full key including variant_flags)
vk::Pipeline pipeline = pipelineManager->getPipeline(key, modules, layout);
```

---

## 5. Vertex Input State Conversion

The renderer converts `vertex_layout` (engine abstraction) to Vulkan vertex input state structures.

### Location Mapping

Vertex attribute locations follow the OpenGL convention established by the engine:

| Location | Attribute | Default Format |
|----------|-----------|----------------|
| 0 | `POSITION` | `R32G32B32_SFLOAT` (3D) or `R32G32_SFLOAT` (2D) |
| 1 | `COLOR` | `R8G8B8A8_UNORM` or `R32G32B32A32_SFLOAT` |
| 2 | `TEXCOORD` | `R32G32_SFLOAT` |
| 3 | `NORMAL` | `R32G32B32_SFLOAT` |
| 4 | `TANGENT` | `R32G32B32A32_SFLOAT` |
| 5 | `MODEL_ID` | `R32_SFLOAT` |
| 6 | `RADIUS` | `R32_SFLOAT` |
| 7 | `UVEC` | `R32G32B32_SFLOAT` |
| 8-11 | `MATRIX4` | `R32G32B32A32_SFLOAT` (4 vec4s) |

**Location Gaps**: Vulkan allows gaps in vertex attribute locations. A layout with position (0) and texcoord (2) but no color (1) is valid. The shader simply will not receive data for unused locations.

### Format Mapping

The `vertex_format_data` enum maps to Vulkan formats via `VERTEX_FORMAT_MAP`:

| `vertex_format_data` | Vulkan Format | Component Count |
|---------------------|---------------|-----------------|
| `POSITION4` | `R32G32B32A32_SFLOAT` | 4 |
| `POSITION3` | `R32G32B32_SFLOAT` | 3 |
| `POSITION2` | `R32G32_SFLOAT` | 2 |
| `SCREEN_POS` | `R32G32_SFLOAT` | 2 |
| `COLOR3` | `R8G8B8_UNORM` | 3 |
| `COLOR4` | `R8G8B8A8_UNORM` | 4 |
| `COLOR4F` | `R32G32B32A32_SFLOAT` | 4 |
| `TEX_COORD2` | `R32G32_SFLOAT` | 2 |
| `TEX_COORD4` | `R32G32B32A32_SFLOAT` | 4 |
| `NORMAL` | `R32G32B32_SFLOAT` | 3 |
| `TANGENT` | `R32G32B32A32_SFLOAT` | 4 |
| `MODEL_ID` | `R32_SFLOAT` | 1 |
| `RADIUS` | `R32_SFLOAT` | 1 |
| `UVEC` | `R32G32B32_SFLOAT` | 3 |
| `MATRIX4` | `R32G32B32A32_SFLOAT` | 4 (spans locations 8-11) |

### Special Case: MATRIX4

A `MATRIX4` vertex format spans 4 consecutive locations (8-11), each with format `R32G32B32A32_SFLOAT`. The conversion creates 4 separate attribute descriptions with 16-byte offsets:

```cpp
if (component->format_type == vertex_format_data::MATRIX4) {
    for (uint32_t row = 0; row < 4; ++row) {
        vk::VertexInputAttributeDescription attr{};
        attr.binding = static_cast<uint32_t>(component->buffer_number);
        attr.location = 8 + row;  // Locations 8, 9, 10, 11
        attr.format = vk::Format::eR32G32B32A32Sfloat;
        attr.offset = static_cast<uint32_t>(component->offset + row * 16);
        result.attributes.push_back(attr);
    }
}
```

### Vertex Attribute Divisors

If a `vertex_layout` component specifies a divisor (for instanced rendering), it is converted to `VkVertexInputBindingDivisorDescription`:

- **Divisor = 1**: Core Vulkan; binding uses `VK_VERTEX_INPUT_RATE_INSTANCE`
- **Divisor > 1**: Requires `VK_EXT_vertex_attribute_divisor`; creates a divisor description

```cpp
if (comp->divisor != 0) {
    binding.inputRate = vk::VertexInputRate::eInstance;
    if (comp->divisor > 1) {
        divisorsByBinding[binding.binding] = static_cast<uint32_t>(comp->divisor);
    }
}
```

If divisor > 1 is requested but `vertexAttributeInstanceRateDivisor` is not enabled, pipeline creation throws an exception.

### Conversion Function and Caching

```cpp
// Converts engine vertex_layout to Vulkan structures
VertexInputState convertVertexLayoutToVulkan(const vertex_layout& layout);

// Cached by layout hash in VulkanPipelineManager
const VertexInputState& getVertexInputState(const vertex_layout& layout);
```

Converted vertex input states are cached by `layout.hash()` in `m_vertexInputCache` to avoid re-converting the same layout.

### Validation

Pipeline creation validates that vertex attribute shaders have Location 0 (position):

```cpp
if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
    bool hasLoc0 = false;
    for (const auto& a : vertexInputState.attributes) {
        if (a.location == 0) hasLoc0 = true;
    }
    Assertion(hasLoc0, "Vertex input pipeline created without Location 0 attribute");
}
```

---

## 6. Vertex Pulling vs Vertex Attributes

The renderer supports two vertex input modes, selected by shader type:

| Mode | Usage | Vertex Attributes | `layout_hash` in Key |
|------|-------|-------------------|---------------------|
| **VertexAttributes** | Most shaders | Traditional `vkCmdBindVertexBuffers()` | Required, must match |
| **VertexPulling** | `SDR_TYPE_MODEL` only | None; shader fetches from storage buffers | Ignored |

### Vertex Attributes Mode

**Mechanism**:
- Vertex data provided via `vkCmdBindVertexBuffers()`
- Vulkan hardware fetches vertex attributes based on pipeline's vertex input state
- Shader declares `layout(location = N) in vec3 position;`

**Pipeline Key**: `layout_hash` must match the provided `vertex_layout`. Mismatch triggers an exception.

**Usage**: All shaders except `SDR_TYPE_MODEL`.

### Vertex Pulling Mode

**Mechanism**:
- Pipeline has empty vertex input state (no attributes)
- Shader fetches vertex data from storage buffers using `gl_VertexIndex`
- Vertex data uploaded to a large vertex heap buffer

**Pipeline Key**: `layout_hash` is ignored in both hash computation and equality comparison.

**Usage**: Only `SDR_TYPE_MODEL`.

**Rationale**:
- Model rendering uses a large vertex heap buffer with variable layouts per model
- Vertex pulling allows fetching arbitrary vertex formats without pipeline variants
- Significantly reduces pipeline count (one model pipeline per format combination)

### Mode Selection

```cpp
bool usesVertexPulling(shader_type type);
VertexInputMode mode = getShaderLayoutSpec(type).vertexInput;
```

### Pipeline Creation Logic

```cpp
if (useVertexPulling) {
    // No vertex attributes; data fetched from storage buffer in shader
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.pVertexBindingDescriptions = nullptr;
    vertexInput.vertexAttributeDescriptionCount = 0;
    vertexInput.pVertexAttributeDescriptions = nullptr;
} else {
    // Traditional vertex attributes from vertex_layout
    const VertexInputState& vertexState = getVertexInputState(layout);
    vertexInput.vertexBindingDescriptionCount = vertexState.bindings.size();
    vertexInput.pVertexBindingDescriptions = vertexState.bindings.data();
    // ... attributes and divisors ...
}
```

---

## 7. Dynamic Rendering Compatibility

The renderer uses `VK_KHR_dynamic_rendering` (core in Vulkan 1.3), eliminating traditional `VkRenderPass` and `VkFramebuffer` objects. Pipeline compatibility is determined by matching `VkPipelineRenderingCreateInfo` at bind time.

### Compatibility Contract

A pipeline is compatible with a rendering session if **all** of these match:

| Property | Pipeline Value | Rendering Info Value |
|----------|----------------|---------------------|
| Color format(s) | `pColorAttachmentFormats[]` | `VkRenderingAttachmentInfo::imageView` format |
| Depth format | `depthAttachmentFormat` | Depth attachment imageView format |
| Stencil format | `stencilAttachmentFormat` | Same as depth if combined format |
| Color attachment count | `colorAttachmentCount` | Number of `pColorAttachments` |
| Sample count | `rasterizationSamples` | Attachment image sample count |

### Stencil Format Handling

The stencil format is automatically derived from the depth format when a combined depth-stencil format is used:

```cpp
vk::Format depthFormat = static_cast<vk::Format>(key.depth_format);
renderingInfo.depthAttachmentFormat = depthFormat;
renderingInfo.stencilAttachmentFormat =
    formatHasStencil(depthFormat) ? depthFormat : vk::Format::eUndefined;

// Helper function
static bool formatHasStencil(vk::Format format) {
    switch (format) {
    case vk::Format::eD32SfloatS8Uint:
    case vk::Format::eD24UnormS8Uint:
        return true;
    default:
        return false;
    }
}
```

### Target Type to Format Mapping

| Target Type | Color Format | Depth Format | Sample Count | Attachments |
|-------------|--------------|--------------|:------------:|:-----------:|
| Swapchain | `B8G8R8A8_SRGB` (preferred) | `D32_SFLOAT_S8_UINT`* | 1 | 1 |
| Scene HDR | `R16G16B16A16_SFLOAT` | `D32_SFLOAT_S8_UINT`* | 1 | 1 |
| G-buffer | `R16G16B16A16_SFLOAT` (x5) | `D32_SFLOAT_S8_UINT`* | 1 | 5 |
| Cockpit Texture | `B8G8R8A8_UNORM` | None | 1 | 1 |
| Bloom MIP | `R16G16B16A16_SFLOAT` | None | 1 | 1 |
| LDR Post | `B8G8R8A8_UNORM` | None | 1 | 1 |

*Depth format is selected from candidates in order: `D32_SFLOAT_S8_UINT`, `D24_UNORM_S8_UINT`, `D32_SFLOAT` based on device support.

### Shader Type and Target Restrictions

Some shader types are only valid against specific render targets:

| Shader Type | Valid Targets | Notes |
|-------------|---------------|-------|
| `SDR_TYPE_MODEL` | G-buffer | Outputs to all 5 G-buffer attachments |
| `SDR_TYPE_DEFERRED_LIGHTING` | Scene HDR | Reads G-buffer, writes lit scene |
| `SDR_TYPE_INTERFACE` | Swapchain, Cockpit | 2D overlay rendering |
| `SDR_TYPE_POST_PROCESS_BRIGHTPASS` | Bloom MIP | Downsample + threshold |
| `SDR_TYPE_POST_PROCESS_BLUR` | Bloom MIP | Horizontal/vertical blur |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | Scene HDR | Composite bloom into scene |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | Swapchain | HDR to LDR conversion |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | Swapchain | God rays overlay |

---

## 8. Pipeline Lifecycle

### Creation Flow

Pipelines are created **lazily on first use**:

```
Draw call requested
    |
    v
Compute pipeline key from current state
    |-- shader_type, variant_flags
    |-- render target formats (color, depth)
    |-- blend_mode, color_write_mask
    |-- stencil state
    '-- vertex_layout_hash (if VertexAttributes mode)
    |
    v
Validate key (for VertexAttributes shaders)
    |-- Assert layout_hash matches layout.hash()
    '-- Throw if mismatch
    |
    v
Check cache: key -> VkPipeline
    |
    +-- [Cache Hit] --> Return existing pipeline
    |
    '-- [Cache Miss] --> Create new pipeline
                            |-- Get shader modules (cached by type + variant)
                            |-- Get vertex input state (cached by layout hash)
                            |-- Build VkGraphicsPipelineCreateInfo
                            |   |-- Shader stages
                            |   |-- Vertex input (empty if VertexPulling)
                            |   |-- VkPipelineRenderingCreateInfo
                            |   |-- Blend state from blend_mode
                            |   |-- Dynamic state list
                            |   '-- Pipeline layout from shader type
                            |-- Call vkCreateGraphicsPipelines()
                            '-- Store in cache
    |
    v
Bind pipeline and issue draw
```

### Pipeline Key Validation

Before cache lookup, the pipeline key is validated for vertex attribute shaders:

```cpp
vk::Pipeline VulkanPipelineManager::getPipeline(
    const PipelineKey& key,
    const ShaderModules& modules,
    const vertex_layout& layout)
{
    PipelineKey adjustedKey = key;
    const auto& layoutSpec = getShaderLayoutSpec(key.type);

    if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
        const size_t expectedHash = layout.hash();
        if (adjustedKey.layout_hash != expectedHash) {
            adjustedKey.layout_hash = expectedHash;
            throw std::runtime_error(
                "PipelineKey.layout_hash mismatches provided vertex_layout");
        }
    }

    auto it = m_pipelines.find(key);
    if (it != m_pipelines.end()) {
        return it->second.get();
    }

    auto pipeline = createPipeline(adjustedKey, modules, layout);
    // ... cache and return ...
}
```

### Dynamic State List Construction

The dynamic state list is built based on extension support:

```cpp
std::vector<vk::DynamicState> BuildDynamicStateList(
    bool supportsExtendedDynamicState3,
    const ExtendedDynamicState3Caps& caps)
{
    // Core Vulkan 1.3 dynamic states (always available)
    std::vector<vk::DynamicState> states = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
        vk::DynamicState::eLineWidth,
        vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,
        vk::DynamicState::ePrimitiveTopology,
        vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,
        vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eStencilTestEnable,
    };

    // Extended Dynamic State 3 (optional)
    if (supportsExtendedDynamicState3) {
        if (caps.colorBlendEnable)
            states.push_back(vk::DynamicState::eColorBlendEnableEXT);
        if (caps.colorWriteMask)
            states.push_back(vk::DynamicState::eColorWriteMaskEXT);
        if (caps.polygonMode)
            states.push_back(vk::DynamicState::ePolygonModeEXT);
        if (caps.rasterizationSamples)
            states.push_back(vk::DynamicState::eRasterizationSamplesEXT);
    }

    return states;
}
```

### Thread Safety

Pipeline creation occurs on the **main rendering thread only**. No synchronization is required for the cache.

### Cache Invalidation

The pipeline cache is **never invalidated** during normal operation. It persists for the lifetime of the renderer.

Pipelines are destroyed only at:
- Renderer shutdown
- Device lost recovery (full recreation)

### No Disk Persistence

The current implementation does not persist `VkPipelineCache` to disk. All pipelines are recompiled on each application launch.

**Implication**: First use of each shader/target combination incurs shader compilation latency.

**Future Improvement**: Implement `vkGetPipelineCacheData()` / `vkCreatePipelineCache()` with file persistence.

---

## 9. Descriptor Update Safety Rules

Descriptor set updates must respect GPU synchronization to avoid data races.

### The Core Rule

> **A descriptor set must not be updated while any in-flight command buffer references it.**

### Safe Update Conditions

| Scenario | Safe? | Reason |
|----------|:-----:|--------|
| Update before any command buffer submission | Yes | No references exist |
| Update after `vkQueueWaitIdle()` | Yes | All work complete |
| Update after fence wait for specific frame | Yes | Frame's work complete |
| Update between frames with per-frame sets | Yes | Set not in flight |
| Update to set currently being recorded | Yes | Recording != executing |
| Update to set in submitted command buffer | **No** | Data race |

### Per-Frame Descriptor Sets

The renderer uses per-frame model descriptor sets (`kFramesInFlight = 2`):

```
Frame 0: modelDescriptorSet[0] -- Safe to update when frame 0 fence signaled
Frame 1: modelDescriptorSet[1] -- Safe to update when frame 1 fence signaled
```

**Update Timing**: Sets are updated at frame start, after waiting on the previous use of that frame slot:

```cpp
void prepareFrameForReuse(VulkanFrame& frame)
{
    frame.wait_for_gpu();  // Fence wait - GPU done with this frame
    // NOW safe to update frame's descriptor set
}
```

### Push Descriptors

Push descriptors (`vkCmdPushDescriptorSetKHR`) are inherently safe because they do not reference a persistent descriptor set. Data is embedded directly in the command buffer.

### Global Descriptor Set

The global set (G-buffer textures for deferred lighting) is shared across all frames. Updates occur:
- Once during deferred lighting setup
- Only when render targets change (window resize, etc.)

**Safety**: Updates occur before any command buffer referencing the new values is submitted.

---

## 10. Buffer Orphaning and Descriptor Caching

Buffer orphaning can replace the underlying `VkBuffer` while descriptors cache the old buffer reference. This must be coordinated correctly.

### The Problem

Buffer orphaning replaces the `VkBuffer` when resizing:

```cpp
void resizeBuffer(handle, newSize)
{
    // Old buffer queued for deferred destruction
    m_deferredReleases.enqueue(retireSerial, [oldBuf]() { destroy(oldBuf); });

    // NEW VkBuffer created
    buffer.buffer = createNewBuffer(newSize);
}
```

Descriptor caching may skip updates if only checking handle equality:

```cpp
// WRONG: VkBuffer may have changed even if handle is the same
if (frame.uniformBinding.bufferHandle == handle) {
    return;  // Skip descriptor update
}
```

### The Invariant

> **Descriptor writes must be keyed on VkBuffer identity, not just handle equality.**

### Implementation

The renderer tracks uniform bindings per-frame via `DynamicUniformBinding`:

```cpp
// VulkanFrame.h
struct DynamicUniformBinding {
    gr_buffer_handle bufferHandle{};
    uint32_t dynamicOffset = 0;
};

// Per-frame binding state
DynamicUniformBinding modelUniformBinding{ gr_buffer_handle::invalid(), 0 };
DynamicUniformBinding sceneUniformBinding{ gr_buffer_handle::invalid(), 0 };
```

Buffer orphaning is handled by the deferred release system: the old buffer remains valid until the GPU has finished using it, coordinated via timeline semaphores.

### Frame Isolation

Each frame has its own `DynamicUniformBinding` state. Buffer orphaning on frame N does not affect frame N-1's cached bindings, which reference the old buffer that remains valid via deferred release until that frame completes.

### Descriptor Update Flow

```cpp
void setModelUniformBinding(VulkanFrame& frame, gr_buffer_handle handle, ...)
{
    // Update descriptor if handle changed (buffer recreation forces handle change)
    if (frame.modelUniformBinding.bufferHandle != handle) {
        vk::DescriptorBufferInfo info{};
        info.buffer = resolveBuffer(handle);
        info.offset = 0;
        info.range = VK_WHOLE_SIZE;

        // Write descriptor
        m_device.updateDescriptorSets(1, &write, 0, nullptr);
    }

    // Track binding state
    frame.modelUniformBinding = DynamicUniformBinding{ handle, dynOffset };
}
```

---

## 11. Feature and Extension Requirements

### Required (Vulkan 1.3 Core)

| Feature/Extension | Version | Purpose |
|-------------------|---------|---------|
| Vulkan 1.3 | Core | Base requirement |
| `VK_KHR_dynamic_rendering` | Core 1.3 | No render pass objects |
| `VK_KHR_synchronization2` | Core 1.3 | 64-bit stage/access masks |
| `VK_EXT_extended_dynamic_state` | Core 1.3 | Cull mode, front face, depth test, etc. |

### Required Features (Hard Checks)

| Feature | Source | Failure Mode |
|---------|--------|--------------|
| `dynamicRendering` | Vulkan 1.3 | Constructor throws exception |
| `pushDescriptor` | Vulkan 1.4 | Throws exception |
| `shaderSampledImageArrayNonUniformIndexing` | Descriptor indexing | Throws exception |
| `runtimeDescriptorArray` | Descriptor indexing | Throws exception |
| `maxDescriptorSetSampledImages >= 1024` | Device limit | Assertion failure |

### Optional (Graceful Degradation)

| Feature | Fallback Behavior |
|---------|-------------------|
| `VK_EXT_extended_dynamic_state3` | Blend/polygon state baked into pipeline |
| `VK_EXT_vertex_attribute_divisor` | Divisor > 1 throws; divisor == 1 uses core |
| `samplerYcbcrConversion` | Movie playback disabled |
| `G8_B8_R8_3PLANE_420_UNORM` format | Movie playback disabled |

---

## 12. Failure Modes and Fallbacks

### Pipeline Creation Failure

If `vkCreateGraphicsPipelines` fails:

```cpp
auto pipelineResult = m_device.createGraphicsPipelineUnique(
    m_pipelineCache, pipelineInfo);
if (pipelineResult.result != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to create Vulkan graphics pipeline.");
}
```

**Current behavior**: Fatal error via exception. No fallback shader system exists.

**Future improvement**: Implement fallback to a simple solid-color shader for graceful degradation.

### Shader Module Load Failure

If SPIR-V module cannot be loaded:

```cpp
std::ifstream file(path, std::ios::binary | std::ios::ate);
if (!file) {
    throw std::runtime_error("Failed to open shader module " + path);
}
```

**Current behavior**: Fatal error via exception.

### Vertex Attribute Divisor Not Supported

If `divisor > 1` is requested but `vertexAttributeInstanceRateDivisor` is not enabled:

```cpp
if (!vertexInputState.divisors.empty() && !m_supportsVertexAttributeDivisor) {
    throw std::runtime_error(
        "vertexAttributeInstanceRateDivisor not enabled but divisor > 1 requested");
}
```

### Invalid Pipeline Key

If `color_attachment_count` is zero:

```cpp
if (key.color_attachment_count == 0) {
    throw std::runtime_error("PipelineKey.color_attachment_count must be at least 1.");
}
```

### Descriptor Pool Exhaustion

The model pool is sized for exactly `kFramesInFlight` sets. Exhaustion indicates a logic error:

```cpp
poolInfo.maxSets = kFramesInFlight; // 2
```

---

## 13. Debugging and Validation

### RenderDoc Integration

Debug regions are inserted for GPU capture correlation:

```cpp
cmd.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{"Model Rendering"});
// ... draw calls ...
cmd.endDebugUtilsLabelEXT();
```

### Validation Layer Messages

Common validation errors related to pipelines and their causes:

| Error Message | Cause | Resolution |
|---------------|-------|------------|
| "Pipeline not compatible with render pass" | Format mismatch between pipeline and rendering info | Verify `color_format`/`depth_format` in pipeline key matches render target |
| "Descriptor set not bound" | Missing `bindDescriptorSets()` call | Ensure descriptor set bound before draw |
| "Dynamic state not set" | Missing `set*()` call for dynamic state | Set all required dynamic state before draw |
| "Push constants not provided" | Pipeline layout expects push constants | Call `pushConstants()` before draw |
| "Vertex attribute location N not provided" | Shader expects attribute not in vertex layout | Verify vertex layout has all required attributes |
| "Color attachment count mismatch" | Pipeline and rendering info have different attachment counts | Match `color_attachment_count` to render target |

### Debugging Pipeline Cache Misses

To identify unexpected pipeline variants, add logging to `createPipeline()`:

```cpp
mprintf(("Creating pipeline: type=%d variant=0x%x color=%d depth=%d blend=%d\n",
    key.type, key.variant_flags, key.color_format, key.depth_format, key.blend_mode));
```

### Inspecting Pipeline State in RenderDoc

1. Capture a frame with RenderDoc
2. Select a draw call
3. View "Pipeline State" panel
4. Check:
   - **Vertex Input**: Binding strides, attribute formats/locations
   - **Rasterizer**: Cull mode, front face (dynamic state values)
   - **Depth/Stencil**: Test/write enables, compare ops
   - **Blend**: Per-attachment blend factors and ops

### Common Issues

**Symptom**: Black or missing geometry
- **Cause**: Wrong pipeline bound (format mismatch)
- **Check**: Validate pipeline key matches current render target

**Symptom**: Z-fighting or depth issues
- **Cause**: Depth state incorrect
- **Check**: Verify `setDepthTestEnable()` and `setDepthCompareOp()` called

**Symptom**: Objects render with wrong blend
- **Cause**: `blend_mode` in key doesn't match intended blending
- **Check**: Trace `gr_alpha_blend` value propagation

---

## 14. Related Documents

- `VULKAN_DESCRIPTOR_SETS.md` - Descriptor set layouts, pools, and update patterns
- `VULKAN_SYNCHRONIZATION.md` - Timeline semaphores, fences, and frame pacing
- `VULKAN_MODEL_RENDERING_PIPELINE.md` - Model rendering data flow and uniform binding
- `VULKAN_DYNAMIC_BUFFERS.md` - Ring buffer allocation and dynamic offsets
- `VULKAN_ERROR_HANDLING.md` - Exception handling and recovery strategies
- `VULKAN_HUD_RENDERING.md` - 2D overlay rendering with Standard layout

---

## Appendix: Quick Reference

### Pipeline Manager Constructor

```cpp
VulkanPipelineManager(
    vk::Device device,
    vk::PipelineLayout pipelineLayout,        // Standard layout
    vk::PipelineLayout modelPipelineLayout,   // Model layout
    vk::PipelineLayout deferredPipelineLayout,// Deferred layout
    vk::PipelineCache pipelineCache,
    bool supportsExtendedDynamicState,
    bool supportsExtendedDynamicState2,
    bool supportsExtendedDynamicState3,
    const ExtendedDynamicState3Caps& extDyn3Caps,
    bool supportsVertexAttributeDivisor,
    bool dynamicRenderingEnabled);
```

### Pipeline Acquisition

```cpp
vk::Pipeline getPipeline(
    const PipelineKey& key,
    const ShaderModules& modules,
    const vertex_layout& layout);
```

### Layout Contract Query

```cpp
bool usesVertexPulling(shader_type type);
PipelineLayoutKind pipelineLayoutForShader(shader_type type);
const ShaderLayoutSpec& getShaderLayoutSpec(shader_type type);
```
