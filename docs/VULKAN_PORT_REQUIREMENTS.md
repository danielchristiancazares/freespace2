# Vulkan Port - Software Requirements Specification

**Version:** 1.0
**Status:** Draft - Requires Stakeholder Review
**Last Updated:** 2024

---

## 1. Purpose and Scope

### 1.1 Purpose
This document specifies the requirements for porting the FreeSpace 2 Open (FSO) rendering backend from OpenGL to Vulkan. It serves as the authoritative source for what must be implemented, what is optional, and what is explicitly out of scope.

### 1.2 Scope
**In Scope:**
- Vulkan 1.1 rendering backend as alternative to OpenGL
- Feature parity with current OpenGL implementation
- Same visual output as OpenGL (pixel-accurate not required, perceptually identical required)
- Windows, Linux platform support

**Out of Scope (Explicit Exclusions):**
- macOS support (MoltenVK possible future work)
- OpenXR/VR support (separate initiative)
- Ray tracing (Vulkan RT extensions)
- Mobile platforms (Android, iOS)
- Performance improvements beyond parity (optimization is separate phase)

---

## 2. Definitions and Acronyms

| Term | Definition |
|------|------------|
| FSO | FreeSpace 2 Open - the open-source engine |
| VMA | Vulkan Memory Allocator library |
| SPIR-V | Standard Portable Intermediate Representation for shaders |
| Swapchain | Vulkan's presentation surface abstraction |
| Pipeline | Vulkan's immutable rendering state object |
| Descriptor | Vulkan's resource binding mechanism |

---

## 3. Constraints

### 3.1 Technical Constraints

| Constraint | Value | Source |
|------------|-------|--------|
| Minimum Vulkan Version | 1.1 | `VulkanRenderer.cpp:26` - `MinVulkanVersion(1, 1, 0, 0)` |
| API Version Used | VK_API_VERSION_1_1 | `VulkanRenderer.cpp:410` |
| Minimum SDL Version | 2.0.6 | `VulkanRenderer.h:9` - `SDL_VERSION_ATLEAST(2, 0, 6)` |
| Memory Allocator | VMA 3.0+ | Plan requirement |
| Shader Format | SPIR-V | Vulkan requirement |

### 3.2 Compatibility Constraints

| Constraint | Requirement |
|------------|-------------|
| Must coexist with OpenGL | Both backends compilable, runtime selectable |
| No breaking changes to gr_screen API | Function pointers must remain compatible |
| Existing mods must work | No changes to asset formats or mod APIs |
| Config file format unchanged | `VideocardFs2open` setting format preserved |

### 3.3 Hardware Constraints

| Constraint | Details |
|------------|---------|
| Discrete GPUs | Required (NVIDIA, AMD) |
| Integrated GPUs | Required (Intel HD 4000+, AMD APU) |
| Virtual GPUs | Supported (VM environments) |
| CPU-only rendering | Not supported |

---

## 4. Functional Requirements

### 4.1 Core Rendering (MUST HAVE)

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-001 | Initialize Vulkan instance and device | Game launches without crash with `-vulkan` flag |
| FR-002 | Create and manage swapchain | Render to window at configured resolution |
| FR-003 | Handle window resize | Resize without crash, correct rendering after |
| FR-004 | Handle minimize/restore | No crash, rendering resumes correctly |
| FR-005 | Handle fullscreen toggle | Mode switch without crash |
| FR-006 | Clear screen to color | `gf_clear()` fills screen with configured color |
| FR-007 | Present frame | `gf_flip()` displays rendered frame |

### 4.2 Resource Management (MUST HAVE)

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

### 4.3 Shader System (MUST HAVE)

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-020 | Load SPIR-V shaders | Pre-compiled shaders load successfully |
| FR-021 | Support shader variants | FLAG-based compilation produces correct results |
| FR-022 | Bind uniform buffers | `gf_bind_uniform_buffer()` updates shader data |
| FR-023 | Support all shader types | 24 shader types from `shader_type` enum |

### 4.4 Rendering Operations (MUST HAVE)

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-030 | Render 3D models | `gf_render_model()` displays ship/station models |
| FR-031 | Render primitives | `gf_render_primitives()` draws basic geometry |
| FR-032 | Render particles | `gf_render_primitives_particle()` displays effects |
| FR-033 | Render UI (NanoVG) | `gf_render_nanovg()` draws vector UI |
| FR-034 | Render UI (RocketUI) | `gf_render_rocket_primitives()` draws HTML UI |
| FR-035 | Render batched bitmaps | `gf_render_primitives_batched()` draws HUD |
| FR-036 | Render video | `gf_render_movie()` plays cutscenes |

### 4.5 State Management (MUST HAVE)

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-040 | Depth testing | `gf_zbuffer_set()` modes work correctly |
| FR-041 | Stencil operations | `gf_stencil_set()` modes work correctly |
| FR-042 | Blend modes | All 6 `gr_alpha_blend` modes render correctly |
| FR-043 | Culling | `gf_set_cull()` enables/disables face culling |
| FR-044 | Scissor/clip regions | `gf_set_clip()`, `gf_reset_clip()` work |
| FR-045 | Viewport | `gf_set_viewport()` sets render area |

### 4.6 Advanced Rendering (MUST HAVE for Visual Parity)

| ID | Requirement | Acceptance Criteria |
|----|-------------|---------------------|
| FR-050 | Deferred lighting | G-buffer renders, lighting accumulates |
| FR-051 | Shadow mapping | 4-cascade shadows render correctly |
| FR-052 | Post-processing | Bloom, AA, tonemapping effects work |
| FR-053 | Distortion effects | Thruster/warp distortion visible |
| FR-054 | Shield effects | Impact visualization renders |
| FR-055 | Decal rendering | Damage decals appear on models |

### 4.7 Capabilities (MUST Report Accurately)

| ID | Capability | Vulkan Support |
|----|------------|----------------|
| FR-060 | CAPABILITY_ENVIRONMENT_MAP | Yes (cubemaps supported) |
| FR-061 | CAPABILITY_NORMAL_MAP | Yes |
| FR-062 | CAPABILITY_HEIGHT_MAP | Yes |
| FR-063 | CAPABILITY_SOFT_PARTICLES | Yes (depth buffer readable) |
| FR-064 | CAPABILITY_DISTORTION | Yes |
| FR-065 | CAPABILITY_POST_PROCESSING | Yes |
| FR-066 | CAPABILITY_DEFERRED_LIGHTING | Yes |
| FR-067 | CAPABILITY_SHADOWS | Yes |
| FR-068 | CAPABILITY_THICK_OUTLINE | Conditional (geometry shader support) |
| FR-069 | CAPABILITY_BATCHED_SUBMODELS | Yes |
| FR-070 | CAPABILITY_TIMESTAMP_QUERY | Yes |
| FR-071 | CAPABILITY_SEPARATE_BLEND_FUNCTIONS | Conditional (independentBlend feature) |
| FR-072 | CAPABILITY_PERSISTENT_BUFFER_MAPPING | Yes (always in Vulkan) |
| FR-073 | CAPABILITY_BPTC | Conditional (BC7 format support) |
| FR-074 | CAPABILITY_LARGE_SHADER | Yes |
| FR-075 | CAPABILITY_INSTANCED_RENDERING | Yes |

### 4.8 Properties (MUST Return Correct Values)

| ID | Property | Source |
|----|----------|--------|
| FR-080 | UNIFORM_BUFFER_OFFSET_ALIGNMENT | `VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment` |
| FR-081 | UNIFORM_BUFFER_MAX_SIZE | `VkPhysicalDeviceLimits::maxUniformBufferRange` |
| FR-082 | MAX_ANISOTROPY | `VkPhysicalDeviceLimits::maxSamplerAnisotropy` |

---

## 5. Non-Functional Requirements

### 5.1 Performance

| ID | Requirement | Metric |
|----|-------------|--------|
| NFR-001 | Frame rate parity | Within 10% of OpenGL on same hardware |
| NFR-002 | No frame time spikes | <100ms stalls (excluding shader compilation) |
| NFR-003 | Memory usage parity | Within 20% of OpenGL memory footprint |
| NFR-004 | Load time parity | Within 20% of OpenGL load times |

### 5.2 Reliability

| ID | Requirement | Metric |
|----|-------------|--------|
| NFR-010 | Validation layer clean | Zero errors after 1000 frames |
| NFR-011 | Crash free | No crashes in standard gameplay |
| NFR-012 | Device lost recovery | Recovers from GPU reset/driver crash |
| NFR-013 | Resource cleanup | No memory leaks on exit |

### 5.3 Compatibility

| ID | Requirement | Metric |
|----|-------------|--------|
| NFR-020 | NVIDIA support | Works on GTX 600 series and newer |
| NFR-021 | AMD support | Works on GCN 1.0 and newer |
| NFR-022 | Intel support | Works on HD 4000 and newer |
| NFR-023 | Driver versions | Works with drivers <2 years old |

### 5.4 Maintainability

| ID | Requirement | Metric |
|----|-------------|--------|
| NFR-030 | Code organization | Follows existing FSO patterns |
| NFR-031 | Documentation | All public functions documented |
| NFR-032 | Debug support | Works with RenderDoc, validation layers |

---

## 6. Assumptions

### 6.1 Documented Assumptions

| ID | Assumption | Impact if Wrong |
|----|------------|-----------------|
| A-001 | VMA handles memory management adequately | Must implement custom allocator |
| A-002 | Vulkan 1.1 provides all needed features | Must require 1.2 or extensions |
| A-003 | Existing shader preprocessing can output GLSL 4.50 | Must modify shader system |
| A-004 | SDL2 Vulkan support is sufficient | Must use platform-specific code |
| A-005 | Single graphics queue is sufficient | Must implement multi-queue |
| A-006 | Pipeline creation stalls are acceptable | Must implement async compilation |
| A-007 | All target hardware supports BC compression | Must implement fallback |
| A-008 | Geometry shaders are available when needed | Must disable thick outlines |

### 6.2 Dependency Assumptions

| Dependency | Assumed Version | Risk |
|------------|-----------------|------|
| Vulkan SDK | 1.1.x or later | Low - widely available |
| VMA | 3.0.0 or later | Low - header-only, MIT license |
| SDL2 | 2.0.6 or later | Low - FSO already requires this |
| glslangValidator | Any recent | Medium - build dependency |

---

## 7. Open Questions (REQUIRE RESOLUTION)

### 7.1 Technical Decisions Needed

| ID | Question | Options | Recommendation |
|----|----------|---------|----------------|
| Q-001 | Shader compilation: build-time or runtime? | Build-time (faster load), Runtime (flexible) | Build-time for base, runtime for variants |
| Q-002 | How to handle pipeline creation stalls? | Sync (stall), Async (complex), Precompile (memory) | Sync initially, optimize if needed |
| Q-003 | Descriptor set strategy? | Per-draw, Per-frame, Bindless | Per-frame allocation with reset |
| Q-004 | Support Vulkan 1.0 or require 1.1? | 1.0 (wider support), 1.1 (better features) | **1.1 (already in code)** |
| Q-005 | What if geometry shaders unavailable? | Fail, Fallback, Disable feature | Disable thick outlines only |

### 7.2 Process Decisions Needed

| ID | Question | Options | Recommendation |
|----|----------|---------|----------------|
| Q-010 | When is "visual parity" achieved? | Screenshot diff, Human review, Both | Human review with screenshot aid |
| Q-011 | Who approves milestone completion? | Developer, Reviewer, Community | At least one reviewer |
| Q-012 | How to handle Vulkan-specific bugs? | Fix immediately, Track separately | Track in issue tracker |
| Q-013 | Beta testing strategy? | Opt-in flag, Separate build | Opt-in `-vulkan` flag |

---

## 8. Acceptance Criteria

### 8.1 Milestone Acceptance

| Milestone | Acceptance Criteria |
|-----------|---------------------|
| M0: Foundation | Game starts with `-vulkan`, handles resize, no validation errors |
| M1: Triangle | Colored triangle renders, verified by screenshot |
| M2: Textured | Textured rotating quad renders |
| M3: Model | GTF Ulysses model renders with textures and lighting |
| M4: Scene | Main menu fully navigable, tech room functional |
| M5: Mission | Training mission 1 completable start-to-finish |
| M6: Parity | Side-by-side comparison approved by reviewer |
| M7: Production | 1-hour gameplay session without validation errors or crashes |

### 8.2 Final Acceptance

The Vulkan port is complete when:
1. All MUST HAVE requirements (FR-*) are implemented
2. All NFR performance metrics are met
3. A reviewer has approved visual parity (M6)
4. Production stability criteria met (M7)
5. No P0/P1 bugs remain open

---

## 9. Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| Driver bugs cause rendering issues | Medium | High | Test on multiple vendors, document workarounds |
| Shader compilation takes too long | Medium | Medium | Implement caching, consider async compilation |
| Memory management issues | Low | High | Use VMA, extensive validation testing |
| Performance worse than OpenGL | Medium | Medium | Profile early, optimize hot paths |
| Feature X not supported on hardware Y | Medium | Low | Graceful degradation, report capabilities |

---

## 10. Glossary of Implementation Details

### 10.1 Function Pointer Mapping

All 80+ `gr_screen` function pointers must be implemented. Priority groupings:

**Critical (Milestone 0-3):** 20 functions
**Important (Milestone 4-5):** 25 functions
**Advanced (Milestone 6):** 20 functions
**Deferred:** 15 functions

See Implementation Plan v2 for detailed breakdown.

### 10.2 Shader Type Coverage

All 24 `shader_type` values must be supported:
- SDR_TYPE_MODEL (most complex)
- SDR_TYPE_EFFECT_PARTICLE
- SDR_TYPE_EFFECT_DISTORTION
- SDR_TYPE_POST_PROCESS_* (8 types)
- SDR_TYPE_DEFERRED_* (2 types)
- SDR_TYPE_BATCHED_BITMAP
- SDR_TYPE_NANOVG
- SDR_TYPE_DECAL
- etc.

---

## 11. Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2024 | - | Initial requirements specification |

---

## 12. Approval

| Role | Name | Date | Signature |
|------|------|------|-----------|
| Technical Lead | _______________ | _____ | _________ |
| Reviewer | _______________ | _____ | _________ |
| Stakeholder | _______________ | _____ | _________ |

---

**Document Status:** This document requires stakeholder review and approval before implementation begins. Open questions in Section 7 must be resolved.
