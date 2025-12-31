# Vulkan Uniform Buffer Alignment and std140 Rules

This document explains how uniform buffer structs must be aligned for Vulkan, the std140 layout rules, and how to correctly add new uniform structs to the renderer.

---

## Quick Reference

**Alignment Essentials**

| Requirement | Value | Applies To |
|-------------|-------|------------|
| Struct size | Multiple of 16 bytes | All uniform structs |
| Buffer offset | Multiple of `minUniformBufferOffsetAlignment` | Dynamic uniform bindings |
| `vec3` base alignment | 16 bytes | Member placement |
| `vec3` actual size | 12 bytes | Data payload |
| Scalar after `vec3` | Packs into remaining 4 bytes | No extra padding needed |
| Array element alignment | 16 bytes each | Arrays of any type |
| Matrix column alignment | 16 bytes each | `mat3`, `mat4` |

**C++ to GLSL Type Mappings**

| C++ Type | GLSL Type | Size | Alignment |
|----------|-----------|------|-----------|
| `float` | `float` | 4 | 4 |
| `int` / `int32_t` | `int` | 4 | 4 |
| `uint32_t` | `uint` | 4 | 4 |
| `vec2d` | `vec2` | 8 | 8 |
| `vec3d` | `vec3` | 12 | 16 |
| `vec4` | `vec4` | 16 | 16 |
| `matrix` (3x3) | `mat3` | 48 | 16 (per column) |
| `matrix4` | `mat4` | 64 | 16 (per column) |

**Checklist for New Structs**

1. Define struct with `alignas(16)` in C++ (Vulkan-specific structs)
2. Ensure total size is a multiple of 16 bytes
3. Add `static_assert(sizeof(MyStruct) % 16 == 0, "...")` after definition
4. Use `VulkanRingBuffer::allocate(size, deviceAlignment)` for buffer offsets
5. Match GLSL uniform block layout exactly

---

## Table of Contents

1. [Overview: Why Alignment Matters](#1-overview-why-alignment-matters)
2. [std140 Layout Rules](#2-std140-layout-rules)
3. [Struct-Level Alignment](#3-struct-level-alignment)
4. [Device Limits: minUniformBufferOffsetAlignment](#4-device-limits-minuniformbufferoffsetalignment)
5. [How Alignment is Enforced](#5-how-alignment-is-enforced)
6. [Adding New Uniform Structs](#6-adding-new-uniform-structs)
7. [GLSL Shader Correspondence](#7-glsl-shader-correspondence)
8. [Common Alignment Mistakes](#8-common-alignment-mistakes)
9. [Examples from the Codebase](#9-examples-from-the-codebase)
10. [Validation and Debugging](#10-validation-and-debugging)
11. [Related Documentation](#11-related-documentation)

---

## 1. Overview: Why Alignment Matters

Uniform buffers in Vulkan must satisfy two distinct alignment requirements:

1. **std140 layout rules**: A GLSL uniform buffer layout standard that ensures consistent memory layout across different GPUs and drivers. This affects *how data is arranged within the struct*.
2. **Device alignment limits**: Vulkan devices require uniform buffer *offsets* (where the struct starts in the buffer) to be aligned to `minUniformBufferOffsetAlignment` (typically 256 bytes, but can be as low as 16).

**Why this matters:**

- Misaligned data causes undefined behavior: garbage values, visual corruption, or crashes
- GPU hardware reads uniform data in aligned chunks (16 bytes or larger)
- Different GPUs may interpret misaligned data differently, causing driver-specific bugs
- Validation layers catch some alignment errors, but struct layout errors are not detectable at runtime

**The Two-Level Alignment Rule**

```
                    Buffer Memory
    +--------------------------------------------------+
    |         |                    |                   |
    | Padding | Your Struct (UBO)  | Padding           |
    |         |                    |                   |
    +--------------------------------------------------+
    ^         ^
    |         |
    |         +-- Must be aligned to minUniformBufferOffsetAlignment (device limit)
    |
    +-- Buffer start

    Within the struct, each member must respect std140 alignment rules.
```

---

## 2. std140 Layout Rules

The std140 layout rules are defined in the [OpenGL specification](https://www.khronos.org/opengl/wiki/Interface_Block_(GLSL)#Explicit_variable_layout). Here is the practical summary:

### Base Alignment Rules

| Type | Base Alignment | Size | Notes |
|------|----------------|------|-------|
| `float` | 4 bytes | 4 bytes | Scalar |
| `int` | 4 bytes | 4 bytes | Scalar |
| `uint` | 4 bytes | 4 bytes | Scalar |
| `vec2` | 8 bytes | 8 bytes | 2-component vector |
| `vec3` | 16 bytes | 12 bytes | 16-byte alignment, but only 12 bytes of data |
| `vec4` | 16 bytes | 16 bytes | 4-component vector |
| `mat3` | 16 bytes (per column) | 48 bytes | 3 columns, each 16-byte aligned |
| `mat4` | 16 bytes (per column) | 64 bytes | 4 columns, each 16-byte aligned |
| Arrays | 16 bytes (per element) | Element size x count | Each element padded to 16 bytes |
| Structs | Max(member alignments, 16) | Rounded to alignment | Nested structs have 16-byte minimum |

### Key Rules

1. **Member offset**: Each member starts at an offset that is a multiple of its base alignment
2. **Struct size**: The total struct size is rounded up to a multiple of the struct's base alignment (minimum 16 bytes for uniform blocks)
3. **vec3 packing**: A `vec3` occupies 16 bytes of alignment, but only 12 bytes of actual data. A scalar following a `vec3` can pack into the remaining 4 bytes
4. **Array elements**: Every array element is aligned to 16 bytes, even if the element type is smaller

### Offset Calculation Example

```
Offset Calculation Walkthrough
==============================

struct example {
    vec4 a;      // alignment: 16, offset: 0,  size: 16  -> next: 16
    float b;     // alignment: 4,  offset: 16, size: 4   -> next: 20
    vec3 c;      // alignment: 16, offset: 32, size: 12  -> next: 44
                 // (offset 20 not aligned to 16, so pad to 32)
    float d;     // alignment: 4,  offset: 44, size: 4   -> next: 48
                 // (packs into vec3's trailing 4 bytes)
};
// Total size: 48 bytes (already multiple of 16)
```

---

## 3. Struct-Level Alignment

Every uniform struct must satisfy these conditions:

1. Total size is a multiple of 16 bytes
2. Each member is aligned according to std140 rules
3. Trailing padding is added explicitly if the natural size is not a multiple of 16

### Calculating Struct Size

**Step 1**: Determine the alignment of each member

**Step 2**: Calculate offsets, inserting padding where alignment requires it

**Step 3**: Round total size up to a multiple of 16

### Example: `deferred_light_data`

The `vec3 + scalar` packing pattern is commonly used in this codebase. Here is how `deferred_light_data` lays out:

```cpp
struct deferred_light_data {
    vec3d diffuseLightColor;  // offset 0,  size 12, alignment 16
    float coneAngle;          // offset 12, size 4  (packs after vec3)

    vec3d lightDir;           // offset 16, size 12, alignment 16
    float coneInnerAngle;     // offset 28, size 4  (packs after vec3)

    vec3d coneDir;            // offset 32, size 12, alignment 16
    float dualCone;           // offset 44, size 4  (packs after vec3)

    vec3d scale;              // offset 48, size 12, alignment 16
    float lightRadius;        // offset 60, size 4  (packs after vec3)

    int lightType;            // offset 64, size 4,  alignment 4
    int enable_shadows;       // offset 68, size 4
    float sourceRadius;       // offset 72, size 4
    float pad0[1];            // offset 76, size 4  (pads to 80)
};
// Total: 80 bytes (80 = 5 x 16, correctly aligned)
```

**Key insight**: Each `vec3d + float` pair occupies exactly 16 bytes with no wasted space. The final `pad0[1]` is needed because `64 + 4 + 4 + 4 = 76`, which is not a multiple of 16.

### The vec3 Packing Rule

In std140, a scalar immediately following a `vec3` shares the same 16-byte slot:

```
Byte offset:  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
            +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
vec3 + float| vec3.x        | vec3.y        | vec3.z        | scalar      |
            +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
```

This packing is valid only when:
- The scalar type has 4-byte alignment (`float`, `int`, `uint`)
- The scalar immediately follows the `vec3` in the struct definition
- No other member requires higher alignment before the scalar

---

## 4. Device Limits: minUniformBufferOffsetAlignment

Vulkan devices require dynamic uniform buffer offsets to be aligned to `minUniformBufferOffsetAlignment`. This is independent of std140 struct layout.

| GPU Vendor | Typical Value |
|------------|---------------|
| NVIDIA | 256 bytes |
| AMD | 256 bytes |
| Intel | 64 bytes |
| Mobile | 16-256 bytes |

### How It Applies

When binding a uniform buffer with a dynamic offset (e.g., when using `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`), the offset must be a multiple of this limit:

```cpp
// Query device alignment
const size_t alignment = renderer.getMinUniformOffsetAlignment();  // e.g., 256

// Allocate uniform data with alignment
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(myStruct), alignment);
// uniformAlloc.offset is now guaranteed to be aligned to 'alignment'
```

### Independent Requirements

| Requirement | What It Affects | Typical Value |
|-------------|-----------------|---------------|
| std140 struct alignment | Member layout within struct | 16 bytes |
| `minUniformBufferOffsetAlignment` | Offset in buffer where struct starts | 256 bytes |

A struct can be perfectly std140-compliant (size is multiple of 16), but the buffer offset where it is placed must still respect the device limit (e.g., 256). These are enforced at different levels.

---

## 5. How Alignment is Enforced

### VulkanRingBuffer (Vulkan Renderer)

The Vulkan renderer uses `VulkanRingBuffer` (`code/graphics/vulkan/VulkanRingBuffer.h`) for per-frame uniform allocations. Each frame has a ring buffer that sub-allocates aligned regions:

```cpp
// From VulkanRingBuffer.h
class VulkanRingBuffer {
  public:
    struct Allocation {
        vk::DeviceSize offset{0};  // Aligned offset within the buffer
        void* mapped{nullptr};     // Host-visible memory pointer
    };

    // Allocate 'size' bytes with specified alignment
    Allocation allocate(vk::DeviceSize size, vk::DeviceSize alignmentOverride = 0);
    // ...
};
```

**Usage pattern:**

```cpp
// Retrieve device alignment requirement
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());

// Allocate aligned region
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(myStruct), uboAlignment);

// Write data to mapped memory
std::memcpy(uniformAlloc.mapped, &myData, sizeof(myData));

// Use uniformAlloc.offset when binding descriptors
```

### Compile-Time Validation

There is no automatic compile-time validation of std140 compliance. You must manually add `static_assert` statements:

```cpp
struct my_uniform {
    vec4 data;
    float value;
    float pad[3];
};
static_assert(sizeof(my_uniform) % 16 == 0,
              "my_uniform must be 16-byte aligned for std140");
static_assert(sizeof(my_uniform) == 32,
              "my_uniform size mismatch");
```

### Runtime Validation

The renderer validates dynamic offsets before binding:

```cpp
// From VulkanRendererResources.cpp
const auto alignment = getMinUniformOffsetAlignment();
const auto dynOffset = static_cast<uint32_t>(offset);

Assertion(alignment > 0, "minUniformBufferOffsetAlignment must be non-zero");
Assertion((dynOffset % alignment) == 0,
          "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
```

**What runtime validation catches:**
- Buffer offsets not aligned to device limits
- Null or invalid buffer handles

**What runtime validation cannot catch:**
- std140 struct layout errors (these manifest as visual corruption)
- Mismatched C++ and GLSL struct definitions

---

## 6. Adding New Uniform Structs

### Step-by-Step Guide

**Step 1: Define the struct in `uniform_structs.h`**

For shared structs (OpenGL + Vulkan):

```cpp
namespace graphics {
namespace generic_data {

struct my_new_uniform {
    vec4 color;          // offset 0,  size 16
    float intensity;     // offset 16, size 4
    int flags;           // offset 20, size 4
    float pad[2];        // offset 24, size 8 -> total 32
};

} // namespace generic_data
} // namespace graphics
```

For Vulkan-specific structs, use `alignas(16)` and `float[3]` instead of `vec3d`:

```cpp
// In VulkanSomething.h
struct alignas(16) MyVulkanUBO {
    float color[3];      // offset 0, size 12
    float intensity;     // offset 12, size 4  (packs after float[3])
    int32_t flags;       // offset 16, size 4
    float _pad[3];       // offset 20, size 12 -> total 32
};
```

**Step 2: Add static assertions**

```cpp
static_assert(sizeof(graphics::generic_data::my_new_uniform) % 16 == 0,
              "my_new_uniform must be 16-byte aligned for std140");
static_assert(sizeof(graphics::generic_data::my_new_uniform) == 32,
              "my_new_uniform size mismatch (expected 32)");
```

**Step 3: Define matching GLSL uniform block**

```glsl
layout(binding = N, std140) uniform MyNewUniform {
    vec4 color;
    float intensity;
    int flags;
    // No need to declare padding in GLSL - it's implicit
};
```

**Step 4: Allocate from VulkanRingBuffer**

```cpp
// Build data on the stack
graphics::generic_data::my_new_uniform data{};
data.color = vec4{1.0f, 0.0f, 0.0f, 1.0f};
data.intensity = 0.5f;
data.flags = 0;

// Allocate with device alignment
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(renderer.getMinUniformBufferAlignment());
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);

// Copy to mapped memory
std::memcpy(uniformAlloc.mapped, &data, sizeof(data));
```

**Step 5: Bind the uniform buffer**

```cpp
vk::DescriptorBufferInfo bufferInfo{};
bufferInfo.buffer = frame.uniformBuffer().buffer();
bufferInfo.offset = uniformAlloc.offset;  // Already aligned
bufferInfo.range = sizeof(data);

// Push descriptor or update descriptor set
cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics,
                         layout,
                         setIndex,
                         {bufferInfo},
                         /* ... */);
```

### Checklist

- [ ] Struct size is a multiple of 16 bytes
- [ ] All `vec3` members are followed by a scalar or have explicit padding
- [ ] Arrays of scalars are avoided (use `vec4` instead) or explicitly padded
- [ ] Matrices use `matrix4` (mat4) or account for mat3's 48-byte size
- [ ] `static_assert` verifies size at compile time
- [ ] GLSL uniform block matches C++ struct exactly
- [ ] `VulkanRingBuffer::allocate()` uses device alignment
- [ ] Validated with RenderDoc or validation layers

---

## 7. GLSL Shader Correspondence

The C++ struct and GLSL uniform block must have identical layouts. Any mismatch causes undefined behavior.

### Example: Deferred Light Uniform Block

**C++ struct** (from `VulkanDeferredLights.h`):

```cpp
struct alignas(16) DeferredLightUBO {
    float diffuseLightColor[3];   // offset 0,  size 12
    float coneAngle;              // offset 12, size 4

    float lightDir[3];            // offset 16, size 12
    float coneInnerAngle;         // offset 28, size 4

    float coneDir[3];             // offset 32, size 12
    uint32_t dualCone;            // offset 44, size 4

    float scale[3];               // offset 48, size 12
    float lightRadius;            // offset 60, size 4

    int32_t lightType;            // offset 64, size 4
    uint32_t enable_shadows;      // offset 68, size 4
    float sourceRadius;           // offset 72, size 4
    float _pad;                   // offset 76, size 4
};
// Total: 80 bytes
```

**GLSL uniform block** (from `deferred.frag`):

```glsl
layout(binding = 1, std140) uniform lightData {
    vec3 diffuseLightColor;       // offset 0
    float coneAngle;              // offset 12

    vec3 lightDir;                // offset 16
    float coneInnerAngle;         // offset 28

    vec3 coneDir;                 // offset 32
    uint dualCone;                // offset 44

    vec3 scale;                   // offset 48
    float lightRadius;            // offset 60

    int lightType;                // offset 64
    uint enable_shadows;          // offset 68
    float sourceRadius;           // offset 72
    // Implicit 4-byte padding    // offset 76
};
```

**Important notes:**
- GLSL does not require explicit padding at the end; std140 handles it
- C++ uses `float[3]` to ensure 12-byte size; GLSL uses `vec3`
- Type correspondence must be exact: `int32_t` maps to `int`, `uint32_t` maps to `uint`

### Type Correspondence Table

| C++ Type | GLSL Type | Notes |
|----------|-----------|-------|
| `float` | `float` | |
| `int32_t` / `int` | `int` | Signed 32-bit |
| `uint32_t` | `uint` | Unsigned 32-bit |
| `float[2]` or `vec2d` | `vec2` | 8-byte aligned |
| `float[3]` or `vec3d` | `vec3` | 16-byte aligned, 12-byte size |
| `float[4]` or `vec4` | `vec4` | 16-byte aligned |
| `matrix4` | `mat4` | Column-major, 64 bytes |
| `matrix` (3x3) | `mat3` | Column-major, 48 bytes |

---

## 8. Common Alignment Mistakes

### Mistake 1: Forgetting Final Padding

**Problem:**

```cpp
struct bad_struct {
    vec4 a;           // offset 0, size 16
    float b;          // offset 16, size 4
    int c;            // offset 20, size 4
};
// Size: 24 bytes - NOT a multiple of 16
```

**Solution:**

```cpp
struct good_struct {
    vec4 a;           // offset 0, size 16
    float b;          // offset 16, size 4
    int c;            // offset 20, size 4
    float pad[2];     // offset 24, size 8 -> total 32
};
```

### Mistake 2: Misunderstanding vec3 Packing

A scalar *can* pack after a `vec3`, but only if it fits and has 4-byte alignment:

**Valid (scalar packs into vec3 slot):**

```cpp
struct valid {
    vec3d position;   // offset 0, size 12
    float value;      // offset 12, size 4 -> total 16
};
```

**Invalid (next member forces new alignment):**

```cpp
struct invalid {
    vec3d position;   // offset 0, size 12
    vec2d uv;         // offset 16, size 8 (vec2 needs 8-byte alignment)
                      // The gap at offset 12-15 is wasted
};
// Total: 24 bytes -> needs padding to 32
```

### Mistake 3: Using Arrays of Scalars

In std140, each array element is aligned to 16 bytes, regardless of element type:

**C++ expectation vs std140 reality:**

```cpp
struct bad_struct {
    float values[3];  // C++ sees 12 bytes
                      // std140 sees 48 bytes (3 x 16)
    int count;        // C++ offset 12 vs std140 offset 48
};
```

**Solutions:**

Option A: Use vec4 instead:
```cpp
struct good_a {
    vec4 values;      // 16 bytes, stores up to 4 floats
    int count;
    float pad[3];     // Pad to 32 bytes
};
```

Option B: Match std140 layout explicitly:
```cpp
struct good_b {
    float value0; float _p0[3];  // 16 bytes
    float value1; float _p1[3];  // 16 bytes
    float value2; float _p2[3];  // 16 bytes
    int count;                    // offset 48
    float pad[3];                 // Pad to 64 bytes
};
```

### Mistake 4: Confusing C++ Layout with std140

C++ compilers may pack structs differently than std140 requires:

**Problem:**

```cpp
struct bad_struct {
    float x, y, z;    // C++ packs tightly: 12 bytes
    float w;          // offset 12 in C++, but std140 treats the above as 3 scalars
};
```

**Solution:** Always use `vec3d` or `float[3]`:

```cpp
struct good_struct {
    vec3d xyz;        // 12 bytes, 16-byte aligned
    float w;          // offset 12, packs after vec3
};
```

### Mistake 5: mat3 Size Miscalculation

A `mat3` in std140 is NOT 36 bytes (3 x 3 x 4). Each column is 16-byte aligned:

```
mat3 layout in std140:
  Column 0: vec3 (12 bytes) + 4 bytes padding = 16 bytes
  Column 1: vec3 (12 bytes) + 4 bytes padding = 16 bytes
  Column 2: vec3 (12 bytes) + 4 bytes padding = 16 bytes
  Total: 48 bytes
```

**Recommendation:** Use `mat4` when possible to avoid confusion, or account for 48 bytes when using `mat3`.

### Mistake 6: Mixing Device and Struct Alignment

**Problem:**

```cpp
// Struct is 32 bytes, device alignment is 256
size_t offset = previousOffset + 32;  // Aligned to struct, not device
```

**Solution:**

```cpp
const size_t deviceAlign = renderer.getMinUniformOffsetAlignment();  // 256
size_t offset = ((previousOffset + deviceAlign - 1) / deviceAlign) * deviceAlign;
// Or use VulkanRingBuffer::allocate() which handles this automatically
```

---

## 9. Examples from the Codebase

### Example 1: Simple Uniform (`movie_uniforms`)

**File:** `code/graphics/util/uniform_structs.h`

```cpp
struct movie_uniforms {
    float alpha;
    float pad[3];  // Explicit padding to 16 bytes
};
```

This is the simplest pattern: a single scalar with explicit padding.

### Example 2: Vector-Scalar Packing (`lightshaft_data`)

**File:** `code/graphics/util/uniform_structs.h`

```cpp
struct lightshaft_data {
    vec2d sun_pos;    // offset 0, size 8
    float density;    // offset 8, size 4
    float weight;     // offset 12, size 4

    float falloff;    // offset 16, size 4
    float intensity;  // offset 20, size 4
    float cp_intensity; // offset 24, size 4
    int samplenum;    // offset 28, size 4
};
// Total: 32 bytes (two 16-byte rows)
```

Note: `vec2` has 8-byte alignment, so scalars can follow it without gap.

### Example 3: Vulkan-Specific Aligned Struct (`DeferredLightUBO`)

**File:** `code/graphics/vulkan/VulkanDeferredLights.h`

```cpp
struct alignas(16) DeferredLightUBO {
    float diffuseLightColor[3];
    float coneAngle;

    float lightDir[3];
    float coneInnerAngle;

    float coneDir[3];
    uint32_t dualCone;

    float scale[3];
    float lightRadius;

    int32_t lightType;
    uint32_t enable_shadows;
    float sourceRadius;
    float _pad;
};
```

**Key points:**
- `alignas(16)` ensures the struct is 16-byte aligned in memory
- Uses `float[3]` for explicit 12-byte fields (matches `vec3` in GLSL)
- Trailing `_pad` ensures struct size is multiple of 16 (80 bytes)
- This is the recommended pattern for new Vulkan uniform structs

### Example 4: Complex Struct (`model_uniform_data`)

**File:** `code/graphics/util/uniform_structs.h`

```cpp
struct model_uniform_data {
    matrix4 modelViewMatrix;      // 64 bytes
    matrix4 modelMatrix;          // 64 bytes
    matrix4 viewMatrix;           // 64 bytes
    matrix4 projMatrix;           // 64 bytes
    matrix4 textureMatrix;        // 64 bytes
    matrix4 shadow_mv_matrix;     // 64 bytes
    matrix4 shadow_proj_matrix[4]; // 256 bytes (4 x 64)

    vec4 color;                   // 16 bytes

    model_light lights[MAX_UNIFORM_LIGHTS];  // 8 x sizeof(model_light)

    // ... many scalar fields with careful alignment ...

    float pad;                    // Final padding
};
```

**Key observations:**
- Matrices are inherently 64 bytes (4 columns x 16 bytes each)
- Arrays of structs (`lights[8]`) inherit the inner struct's alignment
- Final `pad` ensures the total size is 16-byte aligned

### Example 5: Ring Buffer Allocation Pattern

**File:** `code/graphics/vulkan/VulkanRendererPostProcessing.cpp`

```cpp
// Build uniform data on stack
graphics::generic_data::tonemapping_data data{};
data.exposure = lighting_profiles::current_exposure();
data.tonemapper = static_cast<int>(lighting_profiles::current_tonemapper());
// ... populate other fields ...

// Allocate with device alignment
const vk::DeviceSize uboAlignment =
    static_cast<vk::DeviceSize>(getMinUniformBufferAlignment());
auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(data), uboAlignment);

// Copy to mapped memory
std::memcpy(uniformAlloc.mapped, &data, sizeof(data));

// Bind descriptor
vk::DescriptorBufferInfo genericInfo{};
genericInfo.buffer = frame.uniformBuffer().buffer();
genericInfo.offset = uniformAlloc.offset;  // Already aligned
genericInfo.range = sizeof(data);
```

---

## 10. Validation and Debugging

### Compile-Time Validation

Add `static_assert` after every uniform struct definition:

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

Enable Vulkan validation layers by setting:
```
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
```

The renderer also includes runtime assertions:

```cpp
Assertion((dynOffset % alignment) == 0,
          "Model uniform offset %u is not aligned to %zu", dynOffset, alignment);
```

### Debugging Misalignment

**Symptoms:**
- Garbage or seemingly random values in shaders
- Visual corruption that changes based on camera position
- Crashes or GPU hangs
- Validation layer errors mentioning offset alignment

**Diagnostic steps:**

1. **Verify struct size:**
   ```cpp
   mprintf(("Struct size: %zu (mod 16: %zu)\n",
            sizeof(myStruct), sizeof(myStruct) % 16));
   ```

2. **Verify buffer offset:**
   ```cpp
   const size_t deviceAlign = renderer.getMinUniformOffsetAlignment();
   mprintf(("Offset: %zu, Aligned: %s\n",
            offset, (offset % deviceAlign == 0) ? "YES" : "NO"));
   ```

3. **Use RenderDoc:**
   - Capture a frame and inspect uniform buffer contents
   - Compare expected layout with actual memory contents
   - Check for unexpected padding or misaligned fields

4. **Print member offsets:**
   ```cpp
   #define PRINT_OFFSET(type, member) \
       mprintf(("  " #member " offset: %zu\n", offsetof(type, member)))

   PRINT_OFFSET(my_uniform, field1);
   PRINT_OFFSET(my_uniform, field2);
   // ...
   ```

### Common Validation Layer Errors

| Error Code | Cause | Fix |
|------------|-------|-----|
| `VUID-VkDescriptorBufferInfo-offset-00340` | Buffer offset not aligned to device limit | Use `VulkanRingBuffer::allocate()` with device alignment |
| `VUID-vkCmdBindDescriptorSets-pDynamicOffsets-*` | Dynamic offset not aligned | Check offset calculation |
| Shader reads garbage | C++/GLSL struct mismatch | Verify member order and types match exactly |

---

## 11. Related Documentation

### Internal Documentation

- [`VULKAN_UNIFORM_BINDINGS.md`](VULKAN_UNIFORM_BINDINGS.md) - Descriptor set layouts and binding points
- [`VULKAN_DESCRIPTOR_SETS.md`](VULKAN_DESCRIPTOR_SETS.md) - Descriptor set architecture
- [`VULKAN_PERFORMANCE_OPTIMIZATION.md`](VULKAN_PERFORMANCE_OPTIMIZATION.md) - Uniform buffer packing strategies
- [`VULKAN_MODEL_RENDERING_PIPELINE.md`](VULKAN_MODEL_RENDERING_PIPELINE.md) - Model uniform data flow

### Source Files

| File | Description |
|------|-------------|
| `code/graphics/util/uniform_structs.h` | Shared uniform struct definitions |
| `code/graphics/vulkan/VulkanRingBuffer.h` | Ring buffer for per-frame allocations |
| `code/graphics/vulkan/VulkanDeferredLights.h` | Vulkan-specific deferred light UBOs |
| `code/graphics/vulkan/VulkanRendererResources.cpp` | Model uniform binding and validation |
| `code/graphics/vulkan/VulkanRendererPostProcessing.cpp` | Post-processing uniform allocation |

### External References

- [OpenGL std140 Layout Rules](https://www.khronos.org/opengl/wiki/Interface_Block_(GLSL)#Explicit_variable_layout) - Authoritative specification
- [Vulkan minUniformBufferOffsetAlignment](https://www.khronos.org/registry/vulkan/specs/1.3-extensions/html/vkspec.html#limits-minUniformBufferOffsetAlignment) - Device limit specification
- [Sascha Willems Vulkan Examples](https://github.com/SaschaWillems/Vulkan) - Reference implementations

---

## Summary

**Key Takeaways**

1. **Two-level alignment**: Struct layout (std140, multiple of 16) and buffer offset (device limit, typically 256) are independent requirements
2. **vec3 packing**: A scalar following a `vec3` packs into its trailing 4 bytes; no extra padding needed
3. **Array elements**: Each array element is 16-byte aligned in std140, even scalars
4. **Use VulkanRingBuffer**: Call `allocate(size, deviceAlignment)` to get properly aligned offsets
5. **Match C++ and GLSL exactly**: Member order, types, and sizes must correspond
6. **Validate**: Add `static_assert` for sizes, enable validation layers, use RenderDoc

**When adding new uniform structs:**

1. Define with `alignas(16)` (Vulkan-specific) or explicit padding
2. Pair each `vec3` with a trailing scalar or add padding
3. Add `static_assert` for size validation
4. Define matching GLSL uniform block with `std140` layout
5. Use `VulkanRingBuffer::allocate()` with device alignment
6. Test with validation layers enabled and inspect with RenderDoc
