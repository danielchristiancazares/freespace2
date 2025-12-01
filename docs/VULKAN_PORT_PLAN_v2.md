# FreeSpace 2 Vulkan Port - Revised Plan v2

## Design Principles

1. **Make it work, make it right, make it fast** - in that order
2. **Vertical slices over horizontal layers** - each milestone produces visible results
3. **No premature abstraction** - add wrappers only when patterns emerge
4. **Fail fast** - validation layers always on in debug; crashes are better than corruption

---

## Current State

### Existing Vulkan Code (Usable As-Is)
- `VulkanRenderer.cpp`: Instance, device, swapchain, basic render pass ✓
- `RenderFrame.cpp`: Frame synchronization with semaphores/fences ✓
- `gr_vulkan.cpp`: Initialization entry point ✓

### Existing OpenGL Reference
- ~4,800 LOC across 8 files
- Function pointer abstraction in `gr_screen` struct (~80 functions)
- SPIR-V shader infrastructure exists

---

## Milestone-Based Plan

### Milestone 0: Build Foundation
**Goal:** Vulkan backend compiles and initializes without crashing

**Tasks:**
1. CMake integration for VMA (header-only, add to include path)
2. Add `WITH_VULKAN` compile flag, ensure clean compilation
3. Implement swapchain recreation on window resize
4. Set up validation layers in debug builds with callback logging
5. Establish GLSL → SPIR-V shader compilation (build-time via `glslangValidator`)

**Deliverable:** Game starts with `-vulkan` flag, shows black screen, handles resize

**Files to modify:**
- `CMakeLists.txt` - Add VMA, Vulkan conditionals
- `VulkanRenderer.cpp` - Add `recreateSwapchain()` method
- `gr_vulkan.cpp` - Add validation callback

**Swapchain Recreation (Critical Missing Piece):**
```cpp
void VulkanRenderer::recreateSwapchain() {
    // 1. Wait for device idle
    m_device->waitIdle();

    // 2. Destroy old resources
    m_swapChainFramebuffers.clear();
    m_swapChainImageViews.clear();
    m_swapChain.reset();

    // 3. Query new surface capabilities
    PhysicalDeviceValues deviceValues;
    // ... repopulate deviceValues

    // 4. Recreate swapchain and dependent resources
    createSwapChain(deviceValues);
    createFrameBuffers();
}
```

---

### Milestone 1: First Triangle
**Goal:** Render a hardcoded colored triangle

**Tasks:**
1. Create simple vertex/fragment SPIR-V shaders (position + color)
2. Create vertex buffer with triangle data
3. Record command buffer: begin pass → bind pipeline → draw → end pass
4. Implement `gf_clear()` and `gf_flip()`

**Deliverable:** Colored triangle on screen

**No new classes needed** - extend `VulkanRenderer` directly

```cpp
// In VulkanRenderer
void renderTestTriangle() {
    auto cmd = beginFrame();

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_testPipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &m_testVertexBuffer, &offset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);

    endFrame(cmd);
}
```

---

### Milestone 2: Textured Quad
**Goal:** Render a textured quad with uniform buffer

**Tasks:**
1. Implement basic buffer creation (`gf_create_buffer`, `gf_update_buffer_data`)
2. Implement texture upload for a single test texture
3. Create descriptor set layout and allocate descriptor set
4. Create pipeline with texture sampling
5. Implement simple MVP uniform buffer

**Deliverable:** Textured quad that can rotate (proves uniforms work)

**Minimal structures needed:**
```cpp
// Simple handle→resource maps (not full manager classes)
SCP_unordered_map<gr_buffer_handle::value_type, VkBuffer> g_buffers;
SCP_unordered_map<gr_buffer_handle::value_type, VmaAllocation> g_bufferAllocs;

SCP_unordered_map<int, VkImage> g_textures;       // bitmap handle → image
SCP_unordered_map<int, VkImageView> g_textureViews;
SCP_unordered_map<int, VmaAllocation> g_textureAllocs;
```

---

### Milestone 3: Model Rendering
**Goal:** Render a single ship model from the game

**Tasks:**
1. Integrate with bmpman for texture loading (`gf_bm_create`, `gf_bm_data`)
2. Implement `gf_render_model()` for basic model rendering
3. Load model shader (main-v.sdr, main-f.sdr) as SPIR-V
4. Implement model uniform buffer (`model_uniform_data` struct)
5. Handle vertex layout conversion

**Deliverable:** Single ship model rendered with textures and basic lighting

**Key insight:** Don't create `TextureManager` - integrate with existing bmpman:
```cpp
// In bmpman callbacks
void vk_bm_create(bitmap_slot* slot) {
    // Create VkImage, VkImageView
    // Store in slot->gr_info (it's a gr_bitmap_info*)
}

void vk_bm_data(int handle, bitmap* bm) {
    // Upload pixel data to existing VkImage
}
```

---

### Milestone 4: Basic Scene
**Goal:** Render main menu and tech room

**Tasks:**
1. Implement remaining buffer functions (`gf_delete_buffer`, `gf_map_buffer`)
2. Implement UI renderers (`gf_render_primitives_batched`, `gf_render_nanovg`)
3. Implement state functions (`gf_zbuffer_set`, `gf_set_clip`, `gf_set_cull`)
4. Create pipeline variants for different blend modes

**Deliverable:** Main menu is navigable, tech room shows ship models

**Pipeline management (simplified):**
```cpp
// Key based on actual state, not all possible state
struct PipelineKey {
    VkRenderPass renderPass;
    shader_type shader;
    uint32_t shaderFlags;
    gr_alpha_blend blendMode;
    gr_zbuffer_type depthMode;
    bool operator==(const PipelineKey&) const;
};

SCP_unordered_map<PipelineKey, VkPipeline> g_pipelineCache;

VkPipeline getOrCreatePipeline(const PipelineKey& key) {
    auto it = g_pipelineCache.find(key);
    if (it != g_pipelineCache.end()) return it->second;

    // Create pipeline synchronously (optimize later if needed)
    VkPipeline pipeline = createPipeline(key);
    g_pipelineCache[key] = pipeline;
    return pipeline;
}
```

---

### Milestone 5: Playable Mission
**Goal:** Complete a simple mission with forward rendering

**Tasks:**
1. Implement particle rendering (`gf_render_primitives_particle`)
2. Implement shield effects (`gf_render_shield_impact`)
3. Implement scene texture system (`gf_scene_texture_begin/end`)
4. Implement render targets (`gf_bm_make_render_target`, `gf_bm_set_render_target`)

**Deliverable:** Can complete training mission 1

---

### Milestone 6: Visual Parity
**Goal:** Match OpenGL visual output

**Tasks:**
1. Implement deferred lighting path
2. Implement shadow mapping
3. Implement post-processing (bloom, FXAA, tonemapping)
4. Implement remaining specialized renderers

**Deliverable:** Side-by-side comparison shows no visual differences

---

### Milestone 7: Production Ready
**Goal:** Stable, performant, handles edge cases

**Tasks:**
1. Handle device lost gracefully
2. Optimize based on profiling data
3. Test on variety of hardware
4. Document known limitations

**Deliverable:** Vulkan can be default backend

---

## Implementation Priorities

### Must Implement (Blocking for each milestone)

**Milestone 0-1:**
- `gf_flip`, `gf_setup_frame`
- `gf_clear`, `gf_set_clear_color`

**Milestone 2:**
- `gf_create_buffer`, `gf_delete_buffer`, `gf_update_buffer_data`
- `gf_bm_create`, `gf_bm_init`, `gf_bm_data`, `gf_bm_free_data`

**Milestone 3:**
- `gf_render_model`
- `gf_maybe_create_shader`
- `gf_bind_uniform_buffer`

**Milestone 4:**
- `gf_render_primitives`, `gf_render_primitives_batched`
- `gf_render_nanovg`, `gf_render_rocket_primitives`
- `gf_zbuffer_set`, `gf_stencil_set`, `gf_set_clip`, `gf_reset_clip`
- `gf_set_cull`, `gf_set_color_buffer`

**Milestone 5:**
- `gf_render_primitives_particle`, `gf_render_primitives_distortion`
- `gf_render_shield_impact`, `gf_render_movie`
- `gf_scene_texture_begin`, `gf_scene_texture_end`
- `gf_bm_make_render_target`, `gf_bm_set_render_target`

**Milestone 6:**
- `gf_deferred_lighting_*` (4 functions)
- `gf_shadow_map_start`, `gf_shadow_map_end`
- `gf_post_process_*` (6 functions)
- `gf_render_decals`, `gf_start/stop_decal_pass`

### Defer Until Needed
- `gf_save_screen`, `gf_restore_screen`, `gf_free_screen`
- `gf_print_screen`, `gf_blob_screen`, `gf_get_region`
- `gf_openxr_*` (6 functions)
- `gf_dump_envmap`, `gf_calculate_irrmap`
- Query objects (5 functions)
- Sync/fence (3 functions)

---

## Shader Compilation Strategy

### Build-Time Compilation (Phase 1)
```bash
# In CMake or build script
glslangValidator -V main-v.sdr -o main-v.spv --target-env vulkan1.1
glslangValidator -V main-f.sdr -o main-f.spv --target-env vulkan1.1
```

**Problem:** The `.sdr` files use custom preprocessing.

**Solution:** Two-stage compilation:
1. Run FSO's shader preprocessor to expand `#predefine`/`#prereplace`
2. Run `glslangValidator` on the result

### Shader Variant Strategy
```cpp
// Don't compile all 2^15 combinations upfront
// Compile on first use, cache result

struct ShaderKey {
    shader_type type;
    uint32_t flags;
};

SCP_unordered_map<ShaderKey, VkShaderModule> g_vertexShaders;
SCP_unordered_map<ShaderKey, VkShaderModule> g_fragmentShaders;

VkShaderModule getVertexShader(shader_type type, uint32_t flags) {
    ShaderKey key{type, flags};
    auto it = g_vertexShaders.find(key);
    if (it != g_vertexShaders.end()) return it->second;

    // Compile on demand (stall on first use - optimize later)
    SCP_string source = preprocessShader(type, flags, ShaderStage::Vertex);
    VkShaderModule module = compileGLSLtoSPIRV(source);
    g_vertexShaders[key] = module;
    return module;
}
```

---

## Error Handling

### Swapchain Out of Date
```cpp
VkResult result = vkAcquireNextImageKHR(..., &imageIndex);
if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
    recreateSwapchain();
    return; // Skip this frame
}
```

### Device Lost
```cpp
if (result == VK_ERROR_DEVICE_LOST) {
    mprintf(("Vulkan device lost! Attempting recovery...\n"));
    // Full reinitialization required
    shutdown();
    initialize();
}
```

### Validation Errors
```cpp
VkBool32 debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                       VkDebugUtilsMessageTypeFlagsEXT type,
                       const VkDebugUtilsMessengerCallbackDataEXT* data,
                       void* user) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        mprintf(("Vulkan: %s\n", data->pMessage));
    }
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        // Break into debugger on validation errors
        ASSERT(0);
    }
    return VK_FALSE;
}
```

---

## Testing Strategy

### Automated Testing
1. **Validation Layer CI:** All builds run with `VK_LAYER_KHRONOS_validation`
2. **Frame Capture:** `--capture-frames=100` flag dumps first 100 frames as PNG
3. **Golden Image Comparison:** Compare against OpenGL reference frames
4. **Smoke Test:** Launch each major screen, verify no validation errors

### Manual Testing Checklist
- [ ] Main menu renders correctly
- [ ] Tech room model viewer works
- [ ] Training mission 1 completable
- [ ] Campaign mission playable
- [ ] Window resize doesn't crash
- [ ] Alt-tab doesn't corrupt rendering
- [ ] Resolution change works
- [ ] Multi-monitor scenarios

---

## Removed From Plan

The following items from v1 are **deferred indefinitely**:

1. ~~VulkanMemoryAllocator wrapper~~ → Use VMA directly
2. ~~CommandBufferPool class~~ → Extend RenderFrame
3. ~~DescriptorSetAllocator/DescriptorWriter~~ → Simple per-frame allocation
4. ~~RenderPassManager~~ → Create passes inline
5. ~~StateManager~~ → Pipelines are immutable; no state tracking needed
6. ~~VulkanDrawContext~~ → Direct Vulkan calls
7. ~~Pipeline derivatives~~ → Optimization; defer
8. ~~Background pipeline compilation~~ → Optimization; defer
9. ~~Bindless textures~~ → Extension; not universally supported
10. ~~Push descriptors~~ → Extension; not core Vulkan
11. ~~Multi-threaded command recording~~ → Not needed for this game
12. ~~Timeline semaphores~~ → Regular semaphores sufficient
13. ~~OpenXR support~~ → Separate initiative

---

## Success Criteria

| Milestone | Criteria |
|-----------|----------|
| 0 | Game launches with `-vulkan`, no validation errors on startup |
| 1 | Triangle renders, resize doesn't crash |
| 2 | Textured rotating quad |
| 3 | Ship model with textures and lighting |
| 4 | Main menu fully functional |
| 5 | Training mission 1 completable |
| 6 | No visual differences from OpenGL in screenshot comparison |
| 7 | 1000 frames without validation error or crash |

---

## Architecture Summary

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

---

## Conclusion

This revised plan prioritizes working software over comprehensive design. Each milestone produces visible, testable results. Abstractions are added only when justified by emerging patterns, not anticipated needs.

The key changes from v1:
1. **Milestone-driven** instead of phase-driven
2. **Minimal abstraction** - use Vulkan and VMA directly
3. **Explicit error handling** for swapchain recreation and device lost
4. **Deferred optimization** - get it working first
5. **Concrete success criteria** for each milestone
