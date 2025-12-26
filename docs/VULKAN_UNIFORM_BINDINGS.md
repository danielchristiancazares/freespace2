# Vulkan Uniform Buffer Bindings

This document describes the uniform buffer structs used in the Vulkan renderer, their mapping to shader bindings, and the std140 layout rules that govern their structure.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Uniform Block Type to Shader Binding Mapping](#2-uniform-block-type-to-shader-binding-mapping)
3. [Uniform Struct Definitions](#3-uniform-struct-definitions)
4. [std140 Layout Rules](#4-std140-layout-rules)
5. [Binding Strategy by Pipeline Layout](#5-binding-strategy-by-pipeline-layout)
6. [Usage Patterns](#6-usage-patterns)
7. [Alignment Requirements](#7-alignment-requirements)
8. [Troubleshooting](#8-troubleshooting)

---

## 1. Overview

Uniform buffers in the Vulkan renderer are defined in `code/graphics/util/uniform_structs.h`. These structs must conform to **std140 layout rules** to ensure binary compatibility between C++ host code and GLSL shaders.

### Key Files

| File | Purpose |
|------|---------|
| `code/graphics/util/uniform_structs.h` | C++ struct definitions (std140 layout) |
| `code/graphics/2d.h` | `uniform_block_type` enum and `shader_type` enum |
| `build/code/shader_structs.h` | Auto-generated struct definitions from shader sources |
| `code/graphics/shaders/*.vert` / `*.frag` | GLSL shader uniform block declarations |
| `code/graphics/vulkan/VulkanRingBuffer.h` | Per-frame ring buffer for uniform allocations |

### std140 Layout Summary

The std140 layout guarantees predictable memory alignment across all platforms:

| Type | Base Alignment | Size |
|------|----------------|------|
| `float`, `int`, `bool` | 4 bytes | 4 bytes |
| `vec2` | 8 bytes | 8 bytes |
| `vec3` | 16 bytes | 12 bytes (but occupies 16 due to alignment) |
| `vec4` | 16 bytes | 16 bytes |
| `mat4` (4x4 matrix) | 16 bytes per column | 64 bytes total |
| Array of any type | 16 bytes per element | varies |
| Struct | 16 bytes (rounded up) | varies |

**Critical Rule**: All struct members must start at an offset that is a multiple of their base alignment. Padding fields (`pad`, `pad0`, `pad1`, etc.) are inserted to enforce this constraint.

---

## 2. Uniform Block Type to Shader Binding Mapping

The `uniform_block_type` enum is defined in `code/graphics/2d.h`. These enum values serve as UBO binding indices in the OpenGL renderer. In Vulkan, shaders declare explicit `layout(binding = N)` bindings that are independent of these enum values.

### OpenGL Enum Values (`uniform_block_type`)

| Enum Name | Value | C++ Struct | Description |
|-----------|-------|------------|-------------|
| `Lights` | 0 | (deprecated) | Legacy lighting system (unused) |
| `ModelData` | 1 | `model_uniform_data` | Per-draw 3D model rendering data |
| `NanoVGData` | 2 | `nanovg_draw_data` | NanoVG vector graphics rendering |
| `DecalInfo` | 3 | `decal_info` | Per-decal texture indices and blend modes |
| `DecalGlobals` | 4 | `decal_globals` | Decal system view/projection matrices |
| `DeferredGlobals` | 5 | `deferred_global_data` | Deferred lighting global parameters |
| `Matrices` | 6 | `matrix_uniforms` | View/projection matrix pair |
| `MovieData` | 7 | `movie_uniforms` | Movie playback alpha (OpenGL only) |
| `GenericData` | 8 | Various (see Section 3.3) | Shader-specific uniform data |

### Vulkan Standard Pipeline Bindings

Most Vulkan shaders (2D, UI, post-processing) use this standard binding layout:

| Set | Binding | Block Name | C++ Struct | Purpose |
|-----|---------|------------|------------|---------|
| 0 | 0 | `matrixData` | `matrix_uniforms` | View/projection matrices |
| 0 | 1 | `genericData` | (shader-specific) | Per-shader uniform data |
| 0 | 2+ | (samplers) | - | Texture bindings |

**Descriptor Strategy**: Push descriptors via `vkCmdPushDescriptorSetKHR` - no persistent descriptor set allocation required.

---

## 3. Uniform Struct Definitions

### 3.1 Matrix Uniforms (`matrix_uniforms`)

The most commonly used uniform block, providing view and projection matrices for 2D and UI rendering.

**Binding**: Set 0, Binding 0 (Standard pipeline layout)
**Size**: 128 bytes (2 mat4)

```cpp
struct matrix_uniforms {
    matrix4 modelViewMatrix;  // offset 0,  size 64
    matrix4 projMatrix;       // offset 64, size 64
};  // Total: 128 bytes
```

**GLSL Declaration**:
```glsl
layout(binding = 0, std140) uniform matrixData {
    mat4 modelViewMatrix;
    mat4 projMatrix;
};
```

### 3.2 Model Uniform Data (`model_uniform_data`)

Contains all per-draw state for 3D model rendering, including transforms, lighting, material properties, and shader control flags.

**Binding**: Set 0, Binding 2 (Model pipeline layout - see `model.vert`)
**Type**: `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`
**Size**: ~1216 bytes (varies; must be aligned to `minUniformBufferOffsetAlignment`)

```cpp
const size_t MAX_UNIFORM_LIGHTS = 8;

struct model_light {
    vec4 position;           // offset 0,  size 16

    vec3d diffuse_color;     // offset 16, size 12
    int light_type;          // offset 28, size 4

    vec3d direction;         // offset 32, size 12
    float attenuation;       // offset 44, size 4

    float ml_sourceRadius;   // offset 48, size 4
    float pad0[3];           // offset 52, size 12 (padding to 64 bytes)
};  // Total: 64 bytes per light

struct model_uniform_data {
    // Matrices (offset 0 - 575)
    matrix4 modelViewMatrix;           // offset 0,   size 64
    matrix4 modelMatrix;               // offset 64,  size 64
    matrix4 viewMatrix;                // offset 128, size 64
    matrix4 projMatrix;                // offset 192, size 64
    matrix4 textureMatrix;             // offset 256, size 64
    matrix4 shadow_mv_matrix;          // offset 320, size 64
    matrix4 shadow_proj_matrix[4];     // offset 384, size 256

    // Color (offset 640)
    vec4 color;                        // offset 640, size 16

    // Lighting array (offset 656 - 1167)
    model_light lights[MAX_UNIFORM_LIGHTS];  // offset 656, size 512

    // Scalar parameters (offset 1168+)
    float outlineWidth;                // offset 1168
    float fogStart;                    // offset 1172
    float fogScale;                    // offset 1176
    int buffer_matrix_offset;          // offset 1180

    // ... additional fields ...
};
```

**Shader Access (Vulkan)**: The `model.vert` shader uses a minimal subset with explicit offsets:
```glsl
layout(set = 0, binding = 2, std140) uniform ModelUniforms {
    mat4 modelViewMatrix;
    mat4 modelMatrix;
    mat4 viewMatrix;
    mat4 projMatrix;
    layout(offset = 256) mat4 textureMatrix;
    layout(offset = 1180) int buffer_matrix_offset;
    layout(offset = 1200) float thruster_scale;
} uModel;
```

**Key Points**:
- Uses **dynamic uniform buffer offsets** for batched model draws
- Multiple draws pack their uniform data into a single buffer with aligned offsets
- Binding is updated per-draw via `setModelUniformBinding()`

### 3.3 Generic Data (`genericData`)

The `genericData` binding at Set 0, Binding 1 uses different struct layouts depending on the active shader. This polymorphic approach allows shader-specific parameters without changing the pipeline layout.

| Shader Type Enum | C++ Struct | Primary Fields |
|------------------|------------|----------------|
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | `generic_data::tonemapping_data` | `exposure`, `tonemapper`, PPC curve params |
| `SDR_TYPE_POST_PROCESS_BLUR` | `generic_data::blur_data` | `texSize`, `level` |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | `generic_data::bloom_composition_data` | `bloom_intensity`, `levels` |
| `SDR_TYPE_POST_PROCESS_FXAA` | `generic_data::fxaa_data` | `rt_w`, `rt_h` |
| `SDR_TYPE_POST_PROCESS_SMAA_*` | `generic_data::smaa_data` | `smaa_rt_metrics` |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | `generic_data::lightshaft_data` | `sun_pos`, `density`, `weight`, `falloff`, `intensity` |
| `SDR_TYPE_POST_PROCESS_MAIN` | `generic_data::post_data` | `timer`, `noise_amount`, `saturation`, `brightness`, etc. |
| `SDR_TYPE_PASSTHROUGH_RENDER` | `generic_data::passthrough_data` | `noTexturing`, `srgb` |
| `SDR_TYPE_SHIELD_DECAL` | `generic_data::shield_impact_data` | `shieldModelViewMatrix`, `shieldProjMatrix`, `hitNormal`, etc. |
| `SDR_TYPE_ROCKET_UI` | `generic_data::rocketui_data` | `projMatrix`, `offset`, `textured`, `baseMapIndex` |
| `SDR_TYPE_SCENE_FOG` | `generic_data::fog_data` | `fog_color`, `fog_start`, `fog_density` |
| `SDR_TYPE_VOLUMETRIC_FOG` | `generic_data::volumetric_fog_data` | Inverse matrices, camera params, nebula params |
| `SDR_TYPE_BATCHED_BITMAP` | `generic_data::batched_data` | `color`, `intensity` |
| `SDR_TYPE_FLAT_COLOR` | `generic_data::flat_color_data` | `color`, `srgb`, `intensity` |
| `SDR_TYPE_EFFECT_PARTICLE` | `generic_data::effect_data` | `window_width`, `window_height`, depth params |
| `SDR_TYPE_EFFECT_DISTORTION` | `generic_data::effect_distort_data` | `window_width`, `window_height`, `use_offset` |

**Example: Tonemapping Data**
```cpp
struct tonemapping_data {
    float exposure;      // offset 0,  size 4
    int tonemapper;      // offset 4,  size 4
    float x0;            // offset 8,  size 4 (PPC curve params)
    float y0;            // offset 12, size 4

    float x1;            // offset 16, size 4
    float toe_B;         // offset 20, size 4
    float toe_lnA;       // offset 24, size 4
    float sh_B;          // offset 28, size 4

    float sh_lnA;        // offset 32, size 4
    float sh_offsetX;    // offset 36, size 4
    float sh_offsetY;    // offset 40, size 4
    float pad[1];        // offset 44, size 4 (16-byte struct alignment)
};  // Total: 48 bytes
```

**Example: Flat Color Data**
```cpp
struct flat_color_data {
    vec4 color;          // offset 0,  size 16

    int srgb;            // offset 16, size 4
    float intensity;     // offset 20, size 4
    float pad[2];        // offset 24, size 8 (16-byte struct alignment)
};  // Total: 32 bytes
```

**GLSL Declaration** (`flat-color.frag`):
```glsl
layout(binding = 1, std140) uniform genericData {
    vec4 color;
    int srgb;
    float intensity;
};
```

**Important**: All `generic_data` structs must have sizes that are multiples of 16 bytes. Padding fields ensure this constraint is met.

### 3.4 NanoVG Draw Data (`nanovg_draw_data`)

Contains all parameters for NanoVG vector graphics rendering, including scissor/paint transforms, colors, and stroke parameters.

**Binding**: Set 0, Binding 1 (as `NanoVGUniformData` in `nanovg.frag`)
**Size**: 208 bytes

```cpp
enum class NanoVGShaderType: int32_t {
    FillGradient = 0, FillImage = 1, Simple = 2, Image = 3
};

struct nanovg_draw_data {
    float scissorMat[12];    // offset 0,   size 48 (3x mat3 columns = 3 vec4s)
    float paintMat[12];      // offset 48,  size 48

    vec4 innerCol;           // offset 96,  size 16
    vec4 outerCol;           // offset 112, size 16

    vec2d scissorExt;        // offset 128, size 8
    vec2d scissorScale;      // offset 136, size 8

    vec2d extent;            // offset 144, size 8
    float radius;            // offset 152, size 4
    float feather;           // offset 156, size 4

    float strokeMult;        // offset 160, size 4
    float strokeThr;         // offset 164, size 4
    int texType;             // offset 168, size 4
    NanoVGShaderType type;   // offset 172, size 4

    vec2d viewSize;          // offset 176, size 8
    int texArrayIndex;       // offset 184, size 4
    float pad3;              // offset 188, size 4
};  // Total: 192 bytes (check alignment)
```

**GLSL Declaration** (`nanovg.frag`):
```glsl
layout(binding = 1, std140) uniform NanoVGUniformData {
    mat3 scissorMat;
    mat3 paintMat;
    vec4 innerCol;
    vec4 outerCol;
    vec2 scissorExt;
    vec2 scissorScale;
    vec2 extent;
    float radius;
    float feather;
    float strokeMult;
    float strokeThr;
    int texType;
    int type;
    vec2 viewSize;
    int texArrayIndex;
};
```

### 3.5 Deferred Lighting Uniforms

#### Deferred Global Data (`deferred_global_data`)

Global parameters for the deferred lighting system, including shadow matrices and screen dimensions.

**Binding**: Used by the deferred system but G-buffer textures are bound via Set 1
**Size**: 400 bytes

```cpp
struct deferred_global_data {
    matrix4 shadow_mv_matrix;          // offset 0,   size 64
    matrix4 shadow_proj_matrix[4];     // offset 64,  size 256

    matrix4 inv_view_matrix;           // offset 320, size 64

    float veryneardist;                // offset 384, size 4
    float neardist;                    // offset 388, size 4
    float middist;                     // offset 392, size 4
    float fardist;                     // offset 396, size 4

    float invScreenWidth;              // offset 400, size 4
    float invScreenHeight;             // offset 404, size 4
    float nearPlane;                   // offset 408, size 4
    float pad;                         // offset 412, size 4
};  // Total: 416 bytes
```

#### Deferred Light Data (`deferred_light_data`)

Per-light parameters for deferred lighting calculations.

**Binding**: Set 0, Binding 1 (via `lightData` uniform block in `deferred.frag`)
**Size**: 64 bytes

```cpp
struct deferred_light_data {
    vec3d diffuseLightColor;   // offset 0,  size 12
    float coneAngle;           // offset 12, size 4

    vec3d lightDir;            // offset 16, size 12
    float coneInnerAngle;      // offset 28, size 4

    vec3d coneDir;             // offset 32, size 12
    float dualCone;            // offset 44, size 4 (Note: shader declares as uint)

    vec3d scale;               // offset 48, size 12
    float lightRadius;         // offset 60, size 4

    int lightType;             // offset 64, size 4
    int enable_shadows;        // offset 68, size 4
    float sourceRadius;        // offset 72, size 4
    float pad0[1];             // offset 76, size 4
};  // Total: 80 bytes
```

**GLSL Declaration** (`deferred.frag`):
```glsl
layout(binding = 1, std140) uniform lightData {
    vec3 diffuseLightColor;
    float coneAngle;

    vec3 lightDir;
    float coneInnerAngle;

    vec3 coneDir;
    uint dualCone;  // Note: C++ uses float, shader uses uint

    vec3 scale;
    float lightRadius;

    int lightType;
    uint enable_shadows;
    float sourceRadius;
};
```

**Note**: The light position is encoded in `matrixData.modelViewMatrix[3].xyz` rather than a separate field.

### 3.6 Decal Uniforms

#### Decal Globals (`decal_globals`)

View and projection matrices for decal rendering, including their inverses for screen-space to world-space reconstruction.

**Binding**: Set 0, Binding 0 (as `decalGlobalData` in decal shaders)

```cpp
struct decal_globals {
    matrix4 viewMatrix;       // offset 0,   size 64
    matrix4 projMatrix;       // offset 64,  size 64
    matrix4 invViewMatrix;    // offset 128, size 64
    matrix4 invProjMatrix;    // offset 192, size 64

    vec2d viewportSize;       // offset 256, size 8
    float pad1[2];            // offset 264, size 8
};  // Total: 272 bytes
```

#### Decal Info (`decal_info`)

Per-decal texture indices and blend mode configuration.

**Binding**: Set 0, Binding 1 (as `decalInfoData` in decal shaders)

```cpp
struct decal_info {
    int diffuse_index;        // offset 0,  size 4
    int glow_index;           // offset 4,  size 4
    int normal_index;         // offset 8,  size 4
    int diffuse_blend_mode;   // offset 12, size 4

    int glow_blend_mode;      // offset 16, size 4
    float pad[3];             // offset 20, size 12
};  // Total: 32 bytes
```

### 3.7 Movie Uniforms (`movie_uniforms`)

Parameters for video playback rendering.

**Note**: In the Vulkan renderer, movie playback uses push constants instead of this uniform block.

```cpp
struct movie_uniforms {
    float alpha;              // offset 0, size 4
    float pad[3];             // offset 4, size 12
};  // Total: 16 bytes
```

---

## 4. std140 Layout Rules

### Rule 1: Base Alignment

Each data type has a required base alignment that determines valid starting offsets:

| Type | Base Alignment | Effective Size in Array |
|------|----------------|------------------------|
| `float`, `int`, `bool` | 4 bytes | 16 bytes (rounded up) |
| `vec2` | 8 bytes | 16 bytes (rounded up) |
| `vec3`, `vec4` | 16 bytes | 16 bytes |
| `mat4` | 16 bytes per column | 64 bytes |
| `mat3` | 16 bytes per column | 48 bytes (3 x vec4) |
| Struct | 16 bytes | 16-byte multiple |

### Rule 2: Member Offset Calculation

Each struct member is placed at the next offset that satisfies its base alignment. Padding is implicitly inserted between members as needed.

**Example with explicit byte offsets**:
```cpp
struct aligned_example {
    vec3d position;      // offset 0:  12 bytes data, 16 byte alignment
    float scale;         // offset 12: 4 bytes (fits in vec3's padding slot)
    vec4 color;          // offset 16: 16 bytes, needs 16-byte alignment
    int count;           // offset 32: 4 bytes
    float data[2];       // offset 48: array starts at 16-byte boundary (per element)
                         //            element 0 at 48, element 1 at 64
    mat4 transform;      // offset 80: 64 bytes, 16-byte column alignment
};  // Total size: 144 bytes
```

### Rule 3: Scalar Packing After vec3

A scalar (float, int) **can** follow a vec3 and pack into the remaining 4 bytes of its 16-byte slot. This is an optimization used throughout the codebase:

```cpp
// VALID - scalar packs after vec3
struct valid_packing {
    vec3d pos;       // offset 0,  size 12, occupies bytes 0-11
    float scale;     // offset 12, size 4,  occupies bytes 12-15
    vec4 color;      // offset 16, properly aligned
};  // Total: 32 bytes
```

### Rule 4: Struct Size Rounding

The total size of a struct must be a multiple of its largest member's base alignment (typically 16 bytes for any struct containing vec3/vec4/mat4):

```cpp
// INCORRECT - struct size not a multiple of 16
struct bad_size {
    vec3d pos;       // offset 0,  size 12
    float scale;     // offset 12, size 4
    int flags;       // offset 16, size 4
};  // Size: 20 bytes - WRONG!

// CORRECT - padded to multiple of 16
struct good_size {
    vec3d pos;       // offset 0,  size 12
    float scale;     // offset 12, size 4
    int flags;       // offset 16, size 4
    float pad[3];    // offset 20, size 12
};  // Size: 32 bytes - correct
```

### Rule 5: Array Element Alignment

Every array element is aligned to 16 bytes, even for scalar types:

```cpp
float values[4];     // Takes 64 bytes (16 per element), not 16 bytes!
vec4 vectors[4];     // Takes 64 bytes (16 per element) - same as expected
```

### Rule 6: Matrix Layout

Matrices are stored as arrays of column vectors in column-major order:

```cpp
mat4 transform;      // 4 columns x 16 bytes = 64 bytes total
// Column 0: bytes 0-15
// Column 1: bytes 16-31
// Column 2: bytes 32-47
// Column 3: bytes 48-63
```

---

## 5. Binding Strategy by Pipeline Layout

### Standard Pipeline Layout

Used for 2D rendering, UI, particles, and post-processing effects.

| Set | Binding | Type | Content | Update Frequency |
|-----|---------|------|---------|------------------|
| 0 | 0 | Uniform Buffer | `matrixData` (`matrix_uniforms`) | Per-draw |
| 0 | 1 | Uniform Buffer | `genericData` (varies by shader) | Per-draw |
| 0 | 2+ | Combined Sampler | Texture bindings | Per-draw |

**Descriptor Strategy**: Push descriptors (`vkCmdPushDescriptorSetKHR`) eliminate the need for descriptor pool allocation and descriptor set management.

### Model Pipeline Layout

Used for 3D model rendering with vertex pulling architecture.

| Set | Binding | Type | Content | Update Frequency |
|-----|---------|------|---------|------------------|
| 0 | 0 | Storage Buffer | `VertexBuffer` (vertex pulling data) | Per-frame |
| 0 | 1 | Sampler Array | `textures[]` (bindless texture array) | Per-frame |
| 0 | 2 | Dynamic Uniform | `ModelUniforms` (subset of `model_uniform_data`) | Per-draw |
| 0 | 3 | Storage Buffer | `TransformBuffer` (batched transforms) | Per-frame |

**Descriptor Strategy**:
- Set 0: Pre-allocated descriptor set with per-frame updates
- Push constants (`ModelPushConstants`) provide per-draw parameters

**Push Constant Structure** (`model.vert`):
```glsl
layout(push_constant) uniform ModelPushConstants {
    uint vertexOffset;        // Byte offset into vertex heap
    uint stride;              // Byte stride between vertices
    uint vertexAttribMask;    // MODEL_ATTRIB_* bitmask
    uint posOffset;           // Attribute byte offsets...
    uint normalOffset;
    uint texCoordOffset;
    uint tangentOffset;
    uint modelIdOffset;
    uint boneIndicesOffset;
    uint boneWeightsOffset;
    uint baseMapIndex;        // Texture indices for bindless sampling
    uint glowMapIndex;
    uint normalMapIndex;
    uint specMapIndex;
    uint matrixIndex;         // Instance/model matrix index
    uint flags;               // Variant flags bitfield
} pcs;
```

### Deferred Pipeline Layout

Used for deferred lighting passes.

| Set | Binding | Type | Content | Update Frequency |
|-----|---------|------|---------|------------------|
| 0 | 0 | Uniform Buffer | `matrixData` (light position via translation) | Per-light |
| 0 | 1 | Uniform Buffer | `lightData` (deferred light parameters) | Per-light |
| 1 | 0-5 | Combined Sampler | G-buffer textures | Per-frame |

**G-buffer Texture Bindings** (Set 1):
| Binding | Attachment |
|---------|------------|
| 0 | ColorBuffer (diffuse albedo) |
| 1 | NormalBuffer (view-space normals + gloss) |
| 2 | PositionBuffer (view-space position + AO) |
| 3 | DepthBuffer (currently unused) |
| 4 | SpecularBuffer (specular color + fresnel) |
| 5 | EmissiveBuffer (emissive contribution) |

**Descriptor Strategy**:
- Set 0: Push descriptors for per-light uniform data
- Set 1: Pre-allocated descriptor set bound via `bindDeferredGlobalDescriptors()`

### Decal Pipeline Layout

Used for decal rendering with screen-space projection.

| Set | Binding | Type | Content |
|-----|---------|------|---------|
| 0 | 0 | Uniform Buffer | `decalGlobalData` (view/proj + inverses) |
| 0 | 1 | Uniform Buffer | `decalInfoData` (texture indices, blend modes) |
| 0 | 2 | Sampler Array | `diffuseMap` (decal diffuse textures) |
| 0 | 3 | Sampler Array | `glowMap` (decal glow textures) |
| 0 | 4 | Sampler Array | `normalMap` (decal normal textures) |
| 1 | 3 | Combined Sampler | `DepthBuffer` (scene depth for projection) |

---

## 6. Usage Patterns

### Pattern 1: Per-Draw Uniforms (Push Descriptors)

Standard pattern for 2D/UI rendering where each draw call has unique uniform data:

```cpp
// 1. Get device alignment requirement
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(renderer.getMinUniformOffsetAlignment());

// 2. Allocate from per-frame ring buffer
auto uniformAlloc = frame.uniformBuffer().allocate(
    sizeof(matrix_uniforms), uboAlignment);

// 3. Copy uniform data to mapped memory
std::memcpy(uniformAlloc.mapped, &matrices, sizeof(matrices));

// 4. Create descriptor buffer info
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;  // Already aligned
matrixInfo.range = sizeof(matrix_uniforms);

// 5. Push descriptor via extension command
vk::WriteDescriptorSet write{};
write.dstBinding = 0;
write.descriptorCount = 1;
write.descriptorType = vk::DescriptorType::eUniformBuffer;
write.pBufferInfo = &matrixInfo;

cmd.pushDescriptorSetKHR(
    vk::PipelineBindPoint::eGraphics,
    pipelineLayout,
    0,  // set index
    write);
```

### Pattern 2: Dynamic Uniform Buffer (Model Pipeline)

Pattern for batched model rendering where multiple draws share a single buffer with dynamic offsets:

```cpp
// 1. Calculate aligned size for each model's uniform data
const size_t deviceAlign = renderer.getMinUniformOffsetAlignment();
const size_t alignedSize =
    (sizeof(model_uniform_data) + deviceAlign - 1) & ~(deviceAlign - 1);

// 2. Pack multiple model uniforms into contiguous buffer
for (size_t i = 0; i < modelCount; ++i) {
    size_t offset = i * alignedSize;
    void* dst = static_cast<char*>(uniformAlloc.mapped) + offset;
    std::memcpy(dst, &modelUniforms[i], sizeof(model_uniform_data));
}

// 3. Bind descriptor set with base offset
// (Dynamic offset added per-draw)
renderer.setModelUniformBinding(frame, bufferHandle, baseOffset, sizeof(model_uniform_data));

// 4. For each draw, bind with dynamic offset
for (size_t i = 0; i < modelCount; ++i) {
    uint32_t dynamicOffset = static_cast<uint32_t>(i * alignedSize);
    cmd.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        modelPipelineLayout,
        0,
        descriptorSet,
        dynamicOffset);  // Dynamic offset for binding 2

    cmd.drawIndexed(/*...*/);
}
```

### Pattern 3: Multiple Uniform Blocks (Packed Allocation)

Pattern for shaders requiring both matrixData and genericData:

```cpp
// 1. Calculate sizes (both should be 16-byte aligned already)
const size_t matrixSize = sizeof(matrix_uniforms);
const size_t genericSize = sizeof(generic_data::post_data);
const size_t totalSize = matrixSize + genericSize;

// 2. Allocate contiguous space
auto uniformAlloc = frame.uniformBuffer().allocate(totalSize, uboAlignment);

// 3. Copy both structs sequentially
std::memcpy(uniformAlloc.mapped, &matrices, matrixSize);
std::memcpy(
    static_cast<char*>(uniformAlloc.mapped) + matrixSize,
    &generic,
    genericSize);

// 4. Create separate descriptor buffer infos
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;
matrixInfo.range = matrixSize;

vk::DescriptorBufferInfo genericInfo{};
genericInfo.buffer = frame.uniformBuffer().buffer();
genericInfo.offset = uniformAlloc.offset + matrixSize;
genericInfo.range = genericSize;

// 5. Push both descriptors in single call
std::array<vk::WriteDescriptorSet, 2> writes{};
writes[0].dstBinding = 0;
writes[0].descriptorCount = 1;
writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
writes[0].pBufferInfo = &matrixInfo;

writes[1].dstBinding = 1;
writes[1].descriptorCount = 1;
writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
writes[1].pBufferInfo = &genericInfo;

cmd.pushDescriptorSetKHR(
    vk::PipelineBindPoint::eGraphics,
    pipelineLayout,
    0,
    writes);
```

---

## 7. Alignment Requirements

### Device Alignment (`minUniformBufferOffsetAlignment`)

Vulkan devices require uniform buffer offsets to be aligned to a device-specific value, typically 256 bytes on desktop GPUs but can vary.

**Query at startup**:
```cpp
vk::PhysicalDeviceProperties props = physicalDevice.getProperties();
VkDeviceSize minAlign = props.limits.minUniformBufferOffsetAlignment;
// Typical values: 256 (NVIDIA, AMD desktop), 64 (Intel), 16 (some mobile)
```

**Impact**: When using dynamic uniform buffers, each uniform data instance must start at an offset that is a multiple of this value, not just the struct size.

### std140 Alignment vs Device Alignment

These are independent requirements that must both be satisfied:

| Requirement | Purpose | Typical Value |
|-------------|---------|---------------|
| std140 alignment | Shader-to-C++ struct compatibility | 16 bytes |
| Device alignment | Buffer offset validity | 256 bytes |

A struct may be valid std140 (e.g., 128 bytes) but still need padding to 256 bytes when packed into a dynamic uniform buffer.

### Ring Buffer Allocation

The per-frame uniform ring buffer (`VulkanRingBuffer`) handles alignment automatically when the correct alignment value is passed:

```cpp
class VulkanRingBuffer {
public:
    struct Allocation {
        vk::DeviceSize offset;  // Aligned offset within buffer
        void* mapped;           // Pointer to host-visible memory
    };

    // Allocate with explicit alignment override
    Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);

    // Try to allocate (returns nullopt if insufficient space)
    std::optional<Allocation> try_allocate(
        vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);

    // Reset for next frame
    void reset();

    // Accessors
    vk::Buffer buffer() const;
    vk::DeviceSize size() const;
    vk::DeviceSize remaining() const;
};
```

**Best Practice**: Always use `renderer.getMinUniformOffsetAlignment()` as the alignment parameter:

```cpp
const vk::DeviceSize alignment =
    static_cast<vk::DeviceSize>(renderer.getMinUniformOffsetAlignment());
auto alloc = frame.uniformBuffer().allocate(structSize, alignment);
// alloc.offset is guaranteed to be aligned to 'alignment'
```

---

## 8. Troubleshooting

### Symptom: Corrupted or Garbage Uniform Data

**Possible Causes**:
1. **Mismatched struct layout**: C++ struct doesn't match GLSL uniform block
2. **Missing padding**: Struct size not a multiple of 16 bytes
3. **Array alignment**: Forgot that arrays use 16-byte per-element alignment

**Debugging Steps**:
1. Add `layout(offset = N)` qualifiers in GLSL to verify expected offsets
2. Print `sizeof(YourStruct)` and compare to expected size
3. Use RenderDoc to inspect buffer contents at the byte level
4. Enable Vulkan validation layers for descriptor validation

### Symptom: Validation Error on Dynamic Offset

**Cause**: Dynamic offset not aligned to `minUniformBufferOffsetAlignment`.

**Solution**:
```cpp
// WRONG - using struct size directly
uint32_t offset = modelIndex * sizeof(model_uniform_data);

// CORRECT - round up to device alignment
const size_t align = renderer.getMinUniformOffsetAlignment();
const size_t alignedSize = (sizeof(model_uniform_data) + align - 1) & ~(align - 1);
uint32_t offset = modelIndex * alignedSize;
```

### Symptom: Push Descriptor Update Has No Effect

**Possible Causes**:
1. Pushed to wrong set index
2. Binding number mismatch between push and shader
3. Pipeline layout doesn't match push descriptor layout

**Debugging**:
1. Verify pipeline layout creation specifies `VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR`
2. Check that binding numbers in `vk::WriteDescriptorSet` match shader declarations
3. Ensure push happens after pipeline bind but before draw

### Symptom: Values Appear Shifted or Offset

**Cause**: vec3 followed by vec4 without considering std140 alignment.

**Example of Bug**:
```cpp
struct buggy {
    vec3d position;  // offset 0, size 12
    vec4 color;      // Expected offset 12, actual offset 16 (std140 rounds up)
};
```

**Solution**: Either reorder fields or add explicit padding:
```cpp
struct fixed {
    vec3d position;  // offset 0, size 12
    float pad;       // offset 12, size 4
    vec4 color;      // offset 16, correctly aligned
};
```

### Symptom: Type Mismatch Warnings

**Cause**: C++ and GLSL types don't match exactly.

**Common Mismatches**:
| C++ Type | GLSL Type | Issue |
|----------|-----------|-------|
| `float` | `uint` | Different bit interpretation |
| `int` | `bool` | GLSL bool is 4 bytes but behavior differs |
| `vec3d` (double) | `vec3` (float) | Size mismatch |

**Solution**: Ensure types match exactly. The codebase uses `vec3d` which maps to 3 floats (not doubles despite the name), but verify for your specific platform.

---

## Appendix A: Quick Reference Tables

### Uniform Block Sizes

| Struct | Size (bytes) | Alignment Notes |
|--------|--------------|-----------------|
| `matrix_uniforms` | 128 | 2 x mat4 |
| `model_uniform_data` | ~1216 | Variable, check sizeof() |
| `nanovg_draw_data` | 192 | Contains mat3 as 3 vec4s |
| `deferred_light_data` | 80 | vec3 + scalar packing used |
| `deferred_global_data` | 416 | 6 x mat4 + scalars |
| `decal_globals` | 272 | 4 x mat4 + vec2 |
| `decal_info` | 32 | Packed ints |

### Binding Quick Reference

| Pipeline | Binding 0 | Binding 1 | Binding 2 | Binding 3+ |
|----------|-----------|-----------|-----------|------------|
| Standard | matrixData | genericData | Textures | - |
| Model | VertexBuffer | Textures | ModelUniforms | TransformBuffer |
| Deferred | matrixData | lightData | - | (Set 1: G-buffer) |
| Decal | decalGlobalData | decalInfoData | Textures | - |

---

## References

### External Documentation
- [OpenGL std140 Layout Rules](https://www.khronos.org/opengl/wiki/Interface_Block_(GLSL)#Explicit_variable_layout)
- [Vulkan Specification: Uniform Buffer Alignment](https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#limits-minUniformBufferOffsetAlignment)
- [Vulkan Specification: Push Descriptors](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_KHR_push_descriptor.html)

### Source Code
- `code/graphics/util/uniform_structs.h` - C++ struct definitions
- `code/graphics/2d.h` - Enum definitions (`uniform_block_type`, `shader_type`)
- `code/graphics/vulkan/VulkanRingBuffer.h` - Per-frame ring buffer
- `code/graphics/vulkan/VulkanRenderer.h` - Renderer API
- `code/graphics/vulkan/VulkanFrame.h` - Per-frame state

### Shader Files (Vulkan)
- `code/graphics/shaders/model.vert` - Model vertex shader (vertex pulling)
- `code/graphics/shaders/model.frag` - Model fragment shader (bindless textures)
- `code/graphics/shaders/deferred.frag` - Deferred lighting fragment shader
- `code/graphics/shaders/deferred.vert` - Deferred lighting vertex shader
- `code/graphics/shaders/default-material.vert` - Standard pipeline vertex shader
- `code/graphics/shaders/nanovg.frag` - NanoVG fragment shader
- `code/graphics/shaders/decal_common.glsl` - Decal shader common code

### Related Documentation
- `docs/VULKAN_UNIFORM_ALIGNMENT.md` - Detailed alignment rules and examples
- `docs/VULKAN_PIPELINE_MANAGEMENT.md` - Pipeline creation and caching
- `docs/VULKAN_DEFERRED_LIGHTING_FLOW.md` - Deferred lighting architecture
- `docs/VULKAN_DESCRIPTOR_SETS.md` - Descriptor set management
- `docs/VULKAN_MODEL_RENDERING_PIPELINE.md` - Model rendering details
