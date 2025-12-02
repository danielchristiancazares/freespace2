# FreeSpace 2 Open - Vulkan Rendering Pipeline Port

**Version:** 2.2
**Status:** Ready for Implementation (pending open question resolution)

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Requirements Specification](#2-requirements-specification)
3. [Design Decisions](#3-design-decisions)
4. [Implementation Plan](#4-implementation-plan)
5. [Technical Reference](#5-technical-reference)
6. [Appendices](#6-appendices)

---

## 1. Executive Summary

### 1.1 Purpose

Port the FreeSpace 2 Open (FSO) rendering backend from OpenGL to Vulkan, achieving feature parity with the existing implementation while maintaining compatibility with the current codebase architecture.

### 1.2 Scope

**In Scope:**
- Vulkan 1.4 rendering backend as alternative to OpenGL
- Feature parity with current OpenGL implementation
- Perceptually identical visual output
- Windows and Linux platform support

**Out of Scope:**
- macOS support (MoltenVK - future work)
- OpenXR/VR support (separate initiative)
- Ray tracing extensions
- Mobile platforms
- Performance optimization beyond parity
- Legacy hardware support (use OpenGL fallback)

### 1.3 Constraints

| Constraint | Value | Rationale |
|------------|-------|-----------|
| Minimum Vulkan Version | 1.4 | Modern API, dynamic rendering, sync2 |
| API Version | VK_API_VERSION_1_4 | Released 2024, wide driver support |
| Minimum SDL Version | 2.0.6 | `VulkanRenderer.h:9` |
| Memory Allocator | VMA 3.0+ | Design decision |
| Shader Format | SPIR-V 1.6 | Vulkan 1.4 requirement |
| Fallback | OpenGL | Users with older hardware use existing OpenGL backend |

### 1.4 Current State

**Existing Vulkan Code:**
- `VulkanRenderer.cpp` (908 lines): Instance, device, swapchain, basic render pass
- `RenderFrame.cpp`: Frame synchronization with semaphores/fences
- `gr_vulkan.cpp`: Initialization entry point
- `vulkan_stubs.cpp` (396 lines): 70+ stub function implementations

**Existing OpenGL Reference:**
- ~4,800 lines across 8 implementation files
- Function pointer abstraction in `gr_screen` struct (~80 functions)
- SPIR-V shader infrastructure exists

### 1.5 Design Principles

1. **Make it work, make it right, make it fast** - in that order
2. **Vertical slices over horizontal layers** - each milestone produces visible results
3. **No premature abstraction** - add wrappers only when patterns emerge
4. **Fail fast** - validation layers always on in debug builds

---

## 2. Requirements Specification

### 2.1 Functional Requirements - Core Rendering

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-001 | Initialize Vulkan instance and device | Game launches with `-vulkan` flag |
| FR-002 | Create and manage swapchain | Render to window at configured resolution |
| FR-003 | Handle window resize | Resize without crash, correct rendering after |
| FR-004 | Handle minimize/restore | No crash, rendering resumes correctly |
| FR-005 | Handle fullscreen toggle | Mode switch without crash |
| FR-006 | Clear screen to color | `gf_clear()` fills screen with configured color |
| FR-007 | Present frame | `gf_flip()` displays rendered frame |

### 2.2 Functional Requirements - Resources

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-010 | Create vertex/index buffers | `gf_create_buffer()` returns valid handle |
| FR-011 | Update buffer data | `gf_update_buffer_data()` uploads correctly |
| FR-012 | Map/unmap buffers | `gf_map_buffer()` returns valid pointer |
| FR-013 | Delete buffers | `gf_delete_buffer()` frees resources |
| FR-014 | Create textures from bitmaps | `gf_bm_create()`, `gf_bm_data()` work |
| FR-015 | Support compressed textures | DXT1, DXT3, DXT5, BC7 formats load |
| FR-016 | Support texture arrays | Animated textures render correctly |
| FR-017 | Support cubemaps | Environment maps render correctly |
| FR-018 | Create render targets | `gf_bm_make_render_target()` works |
| FR-019 | Render to texture | `gf_bm_set_render_target()` works |

### 2.3 Functional Requirements - Shaders

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-020 | Load SPIR-V shaders | Pre-compiled shaders load successfully |
| FR-021 | Support shader variants | FLAG-based compilation produces correct results |
| FR-022 | Bind uniform buffers | `gf_bind_uniform_buffer()` updates shader data |
| FR-023 | Support all 24 shader types | All `shader_type` enum values work |

### 2.4 Functional Requirements - Rendering

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-030 | Render 3D models | `gf_render_model()` displays ship/station models |
| FR-031 | Render primitives | `gf_render_primitives()` draws basic geometry |
| FR-032 | Render particles | `gf_render_primitives_particle()` displays effects |
| FR-033 | Render UI (NanoVG) | `gf_render_nanovg()` draws vector UI |
| FR-034 | Render UI (RocketUI) | `gf_render_rocket_primitives()` draws HTML UI |
| FR-035 | Render batched bitmaps | `gf_render_primitives_batched()` draws HUD |
| FR-036 | Render video | `gf_render_movie()` plays cutscenes |

### 2.5 Functional Requirements - State

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-040 | Depth testing | `gf_zbuffer_set()` modes work correctly |
| FR-041 | Stencil operations | `gf_stencil_set()` modes work correctly |
| FR-042 | Blend modes | All 6 `gr_alpha_blend` modes work |
| FR-043 | Culling | `gf_set_cull()` enables/disables face culling |
| FR-044 | Scissor/clip regions | `gf_set_clip()`, `gf_reset_clip()` work |
| FR-045 | Viewport | `gf_set_viewport()` sets render area |

### 2.6 Functional Requirements - Advanced

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-050 | Deferred lighting | G-buffer renders, lighting accumulates |
| FR-051 | Shadow mapping | 4-cascade shadows render correctly |
| FR-052 | Post-processing | Bloom, AA, tonemapping effects work |
| FR-053 | Distortion effects | Thruster/warp distortion visible |
| FR-054 | Shield effects | Impact visualization renders |
| FR-055 | Decal rendering | Damage decals appear on models |

### 2.7 Capability Requirements

| ID | Capability | Vulkan Support |
|----|------------|----------------|
| FR-060 | CAPABILITY_ENVIRONMENT_MAP | Yes |
| FR-061 | CAPABILITY_NORMAL_MAP | Yes |
| FR-062 | CAPABILITY_HEIGHT_MAP | Yes |
| FR-063 | CAPABILITY_SOFT_PARTICLES | Yes |
| FR-064 | CAPABILITY_DISTORTION | Yes |
| FR-065 | CAPABILITY_POST_PROCESSING | Yes |
| FR-066 | CAPABILITY_DEFERRED_LIGHTING | Yes |
| FR-067 | CAPABILITY_SHADOWS | Yes |
| FR-068 | CAPABILITY_THICK_OUTLINE | Conditional (geometry shader) |
| FR-069 | CAPABILITY_BATCHED_SUBMODELS | Yes |
| FR-070 | CAPABILITY_TIMESTAMP_QUERY | Yes |
| FR-071 | CAPABILITY_SEPARATE_BLEND_FUNCTIONS | Conditional |
| FR-072 | CAPABILITY_PERSISTENT_BUFFER_MAPPING | Yes (always in Vulkan) |
| FR-073 | CAPABILITY_BPTC | Conditional (BC7 support) |
| FR-074 | CAPABILITY_LARGE_SHADER | Yes |
| FR-075 | CAPABILITY_INSTANCED_RENDERING | Yes |

### 2.8 Non-Functional Requirements

| ID | Category | Requirement | Metric |
|----|----------|-------------|--------|
| NFR-001 | Performance | Frame rate parity | Within 10% of OpenGL |
| NFR-002 | Performance | No frame time spikes | <100ms stalls |
| NFR-003 | Performance | Memory usage parity | Within 20% of OpenGL |
| NFR-010 | Reliability | Validation clean | Zero errors after 1000 frames |
| NFR-011 | Reliability | Crash free | No crashes in standard gameplay |
| NFR-012 | Reliability | Device lost recovery | Recovers from GPU reset |
| NFR-020 | Compatibility | NVIDIA support | GTX 900 series+ (Maxwell Gen2+) |
| NFR-021 | Compatibility | AMD support | RX 400 series+ (Polaris/GCN 4.0+) |
| NFR-022 | Compatibility | Intel support | HD 500 series+ (Skylake Gen9+) |
| NFR-023 | Compatibility | Older hardware | Use OpenGL backend (automatic fallback) |

---

## 3. Design Decisions

### 3.1 Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                      gr_screen                          │
│  (Function pointers - unchanged from OpenGL approach)   │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│                   gr_vulkan.cpp                         │
│  (Entry point, function pointer assignment)             │
└────────────────────────┬────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────┐
│                  VulkanRenderer                         │
│  - Instance, device, queues                             │
│  - Swapchain management                                 │
│  - Frame synchronization (via RenderFrame)              │
│  - Pipeline cache (simple hash map)                     │
│  - Shader compilation (on-demand)                       │
└─────────────┬───────────────────────┬───────────────────┘
              │                       │
┌─────────────▼──────────┐ ┌─────────▼───────────────────┐
│   Global Resource Maps │ │          VMA                │
│  - g_buffers           │ │  (Used directly, no wrapper)│
│  - g_textures          │ │                             │
│  - g_pipelineCache     │ │                             │
│  - g_shaderCache       │ │                             │
└────────────────────────┘ └─────────────────────────────┘
```

### 3.2 Key Design Decisions

#### 3.2.1 No Unnecessary Abstraction Layers

**Rejected approach:** Creating wrapper classes for every Vulkan concept
- ~~VulkanMemoryAllocator~~ → Use VMA directly
- ~~CommandBufferPool~~ → Extend existing RenderFrame
- ~~BufferManager~~ → Simple `SCP_unordered_map<handle, resource>`
- ~~TextureManager~~ → Integrate with existing bmpman
- ~~StateManager~~ → Vulkan uses immutable pipelines; no state tracking needed

**Rationale:** Vulkan's explicit API doesn't benefit from additional abstraction. Start simple, add wrappers only when patterns emerge from working code.

#### 3.2.2 Pipeline Caching Strategy

Vulkan requires immutable pipeline objects. Strategy:

```cpp
struct PipelineKey {
    shader_type shader;
    uint32_t shaderFlags;
    gr_alpha_blend blendMode;
    gr_zbuffer_type depthMode;
    int cullMode;
    VkRenderPass renderPass;
};

SCP_unordered_map<PipelineKey, VkPipeline> g_pipelineCache;
```

Create pipelines on first use, cache by state combination. Accept initial stalls; optimize only if profiling shows need.

#### 3.2.3 Shader Compilation

Two-stage compilation for `.sdr` files:
1. Run FSO preprocessor to expand `#predefine`/`#prereplace`
2. Compile result with `glslangValidator` to SPIR-V

Compile on-demand, cache results. Defer async compilation until proven necessary.

#### 3.2.4 Descriptor Set Strategy

Per-frame allocation with pool reset:
- Allocate from pool during frame
- Reset entire pool at frame start
- Simple, avoids complex bookkeeping

#### 3.2.5 Resource Management

Simple handle-to-resource maps:

```cpp
namespace vulkan {
    struct BufferResource {
        VkBuffer buffer;
        VmaAllocation allocation;
        size_t size;
        void* mappedPtr;
    };
    SCP_unordered_map<int, BufferResource> g_buffers;

    struct TextureResource {
        VkImage image;
        VkImageView view;
        VmaAllocation allocation;
    };
    SCP_unordered_map<int, TextureResource> g_textures;
}
```

### 3.3 Critical Infrastructure

#### 3.3.1 Swapchain Recreation

Required for window resize, minimize, display mode changes:

```cpp
void VulkanRenderer::recreateSwapchain() {
    int width = 0, height = 0;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    while (width == 0 || height == 0) {
        SDL_Vulkan_GetDrawableSize(window, &width, &height);
        SDL_WaitEvent(nullptr);
    }

    m_device->waitIdle();
    m_swapChainFramebuffers.clear();
    m_swapChainImageViews.clear();
    m_swapChain.reset();

    createSwapChain(deviceValues);
    createFrameBuffers();
}
```

#### 3.3.2 Error Handling

```cpp
// Swapchain out of date
VkResult result = vkAcquireNextImageKHR(...);
if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    recreateSwapchain();
    return;
}

// Device lost
if (result == VK_ERROR_DEVICE_LOST) {
    mprintf(("Vulkan device lost! Attempting recovery...\n"));
    shutdown();
    initialize();
}
```

#### 3.3.3 Validation Layer Integration

```cpp
VkBool32 debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                       VkDebugUtilsMessageTypeFlagsEXT type,
                       const VkDebugUtilsMessengerCallbackDataEXT* data,
                       void* user) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        mprintf(("Vulkan: %s\n", data->pMessage));
    }
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ASSERT(0);
    }
    return VK_FALSE;
}
```

---

## 4. Implementation Plan

### 4.1 Pre-Implementation Checklist

Before starting Milestone 0:
- [ ] Open questions Q-001 through Q-013 resolved (see Section 6.1)
- [ ] VMA library added to build system
- [ ] `glslangValidator` available for shader compilation
- [ ] Test hardware identified (NVIDIA, AMD, Intel)

### 4.2 Milestone 0: Build Foundation

**Requirements:** FR-001, FR-002, FR-003, FR-004, FR-005
**Acceptance:** Game starts with `-vulkan`, handles resize, no validation errors

| Task | Files | Test |
|------|-------|------|
| Add VMA to build system | `CMakeLists.txt` | Compiles |
| Add `WITH_VULKAN` flag | `CMakeLists.txt` | Both ON/OFF compile |
| Implement swapchain recreation | `VulkanRenderer.cpp` | Resize 5 times |
| Handle VK_ERROR_OUT_OF_DATE | `VulkanRenderer.cpp` | Resize during render |
| Handle VK_ERROR_DEVICE_LOST | `VulkanRenderer.cpp` | Document recovery |
| Set up validation callback | `VulkanRenderer.cpp` | Errors logged |
| Integrate VMA | `VulkanRenderer.cpp` | No direct vkAllocateMemory |

### 4.3 Milestone 1: First Triangle

**Requirements:** FR-006, FR-007
**Acceptance:** Colored triangle renders

| Task | Files | Test |
|------|-------|------|
| Create test SPIR-V shaders | `shaders/test.vert/frag` | Compiles |
| Create vertex buffer | `VulkanRenderer.cpp` | No validation errors |
| Create graphics pipeline | `VulkanRenderer.cpp` | Pipeline created |
| Implement `gf_clear()` | Vulkan impl | Screen clears |
| Implement `gf_flip()` | Vulkan impl | Triangle visible |

### 4.4 Milestone 2: Textured Quad

**Requirements:** FR-010, FR-011, FR-014, FR-022
**Acceptance:** Textured rotating quad renders

| Task | Files | Test |
|------|-------|------|
| Implement `gf_create_buffer()` | `VulkanBuffer.cpp` | Valid handle |
| Implement `gf_update_buffer_data()` | `VulkanBuffer.cpp` | Data uploaded |
| Create texture from test image | `VulkanTexture.cpp` | VkImage created |
| Create sampler | `VulkanTexture.cpp` | Filtering works |
| Create descriptor set layout | `VulkanRenderer.cpp` | Texture binds |
| Implement MVP uniform | `VulkanRenderer.cpp` | Quad rotates |

### 4.5 Milestone 3: Model Rendering

**Requirements:** FR-014-017, FR-020-023, FR-030
**Acceptance:** GTF Ulysses model renders with textures and lighting

| Task | Files | Test |
|------|-------|------|
| Integrate with bmpman | `VulkanTexture.cpp` | `gf_bm_*` work |
| Support DXT compression | `VulkanTexture.cpp` | DXT textures load |
| Compile main shaders | Build script | Shaders compile |
| Implement `gf_maybe_create_shader()` | `VulkanShader.cpp` | Shaders load |
| Implement model uniforms | `VulkanRenderer.cpp` | Uniforms bind |
| Implement `gf_render_model()` | `VulkanRenderer.cpp` | Model visible |

### 4.6 Milestone 4: Basic Scene

**Requirements:** FR-012, FR-013, FR-031, FR-033-035, FR-040-045
**Acceptance:** Main menu navigable, tech room functional

| Task | Files | Test |
|------|-------|------|
| Implement `gf_delete_buffer()` | `VulkanBuffer.cpp` | No leaks |
| Implement `gf_map_buffer()` | `VulkanBuffer.cpp` | Valid pointer |
| Implement `gf_render_primitives()` | `VulkanRenderer.cpp` | Geometry renders |
| Implement UI renderers | `VulkanRenderer.cpp` | UI visible |
| Implement state functions | `VulkanRenderer.cpp` | States work |
| Create blend mode pipelines | `VulkanRenderer.cpp` | All 6 modes work |

### 4.7 Milestone 5: Playable Mission

**Requirements:** FR-018, FR-019, FR-032, FR-036, FR-053, FR-054
**Acceptance:** Training mission 1 completable

| Task | Files | Test |
|------|-------|------|
| Implement particle rendering | `VulkanRenderer.cpp` | Particles visible |
| Implement distortion effects | `VulkanRenderer.cpp` | Distortion works |
| Implement shield effects | `VulkanRenderer.cpp` | Shield hits visible |
| Implement video rendering | `VulkanRenderer.cpp` | Cutscenes play |
| Implement scene textures | `VulkanRenderer.cpp` | Scene capture works |
| Implement render targets | `VulkanTexture.cpp` | RTT works |

### 4.8 Milestone 6: Visual Parity

**Requirements:** FR-050-055, FR-060-082
**Acceptance:** Reviewer approves side-by-side comparison

| Task | Files | Test |
|------|-------|------|
| Implement G-buffer pass | `VulkanDeferred.cpp` | G-buffer fills |
| Implement deferred lighting | `VulkanDeferred.cpp` | Lights accumulate |
| Implement shadow mapping | `VulkanShadows.cpp` | Shadows render |
| Implement post-processing | `VulkanPostProcess.cpp` | Effects work |
| Implement decals | `VulkanRenderer.cpp` | Decals visible |
| Implement capabilities | `VulkanRenderer.cpp` | All report correctly |

### 4.9 Milestone 7: Production Ready

**Requirements:** NFR-001 through NFR-032
**Acceptance:** 1-hour gameplay without validation errors or crashes

| Task | Files | Test |
|------|-------|------|
| Profile and optimize | Various | NFR-001 met |
| Fix validation warnings | Various | Zero warnings |
| Test NVIDIA hardware | - | NFR-020 met |
| Test AMD hardware | - | NFR-021 met |
| Test Intel hardware | - | NFR-022 met |
| Document limitations | `VULKAN_NOTES.md` | Complete |

### 4.10 Function Implementation Checklist

**Milestone 0-1 (8 functions):**
- [ ] `gf_flip`, `gf_setup_frame`
- [ ] `gf_clear`, `gf_set_clear_color`
- [ ] `gf_zbuffer_get`, `gf_zbuffer_set`, `gf_zbuffer_clear`
- [ ] `gf_set_viewport`

**Milestone 2 (8 functions):**
- [ ] `gf_create_buffer`, `gf_update_buffer_data`, `gf_update_buffer_data_offset`
- [ ] `gf_bm_create`, `gf_bm_init`, `gf_bm_data`, `gf_bm_page_in_start`
- [ ] `gf_preload`

**Milestone 3 (4 functions):**
- [ ] `gf_render_model`
- [ ] `gf_maybe_create_shader`
- [ ] `gf_bind_uniform_buffer`
- [ ] `gf_update_transform_buffer`

**Milestone 4 (14 functions):**
- [ ] `gf_delete_buffer`, `gf_map_buffer`, `gf_flush_mapped_buffer`
- [ ] `gf_render_primitives`, `gf_render_primitives_batched`
- [ ] `gf_render_nanovg`, `gf_render_rocket_primitives`
- [ ] `gf_set_clip`, `gf_reset_clip`, `gf_set_cull`, `gf_set_color_buffer`
- [ ] `gf_stencil_set`, `gf_stencil_clear`, `gf_alpha_mask_set`

**Milestone 5 (11 functions):**
- [ ] `gf_render_primitives_particle`, `gf_render_primitives_distortion`
- [ ] `gf_render_shield_impact`, `gf_render_movie`
- [ ] `gf_scene_texture_begin`, `gf_scene_texture_end`, `gf_copy_effect_texture`
- [ ] `gf_bm_make_render_target`, `gf_bm_set_render_target`
- [ ] `gf_bm_free_data`, `gf_set_texture_addressing`

**Milestone 6 (18 functions):**
- [ ] `gf_deferred_lighting_begin/end/msaa/finish`
- [ ] `gf_shadow_map_start`, `gf_shadow_map_end`
- [ ] `gf_post_process_begin/end/set_effect/set_defaults/save_zbuffer/restore_zbuffer`
- [ ] `gf_render_decals`, `gf_start_decal_pass`, `gf_stop_decal_pass`
- [ ] `gf_is_capable`, `gf_get_property`, `gf_sphere`

**Deferred (17 functions):**
- [ ] `gf_save_screen`, `gf_restore_screen`, `gf_free_screen`
- [ ] `gf_get_region`, `gf_print_screen`, `gf_blob_screen`
- [ ] `gf_create_query_object`, `gf_query_value`, `gf_query_value_available`
- [ ] `gf_get_query_value`, `gf_delete_query_object`
- [ ] `gf_sync_fence`, `gf_sync_wait`, `gf_sync_delete`
- [ ] `gf_dump_envmap`, `gf_calculate_irrmap`
- [ ] `gf_update_texture`, `gf_get_bitmap_from_texture`

**Total: 80 functions**

---

## 5. Technical Reference

### 5.1 Blend Mode Mapping

```cpp
void mapBlendMode(gr_alpha_blend mode, VkBlendFactor& src, VkBlendFactor& dst) {
    switch (mode) {
    case ALPHA_BLEND_NONE:
        src = VK_BLEND_FACTOR_ONE;
        dst = VK_BLEND_FACTOR_ZERO;
        break;
    case ALPHA_BLEND_ADDITIVE:
        src = VK_BLEND_FACTOR_ONE;
        dst = VK_BLEND_FACTOR_ONE;
        break;
    case ALPHA_BLEND_ALPHA_ADDITIVE:
        src = VK_BLEND_FACTOR_SRC_ALPHA;
        dst = VK_BLEND_FACTOR_ONE;
        break;
    case ALPHA_BLEND_ALPHA_BLEND_ALPHA:
        src = VK_BLEND_FACTOR_SRC_ALPHA;
        dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR:
        src = VK_BLEND_FACTOR_SRC_ALPHA;
        dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        break;
    case ALPHA_BLEND_PREMULTIPLIED:
        src = VK_BLEND_FACTOR_ONE;
        dst = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
}
```

### 5.2 Texture Format Mapping

```cpp
VkFormat mapTextureFormat(int bpp, int flags, bool compressed) {
    if (compressed) {
        if (flags & DDS_DXT1) return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        if (flags & DDS_DXT3) return VK_FORMAT_BC2_UNORM_BLOCK;
        if (flags & DDS_DXT5) return VK_FORMAT_BC3_UNORM_BLOCK;
        if (flags & DDS_BC7)  return VK_FORMAT_BC7_UNORM_BLOCK;
    }
    // Always use 32-bit for uncompressed (24-bit has poor support)
    return VK_FORMAT_R8G8B8A8_UNORM;
}
```

### 5.3 Vertex Format Mapping

```cpp
VkFormat mapVertexFormat(vertex_format_data::vertex_format fmt) {
    switch (fmt) {
    case POSITION4:  return VK_FORMAT_R32G32B32A32_SFLOAT;
    case POSITION3:  return VK_FORMAT_R32G32B32_SFLOAT;
    case POSITION2:  return VK_FORMAT_R32G32_SFLOAT;
    case COLOR4:     return VK_FORMAT_R8G8B8A8_UNORM;
    case COLOR4F:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TEX_COORD2: return VK_FORMAT_R32G32_SFLOAT;
    case TEX_COORD4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case NORMAL:     return VK_FORMAT_R32G32B32_SFLOAT;
    case TANGENT:    return VK_FORMAT_R32G32B32A32_SFLOAT;
    case MODEL_ID:   return VK_FORMAT_R32_SFLOAT;
    case RADIUS:     return VK_FORMAT_R32_SFLOAT;
    case UVEC:       return VK_FORMAT_R32G32B32_SFLOAT;
    default:         return VK_FORMAT_UNDEFINED;
    }
}
```

### 5.4 Primitive Type Mapping

```cpp
VkPrimitiveTopology mapPrimitiveType(primitive_type type) {
    switch (type) {
    case PRIM_TYPE_POINTS:    return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case PRIM_TYPE_LINES:     return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case PRIM_TYPE_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case PRIM_TYPE_TRIS:      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case PRIM_TYPE_TRISTRIP:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case PRIM_TYPE_TRIFAN:    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:                  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
```

### 5.5 OpenGL Reference Files

| Component | File | Lines | Purpose |
|-----------|------|-------|---------|
| Device | `gropengl.cpp` | 1,610 | Init, buffers |
| Draw | `gropengldraw.cpp` | 1,203 | Rendering, FBOs |
| State | `gropenglstate.cpp` | 908 | State management |
| Shaders | `gropenglshader.cpp` | 1,152 | Compilation |
| Textures | `gropengltexture.cpp` | 1,983 | Texture cache |
| Deferred | `gropengldeferred.cpp` | ~900 | G-buffer |
| Post | `gropenglpostprocessing.cpp` | 1,215 | Effects |
| Transform | `gropengltnl.cpp` | 1,268 | Vertex handling |

---

## 6. Appendices

### 6.1 Open Questions Requiring Resolution

| ID | Question | Options | Recommendation |
|----|----------|---------|----------------|
| Q-001 | Shader compilation: build-time or runtime? | Build/Runtime | Build-time base, runtime variants |
| Q-002 | Pipeline stall handling? | Sync/Async/Precompile | Sync initially |
| Q-003 | Descriptor set strategy? | Per-draw/Per-frame/Bindless | Per-frame with reset |
| Q-010 | Visual parity approval? | Screenshot/Human/Both | Human with screenshot aid |
| Q-011 | Milestone approval authority? | Developer/Reviewer | At least one reviewer |
| Q-012 | Vulkan-specific bugs? | Fix/Track | Track in issue tracker |
| Q-013 | Beta testing strategy? | Opt-in/Separate | Opt-in `-vulkan` flag |

### 6.2 Documented Assumptions

| ID | Assumption | Impact if Wrong |
|----|------------|-----------------|
| A-001 | VMA handles memory adequately | Must implement custom allocator |
| A-002 | Vulkan 1.4 drivers available on target hardware | User falls back to OpenGL |
| A-003 | Shader preprocessor outputs GLSL 4.60 | Must modify shader system |
| A-004 | SDL2 Vulkan support sufficient | Must use platform-specific code |
| A-005 | Single graphics queue sufficient | Must implement multi-queue |
| A-006 | Dynamic rendering simplifies code | N/A - core in Vulkan 1.4 |
| A-007 | All Vulkan 1.4 hardware supports BC compression | True - mandatory in spec |
| A-008 | Geometry shaders available | True - mandatory in Vulkan 1.4 |

### 6.3 Risk Register

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Driver bugs | Medium | High | Test multiple vendors |
| Shader compilation slow | Medium | Medium | Implement caching |
| Memory issues | Low | High | Use VMA, validation |
| Performance regression | Medium | Medium | Profile early |
| Feature not supported | Medium | Low | Graceful degradation |

### 6.4 Testing Strategy

#### 6.4.1 Testing Infrastructure Requirements

**Pre-Implementation (Milestone 0):**

| Component | Description | Location |
|-----------|-------------|----------|
| VulkanTestFixture | GTest fixture with headless Vulkan context | `test/src/graphics/vulkan/VulkanTestFixture.h` |
| MockVulkanDevice | Mock device for unit tests without GPU | `test/src/graphics/vulkan/MockVulkanDevice.h` |
| ScreenshotCapture | Frame capture utility | `code/graphics/util/screenshot_capture.cpp` |
| ImageCompare | Perceptual diff tool (SSIM-based) | `test/src/graphics/ImageCompare.cpp` |
| Golden images | Reference screenshots from OpenGL | `test/test_data/golden_images/` |

**Build System Changes:**

```cmake
# test/src/graphics/vulkan/CMakeLists.txt
add_executable(vulkan_tests
    VulkanTestFixture.cpp
    test_format_mapping.cpp
    test_blend_modes.cpp
    test_pipeline_cache.cpp
    test_buffer_management.cpp
    test_texture_creation.cpp
    test_shader_loading.cpp
    test_descriptor_sets.cpp
    test_render_pass.cpp
    test_swapchain.cpp
)
target_link_libraries(vulkan_tests PRIVATE gtest gtest_main code)
target_compile_definitions(vulkan_tests PRIVATE FSO_BUILD_TESTS=1)
```

#### 6.4.2 Unit Tests

**Test File: `test/src/graphics/vulkan/test_format_mapping.cpp`**

Tests texture format conversion from FSO formats to Vulkan formats.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanFormatMapping.h"

class FormatMappingTest : public ::testing::Test {};

// FR-015: Support compressed textures
TEST_F(FormatMappingTest, DXT1_MapsToBC1) {
    VkFormat result = vulkan::mapTextureFormat(32, DDS_DXT1, true);
    EXPECT_EQ(VK_FORMAT_BC1_RGB_UNORM_BLOCK, result);
}

TEST_F(FormatMappingTest, DXT3_MapsToBC2) {
    VkFormat result = vulkan::mapTextureFormat(32, DDS_DXT3, true);
    EXPECT_EQ(VK_FORMAT_BC2_UNORM_BLOCK, result);
}

TEST_F(FormatMappingTest, DXT5_MapsToBC3) {
    VkFormat result = vulkan::mapTextureFormat(32, DDS_DXT5, true);
    EXPECT_EQ(VK_FORMAT_BC3_UNORM_BLOCK, result);
}

TEST_F(FormatMappingTest, BC7_MapsToBC7) {
    VkFormat result = vulkan::mapTextureFormat(32, DDS_BC7, true);
    EXPECT_EQ(VK_FORMAT_BC7_UNORM_BLOCK, result);
}

// 24-bit should always convert to 32-bit (poor hardware support)
TEST_F(FormatMappingTest, Uncompressed24Bit_ConvertsTo32Bit) {
    VkFormat result = vulkan::mapTextureFormat(24, 0, false);
    EXPECT_EQ(VK_FORMAT_R8G8B8A8_UNORM, result);
}

TEST_F(FormatMappingTest, Uncompressed32Bit_MapsToRGBA8) {
    VkFormat result = vulkan::mapTextureFormat(32, 0, false);
    EXPECT_EQ(VK_FORMAT_R8G8B8A8_UNORM, result);
}

// FR-018: Render targets need correct format
TEST_F(FormatMappingTest, RenderTarget_HDR_MapsToRGBA16F) {
    VkFormat result = vulkan::mapRenderTargetFormat(BMP_FLAG_RENDER_TARGET_HDR);
    EXPECT_EQ(VK_FORMAT_R16G16B16A16_SFLOAT, result);
}
```

**Test File: `test/src/graphics/vulkan/test_blend_modes.cpp`**

Tests blend mode mapping for all 6 FSO blend modes.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanBlendModes.h"

class BlendModeTest : public ::testing::Test {};

// FR-042: All 6 gr_alpha_blend modes work
TEST_F(BlendModeTest, ALPHA_BLEND_NONE) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(ALPHA_BLEND_NONE, src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ZERO, dst);
}

TEST_F(BlendModeTest, ALPHA_BLEND_ADDITIVE) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(ALPHA_BLEND_ADDITIVE, src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE, dst);
}

TEST_F(BlendModeTest, ALPHA_BLEND_ALPHA_ADDITIVE) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(ALPHA_BLEND_ALPHA_ADDITIVE, src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_SRC_ALPHA, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE, dst);
}

TEST_F(BlendModeTest, ALPHA_BLEND_ALPHA_BLEND_ALPHA) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(ALPHA_BLEND_ALPHA_BLEND_ALPHA, src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_SRC_ALPHA, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, dst);
}

TEST_F(BlendModeTest, ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR, src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_SRC_ALPHA, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, dst);
}

TEST_F(BlendModeTest, ALPHA_BLEND_PREMULTIPLIED) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(ALPHA_BLEND_PREMULTIPLIED, src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, dst);
}

// Edge case: Invalid blend mode
TEST_F(BlendModeTest, InvalidMode_DefaultsToNone) {
    VkBlendFactor src, dst;
    vulkan::mapBlendMode(static_cast<gr_alpha_blend>(999), src, dst);
    EXPECT_EQ(VK_BLEND_FACTOR_ONE, src);
    EXPECT_EQ(VK_BLEND_FACTOR_ZERO, dst);
}
```

**Test File: `test/src/graphics/vulkan/test_pipeline_cache.cpp`**

Tests pipeline cache key generation and lookup.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanPipelineCache.h"

class PipelineCacheTest : public ::testing::Test {
protected:
    vulkan::PipelineCache cache;
};

// Pipeline key hashing
TEST_F(PipelineCacheTest, IdenticalKeys_ProduceSameHash) {
    vulkan::PipelineKey key1{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};
    vulkan::PipelineKey key2{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};

    EXPECT_EQ(std::hash<vulkan::PipelineKey>{}(key1),
              std::hash<vulkan::PipelineKey>{}(key2));
}

TEST_F(PipelineCacheTest, DifferentShaderFlags_ProduceDifferentHash) {
    vulkan::PipelineKey key1{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};
    vulkan::PipelineKey key2{SDR_TYPE_MODEL, SDR_FLAG_DIFFUSE, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};

    EXPECT_NE(std::hash<vulkan::PipelineKey>{}(key1),
              std::hash<vulkan::PipelineKey>{}(key2));
}

TEST_F(PipelineCacheTest, DifferentBlendMode_ProduceDifferentHash) {
    vulkan::PipelineKey key1{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};
    vulkan::PipelineKey key2{SDR_TYPE_MODEL, 0, ALPHA_BLEND_ADDITIVE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};

    EXPECT_NE(std::hash<vulkan::PipelineKey>{}(key1),
              std::hash<vulkan::PipelineKey>{}(key2));
}

// Cache behavior
TEST_F(PipelineCacheTest, GetOrCreate_ReturnsNullForMissingPipeline) {
    vulkan::PipelineKey key{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};

    EXPECT_EQ(VK_NULL_HANDLE, cache.get(key));
}

TEST_F(PipelineCacheTest, Insert_ThenGet_ReturnsSamePipeline) {
    vulkan::PipelineKey key{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};
    VkPipeline mockPipeline = reinterpret_cast<VkPipeline>(0x12345678);

    cache.insert(key, mockPipeline);
    EXPECT_EQ(mockPipeline, cache.get(key));
}

TEST_F(PipelineCacheTest, Clear_RemovesAllEntries) {
    vulkan::PipelineKey key{SDR_TYPE_MODEL, 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};
    VkPipeline mockPipeline = reinterpret_cast<VkPipeline>(0x12345678);

    cache.insert(key, mockPipeline);
    cache.clear();
    EXPECT_EQ(VK_NULL_HANDLE, cache.get(key));
}

// All 24 shader types produce unique keys
TEST_F(PipelineCacheTest, AllShaderTypes_ProduceUniqueKeys) {
    std::unordered_set<size_t> hashes;
    for (int i = 0; i < SDR_TYPE_MAX; i++) {
        vulkan::PipelineKey key{static_cast<shader_type>(i), 0, ALPHA_BLEND_NONE, ZBUFFER_TYPE_FULL, 1, VK_NULL_HANDLE};
        hashes.insert(std::hash<vulkan::PipelineKey>{}(key));
    }
    EXPECT_EQ(SDR_TYPE_MAX, hashes.size());
}
```

**Test File: `test/src/graphics/vulkan/test_buffer_management.cpp`**

Tests buffer creation, updates, and deletion.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"

class BufferManagementTest : public vulkan::VulkanTestFixture {};

// FR-010: Create vertex/index buffers
TEST_F(BufferManagementTest, CreateBuffer_ReturnsValidHandle) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    EXPECT_GT(handle, 0);
    gr_delete_buffer(handle);
}

TEST_F(BufferManagementTest, CreateBuffer_DifferentHandlesForMultiple) {
    int handle1 = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    int handle2 = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    EXPECT_NE(handle1, handle2);
    gr_delete_buffer(handle1);
    gr_delete_buffer(handle2);
}

// FR-011: Update buffer data
TEST_F(BufferManagementTest, UpdateBufferData_AcceptsValidData) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    float vertices[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f};

    bool result = gr_update_buffer_data(handle, sizeof(vertices), vertices);
    EXPECT_TRUE(result);
    gr_delete_buffer(handle);
}

TEST_F(BufferManagementTest, UpdateBufferData_FailsOnInvalidHandle) {
    float vertices[] = {0.0f, 0.0f, 0.0f};
    bool result = gr_update_buffer_data(-1, sizeof(vertices), vertices);
    EXPECT_FALSE(result);
}

TEST_F(BufferManagementTest, UpdateBufferData_FailsOnNullData) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    bool result = gr_update_buffer_data(handle, 100, nullptr);
    EXPECT_FALSE(result);
    gr_delete_buffer(handle);
}

// FR-012: Map/unmap buffers
TEST_F(BufferManagementTest, MapBuffer_ReturnsValidPointer) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Dynamic);
    float vertices[] = {0.0f, 0.0f, 0.0f};
    gr_update_buffer_data(handle, sizeof(vertices), vertices);

    void* ptr = gr_map_buffer(handle);
    EXPECT_NE(nullptr, ptr);
    gr_flush_mapped_buffer(handle);
    gr_delete_buffer(handle);
}

TEST_F(BufferManagementTest, MapBuffer_FailsOnStaticBuffer) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    void* ptr = gr_map_buffer(handle);
    EXPECT_EQ(nullptr, ptr);
    gr_delete_buffer(handle);
}

// FR-013: Delete buffers
TEST_F(BufferManagementTest, DeleteBuffer_FreesResources) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    gr_delete_buffer(handle);

    // Attempting to update deleted buffer should fail
    float vertices[] = {0.0f, 0.0f, 0.0f};
    bool result = gr_update_buffer_data(handle, sizeof(vertices), vertices);
    EXPECT_FALSE(result);
}

TEST_F(BufferManagementTest, DeleteBuffer_HandlesDoubleDelete) {
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    gr_delete_buffer(handle);
    // Should not crash
    gr_delete_buffer(handle);
}

// Memory tracking (NFR-003)
TEST_F(BufferManagementTest, MemoryUsage_TracksAllocations) {
    size_t before = vulkan::getBufferMemoryUsage();
    int handle = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Static);
    float vertices[1000];
    gr_update_buffer_data(handle, sizeof(vertices), vertices);

    size_t after = vulkan::getBufferMemoryUsage();
    EXPECT_GT(after, before);

    gr_delete_buffer(handle);
    size_t final = vulkan::getBufferMemoryUsage();
    EXPECT_EQ(before, final);
}
```

**Test File: `test/src/graphics/vulkan/test_texture_creation.cpp`**

Tests texture creation, compression support, and render targets.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"

class TextureCreationTest : public vulkan::VulkanTestFixture {};

// FR-014: Create textures from bitmaps
TEST_F(TextureCreationTest, CreateTexture_2D_ReturnsValidHandle) {
    int handle = bm_create(8, 256, 256, 0);  // 8-bit, 256x256
    EXPECT_GE(handle, 0);
    bm_release(handle);
}

TEST_F(TextureCreationTest, CreateTexture_VariousSizes) {
    // Power of two
    int h1 = bm_create(8, 512, 512, 0);
    EXPECT_GE(h1, 0);

    // Non-power of two
    int h2 = bm_create(8, 300, 200, 0);
    EXPECT_GE(h2, 0);

    // 1x1 minimum
    int h3 = bm_create(8, 1, 1, 0);
    EXPECT_GE(h3, 0);

    bm_release(h1);
    bm_release(h2);
    bm_release(h3);
}

// FR-015: Support compressed textures
TEST_F(TextureCreationTest, CompressedTexture_DXT1_Loads) {
    int handle = bm_load("test_dxt1.dds");
    EXPECT_GE(handle, 0);

    int compression = bm_get_compression_type(handle);
    EXPECT_EQ(DDS_DXT1, compression);
    bm_release(handle);
}

TEST_F(TextureCreationTest, CompressedTexture_DXT5_Loads) {
    int handle = bm_load("test_dxt5.dds");
    EXPECT_GE(handle, 0);

    int compression = bm_get_compression_type(handle);
    EXPECT_EQ(DDS_DXT5, compression);
    bm_release(handle);
}

TEST_F(TextureCreationTest, CompressedTexture_BC7_LoadsIfSupported) {
    if (!gr_is_capable(CAPABILITY_BPTC)) {
        GTEST_SKIP() << "BC7 not supported on this hardware";
    }

    int handle = bm_load("test_bc7.dds");
    EXPECT_GE(handle, 0);
    bm_release(handle);
}

// FR-016: Support texture arrays
TEST_F(TextureCreationTest, TextureArray_CreatesCorrectly) {
    int handle = bm_create(8, 256, 256, BMP_FLAG_RENDER_TARGET_ARRAY);
    EXPECT_GE(handle, 0);
    bm_release(handle);
}

// FR-017: Support cubemaps
TEST_F(TextureCreationTest, Cubemap_LoadsAllFaces) {
    int handle = bm_load("test_cubemap.dds");
    EXPECT_GE(handle, 0);

    // Verify it's actually a cubemap
    int flags = bm_get_flags(handle);
    EXPECT_TRUE(flags & BMP_FLAG_CUBEMAP);
    bm_release(handle);
}

// FR-018: Create render targets
TEST_F(TextureCreationTest, RenderTarget_CreatesSuccessfully) {
    int handle = bm_make_render_target(512, 512, BMP_FLAG_RENDER_TARGET_STATIC);
    EXPECT_GE(handle, 0);
    bm_release(handle);
}

TEST_F(TextureCreationTest, RenderTarget_HDR_CreatesWithCorrectFormat) {
    int handle = bm_make_render_target(512, 512, BMP_FLAG_RENDER_TARGET_HDR);
    EXPECT_GE(handle, 0);

    // Verify HDR format (16-bit float per channel)
    VkFormat format = vulkan::getTextureFormat(handle);
    EXPECT_EQ(VK_FORMAT_R16G16B16A16_SFLOAT, format);
    bm_release(handle);
}

// FR-019: Render to texture
TEST_F(TextureCreationTest, RenderTarget_CanBeSetAsTarget) {
    int handle = bm_make_render_target(512, 512, BMP_FLAG_RENDER_TARGET_STATIC);

    bool result = gr_set_render_target(handle);
    EXPECT_TRUE(result);

    gr_set_render_target(-1);  // Reset to backbuffer
    bm_release(handle);
}
```

**Test File: `test/src/graphics/vulkan/test_shader_loading.cpp`**

Tests shader compilation and loading.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"

class ShaderLoadingTest : public vulkan::VulkanTestFixture {};

// FR-020: Load SPIR-V shaders
TEST_F(ShaderLoadingTest, LoadShader_ValidSPIRV_Succeeds) {
    int handle = gr_maybe_create_shader(SDR_TYPE_MODEL, 0);
    EXPECT_GE(handle, 0);
}

TEST_F(ShaderLoadingTest, LoadShader_InvalidPath_ReturnsError) {
    // Non-existent shader type should fail gracefully
    int handle = gr_maybe_create_shader(static_cast<shader_type>(9999), 0);
    EXPECT_LT(handle, 0);
}

// FR-021: Support shader variants (flag-based)
TEST_F(ShaderLoadingTest, ShaderVariant_DifferentFlags_CreatesDifferentShaders) {
    int base = gr_maybe_create_shader(SDR_TYPE_MODEL, 0);
    int withDiffuse = gr_maybe_create_shader(SDR_TYPE_MODEL, SDR_FLAG_DIFFUSE);
    int withNormal = gr_maybe_create_shader(SDR_TYPE_MODEL, SDR_FLAG_NORMAL);

    // All should be valid
    EXPECT_GE(base, 0);
    EXPECT_GE(withDiffuse, 0);
    EXPECT_GE(withNormal, 0);
}

TEST_F(ShaderLoadingTest, ShaderVariant_CombinedFlags_Works) {
    uint32_t flags = SDR_FLAG_DIFFUSE | SDR_FLAG_NORMAL | SDR_FLAG_SPEC;
    int handle = gr_maybe_create_shader(SDR_TYPE_MODEL, flags);
    EXPECT_GE(handle, 0);
}

// FR-022: Bind uniform buffers
TEST_F(ShaderLoadingTest, UniformBuffer_BindsSuccessfully) {
    int shader = gr_maybe_create_shader(SDR_TYPE_MODEL, 0);
    ASSERT_GE(shader, 0);

    matrix4 mvp;
    vm_matrix4_identity(&mvp);

    bool result = gr_bind_uniform_buffer(shader, "mvpMatrix", sizeof(mvp), &mvp);
    EXPECT_TRUE(result);
}

// FR-023: Support all 24 shader types
TEST_F(ShaderLoadingTest, AllShaderTypes_LoadSuccessfully) {
    std::vector<shader_type> requiredTypes = {
        SDR_TYPE_MODEL,
        SDR_TYPE_EFFECT_PARTICLE,
        SDR_TYPE_EFFECT_DISTORTION,
        SDR_TYPE_POST_PROCESS_MAIN,
        SDR_TYPE_POST_PROCESS_BLUR,
        SDR_TYPE_POST_PROCESS_BLOOM_COMP,
        SDR_TYPE_POST_PROCESS_BRIGHTPASS,
        SDR_TYPE_POST_PROCESS_FXAA,
        SDR_TYPE_POST_PROCESS_SMAA_EDGE,
        SDR_TYPE_POST_PROCESS_SMAA_BLEND,
        SDR_TYPE_POST_PROCESS_SMAA_NEIGHBOR,
        SDR_TYPE_DEFERRED_LIGHTING,
        SDR_TYPE_DEFERRED_CLEAR,
        SDR_TYPE_VIDEO_PROCESS,
        SDR_TYPE_PASSTHROUGH,
        // ... etc for all 24
    };

    for (auto type : requiredTypes) {
        int handle = gr_maybe_create_shader(type, 0);
        EXPECT_GE(handle, 0) << "Failed to load shader type: " << static_cast<int>(type);
    }
}
```

**Test File: `test/src/graphics/vulkan/test_swapchain.cpp`**

Tests swapchain creation and resize handling.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"

class SwapchainTest : public vulkan::VulkanTestFixture {};

// FR-002: Create and manage swapchain
TEST_F(SwapchainTest, Swapchain_CreatesSuccessfully) {
    EXPECT_TRUE(vulkan::isSwapchainValid());
}

TEST_F(SwapchainTest, Swapchain_HasCorrectImageCount) {
    uint32_t count = vulkan::getSwapchainImageCount();
    EXPECT_GE(count, 2u);  // At least double-buffered
    EXPECT_LE(count, 3u);  // At most triple-buffered
}

// FR-003: Handle window resize
TEST_F(SwapchainTest, Resize_RecreatessSwapchain) {
    VkExtent2D before = vulkan::getSwapchainExtent();

    // Simulate resize
    vulkan::onWindowResize(800, 600);

    VkExtent2D after = vulkan::getSwapchainExtent();
    EXPECT_EQ(800u, after.width);
    EXPECT_EQ(600u, after.height);
}

TEST_F(SwapchainTest, Resize_ToZero_DoesNotCrash) {
    // Minimize simulation
    vulkan::onWindowResize(0, 0);

    // Should not crash, swapchain invalid until restored
    EXPECT_FALSE(vulkan::isSwapchainValid());
}

TEST_F(SwapchainTest, Resize_FromZero_RestoresSwapchain) {
    vulkan::onWindowResize(0, 0);
    vulkan::onWindowResize(1024, 768);

    EXPECT_TRUE(vulkan::isSwapchainValid());
    VkExtent2D extent = vulkan::getSwapchainExtent();
    EXPECT_EQ(1024u, extent.width);
    EXPECT_EQ(768u, extent.height);
}

// FR-004: Handle minimize/restore
TEST_F(SwapchainTest, Minimize_SuspendsRendering) {
    vulkan::onWindowMinimize();
    EXPECT_TRUE(vulkan::isRenderingSuspended());
}

TEST_F(SwapchainTest, Restore_ResumesRendering) {
    vulkan::onWindowMinimize();
    vulkan::onWindowRestore();
    EXPECT_FALSE(vulkan::isRenderingSuspended());
}

// FR-005: Handle fullscreen toggle
TEST_F(SwapchainTest, FullscreenToggle_RecreatessSwapchain) {
    vulkan::setFullscreen(true);
    EXPECT_TRUE(vulkan::isSwapchainValid());

    vulkan::setFullscreen(false);
    EXPECT_TRUE(vulkan::isSwapchainValid());
}

// Error handling
TEST_F(SwapchainTest, OutOfDateError_TriggersRecreation) {
    // Force OUT_OF_DATE condition
    vulkan::simulateOutOfDate();

    // Next frame should handle it gracefully
    bool frameOk = vulkan::beginFrame();
    EXPECT_TRUE(frameOk);
    EXPECT_TRUE(vulkan::isSwapchainValid());
}
```

**Test File: `test/src/graphics/vulkan/test_render_state.cpp`**

Tests render state functions.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"

class RenderStateTest : public vulkan::VulkanTestFixture {};

// FR-040: Depth testing
TEST_F(RenderStateTest, DepthTest_AllModesWork) {
    EXPECT_NO_THROW(gr_zbuffer_set(ZBUFFER_TYPE_NONE));
    EXPECT_NO_THROW(gr_zbuffer_set(ZBUFFER_TYPE_READ));
    EXPECT_NO_THROW(gr_zbuffer_set(ZBUFFER_TYPE_WRITE));
    EXPECT_NO_THROW(gr_zbuffer_set(ZBUFFER_TYPE_FULL));
}

TEST_F(RenderStateTest, DepthTest_GetReturnsLastSet) {
    gr_zbuffer_set(ZBUFFER_TYPE_READ);
    EXPECT_EQ(ZBUFFER_TYPE_READ, gr_zbuffer_get());

    gr_zbuffer_set(ZBUFFER_TYPE_FULL);
    EXPECT_EQ(ZBUFFER_TYPE_FULL, gr_zbuffer_get());
}

// FR-041: Stencil operations
TEST_F(RenderStateTest, Stencil_SetAndClear) {
    EXPECT_NO_THROW(gr_stencil_set(GR_STENCIL_WRITE));
    EXPECT_NO_THROW(gr_stencil_clear());
}

// FR-043: Culling
TEST_F(RenderStateTest, Cull_AllModesWork) {
    EXPECT_NO_THROW(gr_set_cull(0));  // No culling
    EXPECT_NO_THROW(gr_set_cull(1));  // Cull back faces
    EXPECT_NO_THROW(gr_set_cull(-1)); // Cull front faces
}

// FR-044: Scissor/clip regions
TEST_F(RenderStateTest, Clip_SetAndReset) {
    gr_set_clip(100, 100, 200, 200, GR_RESIZE_NONE);
    // Verify clip is set (implementation dependent)

    gr_reset_clip();
    // Verify clip is reset to full viewport
}

// FR-045: Viewport
TEST_F(RenderStateTest, Viewport_SetsCorrectly) {
    gr_set_viewport(0, 0, 800, 600);
    // Verify viewport dimensions
}
```

#### 6.4.3 Integration Tests

**Test File: `test/src/graphics/vulkan/integration/test_render_frame.cpp`**

End-to-end frame rendering tests.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"
#include "graphics/ImageCompare.h"

class RenderFrameIntegrationTest : public vulkan::VulkanIntegrationTestFixture {
protected:
    void SetUp() override {
        vulkan::VulkanIntegrationTestFixture::SetUp();
        // Load golden images for comparison
        goldenImages.load("test/test_data/golden_images/");
    }

    test::GoldenImageSet goldenImages;
};

// FR-006, FR-007: Clear and flip
TEST_F(RenderFrameIntegrationTest, ClearScreen_ProducesCorrectOutput) {
    gr_set_clear_color(255, 0, 0);  // Red
    gr_clear();
    gr_flip();

    auto screenshot = captureFrame();
    EXPECT_TRUE(isUniformColor(screenshot, Color(255, 0, 0)));
}

// Basic geometry rendering
TEST_F(RenderFrameIntegrationTest, RenderTriangle_MatchesGolden) {
    renderTestTriangle();
    gr_flip();

    auto screenshot = captureFrame();
    auto golden = goldenImages.get("triangle");

    double ssim = ImageCompare::ssim(screenshot, golden);
    EXPECT_GT(ssim, 0.99) << "Triangle rendering does not match golden image";
}

TEST_F(RenderFrameIntegrationTest, RenderTexturedQuad_MatchesGolden) {
    renderTestTexturedQuad();
    gr_flip();

    auto screenshot = captureFrame();
    auto golden = goldenImages.get("textured_quad");

    double ssim = ImageCompare::ssim(screenshot, golden);
    EXPECT_GT(ssim, 0.98) << "Textured quad does not match golden image";
}

// Model rendering
TEST_F(RenderFrameIntegrationTest, RenderModel_GTFUlysses_MatchesGolden) {
    loadModel("gtf_ulysses.pof");
    renderModel();
    gr_flip();

    auto screenshot = captureFrame();
    auto golden = goldenImages.get("gtf_ulysses");

    double ssim = ImageCompare::ssim(screenshot, golden);
    EXPECT_GT(ssim, 0.95) << "Model rendering does not match golden image";
}

// Blend modes visual test
TEST_F(RenderFrameIntegrationTest, AllBlendModes_RenderCorrectly) {
    const std::vector<std::pair<gr_alpha_blend, std::string>> modes = {
        {ALPHA_BLEND_NONE, "blend_none"},
        {ALPHA_BLEND_ADDITIVE, "blend_additive"},
        {ALPHA_BLEND_ALPHA_BLEND_ALPHA, "blend_alpha"},
        {ALPHA_BLEND_PREMULTIPLIED, "blend_premult"},
    };

    for (const auto& [mode, name] : modes) {
        renderBlendTest(mode);
        gr_flip();

        auto screenshot = captureFrame();
        auto golden = goldenImages.get(name);

        double ssim = ImageCompare::ssim(screenshot, golden);
        EXPECT_GT(ssim, 0.97) << "Blend mode " << name << " does not match";
    }
}
```

**Test File: `test/src/graphics/vulkan/integration/test_validation_clean.cpp`**

Ensures no Vulkan validation errors during standard operations.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"

class ValidationCleanTest : public vulkan::VulkanIntegrationTestFixture {
protected:
    void SetUp() override {
        vulkan::VulkanIntegrationTestFixture::SetUp();
        vulkan::resetValidationErrorCount();
    }

    void TearDown() override {
        EXPECT_EQ(0, vulkan::getValidationErrorCount())
            << "Validation errors occurred during test";
        vulkan::VulkanIntegrationTestFixture::TearDown();
    }
};

// NFR-010: Validation clean after 1000 frames
TEST_F(ValidationCleanTest, Render1000Frames_NoValidationErrors) {
    for (int i = 0; i < 1000; i++) {
        gr_clear();
        renderTestScene();
        gr_flip();
    }

    EXPECT_EQ(0, vulkan::getValidationErrorCount());
}

TEST_F(ValidationCleanTest, ResizeRepeatedly_NoValidationErrors) {
    for (int i = 0; i < 50; i++) {
        int width = 640 + (i * 10);
        int height = 480 + (i * 8);
        vulkan::onWindowResize(width, height);

        gr_clear();
        gr_flip();
    }

    EXPECT_EQ(0, vulkan::getValidationErrorCount());
}

TEST_F(ValidationCleanTest, RapidResourceCreationDeletion_NoValidationErrors) {
    for (int i = 0; i < 100; i++) {
        int buf = gr_create_buffer(BufferType::Vertex, BufferUsageHint::Dynamic);
        int tex = bm_make_render_target(256, 256, 0);

        gr_delete_buffer(buf);
        bm_release(tex);
    }

    EXPECT_EQ(0, vulkan::getValidationErrorCount());
}
```

**Test File: `test/src/graphics/vulkan/integration/test_mission_playthrough.cpp`**

High-level mission playthrough tests.

```cpp
#include <gtest/gtest.h>
#include "graphics/vulkan/VulkanTestFixture.h"
#include "mission/missionload.h"

class MissionPlaythroughTest : public vulkan::VulkanIntegrationTestFixture {
protected:
    void SetUp() override {
        vulkan::VulkanIntegrationTestFixture::SetUp();
        game_init();
    }

    void TearDown() override {
        game_shutdown();
        vulkan::VulkanIntegrationTestFixture::TearDown();
    }
};

// Training mission playthrough (Milestone 5 acceptance)
TEST_F(MissionPlaythroughTest, TrainingMission1_LoadsAndRenders) {
    bool loaded = mission_load("training-1.fs2");
    ASSERT_TRUE(loaded);

    // Simulate 60 seconds of gameplay
    for (int frame = 0; frame < 3600; frame++) {
        game_process_frame();
        gr_flip();
    }

    EXPECT_EQ(0, vulkan::getValidationErrorCount());
}

// NFR-011: Crash free in standard gameplay
TEST_F(MissionPlaythroughTest, StandardMission_NocrashesOrErrors) {
    bool loaded = mission_load("sm1-01.fs2");
    ASSERT_TRUE(loaded);

    // Simulate 5 minutes of gameplay (18000 frames at 60fps)
    for (int frame = 0; frame < 18000; frame++) {
        ASSERT_NO_THROW(game_process_frame());
        ASSERT_NO_THROW(gr_flip());
    }
}
```

#### 6.4.4 Visual Regression Testing Infrastructure

**Screenshot Capture Tool:**

```cpp
// code/graphics/util/screenshot_capture.h
#pragma once
#include <string>
#include <vector>

namespace graphics {

struct ScreenshotOptions {
    bool includeUI = true;
    bool includeHUD = true;
    int width = 0;   // 0 = current resolution
    int height = 0;
};

class ScreenshotCapture {
public:
    // Capture current frame to memory
    static std::vector<uint8_t> captureFrame(const ScreenshotOptions& opts = {});

    // Save frame to file
    static bool saveToFile(const std::string& path, const ScreenshotOptions& opts = {});

    // Capture N frames and save sequentially
    static void captureSequence(const std::string& prefix, int count);
};

} // namespace graphics
```

**Image Comparison Tool:**

```cpp
// test/src/graphics/ImageCompare.h
#pragma once
#include <vector>
#include <cstdint>

namespace test {

struct ImageDiff {
    double ssim;              // Structural similarity (0-1, 1=identical)
    double mse;               // Mean squared error
    int differentPixels;      // Count of pixels differing by >threshold
    std::vector<uint8_t> diffImage;  // Visual diff output
};

class ImageCompare {
public:
    // Structural Similarity Index (perceptual comparison)
    static double ssim(const std::vector<uint8_t>& img1,
                       const std::vector<uint8_t>& img2,
                       int width, int height);

    // Full comparison with diff image generation
    static ImageDiff compare(const std::vector<uint8_t>& actual,
                             const std::vector<uint8_t>& expected,
                             int width, int height,
                             int threshold = 5);

    // Load golden image from file
    static std::vector<uint8_t> loadGolden(const std::string& name);

    // Save diff image for debugging
    static void saveDiff(const std::string& path, const ImageDiff& diff,
                         int width, int height);
};

} // namespace test
```

**Golden Image Generation Script:**

```bash
#!/bin/bash
# scripts/generate_golden_images.sh
# Run with OpenGL to generate golden reference images

FSO_EXE=${1:-./fs2_open}
OUTPUT_DIR=${2:-test/test_data/golden_images}

mkdir -p "$OUTPUT_DIR"

# Generate test scene images
$FSO_EXE -nosound -window -res 1024x768 \
    -capture_golden "$OUTPUT_DIR/triangle.png" -run_test triangle
$FSO_EXE -nosound -window -res 1024x768 \
    -capture_golden "$OUTPUT_DIR/textured_quad.png" -run_test textured_quad
$FSO_EXE -nosound -window -res 1024x768 \
    -capture_golden "$OUTPUT_DIR/gtf_ulysses.png" -run_test model_render

# Generate blend mode images
for mode in none additive alpha premult; do
    $FSO_EXE -nosound -window -res 1024x768 \
        -capture_golden "$OUTPUT_DIR/blend_$mode.png" -run_test blend_$mode
done

echo "Golden images generated in $OUTPUT_DIR"
```

#### 6.4.5 Continuous Integration

**GitHub Actions Workflow:**

```yaml
# .github/workflows/vulkan-tests.yml
name: Vulkan Tests

on:
  push:
    paths:
      - 'code/graphics/vulkan/**'
      - 'test/src/graphics/vulkan/**'
  pull_request:
    paths:
      - 'code/graphics/vulkan/**'
      - 'test/src/graphics/vulkan/**'

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - name: Install Vulkan SDK
        run: |
          wget -qO- https://packages.lunarg.com/lunarg-signing-key-pub.asc | sudo tee /etc/apt/trusted.gpg.d/lunarg.asc
          sudo wget -qO /etc/apt/sources.list.d/lunarg-vulkan-jammy.list https://packages.lunarg.com/vulkan/lunarg-vulkan-jammy.list
          sudo apt update
          sudo apt install vulkan-sdk

      - name: Configure
        run: cmake -B build -DFSO_BUILD_TESTS=ON -DWITH_VULKAN=ON

      - name: Build
        run: cmake --build build --target vulkan_tests

      - name: Run Unit Tests
        run: ./build/test/vulkan_tests --gtest_output=xml:test-results.xml

      - name: Upload Results
        uses: actions/upload-artifact@v4
        with:
          name: test-results
          path: test-results.xml

  integration-tests:
    runs-on: ubuntu-latest
    needs: unit-tests
    steps:
      - uses: actions/checkout@v4

      - name: Install Dependencies
        run: |
          sudo apt update
          sudo apt install vulkan-sdk mesa-vulkan-drivers

      - name: Configure
        run: cmake -B build -DFSO_BUILD_TESTS=ON -DWITH_VULKAN=ON

      - name: Build
        run: cmake --build build

      - name: Run Integration Tests (Software Rendering)
        env:
          VK_ICD_FILENAMES: /usr/share/vulkan/icd.d/lvp_icd.x86_64.json
        run: ./build/test/vulkan_integration_tests

      - name: Upload Screenshots
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: failed-screenshots
          path: test-output/*.png

  visual-regression:
    runs-on: ubuntu-latest
    needs: integration-tests
    steps:
      - uses: actions/checkout@v4
        with:
          lfs: true  # Golden images stored in LFS

      - name: Run Visual Comparison
        run: ./scripts/run_visual_tests.sh

      - name: Upload Diff Images
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: visual-diffs
          path: test-output/diffs/
```

#### 6.4.6 Test Coverage Requirements

| Component | Unit Test Coverage | Integration Test |
|-----------|-------------------|------------------|
| Format mapping | 100% of formats | N/A |
| Blend modes | 100% (6 modes) | Visual comparison |
| Pipeline cache | Key hashing, CRUD | 1000-frame validation |
| Buffer management | Create/Update/Map/Delete | Memory leak check |
| Texture creation | All types, compression | Visual comparison |
| Shader loading | All 24 types | Render output |
| Swapchain | Resize, minimize, fullscreen | Stress test |
| Render state | All state functions | Combined render test |

#### 6.4.7 Test Data Requirements

**Required Test Assets (in `test/test_data/`):**

| File | Purpose |
|------|---------|
| `test_dxt1.dds` | DXT1 compression test |
| `test_dxt5.dds` | DXT5 compression test |
| `test_bc7.dds` | BC7 compression test |
| `test_cubemap.dds` | Cubemap loading test |
| `test_texture_256.png` | Basic texture test |
| `gtf_ulysses.pof` | Model rendering test |
| `training-1.fs2` | Mission playthrough test |

**Golden Images (in `test/test_data/golden_images/`):**

| File | Resolution | Purpose |
|------|------------|---------|
| `triangle.png` | 1024x768 | Basic geometry |
| `textured_quad.png` | 1024x768 | Texture sampling |
| `gtf_ulysses.png` | 1024x768 | Model rendering |
| `blend_*.png` | 1024x768 | Blend mode validation |
| `main_menu.png` | 1024x768 | UI rendering |
| `deferred_lighting.png` | 1024x768 | Advanced rendering |

#### 6.4.8 Manual Testing Checklist

For scenarios that cannot be automated:

**Pre-Release Checklist:**
- [ ] Main menu renders correctly
- [ ] Tech room model viewer works (rotate, zoom)
- [ ] Campaign briefing plays video
- [ ] Training mission 1 completable start-to-finish
- [ ] Window resize doesn't crash (test 5 resizes)
- [ ] Alt-tab doesn't corrupt rendering
- [ ] Resolution change works (test 3 resolutions)
- [ ] Fullscreen toggle works both directions
- [ ] Multi-monitor setup doesn't crash
- [ ] AMD hardware: 1-hour gameplay session
- [ ] NVIDIA hardware: 1-hour gameplay session
- [ ] Intel hardware: 1-hour gameplay session

### 6.5 Acceptance Criteria Summary

| Milestone | Criteria |
|-----------|----------|
| M0 | Launches with `-vulkan`, handles resize, no validation errors |
| M1 | Triangle renders |
| M2 | Textured rotating quad |
| M3 | Ship model with textures/lighting |
| M4 | Main menu fully functional |
| M5 | Training mission 1 completable |
| M6 | Reviewer approves visual parity |
| M7 | 1-hour gameplay, zero validation errors |

---

## Document History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | - | Initial comprehensive plan |
| 2.0 | - | Consolidated from multiple documents; added requirements, design decisions, critical review feedback |
| 2.1 | - | Added comprehensive testing strategy: unit tests, integration tests, visual regression infrastructure, CI workflow |
| 2.2 | - | Updated to Vulkan 1.4 (from 1.1); older hardware uses OpenGL fallback; removed geometry shader concern |

---

**Document Status:** Ready for implementation pending resolution of open questions in Section 6.1.
