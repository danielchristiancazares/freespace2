# FreeSpace 2 Vulkan Port - Implementation Plan (Final)

**Requirements Document:** [VULKAN_PORT_REQUIREMENTS.md](VULKAN_PORT_REQUIREMENTS.md)
**Status:** Ready for Implementation
**Prerequisites:** Resolve open questions Q-001 through Q-013 in requirements document

---

## Executive Summary

This plan implements the Vulkan rendering backend for FreeSpace 2 Open, providing feature parity with the existing OpenGL implementation. The work is organized into 8 milestones, each producing a testable deliverable.

**Constraints (from Requirements):**
- Vulkan 1.1 minimum (VK_API_VERSION_1_1)
- SDL 2.0.6+ for Vulkan surface support
- VMA 3.0+ for memory management
- Must coexist with OpenGL backend

---

## Pre-Implementation Checklist

Before starting Milestone 0, resolve these items:

- [ ] **Q-001 Resolved:** Shader compilation strategy decided
- [ ] **Q-002 Resolved:** Pipeline stall handling approach chosen
- [ ] **Q-003 Resolved:** Descriptor set strategy chosen
- [ ] **Q-005 Resolved:** Geometry shader fallback behavior defined
- [ ] **Q-010 Resolved:** Visual parity approval process defined
- [ ] **Q-011 Resolved:** Milestone approval authority identified
- [ ] VMA library added to repository/build system
- [ ] glslangValidator available for shader compilation
- [ ] Test hardware identified (NVIDIA, AMD, Intel)

---

## Milestone 0: Build Foundation

**Requirements Addressed:** FR-001, FR-002, FR-003, FR-004, FR-005
**Acceptance:** Game starts with `-vulkan`, handles resize, no validation errors

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 0.1 | Add VMA to build system | `CMakeLists.txt` | Compiles without error |
| 0.2 | Add `WITH_VULKAN` compile flag | `CMakeLists.txt`, headers | Both ON/OFF compile |
| 0.3 | Implement swapchain recreation | `VulkanRenderer.cpp` | Resize window 5 times |
| 0.4 | Add VK_ERROR_OUT_OF_DATE handling | `VulkanRenderer.cpp` | Resize during render |
| 0.5 | Add VK_ERROR_DEVICE_LOST handling | `VulkanRenderer.cpp` | Document recovery path |
| 0.6 | Set up validation layer callback | `VulkanRenderer.cpp` | Errors logged to console |
| 0.7 | Integrate VMA for allocations | `VulkanRenderer.cpp` | No direct vkAllocateMemory |

### Swapchain Recreation (Critical)

```cpp
void VulkanRenderer::recreateSwapchain() {
    // Handle minimized window (0x0 size)
    int width = 0, height = 0;
    SDL_Vulkan_GetDrawableSize(window, &width, &height);
    while (width == 0 || height == 0) {
        SDL_Vulkan_GetDrawableSize(window, &width, &height);
        SDL_WaitEvent(nullptr);
    }

    m_device->waitIdle();

    // Cleanup old resources
    m_swapChainFramebuffers.clear();
    m_swapChainImageViews.clear();
    m_swapChain.reset();

    // Recreate
    PhysicalDeviceValues deviceValues;
    // ... repopulate from physical device
    createSwapChain(deviceValues);
    createFrameBuffers();
}
```

### Validation Callback Setup

```cpp
void setupDebugMessenger() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
}
```

### Deliverable
- Game launches with `-vulkan` flag
- Window resize works without crash
- Console shows "Vulkan initialized" message
- No validation errors in debug build

---

## Milestone 1: First Triangle

**Requirements Addressed:** FR-006, FR-007 (partial)
**Acceptance:** Colored triangle renders, resize doesn't crash

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 1.1 | Create test SPIR-V shaders | `shaders/test.vert`, `test.frag` | Compiles with glslangValidator |
| 1.2 | Create hardcoded vertex buffer | `VulkanRenderer.cpp` | No validation errors |
| 1.3 | Create graphics pipeline | `VulkanRenderer.cpp` | Pipeline created successfully |
| 1.4 | Implement `gf_clear()` | `vulkan_stubs.cpp` → real impl | Screen clears to color |
| 1.5 | Implement basic `gf_flip()` | `vulkan_stubs.cpp` → real impl | Triangle visible |

### Test Shaders

**test.vert:**
```glsl
#version 450
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragColor = inColor;
}
```

**test.frag:**
```glsl
#version 450
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
```

### Deliverable
- Colored triangle centered on screen
- `gf_clear()` fills with configured color
- `gf_flip()` presents frame
- Screenshot captured for reference

---

## Milestone 2: Textured Quad

**Requirements Addressed:** FR-010, FR-011, FR-014, FR-022
**Acceptance:** Textured rotating quad renders

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 2.1 | Implement `gf_create_buffer()` | New: `VulkanBuffer.cpp` | Returns valid handle |
| 2.2 | Implement `gf_update_buffer_data()` | `VulkanBuffer.cpp` | Data uploaded |
| 2.3 | Create texture from test image | `VulkanTexture.cpp` | VkImage created |
| 2.4 | Create sampler | `VulkanTexture.cpp` | Filtering works |
| 2.5 | Create descriptor set layout | `VulkanRenderer.cpp` | Texture binds |
| 2.6 | Implement MVP uniform | `VulkanRenderer.cpp` | Quad rotates |

### Resource Maps (Minimal Abstraction)

```cpp
// Global resource tracking - simple maps, not manager classes
namespace vulkan {
    struct BufferResource {
        VkBuffer buffer;
        VmaAllocation allocation;
        size_t size;
        void* mappedPtr;  // null if not mapped
    };

    SCP_unordered_map<int, BufferResource> g_buffers;
    int g_nextBufferHandle = 1;

    struct TextureResource {
        VkImage image;
        VkImageView view;
        VmaAllocation allocation;
        VkFormat format;
        uint32_t width, height, mipLevels;
    };

    SCP_unordered_map<int, TextureResource> g_textures;
}
```

### Deliverable
- Textured quad with test image
- Quad rotates via uniform update
- Buffer creation/update verified
- Screenshot captured

---

## Milestone 3: Model Rendering

**Requirements Addressed:** FR-014-017, FR-020-023, FR-030
**Acceptance:** GTF Ulysses model renders with textures and lighting

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 3.1 | Integrate with bmpman | `gropenvulkantexture.cpp` | `gf_bm_create/data` work |
| 3.2 | Support DXT compression | `VulkanTexture.cpp` | DXT textures load |
| 3.3 | Compile main-v/f.sdr to SPIR-V | Build script | Shaders compile |
| 3.4 | Implement `gf_maybe_create_shader()` | `VulkanShader.cpp` | Shader loads |
| 3.5 | Implement model_uniform_data | `VulkanRenderer.cpp` | Uniforms bind |
| 3.6 | Implement `gf_render_model()` | `VulkanRenderer.cpp` | Model visible |
| 3.7 | Handle vertex layout conversion | `VulkanRenderer.cpp` | Vertices correct |

### Shader Preprocessing

```bash
# Two-stage compilation for .sdr files
# Stage 1: FSO preprocessor expands #predefine/#prereplace
./shader_preprocess main-v.sdr --flags=0 > main-v.glsl

# Stage 2: Compile to SPIR-V
glslangValidator -V main-v.glsl -o main-v.spv --target-env vulkan1.1
```

### bmpman Integration

```cpp
void vk_bm_create(bitmap_slot* slot) {
    auto* info = new VulkanTextureInfo();
    // Create VkImage based on slot dimensions
    // Store in slot->gr_info
    slot->gr_info = info;
}

bool vk_bm_data(int handle, bitmap* bm) {
    auto* info = static_cast<VulkanTextureInfo*>(bm_get_gr_info(handle));
    // Upload pixel data to info->image via staging buffer
    return true;
}
```

### Deliverable
- GTF Ulysses model renders correctly
- Textures applied (diffuse at minimum)
- Basic lighting visible
- Screenshot compared to OpenGL reference

---

## Milestone 4: Basic Scene

**Requirements Addressed:** FR-012, FR-013, FR-031, FR-033-035, FR-040-045
**Acceptance:** Main menu fully navigable, tech room functional

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 4.1 | Implement `gf_delete_buffer()` | `VulkanBuffer.cpp` | No leaks |
| 4.2 | Implement `gf_map_buffer()` | `VulkanBuffer.cpp` | Returns valid ptr |
| 4.3 | Implement `gf_render_primitives()` | `VulkanRenderer.cpp` | Basic geometry |
| 4.4 | Implement `gf_render_primitives_batched()` | `VulkanRenderer.cpp` | HUD elements |
| 4.5 | Implement `gf_render_nanovg()` | `VulkanRenderer.cpp` | Vector UI |
| 4.6 | Implement `gf_render_rocket_primitives()` | `VulkanRenderer.cpp` | HTML UI |
| 4.7 | Implement `gf_zbuffer_set()` | `VulkanRenderer.cpp` | Depth test modes |
| 4.8 | Implement `gf_set_clip()` / `gf_reset_clip()` | `VulkanRenderer.cpp` | Scissor rect |
| 4.9 | Implement `gf_set_cull()` | `VulkanRenderer.cpp` | Back-face culling |
| 4.10 | Create pipeline variants for blend modes | `VulkanRenderer.cpp` | All 6 modes work |

### Pipeline Cache

```cpp
struct PipelineKey {
    shader_type shader;
    uint32_t shaderFlags;
    gr_alpha_blend blendMode;
    gr_zbuffer_type depthMode;
    int cullMode;
    VkRenderPass renderPass;

    bool operator==(const PipelineKey& o) const {
        return shader == o.shader && shaderFlags == o.shaderFlags &&
               blendMode == o.blendMode && depthMode == o.depthMode &&
               cullMode == o.cullMode && renderPass == o.renderPass;
    }
};

// Hash function for PipelineKey
namespace std {
    template<> struct hash<PipelineKey> {
        size_t operator()(const PipelineKey& k) const {
            return hash_combine(k.shader, k.shaderFlags, k.blendMode,
                               k.depthMode, k.cullMode);
        }
    };
}

SCP_unordered_map<PipelineKey, VkPipeline> g_pipelineCache;
```

### Deliverable
- Main menu renders completely
- All buttons clickable
- Tech room model viewer works
- Options menu functional
- Screenshot matches OpenGL

---

## Milestone 5: Playable Mission

**Requirements Addressed:** FR-018, FR-019, FR-032, FR-036, FR-053, FR-054
**Acceptance:** Training mission 1 completable start-to-finish

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 5.1 | Implement `gf_render_primitives_particle()` | `VulkanRenderer.cpp` | Particles visible |
| 5.2 | Implement `gf_render_primitives_distortion()` | `VulkanRenderer.cpp` | Distortion effects |
| 5.3 | Implement `gf_render_shield_impact()` | `VulkanRenderer.cpp` | Shield hits |
| 5.4 | Implement `gf_render_movie()` | `VulkanRenderer.cpp` | Cutscenes play |
| 5.5 | Implement `gf_scene_texture_begin/end()` | `VulkanRenderer.cpp` | Scene capture |
| 5.6 | Implement `gf_bm_make_render_target()` | `VulkanTexture.cpp` | RTT works |
| 5.7 | Implement `gf_bm_set_render_target()` | `VulkanTexture.cpp` | Target switch |
| 5.8 | Implement `gf_copy_effect_texture()` | `VulkanRenderer.cpp` | Effect copy |

### Render Target Creation

```cpp
int vk_bm_make_render_target(int handle, int* width, int* height,
                              int* bpp, int* mm_lvl, int flags) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = *width;
    imageInfo.extent.height = *height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = *mm_lvl > 0 ? *mm_lvl : 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // or from bpp
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;

    // Create image via VMA
    // Create framebuffer for this render target
    // Store in render target map

    return 1; // success
}
```

### Deliverable
- Training mission 1 completable
- Weapons fire visible
- Explosions/particles work
- Shield effects visible
- Briefing/debrief screens work

---

## Milestone 6: Visual Parity

**Requirements Addressed:** FR-050-055, FR-060-082
**Acceptance:** Side-by-side comparison approved by reviewer

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 6.1 | Implement G-buffer render pass | `VulkanDeferred.cpp` | G-buffer fills |
| 6.2 | Implement deferred lighting | `VulkanDeferred.cpp` | Lights accumulate |
| 6.3 | Implement `gf_deferred_lighting_*()` (4) | `VulkanDeferred.cpp` | API works |
| 6.4 | Implement shadow map pass | `VulkanShadows.cpp` | Shadows render |
| 6.5 | Implement `gf_shadow_map_start/end()` | `VulkanShadows.cpp` | API works |
| 6.6 | Implement post-processing | `VulkanPostProcess.cpp` | Effects apply |
| 6.7 | Implement bloom | `VulkanPostProcess.cpp` | Glow visible |
| 6.8 | Implement FXAA | `VulkanPostProcess.cpp` | Edges smooth |
| 6.9 | Implement tonemapping | `VulkanPostProcess.cpp` | HDR→LDR |
| 6.10 | Implement `gf_post_process_*()` (6) | `VulkanPostProcess.cpp` | API works |
| 6.11 | Implement decal rendering | `VulkanRenderer.cpp` | Decals visible |
| 6.12 | Implement all capability queries | `VulkanRenderer.cpp` | FR-060-075 pass |
| 6.13 | Implement all property queries | `VulkanRenderer.cpp` | FR-080-082 pass |

### Visual Comparison Process

1. Capture reference screenshots from OpenGL build
2. Capture equivalent screenshots from Vulkan build
3. Compute difference image
4. Human reviewer evaluates:
   - No missing geometry
   - Colors within tolerance
   - Effects present and correct
   - No rendering artifacts

### Deliverable
- All screenshots match OpenGL reference
- Reviewer sign-off on visual parity
- All capabilities report correctly
- Full campaign mission playable

---

## Milestone 7: Production Ready

**Requirements Addressed:** NFR-001 through NFR-032
**Acceptance:** 1-hour gameplay without validation errors or crashes

### Tasks

| # | Task | Files | Acceptance Test |
|---|------|-------|-----------------|
| 7.1 | Profile and optimize hot paths | Various | NFR-001 met |
| 7.2 | Fix all validation warnings | Various | Zero warnings |
| 7.3 | Test on NVIDIA hardware | - | NFR-020 met |
| 7.4 | Test on AMD hardware | - | NFR-021 met |
| 7.5 | Test on Intel hardware | - | NFR-022 met |
| 7.6 | Document known limitations | `VULKAN_NOTES.md` | NFR-031 met |
| 7.7 | Verify memory cleanup | - | NFR-013 met |
| 7.8 | Implement remaining deferred functions | Various | All functions |

### Deferred Functions to Implement

```cpp
// Screen capture (low priority)
gf_save_screen, gf_restore_screen, gf_free_screen
gf_print_screen, gf_blob_screen, gf_get_region

// Query objects
gf_create_query_object, gf_query_value, gf_query_value_available
gf_get_query_value, gf_delete_query_object

// Sync
gf_sync_fence, gf_sync_wait, gf_sync_delete

// Environment
gf_dump_envmap, gf_calculate_irrmap
```

### Deliverable
- 1-hour continuous gameplay test passed
- Zero validation errors
- Performance within 10% of OpenGL
- Documentation complete
- Ready for beta testing

---

## Function Implementation Checklist

### Milestone 0-1 (8 functions)
- [ ] `gf_flip`
- [ ] `gf_setup_frame`
- [ ] `gf_clear`
- [ ] `gf_set_clear_color`
- [ ] `gf_zbuffer_get`
- [ ] `gf_zbuffer_set`
- [ ] `gf_zbuffer_clear`
- [ ] `gf_set_viewport`

### Milestone 2 (8 functions)
- [ ] `gf_create_buffer`
- [ ] `gf_update_buffer_data`
- [ ] `gf_update_buffer_data_offset`
- [ ] `gf_bm_create`
- [ ] `gf_bm_init`
- [ ] `gf_bm_data`
- [ ] `gf_bm_page_in_start`
- [ ] `gf_preload`

### Milestone 3 (4 functions)
- [ ] `gf_render_model`
- [ ] `gf_maybe_create_shader`
- [ ] `gf_bind_uniform_buffer`
- [ ] `gf_update_transform_buffer`

### Milestone 4 (14 functions)
- [ ] `gf_delete_buffer`
- [ ] `gf_map_buffer`
- [ ] `gf_flush_mapped_buffer`
- [ ] `gf_render_primitives`
- [ ] `gf_render_primitives_batched`
- [ ] `gf_render_nanovg`
- [ ] `gf_render_rocket_primitives`
- [ ] `gf_set_clip`
- [ ] `gf_reset_clip`
- [ ] `gf_set_cull`
- [ ] `gf_set_color_buffer`
- [ ] `gf_stencil_set`
- [ ] `gf_stencil_clear`
- [ ] `gf_alpha_mask_set`

### Milestone 5 (11 functions)
- [ ] `gf_render_primitives_particle`
- [ ] `gf_render_primitives_distortion`
- [ ] `gf_render_shield_impact`
- [ ] `gf_render_movie`
- [ ] `gf_scene_texture_begin`
- [ ] `gf_scene_texture_end`
- [ ] `gf_copy_effect_texture`
- [ ] `gf_bm_make_render_target`
- [ ] `gf_bm_set_render_target`
- [ ] `gf_bm_free_data`
- [ ] `gf_set_texture_addressing`

### Milestone 6 (18 functions)
- [ ] `gf_deferred_lighting_begin`
- [ ] `gf_deferred_lighting_end`
- [ ] `gf_deferred_lighting_msaa`
- [ ] `gf_deferred_lighting_finish`
- [ ] `gf_shadow_map_start`
- [ ] `gf_shadow_map_end`
- [ ] `gf_post_process_begin`
- [ ] `gf_post_process_end`
- [ ] `gf_post_process_set_effect`
- [ ] `gf_post_process_set_defaults`
- [ ] `gf_post_process_save_zbuffer`
- [ ] `gf_post_process_restore_zbuffer`
- [ ] `gf_render_decals`
- [ ] `gf_start_decal_pass`
- [ ] `gf_stop_decal_pass`
- [ ] `gf_is_capable`
- [ ] `gf_get_property`
- [ ] `gf_sphere`

### Milestone 7 / Deferred (17 functions)
- [ ] `gf_save_screen`
- [ ] `gf_restore_screen`
- [ ] `gf_free_screen`
- [ ] `gf_get_region`
- [ ] `gf_print_screen`
- [ ] `gf_blob_screen`
- [ ] `gf_create_query_object`
- [ ] `gf_query_value`
- [ ] `gf_query_value_available`
- [ ] `gf_get_query_value`
- [ ] `gf_delete_query_object`
- [ ] `gf_sync_fence`
- [ ] `gf_sync_wait`
- [ ] `gf_sync_delete`
- [ ] `gf_dump_envmap`
- [ ] `gf_calculate_irrmap`
- [ ] `gf_update_texture`
- [ ] `gf_get_bitmap_from_texture`

**Total: 80 functions**

---

## Risk Register

| Risk | Mitigation | Owner | Status |
|------|------------|-------|--------|
| Shader preprocessing fails for Vulkan | Test early in M3, have fallback | - | Open |
| Pipeline creation causes stutter | Accept initially, optimize in M7 | - | Open |
| Driver-specific bugs | Test on all vendors by M7 | - | Open |
| Memory leaks | VMA + validation layers | - | Open |
| Performance regression | Profile by M7 | - | Open |

---

## Approval

This plan is ready for implementation once:
1. [ ] Open questions in Requirements document resolved
2. [ ] Pre-implementation checklist completed
3. [ ] Technical lead approval
4. [ ] Resource allocation confirmed

---

**Related Documents:**
- [VULKAN_PORT_REQUIREMENTS.md](VULKAN_PORT_REQUIREMENTS.md) - Requirements specification
- [VULKAN_PORT_PLAN_REVIEW.md](VULKAN_PORT_PLAN_REVIEW.md) - Critical review of v1 plan
