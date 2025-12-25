# Recent Vulkan Graphics Fixes and Changes

*Last updated: December 24, 2024*

## 1. HUD Alpha Rendering Fix (fcdd1533d)

The most critical HUD fix addresses texture type detection for alpha blending:

**File: `VulkanGraphics.cpp:1182-1184`**
```cpp
// material::get_texture_type() returns a TCACHE_TYPE_* value, not material::texture_type.
// Use the TCACHE_TYPE_* constant here so AA-bitmaps (font/HUD alpha masks) are handled correctly.
int alphaTexture = (material_info->get_texture_type() == TCACHE_TYPE_AABITMAP) ? 1 : 0;
```

The issue: `material::get_texture_type()` returns a `TCACHE_TYPE_*` constant, not a `material::TEX_TYPE_*` value. Using the wrong enum caused AA bitmaps (fonts and HUD alpha masks) to not be recognized, breaking alpha masking in text and UI rendering.

## 2. 16-Bit Texture Unpacking Fix (fcdd1533d)

Fixed incorrect bit-packing extraction for 16-bit textures in three locations within `VulkanTextureManager.cpp`. The original code incorrectly extracted color channels:

```cpp
// WRONG - incorrect green channel extraction (6 bits instead of 5)
dst[i * 4 + 1] = static_cast<uint8_t>(((pixel >> 5) & 0x3F) * 255 / 63);   // G

// CORRECT - A1R5G5B5 format (alpha bit + 5-5-5 RGB)
const uint8_t b = static_cast<uint8_t>((pixel & 0x1F) * 255 / 31);
const uint8_t g = static_cast<uint8_t>(((pixel >> 5) & 0x1F) * 255 / 31);
const uint8_t r = static_cast<uint8_t>(((pixel >> 10) & 0x1F) * 255 / 31);
const uint8_t a = (pixel & 0x8000) ? 255u : 0u;  // Extract alpha bit
```

**Three code locations with identical unpacking logic:**

| Location | Lines | Function | Purpose |
|----------|-------|----------|---------|
| 1 | `VulkanTextureManager.cpp:428-440` | `createTextureFromBitmap()` | Initial texture creation from bmpman data |
| 2 | `VulkanTextureManager.cpp:795-807` | `uploadTextureData()` | Frame-based texture data upload |
| 3 | `VulkanTextureManager.cpp:1130-1145` | `updateTexture()` | Dynamic texture updates via `gr_update_texture()` |

All three locations expand A1R5G5B5 (16bpp) to BGRA8 to match `eB8G8R8A8Unorm` format.

## 3. Dynamic Buffer Orphaning (45a417e83)

**File: `VulkanBufferManager.cpp:210-243`**

Implements OpenGL buffer orphaning semantics for dynamic buffer updates to avoid GPU-in-flight data corruption:

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

This prevents frame-in-flight synchronization issues by allocating new backing storage rather than reusing existing buffers that may still be in use by pending GPU commands.

## 4. Zero-Size Buffer Sub-Update Handling (32a722adb)

**File: `VulkanBufferManager.cpp:247-251`**

Added protection against zero-size buffer sub-updates, which can occur in edge cases such as building an empty uniform buffer when nothing is visible:

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

This matches OpenGL's permissive behavior where `glBufferSubData()` with size 0 is a valid no-op.

## 5. Scissor Reset at Frame Start (980946e8b)

**File: `VulkanGraphics.cpp:285-290`**

Fixed "black coverage holes" caused by stale scissor rectangles persisting across frames:

```cpp
// Scissor: force full-screen at frame start.
// Vulkan scissor is always active (unlike OpenGL's scissor test enable/disable), so if UI code
// leaves the clip rectangle set at end-of-frame, using the clip here will cause the *next frame's*
// first draws (often the 3D scene) to be clipped to that stale rect, producing "black coverage holes".
// Clipped UI draws still call gr_set_clip/gr_reset_clip and update scissor explicitly.
vk::Rect2D scissor = createFullScreenScissor();
```

The helper function at lines 160-168:
```cpp
vk::Rect2D createFullScreenScissor()
{
    const int w = std::max(0, gr_screen.max_w);
    const int h = std::max(0, gr_screen.max_h);
    return vk::Rect2D{ vk::Offset2D{0, 0}, vk::Extent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)} };
}
```

Unlike OpenGL where scissor test can be disabled, Vulkan's scissor is always active. This fix ensures each frame starts with a full-screen scissor to prevent UI clipping from affecting subsequent frames.

## 6. Recording Frame Token Sealing (13fbddac8)

**File: `VulkanFrameFlow.h:18-37`**

Sealed the `RecordingFrame` token to enforce capability-based frame access:

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

Key design points:
- **Non-copyable**: Prevents token duplication
- **Move-only**: Allows ownership transfer through the frame pipeline
- **Private constructor**: Only `VulkanRenderer` can create tokens (via `friend` declaration)
- **Private `cmd()` accessor**: Command buffer access restricted to privileged code

This enforces the invariant that command recording only occurs when a valid token exists.

## 7. Builtin Texture Sentinel Removal (25ab5dde1)

Removed special-case sentinel handling for builtin textures that was causing incorrect texture binding behavior. Textures are now managed uniformly through the standard texture manager path.

## 8. AA Bitmap Alpha Coverage Fix (45a417e83)

**File: `interface.frag:25-35`**

Fixed shader alpha threshold logic for AA bitmaps (fonts/masks):

```glsl
// For AA bitmaps (R8 textures), alpha mask lives in .r channel, not .a
float coverage = (alphaTexture == 1) ? baseColor.r : baseColor.a;
if (alphaThreshold > coverage) discard;

// Don't apply sRGB conversion to alpha textures (they're masks, not colors)
baseColor.rgb = (srgb == 1 && alphaTexture == 0) ? srgb_to_linear(baseColor.rgb) : baseColor.rgb;
```

## 9. Vulkan Feature Chain Updates

**File: `VulkanDevice.cpp:625-739`**

Added Vulkan 1.1 feature queries to the feature chain and explicitly enabled sampler YCbCr conversion support:

```cpp
// Query phase (lines 625-639):
vals.features11 = vk::PhysicalDeviceVulkan11Features{};
vals.features12 = vk::PhysicalDeviceVulkan12Features{};
vals.features13 = vk::PhysicalDeviceVulkan13Features{};
vals.features14 = vk::PhysicalDeviceVulkan14Features{};
feats.pNext = &vals.features11;
vals.features11.pNext = &vals.features12;
// ... chain continues

// Enable phase (lines 694-698):
vk::PhysicalDeviceVulkan11Features enabled11 = deviceValues.features11;
enabled11.samplerYcbcrConversion = deviceValues.features11.samplerYcbcrConversion ? VK_TRUE : VK_FALSE;
```

This enables support for native YCbCr texture sampling used in movie rendering.

## 10. Texture Upload Streaming (5ecd8e418)

**File: `VulkanTextureManager.cpp:1095-1210`**

Implements dynamic texture updates via `gr_update_texture()` for streaming animations and real-time texture modifications:

Key features:
- Creates resident textures on demand if not already loaded
- Handles format-specific pixel conversions (8bpp masks, 16bpp A1R5G5B5, 24bpp RGB, 32bpp BGRA)
- Prevents updates to block-compressed or multi-layer array textures
- Suspends dynamic rendering before transfer operations (GPU synchronization)

---

## 11. Movie Rendering Pipeline (6a4bff512)

### Overview

Native Vulkan movie texture management with hardware-accelerated YCbCr support via `VkSamplerYcbcrConversion`.

**Files:**
- `VulkanMovieManager.h` (class definition, 131 lines)
- `VulkanMovieManager.cpp` (implementation, 744 lines)
- `code/graphics/shaders/movie.vert` (vertex shader, 35 lines)
- `code/graphics/shaders/movie.frag` (fragment shader, 30 lines)

### VulkanMovieManager Class Architecture

**File: `VulkanMovieManager.h:19-127`**

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

Note: The upload function is named `uploadMovieFrame()`, not `uploadMovieTexture()`.

### YcbcrConfig Structure

**File: `VulkanMovieManager.h:54-60`**

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

### YcbcrIndex Mapping

**File: `VulkanMovieManager.cpp:737-740`**

The 4 configurations are indexed by a 2-bit key:

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

### MovieTexture Internal Structure

**File: `VulkanMovieManager.h:62-81`**

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

### Planar Memory Layout

**File: `VulkanMovieManager.cpp:621-637`**

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

For a 1920x1080 video:
- Y plane: 1920 x 1080 = 2,073,600 bytes
- U plane: 960 x 540 = 518,400 bytes
- V plane: 960 x 540 = 518,400 bytes
- Total: ~3.1 MB per frame (with alignment padding)

### Layout Transition Tracking

**File: `VulkanMovieManager.cpp:639-693`**

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

### Pipeline Setup

**File: `VulkanMovieManager.cpp:246-343`**

Each YCbCr configuration gets its own pipeline with:
- **Vertex input**: Empty (vertex-less rendering)
- **Primitive topology**: `eTriangleList` (6 vertices = 2 triangles)
- **Blending**: Standard alpha blending (`eSrcAlpha`/`eOneMinusSrcAlpha`)
- **Depth test**: Disabled
- **Dynamic state**: Viewport, scissor, primitive topology

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

### Movie Shader Details

#### Vertex Shader (movie.vert)

**File: `code/graphics/shaders/movie.vert:14-33`**

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

Key techniques:
- **Vertex-less rendering**: No vertex buffers; positions generated from `gl_VertexIndex`
- **Procedural quad**: 6 vertices forming 2 triangles in a triangle list
- **Push constant rect**: Screen-space rectangle bounds passed via push constants
- **NDC conversion**: Screen coordinates to normalized device coordinates with Y-flip

#### Fragment Shader (movie.frag)

**File: `code/graphics/shaders/movie.frag:20-28`**

```glsl
void main()
{
    vec3 rgb = texture(movieTex, fragTexCoord).rgb;

    // Keep blending consistent with other sRGB sources.
    rgb = srgb_to_linear(rgb);

    outColor = vec4(rgb, pc.alpha);
}
```

Key points:
- **YCbCr conversion**: Performed automatically by the sampler (via `VkSamplerYcbcrConversion`)
- **sRGB linearization**: Output converted to linear space for correct blending with sRGB framebuffer
- **Alpha from push constants**: Global alpha for fade effects

#### Push Constants Layout

**File: `VulkanMovieManager.cpp:20-29`**

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

## 12. Shader Manager Extensions

**Files: `VulkanShaderManager.h:16-50`, `VulkanShaderManager.cpp:1-181`**

### Dual Caching System

The shader manager maintains two separate caches:

```cpp
// Type+flags based cache (for standard shader types)
std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_vertexModules;
std::unordered_map<Key, vk::UniqueShaderModule, KeyHasher> m_fragmentModules;

// Filename-based cache (for Vulkan-only shaders)
std::unordered_map<SCP_string, vk::UniqueShaderModule> m_filenameModules;
```

### Variant Flags System

**File: `VulkanShaderManager.cpp:26-112`**

Some shader types ignore variant flags for cache lookup:

```cpp
case shader_type::SDR_TYPE_MODEL: {
    // Model path uses a unified shader pair; ignore variant flags for module lookup/cache.
    key.flags = 0;
    // ...
}
case shader_type::SDR_TYPE_NANOVG: {
    // NanoVG path uses a unified shader pair; ignore variant flags for module lookup/cache.
    key.flags = 0;
    // ...
}
case shader_type::SDR_TYPE_ROCKET_UI: {
    // Rocket UI path uses a unified shader pair; ignore variant flags for module lookup/cache.
    key.flags = 0;
    // ...
}
case shader_type::SDR_TYPE_POST_PROCESS_TONEMAPPING: {
    // Vulkan tonemapping pass: always outputs linear (swapchain is sRGB).
    key.flags = 0;
    // ...
}
```

Shader types that ignore variant flags:
- `SDR_TYPE_MODEL`
- `SDR_TYPE_NANOVG`
- `SDR_TYPE_ROCKET_UI`
- `SDR_TYPE_POST_PROCESS_TONEMAPPING`

### Filename-Based Lookup

**File: `VulkanShaderManager.cpp:114-131`**

For Vulkan-only shaders that don't map to standard layout contracts:

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

## 13. Decoder Color Metadata

**File: `Decoder.h`**

Extended `MovieProperties` struct to include color metadata for correct YCbCr conversion:

```cpp
MovieColorSpace colorSpace = MovieColorSpace::BT709;
MovieColorRange colorRange = MovieColorRange::Narrow;
```

This enables proper handling of different video color standards (BT.601 for SD content, BT.709 for HD content).

---

## Summary of Code Patterns

1. **Typestate via constants**: Using correct enum values (`TCACHE_TYPE_AABITMAP` not `material::TEX_TYPE_AABITMAP`) to enforce correct behavior.

2. **Orphaning for synchronization**: Allocating new buffer storage for dynamic updates to avoid stale GPU data.

3. **Channel-aware shaders**: Checking texture type in shaders to correctly interpret channel meanings (alpha vs. color).

4. **Feature capability checking**: Querying and chaining Vulkan features before device creation to enable device-specific capabilities.

5. **Format-specific pixel conversion**: Handling variable bpp and format specifications with explicit bit-extraction logic.

6. **Native API paths**: Adding Vulkan-specific rendering paths (YCbCr movie rendering) alongside generic OpenGL-style APIs.

7. **Capability tokens**: Using move-only types with private constructors to make invalid states structurally unreachable (e.g., `RecordingFrame`).

8. **Defensive edge-case handling**: Treating zero-size operations as no-ops to match OpenGL's permissive semantics.

9. **Frame-start state reset**: Explicitly resetting dynamic state (scissor) at frame boundaries to prevent cross-frame pollution.

All commits follow the pattern of fixing defects by making invalid states structurally unreachable rather than adding runtime guards.
