# Recent Vulkan Graphics Fixes and Changes

*Last updated: December 25, 2025*

This document tracks significant bug fixes, improvements, and new features in the Vulkan renderer. Each entry includes root cause analysis, the fix applied, and relevant code references. For architectural context, see `VULKAN_ARCHITECTURE.md`.

---

## Table of Contents

- [Critical Bug Fixes](#critical-bug-fixes)
  - [1. HUD Alpha Rendering Fix](#1-hud-alpha-rendering-fix)
  - [2. 16-Bit Texture Unpacking Fix](#2-16-bit-texture-unpacking-fix)
  - [3. Scissor Reset at Frame Start](#3-scissor-reset-at-frame-start)
  - [4. Lightshafts Pass Blending](#4-lightshafts-pass-blending)
- [Synchronization and Buffer Management](#synchronization-and-buffer-management)
  - [5. Dynamic Buffer Orphaning](#5-dynamic-buffer-orphaning)
  - [6. Zero-Size Buffer Sub-Update Handling](#6-zero-size-buffer-sub-update-handling)
- [API and Architecture Improvements](#api-and-architecture-improvements)
  - [7. Recording Frame Token Sealing](#7-recording-frame-token-sealing)
  - [8. Builtin Texture Sentinel Removal](#8-builtin-texture-sentinel-removal)
  - [9. Vulkan Feature Chain Updates](#9-vulkan-feature-chain-updates)
- [Shader Fixes](#shader-fixes)
  - [10. AA Bitmap Alpha Coverage Fix](#10-aa-bitmap-alpha-coverage-fix)
- [New Features](#new-features)
  - [11. Shield Impact Decals](#11-shield-impact-decals)
  - [12. Texture Upload Streaming](#12-texture-upload-streaming)
  - [13. Movie Rendering Pipeline](#13-movie-rendering-pipeline)
  - [14. Shader Manager Extensions](#14-shader-manager-extensions)
  - [15. Decoder Color Metadata](#15-decoder-color-metadata)
- [Summary of Code Patterns](#summary-of-code-patterns)
- [Related Documentation](#related-documentation)

---

## Critical Bug Fixes

These fixes address issues that caused visible rendering artifacts or incorrect output.

### 1. HUD Alpha Rendering Fix

**Commit:** `3fbff05b5`
**Severity:** Critical (broken HUD/text rendering)
**File:** `VulkanGraphics.cpp:1632-1634`

**Problem:** Text and HUD elements rendered incorrectly because the alpha blending path was not being triggered for AA bitmaps.

**Root Cause:** The code compared `material::get_texture_type()` against `material::TEX_TYPE_AABITMAP`, but this method returns a `TCACHE_TYPE_*` constant, not a `material::texture_type` enum value. The type mismatch caused AA bitmaps (fonts and HUD alpha masks) to never be recognized.

**Fix:**
```cpp
// material::get_texture_type() returns a TCACHE_TYPE_* value, not material::texture_type.
// Use the TCACHE_TYPE_* constant here so AA-bitmaps (font/HUD alpha masks) are handled correctly.
int alphaTexture = (material_info->get_texture_type() == TCACHE_TYPE_AABITMAP) ? 1 : 0;
```

**Impact:** Restores correct rendering of all fonts, HUD indicators, and UI alpha masks.

---

### 2. 16-Bit Texture Unpacking Fix

**Commit:** `3fbff05b5`
**Severity:** Critical (color corruption in legacy textures)
**File:** `VulkanTextureManager.cpp` (three locations)

**Problem:** Textures using A1R5G5B5 format (16-bit with 1-bit alpha) displayed with incorrect colors.

**Root Cause:** The bit-extraction logic incorrectly treated the format as R5G6B5 (no alpha, 6-bit green) instead of A1R5G5B5 (1-bit alpha, 5-bit green).

**Original (Incorrect):**
```cpp
// WRONG - extracted 6-bit green, ignored alpha bit
dst[i * 4 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);   // G
```

**Fixed (Correct):**
```cpp
// A1R5G5B5 format: 1-bit alpha + 5-5-5 RGB
const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
const uint8_t a = (pixel & 0x8000) ? 255u : 0u;  // Extract alpha bit
```

**Affected Locations:**

| # | Lines | Function | Context |
|---|-------|----------|---------|
| 1 | 427-443 | `createTextureFromBitmap()` | Initial texture creation from bmpman data |
| 2 | 794-810 | `uploadTextureData()` | Frame-based texture data upload |
| 3 | 1129-1144 | `updateTexture()` | Dynamic texture updates via `gr_update_texture()` |

All three locations expand A1R5G5B5 (16bpp) to BGRA8 to match `eB8G8R8A8Unorm` format.

---

### 3. Scissor Reset at Frame Start

**Commit:** `980946e8b`
**Severity:** Critical (black holes in rendered scene)
**File:** `VulkanGraphics.cpp:285-290`

**Problem:** Random rectangular black regions appeared in the 3D scene, especially after interacting with UI elements.

**Root Cause:** Vulkan's scissor test is always active (unlike OpenGL where `glEnable(GL_SCISSOR_TEST)` toggles it). If UI code left a scissor rectangle set at end-of-frame, the next frame's 3D scene draws would be clipped to that stale rectangle.

**Fix:** Force full-screen scissor at frame start:
```cpp
// Scissor: force full-screen at frame start.
// Vulkan scissor is always active (unlike OpenGL's scissor test enable/disable), so if UI code
// leaves the clip rectangle set at end-of-frame, using the clip here will cause the *next frame's*
// first draws (often the 3D scene) to be clipped to that stale rect, producing "black coverage holes".
// Clipped UI draws still call gr_set_clip/gr_reset_clip and update scissor explicitly.
vk::Rect2D scissor = createFullScreenScissor();
```

**Helper function** (lines 160-168):
```cpp
vk::Rect2D createFullScreenScissor()
{
    const int w = std::max(0, gr_screen.max_w);
    const int h = std::max(0, gr_screen.max_h);
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{0, 0};
    scissor.extent = vk::Extent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    return scissor;
}
```

**Key Insight:** This is a fundamental Vulkan vs. OpenGL semantic difference that affects all ported code relying on scissor test toggling.

---

### 4. Lightshafts Pass Blending

**Commit:** `ae3a224be`
**Severity:** High (incorrect lightshaft compositing)
**File:** `VulkanRenderer.cpp`

**Problem:** Lightshaft effects were not blending correctly with the scene, resulting in visual artifacts.

**Root Cause:** The lightshafts post-processing pass was using incorrect blend state configuration for additive compositing into the LDR framebuffer.

**Fix:** Configure the lightshafts pipeline for proper additive blending with the scene color buffer.

**Impact:** Lightshaft effects now correctly overlay on the rendered scene with proper additive blending.

---

## Synchronization and Buffer Management

These fixes address GPU/CPU synchronization issues and buffer management edge cases.

### 5. Dynamic Buffer Orphaning

**Commit:** `3fbff05b5`
**Severity:** High (GPU corruption with multiple frames in flight)
**File:** `VulkanBufferManager.cpp:210-243`

**Problem:** Frame-in-flight corruption occurred when updating dynamic buffers, manifesting as flickering geometry or incorrect mesh data.

**Root Cause:** OpenGL's `glBufferData()` performs "buffer orphaning" for dynamic buffers--it allocates new backing storage rather than reusing the existing buffer. The engine relies on this behavior to avoid overwriting data still in use by pending GPU commands. The Vulkan backend was not replicating this semantic.

**Fix:** Implement explicit orphaning for Dynamic and Streaming buffers:
```cpp
// Match OpenGL semantics:
// - gr_update_buffer_data() maps to glBufferData(), which recreates storage (orphaning) for non-persistent buffers.
// - The engine relies on this for Dynamic/Streaming buffers to avoid overwriting GPU-in-flight data with
//   multiple frames in flight.
if (buffer.usage == BufferUsageHint::Dynamic || buffer.usage == BufferUsageHint::Streaming) {
    // Always recreate storage (even if size is unchanged).
    resizeBuffer(handle, size);
} else {
    ensureBuffer(handle, static_cast<vk::DeviceSize>(size));
}
```

**See Also:** `VULKAN_DYNAMIC_BUFFERS.md` for complete buffer management documentation.

---

### 6. Zero-Size Buffer Sub-Update Handling

**Commit:** `32a722adb`
**Severity:** Medium (potential validation errors or crashes)
**File:** `VulkanBufferManager.cpp:245-251`

**Problem:** Edge cases could trigger zero-size buffer updates, which Vulkan may reject or handle inconsistently across drivers.

**Root Cause:** The engine may issue zero-byte buffer updates in edge cases such as building an empty uniform buffer when nothing is visible. OpenGL's `glBufferSubData()` permits this as a valid no-op.

**Fix:**
```cpp
void VulkanBufferManager::updateBufferDataOffset(gr_buffer_handle handle, size_t offset, size_t size, const void* data)
{
    // OpenGL allows 0-byte glBufferSubData calls; the engine may issue these in edge cases
    // (e.g., building an empty uniform buffer when nothing is visible). Treat as a no-op.
    if (size == 0) {
        return;
    }
    // ...
}
```

---

## API and Architecture Improvements

These changes improve the Vulkan backend's architecture, type safety, and feature support.

### 7. Recording Frame Token Sealing

**Commit:** `13fbddac8`
**Severity:** N/A (design improvement, not bug fix)
**File:** `VulkanFrameFlow.h:18-37`

**Purpose:** Enforce capability-based frame access by making the `RecordingFrame` token unforgeable.

**Implementation:**
```cpp
struct RecordingFrame {
    RecordingFrame(const RecordingFrame&) = delete;
    RecordingFrame& operator=(const RecordingFrame&) = delete;
    RecordingFrame(RecordingFrame&&) = default;
    RecordingFrame& operator=(RecordingFrame&&) = default;

    VulkanFrame& ref() const { return frame.get(); }

  private:
    // Only VulkanRenderer may mint the recording token. This makes "recording is active"
    // unforgeable by construction (DESIGN_PHILOSOPHY: capability tokens).
    RecordingFrame(VulkanFrame& f, uint32_t img) : frame(f), imageIndex(img) {}

    std::reference_wrapper<VulkanFrame> frame;
    uint32_t imageIndex;

    vk::CommandBuffer cmd() const { return frame.get().commandBuffer(); }

    friend class VulkanRenderer;
};
```

**Design Properties:**

| Property | Mechanism | Purpose |
|----------|-----------|---------|
| Non-copyable | `= delete` on copy ops | Prevents token duplication |
| Move-only | `= default` on move ops | Allows ownership transfer through frame pipeline |
| Private constructor | `friend class VulkanRenderer` | Only renderer can create tokens |
| Private `cmd()` | Not exposed publicly | Command buffer access restricted to privileged code |

This enforces the invariant that command recording only occurs when a valid token exists. See `VULKAN_CAPABILITY_TOKENS.md` for the broader token design philosophy.

---

### 8. Builtin Texture Sentinel Removal

**Commit:** `25ab5dde1`
**Severity:** Low (cleanup/simplification)

**Change:** Removed special-case sentinel handling for builtin textures that was causing incorrect texture binding behavior. Textures are now managed uniformly through the standard texture manager path.

**Benefit:** Simpler code path, consistent behavior, fewer edge cases to maintain.

---

### 9. Vulkan Feature Chain Updates

**Commit:** (multiple)
**Severity:** N/A (feature enablement)
**File:** `VulkanDevice.cpp:624-746`

**Change:** Added Vulkan 1.1/1.2/1.3/1.4 feature queries to the device creation feature chain and explicitly enabled sampler YCbCr conversion support.

**Query Phase** (lines 624-642):
```cpp
vals.features11 = vk::PhysicalDeviceVulkan11Features{};
vals.features12 = vk::PhysicalDeviceVulkan12Features{};
vals.features13 = vk::PhysicalDeviceVulkan13Features{};
vals.features14 = vk::PhysicalDeviceVulkan14Features{};
vals.extDynamicState = vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT{};
vals.extDynamicState2 = vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT{};
vals.extDynamicState3 = vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT{};
feats.pNext = &vals.features11;
vals.features11.pNext = &vals.features12;
// ... chain continues through dynamic state extensions
```

**Enable Phase** (lines 690-714):
```cpp
vk::PhysicalDeviceVulkan11Features enabled11 = deviceValues.features11;
enabled11.samplerYcbcrConversion = deviceValues.features11.samplerYcbcrConversion ? VK_TRUE : VK_FALSE;
// Explicitly enable dynamicRendering and vertexAttributeInstanceRateDivisor
enabled13.dynamicRendering = deviceValues.features13.dynamicRendering ? VK_TRUE : VK_FALSE;
enabled14.vertexAttributeInstanceRateDivisor = deviceValues.features14.vertexAttributeInstanceRateDivisor;
```

**Impact:** Enables hardware-accelerated YCbCr conversion for native movie texture rendering and modern Vulkan dynamic state features.

---

## Shader Fixes

### 10. AA Bitmap Alpha Coverage Fix

**Commit:** `3fbff05b5`
**Severity:** High (incorrect HUD transparency)
**File:** `interface.frag:24-35`

**Problem:** AA bitmaps (fonts and masks) store their alpha in the red channel (R8 format), not the alpha channel. The shader was reading the wrong channel for coverage tests.

**Fix:**
```glsl
// AA bitmaps (fonts, etc.) are uploaded as single-channel (R8) textures where the mask lives in .r.
// Don't use .a in that case (it will be 1.0 for R8).
float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
if (alphaThreshold > coverage) discard;

// Convert texture from sRGB if needed
// For alpha textures, baseColor is a mask, not color data, so don't apply sRGB conversion to it.
baseColor.rgb = (srgb == 1 && alphaTexture == 0) ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;
```

**Key Points:**
- R8 textures used for fonts/masks store coverage in `.r`, not `.a`
- sRGB conversion must be skipped for mask textures (they encode coverage, not color)
- Works in conjunction with the HUD Alpha Rendering Fix (#1)

---

## New Features

### 11. Shield Impact Decals

**Commit:** `0af11d41f`
**Files:**
- `shield-impact.vert` (vertex shader, 41 lines)
- `shield-impact.frag` (fragment shader, 43 lines)
- `VulkanGraphics.cpp` (rendering integration)
- `VulkanShaderManager.cpp` (shader type registration)

**Purpose:** Native Vulkan implementation of shield impact visual effects, matching the legacy OpenGL `shield-impact-v.sdr` and `shield-impact-f.sdr` behavior.

#### Vertex Shader (shield-impact.vert)

**File:** `code/graphics/shaders/shield-impact.vert:28-38`

```glsl
void main()
{
    gl_Position = projMatrix * modelViewMatrix * vertPosition;

    // Match legacy shield-impact-v.sdr behavior: dot in the shield mesh's local space.
    fragNormOffset = dot(hitNormal, vertNormal);

    fragImpactUV = shieldProjMatrix * shieldModelViewMatrix * vertPosition;
    fragImpactUV += 1.0f;
    fragImpactUV *= 0.5f;
}
```

**Techniques:**
- Computes impact UV coordinates via shield-space projection
- Normal offset determines visibility based on hit direction
- UV range normalized from [-1,1] to [0,1] for texture sampling

#### Fragment Shader (shield-impact.frag)

**File:** `code/graphics/shaders/shield-impact.frag:27-40`

```glsl
void main()
{
    // Match legacy shield-impact-f.sdr semantics.
    if (fragNormOffset < 0.0) discard;
    if (fragImpactUV.x < 0.0 || fragImpactUV.x > 1.0 || fragImpactUV.y < 0.0 || fragImpactUV.y > 1.0) discard;

    vec4 shieldColor = texture(shieldMap, vec3(fragImpactUV.xy, float(shieldMapIndex)));
    shieldColor.rgb = (srgb == 1) ? srgb_to_linear(shieldColor.rgb) * EMISSIVE_GAIN : shieldColor.rgb;

    vec4 blendColor = color;
    blendColor.rgb = (srgb == 1) ? srgb_to_linear(blendColor.rgb) * EMISSIVE_GAIN : blendColor.rgb;

    fragOut0 = shieldColor * blendColor;
}
```

**Key Points:**
- Discards fragments facing away from impact (negative normal offset)
- UV bounds checking prevents texture bleeding
- Emissive gain (2.0x) applied for glow effect
- sRGB linearization for correct HDR blending

---

### 12. Texture Upload Streaming

**Commit:** `1c88b118b`
**File:** `VulkanTextureManager.cpp:1095-1210`

**Purpose:** Implements dynamic texture updates via `gr_update_texture()` for streaming animations and real-time texture modifications.

**Capabilities:**
- Creates resident textures on demand if not already loaded
- Handles format-specific pixel conversions:
  - 8bpp masks (grayscale)
  - 16bpp A1R5G5B5
  - 24bpp RGB
  - 32bpp BGRA
- Prevents updates to block-compressed or multi-layer array textures
- Suspends dynamic rendering before transfer operations (GPU synchronization)

**See Also:** `VULKAN_TEXTURE_BINDING.md` for texture binding architecture.

---

### 13. Movie Rendering Pipeline

**Commit:** `6a4bff512`
**Files:**
- `VulkanMovieManager.h` (class definition, 131 lines)
- `VulkanMovieManager.cpp` (implementation, 744 lines)
- `code/graphics/shaders/movie.vert` (vertex shader, 35 lines)
- `code/graphics/shaders/movie.frag` (fragment shader, 30 lines)

**Purpose:** Native Vulkan movie texture management with hardware-accelerated YCbCr conversion via `VkSamplerYcbcrConversion`.

#### Class Architecture

**File:** `VulkanMovieManager.h:19-51`

```cpp
class VulkanMovieManager {
public:
    VulkanMovieManager(VulkanDevice& device, VulkanShaderManager& shaders);
    bool initialize(uint32_t maxMovieTextures);
    bool isAvailable() const { return m_available; }

    MovieTextureHandle createMovieTexture(uint32_t width, uint32_t height,
        MovieColorSpace colorspace, MovieColorRange range);
    void uploadMovieFrame(const UploadCtx& ctx, MovieTextureHandle handle,
        const ubyte* y, int yStride, const ubyte* u, int uStride, const ubyte* v, int vStride);
    void drawMovieTexture(const RenderCtx& ctx, MovieTextureHandle handle,
        float x1, float y1, float x2, float y2, float alpha);
    void releaseMovieTexture(MovieTextureHandle handle);
    // ...
};
```

#### YcbcrConfig Structure

**File:** `VulkanMovieManager.h:54-60`

Each configuration bundles all Vulkan objects needed for a specific colorspace/range combination:

```cpp
struct YcbcrConfig {
    vk::UniqueSamplerYcbcrConversion conversion;  // Hardware YCbCr->RGB conversion
    vk::UniqueSampler sampler;                     // Immutable sampler with conversion attached
    vk::UniqueDescriptorSetLayout setLayout;       // Single combined-image-sampler binding
    vk::UniquePipelineLayout pipelineLayout;       // Push constants + descriptor set
    vk::UniquePipeline pipeline;                   // Graphics pipeline for this config
};
```

#### YcbcrIndex Mapping

**File:** `VulkanMovieManager.cpp:737-740`

The 4 configurations are indexed by a 2-bit key computed from colorspace and range:

```cpp
uint32_t VulkanMovieManager::ycbcrIndex(MovieColorSpace colorspace, MovieColorRange range) const
{
    return static_cast<uint32_t>(colorspace) * 2u + static_cast<uint32_t>(range);
}
```

| Index | ColorSpace | Range | Vulkan Model | Vulkan Range |
|-------|------------|-------|--------------|--------------|
| 0 | BT601 | Narrow | `eYcbcr601` | `eItuNarrow` |
| 1 | BT601 | Full | `eYcbcr601` | `eItuFull` |
| 2 | BT709 | Narrow | `eYcbcr709` | `eItuNarrow` |
| 3 | BT709 | Full | `eYcbcr709` | `eItuFull` |

BT.601 is typically used for SD content; BT.709 for HD content.

#### MovieTexture Internal Structure

**File:** `VulkanMovieManager.h:62-81`

```cpp
struct MovieTexture {
    vk::UniqueImage image;               // Multi-planar G8_B8_R8 image
    vk::UniqueDeviceMemory memory;       // Device-local memory (possibly dedicated)
    vk::UniqueImageView imageView;       // View with YCbCr conversion attached

    vk::DescriptorSet descriptorSet = VK_NULL_HANDLE;  // Immutable sampler binding

    // Planar staging layout
    uint32_t uploadYStride = 0;          // Y plane row stride (4-byte aligned)
    uint32_t uploadUVStride = 0;         // U/V plane row stride (4-byte aligned)
    vk::DeviceSize yOffset = 0;          // Y plane offset in staging buffer
    vk::DeviceSize uOffset = 0;          // U plane offset in staging buffer
    vk::DeviceSize vOffset = 0;          // V plane offset in staging buffer
    vk::DeviceSize stagingFrameSize = 0; // Total staging allocation per frame

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t ycbcrConfigIndex = 0;       // Index into m_ycbcrConfigs[]
    vk::ImageLayout currentLayout = vk::ImageLayout::eUndefined;
    uint64_t lastUsedSerial = 0;         // For deferred release timing
};
```

#### Planar Memory Layout

**File:** `VulkanMovieManager.cpp:621-637`

The staging buffer layout for YUV420 frames is computed at texture creation:

```cpp
void VulkanMovieManager::initMovieStagingLayout(MovieTexture& tex)
{
    const uint32_t uvW = tex.width / 2;
    const uint32_t uvH = tex.height / 2;

    tex.uploadYStride = alignUpU32(tex.width, 4);     // Y plane: full resolution, 4-byte aligned
    tex.uploadUVStride = alignUpU32(uvW, 4);          // UV planes: half resolution, 4-byte aligned

    const vk::DeviceSize ySize = vk::DeviceSize(tex.uploadYStride) * tex.height;
    const vk::DeviceSize uSize = vk::DeviceSize(tex.uploadUVStride) * uvH;
    const vk::DeviceSize vSize = vk::DeviceSize(tex.uploadUVStride) * uvH;

    tex.yOffset = 0;
    tex.uOffset = alignUpSize(tex.yOffset + ySize, 4);
    tex.vOffset = alignUpSize(tex.uOffset + uSize, 4);
    tex.stagingFrameSize = alignUpSize(tex.vOffset + vSize, 4);
}
```

**Example:** For a 1920x1080 video:
- Y plane: 1920 x 1080 = 2,073,600 bytes
- U plane: 960 x 540 = 518,400 bytes
- V plane: 960 x 540 = 518,400 bytes
- Total: ~3.1 MB per frame (with alignment padding)

#### Layout Transition Tracking

**File:** `VulkanMovieManager.cpp:639-693`

Image layout transitions are tracked per-texture and performed via `vkCmdPipelineBarrier2`:

```cpp
void VulkanMovieManager::transitionForUpload(vk::CommandBuffer cmd, MovieTexture& tex)
{
    vk::ImageMemoryBarrier2 barrier{};
    barrier.oldLayout = tex.currentLayout;
    barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.srcStageMask = (tex.currentLayout == vk::ImageLayout::eShaderReadOnlyOptimal)
        ? vk::PipelineStageFlagBits2::eFragmentShader
        : vk::PipelineStageFlagBits2::eTopOfPipe;
    // ... barrier setup
    cmd.pipelineBarrier2(dep);
    tex.currentLayout = vk::ImageLayout::eTransferDstOptimal;
}

void VulkanMovieManager::transitionForSampling(vk::CommandBuffer cmd, MovieTexture& tex)
{
    // Transitions from eTransferDstOptimal to eShaderReadOnlyOptimal
    // with Transfer -> FragmentShader synchronization
    tex.currentLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
}
```

#### Pipeline Configuration

**File:** `VulkanMovieManager.cpp:246-343`

Each YCbCr configuration gets its own graphics pipeline with:

| Setting | Value | Notes |
|---------|-------|-------|
| Vertex input | Empty | Vertex-less rendering |
| Primitive topology | `eTriangleList` | 6 vertices = 2 triangles |
| Blending | `eSrcAlpha`/`eOneMinusSrcAlpha` | Standard alpha blending |
| Depth test | Disabled | 2D overlay rendering |
| Dynamic state | Viewport, scissor, primitive topology | Runtime configurable |

```cpp
vk::PipelineVertexInputStateCreateInfo vi{};  // Empty - no vertex buffers

vk::PipelineInputAssemblyStateCreateInfo ia{};
ia.topology = vk::PrimitiveTopology::eTriangleList;

std::array<vk::DynamicState, 3> dynStates = {
    vk::DynamicState::eViewport,
    vk::DynamicState::eScissor,
    vk::DynamicState::ePrimitiveTopology,
};
```

#### Vertex Shader (movie.vert)

**File:** `code/graphics/shaders/movie.vert:14-33`

Uses vertex-less rendering with a procedural full-screen quad:

```glsl
void main()
{
    // Two triangles, using vertex-less drawing.
    vec2 positions[6] = vec2[](
        vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0),
        vec2(1.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0)
    );

    vec2 uv = positions[gl_VertexIndex];
    fragTexCoord = uv;

    // Inputs are in screen space with top-left origin (y down).
    vec2 screen = mix(pc.rectMin, pc.rectMax, uv);

    // Convert to NDC. The Vulkan backend uses a flipped viewport to preserve OpenGL-style conventions.
    vec2 ndc;
    ndc.x = 2.0 * (screen.x / pc.screenSize.x) - 1.0;
    ndc.y = 1.0 - 2.0 * (screen.y / pc.screenSize.y);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
```

**Techniques Used:**
- **Vertex-less rendering:** No vertex buffers; positions generated from `gl_VertexIndex`
- **Procedural quad:** 6 vertices forming 2 triangles in a triangle list
- **Push constant rect:** Screen-space rectangle bounds passed via push constants
- **NDC conversion:** Screen coordinates to normalized device coordinates with Y-flip

#### Fragment Shader (movie.frag)

**File:** `code/graphics/shaders/movie.frag:20-28`

```glsl
void main()
{
    vec3 rgb = texture(movieTex, fragTexCoord).rgb;

    // Keep blending consistent with other sRGB sources.
    rgb = srgb_to_linear(rgb);

    outColor = vec4(rgb, pc.alpha);
}
```

**Key Points:**
- **YCbCr conversion:** Performed automatically by the immutable sampler (via `VkSamplerYcbcrConversion`)
- **sRGB linearization:** Output converted to linear space for correct blending with sRGB framebuffer
- **Alpha from push constants:** Global alpha enables fade effects

#### Push Constants Layout

**File:** `VulkanMovieManager.cpp:20-29`

```cpp
struct MoviePushConstants {
    float screenSize[2];   // offset 0:  Screen dimensions for NDC conversion
    float rectMin[2];      // offset 8:  Top-left of movie rect (screen space)
    float rectMax[2];      // offset 16: Bottom-right of movie rect (screen space)
    float alpha;           // offset 24: Global alpha for fade effects
    float pad;             // offset 28: Alignment padding
};
static_assert(sizeof(MoviePushConstants) == 32, "MoviePushConstants must be 32 bytes");
```

---

### 14. Shader Manager Extensions

**Files:** `VulkanShaderManager.h:16-50`, `VulkanShaderManager.cpp:26-205`

#### Dual Caching System

The shader manager maintains two separate caches to handle different shader lookup patterns:

```cpp
// Type+flags based cache (for standard shader types)
std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_vertexModules;
std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_fragmentModules;

// Filename-based cache (for Vulkan-only shaders)
std::unordered_map<SCP_string, vk::UniqueShaderModule> m_filenameModules;
```

#### Variant Flags Handling

**File:** `VulkanShaderManager.cpp:44-180`

Some shader types use unified shader pairs and ignore variant flags for cache lookup:

```cpp
case shader_type::SDR_TYPE_MODEL: {
    // Model path uses a unified shader pair; ignore variant flags for module lookup/cache.
    key.flags = 0;
    // ...
}
```

**Shader Types That Ignore Variant Flags:**

| Type | Reason |
|------|--------|
| `SDR_TYPE_MODEL` | Unified model shader pair |
| `SDR_TYPE_NANOVG` | Unified NanoVG shader pair |
| `SDR_TYPE_ROCKET_UI` | Unified Rocket UI shader pair |
| `SDR_TYPE_POST_PROCESS_TONEMAPPING` | Always outputs linear (swapchain is sRGB) |
| `SDR_TYPE_SHIELD_DECAL` | Unified shield impact shader pair |

#### Filename-Based Lookup

**File:** `VulkanShaderManager.cpp:188-205`

For Vulkan-only shaders that do not map to standard layout contracts:

```cpp
ShaderModules VulkanShaderManager::getModulesByFilenames(const SCP_string& vertFilename, const SCP_string& fragFilename)
{
    return { loadModuleByFilename(vertFilename), loadModuleByFilename(fragFilename) };
}

vk::ShaderModule VulkanShaderManager::loadModuleByFilename(const SCP_string& filename)
{
    auto it = m_filenameModules.find(filename);
    if (it != m_filenameModules.end()) {
        return it->second.get();
    }

    const auto fullPath = (fs::path(m_shaderRoot) / filename).string();
    auto module = loadModule(fullPath);
    auto handle = module.get();
    m_filenameModules.emplace(filename, std::move(module));
    return handle;
}
```

This enables custom shaders like `movie.vert.spv`/`movie.frag.spv` without defining formal layout contracts.

---

### 15. Decoder Color Metadata

**File:** `code/cutscene/Decoder.h:52-63`

Extended `MovieProperties` struct to include color metadata for correct YCbCr conversion:

```cpp
struct MovieProperties {
    FrameSize size;

    float fps = -1.0f;
    float duration = 0.0f;

    FramePixelFormat pixelFormat = FramePixelFormat::Invalid;

    // Color metadata for correct YCbCr conversion selection (best-effort; defaults chosen when unknown).
    MovieColorSpace colorSpace = MovieColorSpace::BT709;
    MovieColorRange colorRange = MovieColorRange::Narrow;
};
```

**Purpose:** Enables proper handling of different video color standards:
- **BT.601:** Standard definition content
- **BT.709:** High definition content
- **Narrow range:** 16-235 (broadcast standard)
- **Full range:** 0-255 (computer graphics)

---

## Summary of Code Patterns

The fixes and features documented here follow consistent design patterns used throughout the Vulkan backend:

| Pattern | Description | Examples |
|---------|-------------|----------|
| **Typestate via constants** | Use correct enum values to enforce type-safe behavior | TCACHE_TYPE vs TEX_TYPE distinction |
| **Orphaning for synchronization** | Allocate new buffer storage for dynamic updates | Dynamic/Streaming buffer handling |
| **Channel-aware shaders** | Check texture type to correctly interpret channel meanings | AA bitmap alpha in `.r` vs `.a` |
| **Feature capability chaining** | Query and chain Vulkan features before device creation | Vulkan 1.1-1.4 feature structures |
| **Format-specific conversion** | Handle variable bpp with explicit bit-extraction logic | A1R5G5B5 to BGRA8 expansion |
| **Native API paths** | Add Vulkan-specific paths alongside generic OpenGL-style APIs | YCbCr movie rendering, shield decals |
| **Capability tokens** | Use move-only types with private constructors | `RecordingFrame` unforgeable token |
| **Defensive edge-case handling** | Treat zero-size operations as no-ops | OpenGL permissive semantics |
| **Frame-start state reset** | Explicitly reset dynamic state at frame boundaries | Scissor reset |

**Design Philosophy:** All fixes follow the pattern of making invalid states structurally unreachable rather than adding runtime guards. See `VULKAN_ARCHITECTURE.md` and the project's `DESIGN_PHILOSOPHY.md` for detailed rationale.

---

## Related Documentation

| Document | Topic |
|----------|-------|
| `VULKAN_ARCHITECTURE.md` | Entry point and high-level renderer overview |
| `VULKAN_CAPABILITY_TOKENS.md` | Token types, creation, and lifetime patterns |
| `VULKAN_SYNCHRONIZATION.md` | Frames-in-flight, semaphores, and barriers |
| `VULKAN_DYNAMIC_BUFFERS.md` | Buffer management and orphaning semantics |
| `VULKAN_TEXTURE_BINDING.md` | Texture binding architecture and residency |
| `VULKAN_RENDER_PASS_STRUCTURE.md` | Dynamic rendering and target transitions |
| `VULKAN_HUD_RENDERING.md` | 2D/UI/HUD rendering contracts |
