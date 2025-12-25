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

---

## 1. Overview

Uniform buffers in the Vulkan renderer are defined in `code/graphics/util/uniform_structs.h`. These structs must conform to **std140 layout rules** to ensure compatibility between C++ host code and GLSL shaders.

### Key Files

| File | Purpose |
|------|---------|
| `code/graphics/util/uniform_structs.h` | C++ struct definitions (std140 layout) |
| `code/graphics/2d.h` | `uniform_block_type` enum and binding point definitions |
| `code/graphics/shaders/*.vert` / `*.frag` | GLSL shader uniform block declarations |

### std140 Layout Summary

- All scalars and vectors are aligned to their size (vec4 = 16 bytes)
- Arrays and structs are aligned to 16 bytes
- Matrices are treated as arrays of vec4 (16-byte aligned)
- Struct members are padded to meet alignment requirements

**Critical Rule**: Everything must be **16-byte aligned**. Padding fields (`pad`, `pad0`, etc.) are used to enforce this.

---

## 2. Uniform Block Type to Shader Binding Mapping

The `uniform_block_type` enum is defined in `code/graphics/2d.h`. These enum values are primarily used by the OpenGL renderer as UBO binding indices. In Vulkan, shaders declare explicit `layout(binding = N)` bindings that may differ from the enum values.

### OpenGL Enum Values (`uniform_block_type`)

| Enum Name | Enum Value | C++ Struct | Usage |
|-----------|------------|------------|-------|
| `Lights` | 0 | (deprecated) | Legacy lighting (not used) |
| `ModelData` | 1 | `model_uniform_data` | 3D model rendering |
| `NanoVGData` | 2 | `nanovg_draw_data` | NanoVG UI rendering |
| `DecalInfo` | 3 | `decal_info` | Decal rendering |
| `DecalGlobals` | 4 | `decal_globals` | Decal global state |
| `DeferredGlobals` | 5 | `deferred_global_data` | Deferred lighting globals |
| `Matrices` | 6 | `matrix_uniforms` | View/projection matrices |
| `MovieData` | 7 | `movie_uniforms` | Movie playback (OpenGL only) |
| `GenericData` | 8 | Various (see below) | Shader-specific data |

### Vulkan Shader Bindings (Standard Pipeline)

Most Vulkan shaders use the standard 2D/post-processing layout:

| Binding | Shader Block Name | C++ Struct | Usage |
|---------|-------------------|------------|-------|
| 0 | `matrixData` | `matrix_uniforms` | View/projection matrices |
| 1 | `genericData` | Various | Shader-specific data |
| 2+ | samplers | - | Texture bindings |

**Note**: Vulkan uses push descriptors (`vkCmdPushDescriptorSetKHR`) for most standard pipeline uniform blocks, making the binding numbers in shaders the authoritative reference.

---

## 3. Uniform Struct Definitions

### 3.1 Model Uniform Data (`model_uniform_data`)

**Binding**: Set 0, Binding 2 (Model pipeline layout - see `model.vert`)
**Usage**: Per-draw model rendering data (via `ModelUniforms` block in shaders)
**Size**: Variable (must be aligned to `minUniformBufferOffsetAlignment`)

```cpp
struct model_uniform_data {
    matrix4 modelViewMatrix;
    matrix4 modelMatrix;
    matrix4 viewMatrix;
    matrix4 projMatrix;
    matrix4 textureMatrix;
    matrix4 shadow_mv_matrix;
    matrix4 shadow_proj_matrix[4];
    
    vec4 color;
    model_light lights[MAX_UNIFORM_LIGHTS];  // MAX_UNIFORM_LIGHTS = 8
    
    // ... many more fields ...
};
```

**Key Points**:
- Used with **dynamic uniform buffer offsets** (`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`)
- Multiple model draws pack their uniform data into a single buffer with aligned offsets
- Binding is updated per-draw via `setModelUniformBinding()`

### 3.2 Matrix Uniforms (`matrix_uniforms`)

**Binding**: Set 0, Binding 0 (Standard pipeline layout)  
**Usage**: View/projection matrices for 2D/UI/post-processing  
**Struct**:
```cpp
struct matrix_uniforms {
    matrix4 modelViewMatrix;
    matrix4 projMatrix;
};
```

### 3.3 Generic Data (`genericData`)

**Binding**: Set 0, Binding 1 (Standard pipeline layout)  
**Usage**: Shader-specific uniform data, varies by shader type

The `genericData` binding uses different structs depending on the shader:

| Shader Type | Struct | Purpose |
|-------------|--------|---------|
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | `generic_data::tonemapping_data` | HDR tonemapping parameters |
| `SDR_TYPE_POST_PROCESS_BLUR` | `generic_data::blur_data` | Blur kernel size and mip level |
| `SDR_TYPE_POST_PROCESS_BLOOM_COMP` | `generic_data::bloom_composition_data` | Bloom intensity and mip levels |
| `SDR_TYPE_POST_PROCESS_FXAA` | `generic_data::fxaa_data` | FXAA render target dimensions |
| `SDR_TYPE_POST_PROCESS_SMAA_*` | `generic_data::smaa_data` | SMAA render target metrics |
| `SDR_TYPE_POST_PROCESS_LIGHTSHAFTS` | `generic_data::lightshaft_data` | Lightshaft parameters |
| `SDR_TYPE_POST_PROCESS_MAIN` | `generic_data::post_data` | Post-processing effects (noise, saturation, etc.) |
| `SDR_TYPE_PASSTHROUGH_RENDER` | `generic_data::passthrough_data` | Passthrough rendering flags |
| `SDR_TYPE_SHIELD_DECAL` | `generic_data::shield_impact_data` | Shield impact rendering |
| `SDR_TYPE_ROCKET_UI` | `generic_data::rocketui_data` | Rocket UI rendering |
| `SDR_TYPE_SCENE_FOG` | `generic_data::fog_data` | Scene fog parameters |
| `SDR_TYPE_VOLUMETRIC_FOG` | `generic_data::volumetric_fog_data` | Volumetric fog parameters |
| `SDR_TYPE_BATCHED_BITMAP` | `generic_data::batched_data` | Batched bitmap rendering |
| `SDR_TYPE_FLAT_COLOR` | `generic_data::flat_color_data` | Flat color rendering |

**Important**: All `generic_data` structs must be 16-byte aligned. Padding fields ensure this.

### 3.4 Deferred Lighting Uniforms

**Deferred Globals** (`deferred_global_data`):
- **Binding**: Set 1, Bindings 0-5 (G-buffer textures as samplers)
- Set 1 contains: ColorBuffer, NormalBuffer, PositionBuffer, DepthBuffer, SpecularBuffer, EmissiveBuffer

**Deferred Light Data** (`deferred_light_data`):
- **Binding**: Set 0, Binding 1 (via `lightData` uniform block in `deferred.frag`)
- Per-light data including color, direction, cone angles, light type, radius
- Matrix data at Set 0, Binding 0 provides light position via `modelViewMatrix[3].xyz`

### 3.5 Other Uniform Structs

- **`nanovg_draw_data`**: NanoVG UI rendering (binding 1 as `NanoVGUniformData` in `nanovg.frag`)
- **`decal_info`**: Per-decal texture indices and blend modes (OpenGL binding 3)
- **`decal_globals`**: Decal view/projection matrices (OpenGL binding 4)
- **`movie_uniforms`**: Movie playback alpha (OpenGL only; Vulkan uses push constants in `movie.frag`)

---

## 4. std140 Layout Rules

### Rule 1: Base Alignment

| Type | Base Alignment |
|------|----------------|
| `float`, `int`, `bool` | 4 bytes |
| `vec2` | 8 bytes |
| `vec3`, `vec4` | 16 bytes |
| `mat4` (4x4 matrix) | 16 bytes (each column) |
| Array | 16 bytes (each element) |
| Struct | 16 bytes (rounded up) |

### Rule 2: Struct Member Alignment

Each struct member is placed at an offset that is a multiple of its base alignment. Padding is inserted as needed.

**Example**:
```cpp
struct example {
    vec3d position;      // offset 0, size 12
    float pad;          // offset 12, size 4 (padding to reach 16-byte boundary)
    vec4 color;         // offset 16, size 16
    int count;          // offset 32, size 4
    float pad2[3];      // offset 36, size 12 (padding so mat4 starts at 16-byte boundary)
    mat4 transform;     // offset 48, size 64 (4 vec4s)
};  // Total size: 112 bytes (multiple of 16)
```

### Rule 3: Array Alignment

Arrays of scalars/vectors are aligned to 16 bytes per element. Arrays of structs align each element to 16 bytes.

**Example**:
```cpp
vec3d positions[4];  // Each vec3 takes 16 bytes (12 data + 4 padding)
```

### Rule 4: Matrix Layout

Matrices are stored as arrays of column vectors (column-major). A `mat4` is 4 `vec4`s = 64 bytes total.

---

## 5. Binding Strategy by Pipeline Layout

### Standard Pipeline Layout

Used for 2D, UI, particles, post-processing:

| Binding | Type | Uniform Block | Update Frequency |
|---------|------|---------------|------------------|
| 0 | Push Descriptors | `matrixData` (`matrix_uniforms`) | Per-draw |
| 1 | Push Descriptors | `genericData` (varies by shader) | Per-draw |
| 2+ | Push Descriptors | Texture samplers | Per-draw |

**Descriptor Strategy**: Push descriptors (`vkCmdPushDescriptorSetKHR`) - no persistent descriptor sets.

### Model Pipeline Layout

Used for 3D model rendering (vertex pulling architecture - see `model.vert`):

| Set | Binding | Type | Content | Update Frequency |
|-----|---------|------|---------|------------------|
| 0 | 0 | Storage Buffer | `VertexBuffer` (vertex pulling data) | Per-frame |
| 0 | 1 | Sampler Array | `textures[]` (bindless texture array) | Per-frame |
| 0 | 2 | Uniform Buffer | `ModelUniforms` (subset of `model_uniform_data`) | Per-draw |
| 0 | 3 | Storage Buffer | `TransformBuffer` (batched transforms) | Per-frame |

**Descriptor Strategy**:
- Set 0: Pre-allocated descriptor set with per-frame updates
- Push constants (`ModelPushConstants`) carry per-draw data: vertex offsets, texture indices, flags
- Model uniform binding updated via `setModelUniformBinding()` with dynamic offset

### Deferred Pipeline Layout

Used for deferred lighting (see `deferred.frag`):

| Set | Binding | Type | Content | Update Frequency |
|-----|---------|------|---------|------------------|
| 0 | 0 | Uniform Buffer | `matrixData` (light transforms) | Per-light |
| 0 | 1 | Uniform Buffer | `lightData` (deferred light parameters) | Per-light |
| 1 | 0-5 | Samplers | G-buffer textures (6 attachments) | Per-frame |

**G-buffer Attachments** (Set 1):
- Binding 0: ColorBuffer
- Binding 1: NormalBuffer
- Binding 2: PositionBuffer
- Binding 3: DepthBuffer
- Binding 4: SpecularBuffer
- Binding 5: EmissiveBuffer

**Descriptor Strategy**:
- Set 0: Push descriptors for per-light uniform data
- Set 1: Pre-allocated descriptor set bound via `bindDeferredGlobalDescriptors()`

---

## 6. Usage Patterns

### Pattern 1: Per-Draw Uniforms (Standard Layout)

```cpp
// Allocate from per-frame ring buffer with device alignment
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(renderer.getMinUniformOffsetAlignment());
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(matrix_uniforms), uboAlignment);

// Copy uniform data to mapped memory
std::memcpy(uniformAlloc.mapped, &matrices, sizeof(matrices));

// Create descriptor buffer info
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;  // Already aligned by allocate()
matrixInfo.range = sizeof(matrix_uniforms);

// Push descriptor via vkCmdPushDescriptorSetKHR
vk::WriteDescriptorSet write{};
write.dstBinding = 0;
write.descriptorCount = 1;
write.descriptorType = vk::DescriptorType::eUniformBuffer;
write.pBufferInfo = &matrixInfo;
cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, pipelineLayout, 0, write);
```

### Pattern 2: Dynamic Uniform Buffer (Model Layout)

```cpp
// Set model uniform binding (updates frame.modelUniformBinding)
renderer.setModelUniformBinding(frame, bufferHandle, offset, size);

// The renderer binds the model descriptor set with dynamic offset internally.
// Model rendering uses push constants (ModelPushConstants) for per-draw data
// such as vertex offsets, texture indices, and flags.
//
// Example push constant structure (from model.vert):
//   uint vertexOffset, stride, vertexAttribMask
//   uint posOffset, normalOffset, texCoordOffset, tangentOffset, modelIdOffset
//   uint boneIndicesOffset, boneWeightsOffset
//   uint baseMapIndex, glowMapIndex, normalMapIndex, specMapIndex
//   uint matrixIndex, flags
```

### Pattern 3: Packed Uniforms (Multiple Blocks)

Some shaders use multiple uniform blocks packed into a single allocation:

```cpp
// Allocate space for both matrixData and genericData
const size_t matrixSize = sizeof(matrix_uniforms);
const size_t genericSize = sizeof(generic_data::post_data);
const size_t totalSize = matrixSize + genericSize;
auto uniformAlloc = frame.uniformBuffer().allocate(totalSize, uboAlignment);

// Copy both structs
std::memcpy(uniformAlloc.mapped, &matrices, matrixSize);
std::memcpy(static_cast<char*>(uniformAlloc.mapped) + matrixSize, &generic, genericSize);

// Push descriptors with different offsets
vk::DescriptorBufferInfo matrixInfo{};
matrixInfo.buffer = frame.uniformBuffer().buffer();
matrixInfo.offset = uniformAlloc.offset;
matrixInfo.range = matrixSize;

vk::DescriptorBufferInfo genericInfo{};
genericInfo.buffer = frame.uniformBuffer().buffer();
genericInfo.offset = uniformAlloc.offset + matrixSize;
genericInfo.range = genericSize;
```

---

## 7. Alignment Requirements

### Device Alignment

Vulkan devices require uniform buffer offsets to be aligned to `minUniformBufferOffsetAlignment` (typically 256 bytes on desktop GPUs).

**Impact**: When using dynamic uniform buffers, each `model_uniform_data` instance must be placed at an offset that is a multiple of this alignment value.

### std140 Alignment

Uniform structs must also respect std140 alignment rules (16-byte alignment for all members).

**Example**: `model_uniform_data` is padded to ensure it's a multiple of 16 bytes, but when placed in a dynamic buffer, it must also be aligned to `minUniformBufferOffsetAlignment`.

### Ring Buffer Allocation

The per-frame uniform ring buffer (`VulkanRingBuffer` in `code/graphics/vulkan/VulkanRingBuffer.h`) handles alignment:

```cpp
// VulkanRingBuffer::Allocation structure
struct Allocation {
    vk::DeviceSize offset;  // Aligned offset within the buffer
    void* mapped;           // Pointer to host-visible memory
};

// Usage
const vk::DeviceSize alignment =
    static_cast<vk::DeviceSize>(renderer.getMinUniformOffsetAlignment());
auto alloc = frame.uniformBuffer().allocate(size, alignment);
// alloc.offset is guaranteed to be aligned to 'alignment'
// alloc.mapped points to the host-visible memory for writing
```

**Best Practice**: Always use `getMinUniformOffsetAlignment()` (typically 256 bytes) for uniform buffer allocations, even if the struct size is already std140-compliant (16-byte aligned).

---

## Appendix: Common Pitfalls

### Pitfall 1: Incorrect vec3 Alignment Understanding

**Clarification**: In std140, a scalar (float/int) CAN follow a vec3 and pack into its remaining 4 bytes. This is a valid optimization used in `deferred_light_data`.

```cpp
// VALID - scalar packs into vec3's remaining bytes
struct valid {
    vec3d pos;       // offset 0, size 12 (occupies 16 bytes in std140)
    float scale;     // offset 12, size 4 (packs into vec3's slot)
    // Total: 16 bytes - this is CORRECT
};

// WRONG - forgetting struct size must be multiple of 16
struct bad {
    vec3d pos;       // offset 0, size 12
    float scale;     // offset 12, size 4
    int flags;       // offset 16, size 4
    // Total: 20 bytes - NOT a multiple of 16!
};

// CORRECT - add padding to reach 16-byte struct size
struct good {
    vec3d pos;       // offset 0, size 12
    float scale;     // offset 12, size 4
    int flags;       // offset 16, size 4
    float pad[3];    // offset 20, size 12 -> total 32 bytes
};
```

### Pitfall 2: Incorrect Dynamic Offset Alignment

**Problem**: Using struct size instead of device alignment for dynamic offsets.

```cpp
// WRONG - struct size may not meet device alignment requirements
uint32_t offset = modelIndex * sizeof(model_uniform_data);

// CORRECT - use device alignment from renderer
const size_t deviceAlign = renderer.getMinUniformOffsetAlignment();
const size_t alignedSize = (sizeof(model_uniform_data) + deviceAlign - 1) & ~(deviceAlign - 1);
uint32_t offset = modelIndex * alignedSize;
```

### Pitfall 3: Mismatched Struct and Shader Layout

**Problem**: C++ struct doesn't match GLSL uniform block layout.

**Solution**: Always verify struct layout matches shader declaration. Use `std140` explicitly in shaders:
```glsl
layout(binding = 1, std140) uniform genericData {
    // ... must match C++ struct exactly
};
```

---

## References

### External Documentation
- [OpenGL std140 Layout Rules](https://www.khronos.org/opengl/wiki/Interface_Block_(GLSL)#Explicit_variable_layout)
- [Vulkan Uniform Buffer Alignment](https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#limits-minUniformBufferOffsetAlignment)

### Source Code
- `code/graphics/util/uniform_structs.h` - C++ struct definitions for all uniform blocks
- `code/graphics/2d.h` - `uniform_block_type` enum and `shader_type` enum
- `code/graphics/vulkan/VulkanRingBuffer.h` - Per-frame ring buffer for uniform allocations
- `code/graphics/vulkan/VulkanRenderer.h` - Renderer API including `setModelUniformBinding()`
- `code/graphics/vulkan/VulkanFrame.h` - Per-frame state including uniform bindings

### Shader Files (Vulkan)
- `code/graphics/shaders/model.vert` - Model vertex shader with vertex pulling
- `code/graphics/shaders/model.frag` - Model fragment shader with bindless textures
- `code/graphics/shaders/deferred.frag` - Deferred lighting fragment shader
- `code/graphics/shaders/default-material.vert` - Standard pipeline vertex shader
- `code/graphics/shaders/nanovg.frag` - NanoVG fragment shader

### Related Documentation
- `docs/vulkan/VULKAN_UNIFORM_ALIGNMENT.md` - Detailed alignment rules and examples
- `docs/vulkan/VULKAN_PIPELINE_MANAGEMENT.md` - Pipeline creation and caching
- `docs/vulkan/VULKAN_DEFERRED_LIGHTING_FLOW.md` - Deferred lighting architecture

