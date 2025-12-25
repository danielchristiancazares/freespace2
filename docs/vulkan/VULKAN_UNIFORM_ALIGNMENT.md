# Vulkan Uniform Buffer Alignment & std140 Rules

This document explains how uniform buffer structs must be aligned for Vulkan, the std140 layout rules, and how to correctly add new uniform structs to the renderer.

---

## Table of Contents

1. [Overview: Why Alignment Matters](#1-overview-why-alignment-matters)
2. [std140 Layout Rules](#2-std140-layout-rules)
3. [Struct-Level Alignment](#3-struct-level-alignment)
4. [Device Limits: minUniformBufferOffsetAlignment](#4-device-limits-minuniformbufferoffsetalignment)
5. [How Alignment is Enforced](#5-how-alignment-is-enforced)
6. [Adding New Uniform Structs](#6-adding-new-uniform-structs)
7. [Common Alignment Mistakes](#7-common-alignment-mistakes)
8. [Examples from the Codebase](#8-examples-from-the-codebase)
9. [Validation and Debugging](#9-validation-and-debugging)

---

## 1. Overview: Why Alignment Matters

Uniform buffers in Vulkan must respect two alignment requirements:

1. **std140 layout rules**: GLSL uniform buffer layout standard that ensures consistent memory layout across different GPUs and drivers.
2. **Device alignment limits**: Vulkan devices require uniform buffer offsets to be aligned to `minUniformBufferOffsetAlignment` (typically 256 bytes, but can be as low as 16).

**Why this matters:**
- Misaligned data causes undefined behavior (garbage values, crashes, corruption)
- GPU hardware reads uniform data in aligned chunks (16-byte or larger)
- Different GPUs may interpret misaligned data differently
- Validation layers will catch some alignment errors, but not all

**Key Point**: The struct itself must be std140-compliant, AND the offset where the struct starts in the buffer must be aligned to `minUniformBufferOffsetAlignment`.

---

## 2. std140 Layout Rules

The std140 layout rules are defined in the [OpenGL specification](https://www.khronos.org/opengl/wiki/Interface_Block_(GLSL)#Explicit_variable_layout). Here's the TL;DR:

### Base Alignment Rules

| Type | Base Alignment | Size |
|------|----------------|------|
| `float` | 4 bytes | 4 bytes |
| `int` | 4 bytes | 4 bytes |
| `vec2` | 8 bytes | 8 bytes |
| `vec3` | 16 bytes | 12 bytes |
| `vec4` | 16 bytes | 16 bytes |
| `mat3` | 16 bytes (per column) | 48 bytes (3 columns) |
| `mat4` | 16 bytes (per column) | 64 bytes (4 columns) |
| Arrays | Round up to 16-byte boundary | Element size × count |

### Struct Alignment Rules

1. **Struct base alignment**: The largest base alignment of any member (minimum 16 bytes)
2. **Struct size**: Rounded up to a multiple of the struct's base alignment
3. **Member offset**: Must be a multiple of the member's base alignment
4. **Array elements**: Each element aligned to 16 bytes (even if element type is smaller)

### Examples

**Example 1: Simple struct**
```cpp
struct simple_data {
    float x;        // offset 0, size 4
    float pad1[3];  // offset 4, size 12 (pads to 16)
    vec3d y;        // offset 16, size 12
    float pad2;     // offset 28, size 4 (pads to 32)
};
// Total size: 32 bytes (aligned to 16)
```

**Example 2: Struct with vec3**
```cpp
struct vec3_example {
    vec3d position;  // offset 0, size 12, but occupies 16 bytes
    float value;     // offset 16, size 4
    // Implicit padding: offset 20-31 (12 bytes)
};
// Total size: 32 bytes (vec3 forces 16-byte alignment)
```

**Example 3: Matrix**
```cpp
struct matrix_example {
    matrix4 transform;  // offset 0, size 64 (4 columns × 16 bytes each)
    vec4 color;         // offset 64, size 16
};
// Total size: 80 bytes (aligned to 16)
```

---

## 3. Struct-Level Alignment

Every uniform struct must:
1. Have a size that is a multiple of 16 bytes
2. Have each member aligned according to std140 rules
3. End with explicit padding if needed

### Calculating Struct Size

**Step 1**: Identify the largest base alignment in the struct (minimum 16)
**Step 2**: Calculate the size of all members with padding
**Step 3**: Round up to a multiple of the struct's base alignment

**Example: `deferred_light_data`**

In std140 layout, a `vec3` has 16-byte alignment but only 12 bytes of actual data. A scalar (like `float`) following a `vec3` can pack into the remaining 4 bytes if it fits. Here's how `deferred_light_data` actually lays out:

```cpp
struct deferred_light_data {
    vec3d diffuseLightColor;  // offset 0, size 12 (16-byte aligned)
    float coneAngle;          // offset 12, size 4 (packs into vec3's padding)

    vec3d lightDir;           // offset 16, size 12 (16-byte aligned)
    float coneInnerAngle;     // offset 28, size 4 (packs into vec3's padding)

    vec3d coneDir;            // offset 32, size 12 (16-byte aligned)
    float dualCone;           // offset 44, size 4 (packs into vec3's padding)

    vec3d scale;              // offset 48, size 12 (16-byte aligned)
    float lightRadius;        // offset 60, size 4 (packs into vec3's padding)

    int lightType;            // offset 64, size 4
    int enable_shadows;       // offset 68, size 4
    float sourceRadius;       // offset 72, size 4
    float pad0[1];            // offset 76, size 4 (pads to 80 bytes)
    // Total: 80 bytes (5 × 16 = 80, correctly 16-byte aligned)
};
```

**Key insight**: The `vec3d + float` pairs pack efficiently because the float fits into the 4 bytes of padding that would otherwise follow the vec3. The struct totals 80 bytes, which IS a multiple of 16, so `pad0[1]` is sufficient.

---

## 4. Device Limits: minUniformBufferOffsetAlignment

Vulkan devices require uniform buffer offsets to be aligned to `minUniformBufferOffsetAlignment`. This is typically **256 bytes** but can be as low as **16 bytes**.

### How It's Used

When binding a uniform buffer with a dynamic offset:

```cpp
// Get device alignment requirement
const size_t alignment = renderer.getMinUniformOffsetAlignment();  // e.g., 256

// Allocate uniform data with alignment
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(myStruct), alignment);

// uniformAlloc.offset is guaranteed to be aligned to 'alignment'
// uniformAlloc.mapped points to the host-visible memory
```

### Key Points

- **Struct size** must be a multiple of 16 (std140)
- **Buffer offset** must be a multiple of `minUniformBufferOffsetAlignment` (device limit)
- These are **independent** requirements

**Example:**
```cpp
struct my_uniform {
    vec4 data;
    float value;
    float pad[3];  // Pad to 32 bytes (16-byte aligned)
};
// sizeof(my_uniform) = 32 bytes (OK for std140)

// But when binding:
const size_t deviceAlign = 256;  // Device requirement
size_t offset = 0;  // Start of buffer
offset = align_up(offset, deviceAlign);  // offset = 256
// Now offset is valid for binding
```

---

## 5. How Alignment is Enforced

### VulkanRingBuffer (Vulkan Renderer)

The Vulkan renderer uses `VulkanRingBuffer` (`code/graphics/vulkan/VulkanRingBuffer.h`) for per-frame uniform allocations. Each frame has a ring buffer that sub-allocates aligned regions:

```cpp
// Allocate uniform data with device alignment
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(myStruct), uboAlignment);

// uniformAlloc.offset is guaranteed to be aligned to 'uboAlignment'
// uniformAlloc.mapped points to the host-visible memory
std::memcpy(uniformAlloc.mapped, &myData, sizeof(myData));
```

The `VulkanRingBuffer::Allocation` struct contains:
- `offset`: The aligned offset within the buffer (use for descriptor binding)
- `mapped`: Pointer to host-visible memory (use for writing data)

### UniformAligner (OpenGL Only)

Note: The `UniformAligner` class (`code/graphics/util/UniformAligner.h`) is used by the **OpenGL** renderer, not Vulkan. It provides similar functionality for managing aligned uniform buffer allocations in the OpenGL deferred lighting path.

### Runtime Validation

The Vulkan renderer validates offsets before binding:

```cpp
void VulkanRenderer::setModelUniformBinding(VulkanFrame& frame,
                                            gr_buffer_handle handle,
                                            size_t offset,
                                            size_t size)
{
    const auto alignment = getMinUniformOffsetAlignment();
    const auto dynOffset = static_cast<uint32_t>(offset);

    Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
    Assertion((dynOffset % alignment) == 0,
              "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
    // ... bind uniform buffer
}
```

**What this catches:**
- Offsets that aren't aligned to device limits
- Does NOT catch std140 struct layout errors (compile-time issue)

### Compile-Time Checks

There's no automatic compile-time validation of std140 compliance. You must:
1. Manually verify struct sizes are multiples of 16
2. Use `static_assert` to verify size:

```cpp
struct my_uniform {
    vec4 data;
    float pad[3];
};
static_assert(sizeof(my_uniform) % 16 == 0,
              "my_uniform must be 16-byte aligned for std140");
```

---

## 6. Adding New Uniform Structs

### Step-by-Step Guide

**Step 1: Define the struct in `uniform_structs.h`**

```cpp
namespace graphics {
namespace generic_data {

struct my_new_uniform {
    vec4 color;
    float intensity;
    int flags;
    float pad[2];  // Pad to 32 bytes (16-byte aligned)
};

} // namespace generic_data
} // namespace graphics
```

**Step 2: Verify alignment**

```cpp
// Add static assert after struct definition
static_assert(sizeof(graphics::generic_data::my_new_uniform) % 16 == 0,
              "my_new_uniform must be 16-byte aligned for std140");
static_assert(sizeof(graphics::generic_data::my_new_uniform) == 32,
              "my_new_uniform size mismatch");
```

**Step 3: Allocate from VulkanRingBuffer**

```cpp
// In rendering code
const vk::DeviceSize deviceAlign =
    static_cast<vk::DeviceSize>(renderer.getMinUniformBufferAlignment());
auto uniformAlloc = frame.uniformBuffer().allocate(
    sizeof(graphics::generic_data::my_new_uniform),
    deviceAlign
);

// Write data (option A: memcpy from stack variable)
graphics::generic_data::my_new_uniform myData{};
myData.color = vec4{1.0f, 0.0f, 0.0f, 1.0f};
myData.intensity = 0.5f;
myData.flags = 0;
std::memcpy(uniformAlloc.mapped, &myData, sizeof(myData));

// Or (option B: write directly to mapped memory)
auto* data = reinterpret_cast<graphics::generic_data::my_new_uniform*>(
    uniformAlloc.mapped
);
data->color = vec4{1.0f, 0.0f, 0.0f, 1.0f};
data->intensity = 0.5f;
data->flags = 0;
```

**Step 4: Bind with aligned offset**

```cpp
vk::DescriptorBufferInfo bufferInfo{};
bufferInfo.buffer = frame.uniformBuffer().buffer();
bufferInfo.offset = uniformAlloc.offset;  // Already aligned
bufferInfo.range = sizeof(graphics::generic_data::my_new_uniform);

// Push descriptor or bind set
cmd.pushDescriptorSetKHR(..., bufferInfo, ...);
```

### Checklist

- [ ] Struct size is a multiple of 16 bytes
- [ ] All vec3 members are followed by a scalar or explicit padding (see packing rules in Section 3)
- [ ] Arrays of scalars are properly aligned (each element to 16 bytes in std140)
- [ ] Matrices are properly aligned (each column to 16 bytes)
- [ ] Added `static_assert` for size validation
- [ ] Used `VulkanRingBuffer::allocate()` with device alignment for buffer offsets
- [ ] Verified offset alignment before binding

---

## 7. Common Alignment Mistakes

### Mistake 1: Misunderstanding vec3 Packing

In std140, a scalar following a vec3 CAN pack into the remaining 4 bytes. This is valid:

```cpp
struct valid_struct {
    vec3d position;  // offset 0, size 12 (16-byte aligned)
    float value;     // offset 12, size 4 (packs into vec3's remaining space)
    // Total: 16 bytes - correctly aligned!
};
```

The mistake is assuming the struct needs more padding when it doesn't, OR assuming you'll get 16-byte padding automatically:

**Potential mistake:**
```cpp
struct maybe_wrong {
    vec3d position;  // offset 0, size 12
    int value;       // offset 12, size 4 (packs after vec3)
    float more;      // offset 16, size 4
    // Total: 20 bytes - NOT a multiple of 16!
};
```

**Fixed:**
```cpp
struct fixed_struct {
    vec3d position;  // offset 0, size 12
    int value;       // offset 12, size 4
    float more;      // offset 16, size 4
    float pad[3];    // offset 20, size 12 -> total 32 bytes
};
```

### Mistake 2: Assuming C++ Struct Layout Matches std140

**Wrong:**
```cpp
struct bad_struct {
    float x, y, z;  // C++ packs these tightly (12 bytes)
    float w;        // Total: 16 bytes
};
// But std140 requires each vec3 to be 16-byte aligned!
```

**Right:**
```cpp
struct good_struct {
    vec3d xyz;      // 12 bytes, but occupies 16
    float w;        // Starts at offset 16
    float pad[3];   // Pad to 32 bytes
};
```

### Mistake 3: Incorrect Array Alignment

In std140, arrays of scalars have each element aligned to 16 bytes. This is different from C++ arrays!

**Problem:**
```cpp
// C++ struct
struct bad_struct {
    float values[3];  // C++ sees 12 bytes, but std140 sees 48 bytes!
    int count;        // C++ offset 12, but std140 offset 48!
};
```

**Solution - use vec4 instead of float arrays:**
```cpp
struct good_struct {
    vec4 values;      // 16 bytes (stores up to 4 floats naturally)
    int count;
    float pad[3];     // Pad C++ sizeof to 32 bytes
};
```

**Or match std140 array layout explicitly:**
```cpp
struct good_struct_array {
    float value0; float _p0[3];  // Element 0 at offset 0
    float value1; float _p1[3];  // Element 1 at offset 16
    float value2; float _p2[3];  // Element 2 at offset 32
    int count;                    // offset 48
    float pad[3];                 // Pad to 64 bytes
};
```

**Note**: Trailing padding arrays (`float pad[3]`) are not read by shaders - they just ensure the C++ struct size is a multiple of 16.

### Mistake 4: Mixing Device Alignment with Struct Alignment

**Wrong:**
```cpp
// Assuming device alignment is 256, struct is 32 bytes
size_t offset = 32;  // Struct-aligned, but NOT device-aligned!
```

**Right:**
```cpp
const size_t deviceAlign = 256;
size_t offset = align_up(previousOffset + structSize, deviceAlign);
// offset is now aligned to both struct size (16) and device (256)
```

### Mistake 5: Not Padding Final Struct Size

**Wrong:**
```cpp
struct bad_struct {
    vec4 a;
    float b;
    int c;
};
// Size: 16 + 4 + 4 = 24 bytes - NOT a multiple of 16!
```

**Right:**
```cpp
struct good_struct {
    vec4 a;
    float b;
    int c;
    float pad[2];  // Pad to 32 bytes
};
```

---

## 8. Examples from the Codebase

### Example 1: `model_uniform_data`

```cpp
struct model_uniform_data {
    matrix4 modelViewMatrix;      // 64 bytes (4 columns × 16)
    matrix4 modelMatrix;          // 64 bytes
    // ... more matrices ...
    vec4 color;                   // 16 bytes
    model_light lights[8];       // Array of 8 lights
    // ... many fields ...
    float pad;                    // Final padding
};
```

**Key observations:**
- Matrices are properly aligned (each column is 16 bytes)
- Arrays (`lights[8]`) are properly aligned
- Final `pad` ensures struct size is 16-byte aligned
- Comment on line 116: `sBasemapIndex` was moved for alignment tracking

### Example 2: `deferred_light_data`

```cpp
struct deferred_light_data {
    vec3d diffuseLightColor;  // offset 0, 12 bytes
    float coneAngle;          // offset 12, 4 bytes (packs after vec3)

    vec3d lightDir;           // offset 16, 12 bytes
    float coneInnerAngle;     // offset 28, 4 bytes

    vec3d coneDir;            // offset 32, 12 bytes
    float dualCone;           // offset 44, 4 bytes

    vec3d scale;              // offset 48, 12 bytes
    float lightRadius;        // offset 60, 4 bytes

    int lightType;            // offset 64, 4 bytes
    int enable_shadows;       // offset 68, 4 bytes
    float sourceRadius;       // offset 72, 4 bytes
    float pad0[1];            // offset 76, 4 bytes -> total 80 bytes
};
```

**Note**: The Vulkan renderer uses a separate struct `DeferredLightUBO` (in `VulkanDeferredLights.h`) with explicit `alignas(16)` and uses `float[3]` arrays instead of `vec3d`. The `deferred_light_data` struct is used by OpenGL. Both are correctly sized at 80 bytes (a multiple of 16).

### Example 3: Vulkan-Specific Aligned Structs

The Vulkan renderer uses `alignas(16)` and `float[3]` arrays to ensure correct layout:

```cpp
// From VulkanDeferredLights.h
struct alignas(16) DeferredLightUBO {
    float diffuseLightColor[3];   // offset 0, 12 bytes
    float coneAngle;              // offset 12, 4 bytes

    float lightDir[3];            // offset 16, 12 bytes
    float coneInnerAngle;         // offset 28, 4 bytes

    float coneDir[3];             // offset 32, 12 bytes
    uint32_t dualCone;            // offset 44, 4 bytes

    float scale[3];               // offset 48, 12 bytes
    float lightRadius;            // offset 60, 4 bytes

    int32_t lightType;            // offset 64, 4 bytes
    uint32_t enable_shadows;      // offset 68, 4 bytes
    float sourceRadius;           // offset 72, 4 bytes
    float _pad;                   // offset 76, 4 bytes -> total 80 bytes
};
```

**Key points:**
- `alignas(16)` ensures the struct is 16-byte aligned in memory
- Uses `float[3]` instead of `vec3d` for explicit 12-byte fields
- Scalars pack after the 3-float arrays (same as vec3+scalar packing)
- Final `_pad` ensures struct size is a multiple of 16 (80 bytes)
- This is the recommended approach for new Vulkan uniform structs

### Example 4: `movie_uniforms`

```cpp
struct movie_uniforms {
    float alpha;
    float pad[3];  // Explicit padding to 16 bytes
};
```

**Good example**: Simple struct with explicit padding to ensure 16-byte alignment.

### Example 5: VulkanRingBuffer Allocation (Tonemapping)

From `VulkanRenderer.cpp` (`recordTonemappingToSwapchain`):

```cpp
// Build data on stack
graphics::generic_data::tonemapping_data data{};
data.exposure = lighting_profiles::current_exposure();
data.tonemapper = static_cast<int>(lighting_profiles::current_tonemapper());
// ... set other fields ...

// Allocate uniform with device alignment
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);

// Copy to mapped memory
std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

// Bind with aligned offset
vk::DescriptorBufferInfo genericInfo{};
genericInfo.buffer = frame.uniformBuffer().buffer();
genericInfo.offset = uniformAlloc.offset;  // Already aligned
genericInfo.range = sizeof(data);
```

---

## 9. Validation and Debugging

### Compile-Time Validation

Add `static_assert` after struct definitions:

```cpp
struct my_uniform {
    vec4 data;
    float pad[3];
};
static_assert(sizeof(my_uniform) % 16 == 0,
              "my_uniform must be 16-byte aligned");
static_assert(sizeof(my_uniform) == 32,
              "my_uniform size mismatch (expected 32)");
```

### Runtime Validation

The renderer validates offsets before binding:

```cpp
// In VulkanRenderer::setModelUniformBinding
const auto dynOffset = static_cast<uint32_t>(offset);
Assertion((dynOffset % alignment) == 0,
          "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
```

### Debugging Misalignment

**Symptoms:**
- Garbage values in shaders
- Crashes or corruption
- Validation layer errors: "VUID-VkDescriptorBufferInfo-offset-00340"

**How to debug:**

1. **Check struct size:**
   ```cpp
   printf("Struct size: %zu (must be multiple of 16)\n", sizeof(myStruct));
   printf("Size mod 16: %zu\n", sizeof(myStruct) % 16);
   ```

2. **Check offset alignment:**
   ```cpp
   const size_t deviceAlign = renderer.getMinUniformOffsetAlignment();
   printf("Offset: %zu, Alignment: %zu, Valid: %s\n",
          offset, deviceAlign,
          (offset % deviceAlign == 0) ? "YES" : "NO");
   ```

3. **Use RenderDoc:**
   - Inspect uniform buffer contents
   - Verify data matches expected layout
   - Check for corruption

4. **Validation layers:**
   - Enable `VK_LAYER_KHRONOS_validation`
   - Look for alignment-related errors

### Common Validation Errors

| Error | Cause | Fix |
|-------|-------|-----|
| `VUID-VkDescriptorBufferInfo-offset-00340` | Offset not aligned to device limit | Use `VulkanRingBuffer::allocate()` with device alignment |
| Garbage values in shader | Struct not std140-compliant | Add padding, verify member offsets |
| Crash on uniform access | Misaligned struct size | Ensure size is multiple of 16 |

---

## Summary

**Key Takeaways:**

1. **std140 requires**: All structs must be 16-byte aligned; vec3s have 16-byte alignment but a following scalar can pack into the remaining 4 bytes; arrays of scalars align each element to 16 bytes
2. **Device limits**: Uniform buffer offsets must be aligned to `minUniformBufferOffsetAlignment` (typically 256 bytes)
3. **Use VulkanRingBuffer**: Call `frame.uniformBuffer().allocate(size, alignment)` to get properly aligned allocations
4. **Validate**: Add `static_assert` for struct sizes, runtime assertions check offsets
5. **Common mistake**: Forgetting padding at the end of struct to reach a multiple of 16 bytes

**When adding new uniform structs:**
1. Define struct with proper padding (see Section 3 for packing rules)
2. Add `static_assert` for size validation
3. Use `VulkanRingBuffer::allocate()` with device alignment for offsets
4. Verify offset alignment before binding
5. Test with validation layers enabled

