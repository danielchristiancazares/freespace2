# Vulkan Pipeline Management

This document covers pipeline object lifecycle, caching strategy, state management, and the contracts that ensure correct pipeline-target compatibility.

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

---

## 1. Pipeline Architecture Overview

The Vulkan renderer uses **dynamic state extensively** to minimize pipeline object count while maintaining correct rendering. Pipelines are cached in-memory and reused based on a composite key.

### Key Design Decisions

| Aspect | Approach | Rationale |
|--------|----------|-----------|
| Render Pass | `VK_KHR_dynamic_rendering` | No VkRenderPass/VkFramebuffer management |
| State Model | Maximally dynamic | Fewer pipeline variants, late binding |
| Cache Strategy | In-memory only | Disk persistence not implemented |
| Warm-up | None (lazy compile) | Pipelines created on first use |

### Pipeline Layout Kinds

Three distinct pipeline layouts serve different rendering paths:

| Layout Kind | Use Case | Descriptor Strategy |
|-------------|----------|---------------------|
| `Standard` | 2D, UI, particles, post-process | Push descriptors + global set |
| `Model` | 3D model rendering | Bindless textures + push constants |
| `Deferred` | Deferred lighting | Push descriptors + G-buffer set |

### Pipeline Layout Selection

Pipeline layout selection is **determined by shader type** via `VulkanLayoutContracts`:

```cpp
PipelineLayoutKind pipelineLayoutForShader(shader_type type);
```

**Selection Rules**:

| Shader Type | Pipeline Layout | Rationale |
|-------------|----------------|-----------|
| `SDR_TYPE_MODEL` | `Model` | Requires bindless texture array (set 0) + dynamic uniform buffer (set 1) |
| `SDR_TYPE_DEFERRED_LIGHTING` | `Deferred` | Requires G-buffer textures (set 0) + per-light push descriptors |
| All others | `Standard` | Uses push descriptors for per-draw uniforms |

**Key Point**: The layout is **baked into the pipeline** at creation time. You cannot bind a pipeline with one layout and use descriptor sets from another layout - they are incompatible.

**Compatibility**: When switching between pipeline families (e.g., Standard → Model), you must rebind descriptor sets using the matching pipeline layout:

```cpp
// Standard pipeline
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, standardPipeline);
cmd.pushDescriptorSetKHR(standardPipelineLayout, 0, ...);

// Model pipeline (different layout!)
cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, modelPipeline);
cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, modelPipelineLayout, ...);
```

---

## 2. Baked vs Dynamic State Matrix

This table defines which state is baked into the pipeline (affects cache key) versus set dynamically per-draw (does not affect cache key).

### Core State

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Shader modules** | ✓ | - | ✓ | Vertex + fragment modules |
| **Pipeline layout** | ✓ | - | ✓ | Determined by shader type |
| **Vertex input layout** | ✓ | - | ✓ | Attribute bindings and formats |
| **Color attachment format** | ✓ | - | ✓ | From render target |
| **Depth attachment format** | ✓ | - | ✓ | From render target |
| **Sample count** | ✓ | - | ✓ | MSAA samples |
| **Color attachment count** | ✓ | - | ✓ | Number of color attachments |

### Dynamic State (Extended Dynamic State 1/2)

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Viewport** | - | ✓ | - | `setViewport()` per draw |
| **Scissor** | - | ✓ | - | `setScissor()` per draw |
| **Line width** | - | ✓ | - | `setLineWidth()` per draw |
| **Cull mode** | - | ✓ | - | `setCullMode()` per draw |
| **Front face** | - | ✓ | - | `setFrontFace()` per draw |
| **Primitive topology** | - | ✓ | - | `setPrimitiveTopology()` per draw |
| **Depth test enable** | - | ✓ | - | `setDepthTestEnable()` per draw |
| **Depth write enable** | - | ✓ | - | `setDepthWriteEnable()` per draw |
| **Depth compare op** | - | ✓ | - | `setDepthCompareOp()` per draw |
| **Stencil test enable** | - | ✓ | - | `setStencilTestEnable()` per draw |

### Extended Dynamic State 3 (Optional)

These features are **extension-only** (not core in Vulkan 1.3) and tracked via `ExtendedDynamicState3Caps`:

| State | Baked | Dynamic | Cache Key | Fallback if Unavailable |
|-------|:-----:|:-------:|:---------:|------------------------|
| **Color blend enable** | ✓* | ✓* | ✓* | Baked into pipeline |
| **Color write mask** | ✓* | ✓* | ✓* | Baked into pipeline |
| **Polygon mode** | ✓* | ✓* | ✓* | Baked into pipeline |
| **Rasterization samples** | ✓ | - | ✓ | Always baked |

*Conditional: Dynamic if extension supported, otherwise baked.

### Alpha Blending

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Blend mode** | ✓ | - | ✓ | `gr_alpha_blend` enum value |
| **Blend factors** | ✓ | - | ✓ | Derived from blend mode |
| **Blend op** | ✓ | - | ✓ | Derived from blend mode |

### Blend Mode to Pipeline State Mapping

Blend modes are converted to Vulkan pipeline state via `buildBlendAttachment()`:

| `gr_alpha_blend` | Blend Enable | Src Color | Dst Color | Color Op | Src Alpha | Dst Alpha | Alpha Op |
|------------------|:------------:|-----------|-----------|----------|-----------|-----------|----------|
| `ALPHA_BLEND_NONE` | `VK_FALSE` | - | - | - | - | - | - |
| `ALPHA_BLEND_ADDITIVE` | `VK_TRUE` | `ONE` | `ONE` | `ADD` | `ONE` | `ONE` | `ADD` |
| `ALPHA_BLEND_ALPHA_ADDITIVE` | `VK_TRUE` | `SRC_ALPHA` | `ONE` | `ADD` | `SRC_ALPHA` | `ONE` | `ADD` |
| `ALPHA_BLEND_ALPHA_BLEND_ALPHA` | `VK_TRUE` | `SRC_ALPHA` | `ONE_MINUS_SRC_ALPHA` | `ADD` | `SRC_ALPHA` | `ONE_MINUS_SRC_ALPHA` | `ADD` |
| `ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR` | `VK_TRUE` | `SRC_ALPHA` | `ONE_MINUS_SRC_COLOR` | `ADD` | `SRC_ALPHA` | `ONE_MINUS_SRC_COLOR` | `ADD` |
| `ALPHA_BLEND_PREMULTIPLIED` | `VK_TRUE` | `ONE` | `ONE_MINUS_SRC_ALPHA` | `ADD` | `ONE` | `ONE_MINUS_SRC_ALPHA` | `ADD` |

**Note**: All blend modes use `VK_BLEND_OP_ADD`. The distinction is in the blend factors.

### Stencil State

Stencil state is **baked into the pipeline** (affects cache key):

| State | Baked | Dynamic | Cache Key | Notes |
|-------|:-----:|:-------:|:---------:|-------|
| **Stencil test enable** | ✓ | ✓* | ✓ | Dynamic if Extended Dynamic State supported |
| **Stencil compare op** | ✓ | - | ✓ | Always baked |
| **Stencil compare mask** | ✓ | - | ✓ | Always baked |
| **Stencil write mask** | ✓ | - | ✓ | Always baked |
| **Stencil reference** | ✓ | - | ✓ | Always baked |
| **Stencil ops** (fail/depth-fail/pass) | ✓ | - | ✓ | Front and back ops baked |

*`stencil_test_enable` is dynamic state (`VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE`), but the stencil operation parameters are baked.

**Current Usage**: Stencil state is primarily used for shield decals and special effects. Most pipelines use default stencil state (test disabled).

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

    // Fixed state
    gr_alpha_blend blend_mode = ALPHA_BLEND_NONE;
    size_t layout_hash = 0;  // Hash of vertex_layout for pipeline keying
    uint32_t color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    // Stencil state (always baked)
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

The key is hashed for map lookup using a boost-style hash combine pattern:

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
    // ... stencil fields ...
    if (!usesVertexPulling(key.type)) {
        h ^= key.layout_hash;
    }
    return h;
}
```

---

## 4. Shader Variant Flags

Shader variant flags allow a single shader type to have multiple implementations selected at runtime. Variants are encoded in `PipelineKey::variant_flags` and affect shader module selection.

### Variant Flag Definitions

Variant flags are defined in `code/graphics/2d.h`:

| Shader Type | Flag | Value | Purpose |
|-------------|------|-------|---------|
| `SDR_TYPE_EFFECT_PARTICLE` | `SDR_FLAG_PARTICLE_POINT_GEN` | `1<<0` | Point sprite generation |
| `SDR_TYPE_POST_PROCESS_BLUR` | `SDR_FLAG_BLUR_HORIZONTAL` | `1<<0` | Horizontal blur direction |
| `SDR_TYPE_POST_PROCESS_BLUR` | `SDR_FLAG_BLUR_VERTICAL` | `1<<1` | Vertical blur direction |
| `SDR_TYPE_NANOVG` | `SDR_FLAG_NANOVG_EDGE_AA` | `1<<0` | Edge anti-aliasing |
| `SDR_TYPE_DECAL` | `SDR_FLAG_DECAL_USE_NORMAL_MAP` | `1<<0` | Normal map enabled |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_4` | `1<<0` | 4x MSAA |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_8` | `1<<1` | 8x MSAA |
| `SDR_TYPE_MSAA_RESOLVE` | `SDR_FLAG_MSAA_SAMPLES_16` | `1<<2` | 16x MSAA |
| `SDR_TYPE_VOLUMETRIC_FOG` | `SDR_FLAG_VOLUMETRICS_DO_EDGE_SMOOTHING` | `1<<0` | Edge smoothing |
| `SDR_TYPE_VOLUMETRIC_FOG` | `SDR_FLAG_VOLUMETRICS_NOISE` | `1<<1` | Noise enabled |
| `SDR_TYPE_COPY` | `SDR_FLAG_COPY_FROM_ARRAY` | `1<<0` | Copy from texture array |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | `SDR_FLAG_TONEMAPPING_LINEAR_OUT` | `1<<0` | Linear output (no sRGB) |
| `SDR_TYPE_ENVMAP_SPHERE_WARP` | `SDR_FLAG_ENV_MAP` | `1<<0` | Environment map enabled |

### Shader Module Selection

Variant flags affect shader module filename selection in `VulkanShaderManager::getModules()`:

**Example: Bloom Blur**
```cpp
case shader_type::SDR_TYPE_POST_PROCESS_BLUR:
    const auto vertPath = fs::path(m_shaderRoot) / "post_uv.vert.spv";
    const auto fragPath =
        (variantFlags & SDR_FLAG_BLUR_HORIZONTAL)
            ? (fs::path(m_shaderRoot) / "blur_h.frag.spv")
            : (fs::path(m_shaderRoot) / "blur_v.frag.spv");
```

**Caching**: Shader modules are cached by `(shader_type, variantFlags)` key using a custom `Key` struct and `KeyHasher`. Different variant flags create different cached modules.

**Pipeline Key Impact**: Variant flags are part of `PipelineKey`, so different variants create different pipelines even if all other state matches.

### Variant Flag Usage Pattern

```cpp
// Create pipeline key with variant flags
PipelineKey key{};
key.type = SDR_TYPE_POST_PROCESS_BLUR;
key.variant_flags = SDR_FLAG_BLUR_HORIZONTAL;

// Get shader modules (selects blur_h.frag.spv)
ShaderModules modules = shaderManager->getModules(key.type, key.variant_flags);

// Create pipeline (cached by full key including variant_flags)
vk::Pipeline pipeline = pipelineManager->getPipeline(key, modules, layout);
```

**Key Point**: Variant flags are **shader-specific**. Not all shader types support variants. Some shaders (e.g., `SDR_TYPE_MODEL`, `SDR_TYPE_NANOVG`) explicitly set `key.flags = 0` in `VulkanShaderManager::getModules()` to ignore variant flags.

---

## 5. Vertex Input State Conversion

The renderer converts `vertex_layout` (engine abstraction) to Vulkan vertex input state (`VkVertexInputBindingDescription`, `VkVertexInputAttributeDescription`).

### Location Mapping

Vertex attribute locations follow OpenGL convention:

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

**Key Point**: Locations can have gaps. A layout with position (0) and texcoord (2) but no color (1) is valid. The shader simply won't receive data for unused locations.

### Format Mapping

`vertex_format_data` enum values map to Vulkan formats:

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

### Special Cases

**MATRIX4**: A `MATRIX4` vertex format spans 4 consecutive locations (8-11), each with format `R32G32B32A32_SFLOAT`. The conversion creates 4 separate attribute descriptions.

**Vertex Attribute Divisors**: If `vertex_layout` specifies a divisor (instanced rendering), it's converted to `VkVertexInputBindingDivisorDescription`. Requires `VK_EXT_vertex_attribute_divisor` extension.

### Conversion Function

```cpp
VertexInputState convertVertexLayoutToVulkan(const vertex_layout& layout);
```

**Caching**: Converted vertex input states are cached by layout hash (`m_vertexInputCache`). This avoids re-converting the same layout multiple times.

**Validation**: The conversion validates that:
- Binding numbers are valid
- Offsets don't exceed buffer bounds
- Formats match the vertex format data

---

## 6. Vertex Pulling vs Vertex Attributes

The renderer supports two vertex input modes:

| Mode | Usage | Vertex Attributes | Pipeline Key Impact |
|------|-------|-------------------|---------------------|
| **Vertex Attributes** | Most shaders | Traditional vertex attributes from `vertex_layout` | `layout_hash` must match |
| **Vertex Pulling** | `SDR_TYPE_MODEL` only | No vertex attributes; shader fetches from buffers | `layout_hash` ignored |

### Vertex Attributes Mode

**How it works**:
- Vertex data is provided via `vkCmdBindVertexBuffers()`
- Vulkan automatically fetches vertex attributes based on pipeline's vertex input state
- Shader declares `layout(location = N) in vec3 position;`

**Pipeline Key**: `layout_hash` must match the provided `vertex_layout`. Mismatch causes pipeline creation failure.

**Usage**: All shaders except `SDR_TYPE_MODEL` use this mode.

### Vertex Pulling Mode

**How it works**:
- No vertex attributes declared in pipeline (empty vertex input state)
- Shader manually fetches vertex data from storage buffers using `gl_VertexIndex`
- Vertex data is uploaded to a storage buffer and indexed in the shader

**Pipeline Key**: `layout_hash` is ignored (not part of cache key comparison).

**Usage**: Only `SDR_TYPE_MODEL` uses this mode.

**Rationale**:
- Model rendering uses a large vertex heap buffer with variable layouts
- Vertex pulling allows fetching arbitrary vertex formats without pipeline variants
- Reduces pipeline count (one model pipeline vs many attribute-based pipelines)

### Selection Logic

Vertex input mode is determined by shader type via `VulkanLayoutContracts`:

```cpp
bool usesVertexPulling(shader_type type);
VertexInputMode vertexInput = getShaderLayoutSpec(type).vertexInput;
```

**Pipeline Creation**: When creating a pipeline, the vertex input state is set based on the mode:

```cpp
if (usesVertexPulling(key.type)) {
    // No vertex attributes
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.vertexAttributeDescriptionCount = 0;
} else {
    // Convert vertex_layout to Vulkan vertex input state
    const auto& vertexState = getVertexInputState(layout);
    // ... set bindings and attributes ...
}
```

---

## 7. Dynamic Rendering Compatibility

Since the renderer uses `VK_KHR_dynamic_rendering` (no traditional VkRenderPass), pipeline compatibility is determined by matching rendering info at bind time.

### Compatibility Contract

A pipeline is compatible with a rendering session if **all** of these match:

| Property | Pipeline Value | Rendering Info Value |
|----------|----------------|---------------------|
| Color format(s) | `VkPipelineRenderingCreateInfo::pColorAttachmentFormats` | `VkRenderingAttachmentInfo::imageView` format |
| Depth format | `VkPipelineRenderingCreateInfo::depthAttachmentFormat` | `VkRenderingAttachmentInfo::imageView` format |
| Stencil format | `VkPipelineRenderingCreateInfo::stencilAttachmentFormat` | (same as depth if combined) |
| Color attachment count | `colorAttachmentCount` | Number of color attachments in `VkRenderingInfo` |
| Sample count | Pipeline sample count | Attachment image sample count |

### Target Type to Format Mapping

| Target Type | Color Format | Depth Format | Sample Count | Attachments |
|-------------|--------------|--------------|--------------|-------------|
| **Swapchain** | `B8G8R8A8_SRGB` (preferred) | `D32_SFLOAT_S8_UINT`* | 1 | 1 |
| **Scene HDR** | `R16G16B16A16_SFLOAT` | `D32_SFLOAT_S8_UINT`* | 1 | 1 |
| **G-buffer** | `R16G16B16A16_SFLOAT` (x5) | `D32_SFLOAT_S8_UINT`* | 1 | 5 |
| **Cockpit Texture** | `B8G8R8A8_UNORM` | None | 1 | 1 |
| **Bloom MIP** | `R16G16B16A16_SFLOAT` | None | 1 | 1 |
| **LDR Post** | `B8G8R8A8_UNORM` | None | 1 | 1 |

*Depth format is selected from candidates in order: `D32_SFLOAT_S8_UINT`, `D24_UNORM_S8_UINT`, `D32_SFLOAT` based on device support.

### Shader Type Restrictions

Some shader types are only valid against specific target types:

| Shader Type | Valid Targets | Notes |
|-------------|---------------|-------|
| `SDR_TYPE_MODEL` | G-buffer | Outputs to all 5 G-buffer attachments |
| `SDR_TYPE_DEFERRED_LIGHTING` | Scene HDR | Reads G-buffer, writes to scene |
| `SDR_TYPE_INTERFACE` | Swapchain, Cockpit | 2D overlay rendering |
| `SDR_TYPE_POST_PROCESS_BRIGHTPASS` | Bloom MIP | Downsample + threshold |
| `SDR_TYPE_POST_PROCESS_BLUR` | Bloom MIP | Horizontal/vertical blur passes |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | Scene HDR | Composite bloom into scene |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | Swapchain | HDR to LDR conversion |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | Swapchain | God rays overlay |

**Note**: Shadow map rendering is currently stubbed (not implemented) in the Vulkan backend.

---

## 8. Pipeline Lifecycle

### Creation Phase

Pipelines are created **lazily on first use**:

```
Draw call requested
    ↓
Compute pipeline key from current state
    ├─ shader_type
    ├─ variant_flags
    ├─ render target formats (color, depth)
    ├─ blend_mode
    └─ vertex_layout_hash (if VertexAttributes mode)
    ↓
Check cache: key → VkPipeline
    ↓
[Cache Miss] → Create new pipeline
    ├─ Get shader modules (cached by type + variant flags)
    ├─ Get vertex input state (cached by layout hash)
    ├─ Build pipeline create info
    │   ├─ Shader stages
    │   ├─ Vertex input (or empty if VertexPulling)
    │   ├─ Dynamic rendering info (formats, sample count)
    │   ├─ Blend state (from blend_mode)
    │   └─ Dynamic state list
    ├─ Call vkCreateGraphicsPipelines()
    └─ Store in cache
    ↓
Bind pipeline and draw
```

### Pipeline Key Validation

Before cache lookup, the pipeline key is validated in `getPipeline()`:

```cpp
PipelineKey adjustedKey = key;
const auto& layoutSpec = getShaderLayoutSpec(key.type);

if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
    const size_t expectedHash = layout.hash();
    if (adjustedKey.layout_hash != expectedHash) {
        adjustedKey.layout_hash = expectedHash;
        throw std::runtime_error(
            "PipelineKey.layout_hash mismatches provided vertex_layout for VertexAttributes shader");
    }
}
```

**Rationale**: Ensures pipeline key matches the provided vertex layout. Throws an exception rather than silently reusing an incorrect pipeline.

### Shader Module Loading

Shader modules are loaded and cached separately from pipelines:

```cpp
ShaderModules modules = shaderManager->getModules(shader_type, variantFlags);
```

**Caching**: Modules are cached by `(shader_type, variantFlags)` key in `VulkanShaderManager`. Different variant flags create different cached modules.

**File Naming**: Shader filenames are determined by shader type and variant flags (e.g., `blur_h.frag.spv` vs `blur_v.frag.spv`).

### Vertex Input State Caching

Vertex input state conversion is cached:

```cpp
const VertexInputState& vertexState = getVertexInputState(layout);
```

**Caching**: Converted states are cached by `layout.hash()` in `m_vertexInputCache`. Avoids re-converting the same layout.

### Thread Safety

Pipeline creation occurs on the **main rendering thread only**. No thread-safety mechanisms are required for the cache.

**Rationale**: All rendering operations (including pipeline creation) happen on a single thread. No concurrent access to pipeline cache.

### Cache Invalidation

The pipeline cache is **never invalidated** during normal operation. It persists for the lifetime of the renderer.

Pipelines are only destroyed at:
- Renderer shutdown
- Device lost recovery (full recreation)

### No Disk Persistence

The current implementation does not persist `VkPipelineCache` to disk. All pipelines are recompiled on each application launch.

**Implication**: First frame of each shader/target combination incurs shader compile cost.

**Future Improvement**: Could implement disk persistence of `VkPipelineCache` to reduce startup time.

---

## 9. Descriptor Update Safety Rules

Descriptor set updates must respect GPU synchronization to avoid data races.

### The Core Rule

> **A descriptor set must not be updated while any in-flight command buffer references it.**

### Safe Update Conditions

| Scenario | Safe? | Reason |
|----------|:-----:|--------|
| Update before any command buffer submission | ✓ | No references exist |
| Update after `vkQueueWaitIdle()` | ✓ | All work complete |
| Update after fence wait for specific frame | ✓ | Frame's work complete |
| Update between frames with per-frame sets | ✓ | Set not in flight |
| Update to set currently bound in recording | ✓ | Recording != executing |
| Update to set in submitted command buffer | ✗ | **DATA RACE** |

### Per-Frame Descriptor Sets

The renderer uses per-frame model descriptor sets (`kFramesInFlight = 2`):

```
Frame 0: modelDescriptorSet[0] ← Safe to update when frame 0 not in flight
Frame 1: modelDescriptorSet[1] ← Safe to update when frame 1 not in flight
```

**Update timing**: Sets are updated at frame start, after waiting on the previous frame's fence:

```cpp
void prepareFrameForReuse(VulkanFrame& frame)
{
    frame.wait_for_gpu();  // Fence wait - GPU done with this frame
    // NOW safe to update frame's descriptor set
}
```

### Push Descriptors

Push descriptors (`vkCmdPushDescriptorSetKHR`) are inherently safe because they don't reference a persistent descriptor set - data is embedded in the command buffer.

### Global Descriptor Set

The global set (G-buffer textures) is shared across all frames. It is updated:
- Once during deferred lighting setup
- Only when render targets change (resize, etc.)

**Safety**: Updates occur before any command buffer referencing the new values is submitted.

---

## 10. Buffer Orphaning and Descriptor Caching

The renderer uses both buffer orphaning and descriptor caching. These must be coordinated correctly.

### The Problem

Buffer orphaning can replace the underlying `VkBuffer`:

```cpp
void resizeBuffer(handle, size)
{
    // Old buffer retired to deferred release
    m_deferredReleases.enqueue(retireSerial, [oldBuf]() { destroy(oldBuf); });

    // NEW buffer created
    buffer.buffer = createNewBuffer(size);
}
```

Meanwhile, descriptor caching may skip updates if "handle unchanged":

```cpp
if (frame.uniformBinding.bufferHandle == handle) {
    // Skip descriptor update - WRONG if VkBuffer changed!
}
```

### The Invariant

> **Descriptor writes must be keyed on VkBuffer identity, not just handle equality.**

### Implementation

The renderer tracks uniform bindings per-frame via `DynamicUniformBinding` (defined in `VulkanFrame.h`):

```cpp
struct DynamicUniformBinding {
    gr_buffer_handle bufferHandle{};
    uint32_t dynamicOffset = 0;
};
```

Each frame maintains its own binding state:
```cpp
// In VulkanFrame:
DynamicUniformBinding modelUniformBinding{ gr_buffer_handle::invalid(), 0 };
DynamicUniformBinding sceneUniformBinding{ gr_buffer_handle::invalid(), 0 };
```

Buffer orphaning is handled by the deferred release system - the old buffer remains valid until the GPU has finished using it, coordinated via timeline semaphores.

### Frame Isolation

Each frame has its own `DynamicUniformBinding` state, so orphaning on frame N doesn't affect frame N-1's cached bindings (which reference the old buffer that's still valid via deferred release).

---

## 11. Feature and Extension Requirements

### Required (Core)

| Feature/Extension | Version | Purpose |
|-------------------|---------|---------|
| Vulkan 1.3 | Core | Dynamic rendering, sync2, extended dynamic state |
| `VK_KHR_dynamic_rendering` | Core 1.3 | No render pass objects |
| `VK_KHR_synchronization2` | Core 1.3 | 64-bit stage/access masks |
| `VK_EXT_extended_dynamic_state` | Core 1.3 | Cull mode, front face, depth test, etc. |

### Required (Hard Check)

| Feature | Check | Failure Mode |
|---------|-------|--------------|
| `pushDescriptor` | Vulkan 1.4 feature | Throws exception |
| `shaderSampledImageArrayNonUniformIndexing` | Descriptor indexing | Throws exception |
| `runtimeDescriptorArray` | Descriptor indexing | Throws exception |
| `maxDescriptorSetSampledImages >= 1024` | Device limit | Assertion failure |

### Optional (Graceful Degradation)

| Feature | Fallback |
|---------|----------|
| `VK_EXT_extended_dynamic_state3` | Bake blend/polygon state into pipeline |
| `samplerYcbcrConversion` | Movie playback disabled |
| `G8_B8_R8_3PLANE_420_UNORM` format | Movie playback disabled |

---

## 12. Failure Modes and Fallbacks

### Pipeline Creation Failure

If `vkCreateGraphicsPipelines` fails:

```cpp
auto pipelineResult = m_device.createGraphicsPipelineUnique(m_pipelineCache, pipelineInfo);
if (pipelineResult.result != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to create Vulkan graphics pipeline.");
}
```

**Current behavior**: Fatal error via exception. No fallback shader system exists.

**Future improvement**: Could implement fallback to a simple solid-color shader for graceful degradation.

### Shader Module Load Failure

If SPIR-V module cannot be loaded from embedded data or filesystem:

```cpp
std::ifstream file(path, std::ios::binary | std::ios::ate);
if (!file) {
    throw std::runtime_error("Failed to open shader module " + path);
}
```

**Current behavior**: Fatal error via exception.

### Descriptor Pool Exhaustion

The model pool is sized for exactly `kFramesInFlight` sets. Exhaustion should be impossible with correct usage:

```cpp
poolInfo.maxSets = kFramesInFlight; // 2
```

If allocation fails, it indicates a logic error (allocating more sets than expected).

---

## Appendix: Debugging Tips

### RenderDoc Integration

Debug regions are inserted for GPU capture correlation:

```cpp
cmd.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{"Model Rendering"});
// ... draw calls ...
cmd.endDebugUtilsLabelEXT();
```

### Validation Layer Messages

Common validation errors related to pipelines:

| Error | Cause | Fix |
|-------|-------|-----|
| "Pipeline not compatible with render pass" | Format mismatch | Check target format in pipeline key |
| "Descriptor set not bound" | Missing bind call | Ensure `bindDescriptorSets` before draw |
| "Dynamic state not set" | Missing `set*` call | Set all required dynamic state before draw |
