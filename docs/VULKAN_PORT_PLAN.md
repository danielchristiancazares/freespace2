# FreeSpace 2 Open - Vulkan Rendering Pipeline Port

**Version:** 2.0
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
- Vulkan 1.1 rendering backend as alternative to OpenGL
- Feature parity with current OpenGL implementation
- Perceptually identical visual output
- Windows and Linux platform support

**Out of Scope:**
- macOS support (MoltenVK - future work)
- OpenXR/VR support (separate initiative)
- Ray tracing extensions
- Mobile platforms
- Performance optimization beyond parity

### 1.3 Constraints

| Constraint | Value | Source |
|------------|-------|--------|
| Minimum Vulkan Version | 1.1 | `VulkanRenderer.cpp:26` |
| API Version | VK_API_VERSION_1_1 | `VulkanRenderer.cpp:410` |
| Minimum SDL Version | 2.0.6 | `VulkanRenderer.h:9` |
| Memory Allocator | VMA 3.0+ | Design decision |
| Shader Format | SPIR-V | Vulkan requirement |

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
| NFR-020 | Compatibility | NVIDIA support | GTX 600 series+ |
| NFR-021 | Compatibility | AMD support | GCN 1.0+ |
| NFR-022 | Compatibility | Intel support | HD 4000+ |

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
| Q-005 | Geometry shader fallback? | Fail/Fallback/Disable | Disable thick outlines |
| Q-010 | Visual parity approval? | Screenshot/Human/Both | Human with screenshot aid |
| Q-011 | Milestone approval authority? | Developer/Reviewer | At least one reviewer |
| Q-012 | Vulkan-specific bugs? | Fix/Track | Track in issue tracker |
| Q-013 | Beta testing strategy? | Opt-in/Separate | Opt-in `-vulkan` flag |

### 6.2 Documented Assumptions

| ID | Assumption | Impact if Wrong |
|----|------------|-----------------|
| A-001 | VMA handles memory adequately | Must implement custom allocator |
| A-002 | Vulkan 1.1 has all needed features | Must require 1.2 or extensions |
| A-003 | Shader preprocessor outputs GLSL 4.50 | Must modify shader system |
| A-004 | SDL2 Vulkan support sufficient | Must use platform-specific code |
| A-005 | Single graphics queue sufficient | Must implement multi-queue |
| A-006 | Pipeline stalls acceptable | Must implement async compilation |
| A-007 | All hardware supports BC compression | Must implement fallback |
| A-008 | Geometry shaders available | Must disable thick outlines |

### 6.3 Risk Register

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Driver bugs | Medium | High | Test multiple vendors |
| Shader compilation slow | Medium | Medium | Implement caching |
| Memory issues | Low | High | Use VMA, validation |
| Performance regression | Medium | Medium | Profile early |
| Feature not supported | Medium | Low | Graceful degradation |

### 6.4 Testing Strategy

**Automated:**
1. Validation layers in all debug builds
2. `--capture-frames=N` dumps N frames as PNG
3. Golden image comparison against OpenGL
4. Smoke test each major screen

**Manual Checklist:**
- [ ] Main menu renders correctly
- [ ] Tech room model viewer works
- [ ] Training mission 1 completable
- [ ] Window resize doesn't crash
- [ ] Alt-tab doesn't corrupt rendering
- [ ] Resolution change works

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

---

**Document Status:** Ready for implementation pending resolution of open questions in Section 6.1.
