# NVIDIA DLSS Integration Plan

This document outlines the architectural changes and implementation steps required to integrate NVIDIA DLSS (Deep Learning Super Sampling) into the FreeSpace 2 Open Vulkan renderer.

## 1. Overview

DLSS uses AI to upscale a lower-resolution rendered image to a higher output resolution, reconstructing high-frequency details and providing temporal stability (anti-aliasing).

### Goals
*   Integrate NVIDIA NGX SDK into the Vulkan backend.
*   Implement specific rendering requirements: Motion Vectors and Projection Jitter.
*   Decouple **Render Resolution** from **Display Resolution**.
*   Insert the DLSS Evaluation pass into the frame graph.

### Scope
*   **Target Backend:** Vulkan (`code/graphics/vulkan/`)
*   **Target Hardware:** NVIDIA RTX 20-series and newer.
*   **DLSS Version:** Latest available (assumed 3.x or 2.x Super Resolution).

---

## 2. Technical Requirements

DLSS requires specific inputs from the rendering engine:

1.  **Color Buffer:** Low-resolution, aliased, HDR input.
2.  **Depth Buffer:** Low-resolution depth buffer.
3.  **Motion Vectors:** High-precision screen-space motion vectors (Velocity buffer).
4.  **Exposure:** Exposure value (if auto-exposure is used).
5.  **Jittered Projection:** Sub-pixel jitter applied to the camera projection matrix (e.g., Halton sequence).
6.  **Texture LOD Bias:** Adjusted mipmap selection for lower render resolution.

---

## 3. Architecture Changes

### 3.1. Resolution Management
Currently, `gr_screen.max_w` / `max_h` drive both render target sizing and swapchain extent.
*   **New State:** Introduce `RenderResolution` vs `DisplayResolution`.
    *   `DisplayResolution`: Size of the Swapchain and UI targets.
    *   `RenderResolution`: Size of the G-Buffer, Depth, and pre-upscale Scene Color.
*   **Scaling:** `VulkanRenderTargets::resize()` must accept `RenderResolution` for 3D assets and `DisplayResolution` for Post-Process/UI buffers.

### 3.2. New G-Buffer Attachment: Velocity
A generic G-buffer layout update is required.
*   **Format:** `VK_FORMAT_R16G16_SFLOAT` (recommended) or `R32G32_SFLOAT`.
*   **Content:** Screen-space velocity `(CurrentPosNDC - PreviousPosNDC)`.
*   **Location:** Add as G-Buffer Attachment #5 (or repurpose an existing slot if bandwidth constrained, though slot 5 is cleanest).

### 3.3. Jitter System
*   **Camera:** Implement a jitter generator (Halton sequence 2x3).
*   **Matrices:**
    *   Apply jitter to `gr_projection_matrix` used for rendering geometry.
    *   **Crucial:** Keep an *unjittered* copy of the matrix for:
        *   Frustum culling.
        *   DLSS input (DLSS needs to know the jitter offset).
        *   Motion vector calculation (optional, depending on formulation).
*   **Phase:** Update jitter offset every frame index.

### 3.4. NGX SDK Wrapper
*   Create `VulkanDLSS` manager class.
*   Manage `VkInstance` / `VkDevice` creation with required NV extensions (`VK_NVX_binary_import`, `VK_KHR_push_descriptor`, etc., check SDK docs).
*   Initialize NGX feature (`NGX_VULKAN_EVALUATE_DLSS_EXT`).

---

## 4. Implementation Tasks

### Phase 1: Foundation (NGX & Resolution)

1.  **SDK Integration:**
    *   Add NVIDIA NGX SDK to `lib/` or `code/external`.
    *   Update CMake/build system to link against NGX.
2.  **VulkanDevice Extension:**
    *   In `VulkanDevice.cpp`, query and enable `VK_NV_ngx` (or relevant extensions).
    *   Initialize NGX context on startup.
3.  **Resolution Decoupling:**
    *   Modify `VulkanRenderTargets` to store separate `m_renderExtent` and `m_displayExtent`.
    *   Update `createGBufferResources` to use `m_renderExtent`.
    *   Update `createPostProcessResources` to use `m_displayExtent`.
    *   Update `gr_screen` or `VulkanGraphics` to handle the scaling factor logic.

### Phase 2: Motion Vectors (The "Hard" Part)

1.  **RenderTarget Update:**
    *   Add `m_velocityImage`, `m_velocityView` to `VulkanRenderTargets`.
    *   Update `kGBufferCount` to 6.
2.  **Pipeline Update:**
    *   Update `VulkanLayoutContracts` and `VulkanPipelineManager` to reflect the new attachment.
    *   Update `DeferredGBufferTarget` definition in `VulkanRenderingSession.h` to include the velocity attachment.
3.  **Shader Update (`model.vert` / `model.frag`):**
    *   **Vertex Shader:** Need `uPrevModelViewProj` matrix.
        *   *Challenge:* The engine currently uploads `uModel` matrices per-draw via a storage buffer. We need to store the *previous* frame's matrix for every object or calculate it.
        *   *Alternative:* For static objects, only Camera moves. `PrevPos = uPrevViewProj * WorldPos`.
        *   *Dynamic Objects:* Need history tracking in `code/graphics` or `code/object`.
    *   **Fragment Shader:** Calculate `Velocity = (CurrentPosNDC.xy / CurrentPosNDC.w) - (PrevPosNDC.xy / PrevPosNDC.w)`.
    *   **Output:** Write to G-Buffer Target 5. Note: Mask out jitter from calculation if possible, or let DLSS handle it (DLSS expects motion vectors *without* jitter usually, or specific handling).

### Phase 3: Jitter & LOD Bias

1.  **Matrix System:**
    *   In `gr_set_proj_matrix`, apply sub-pixel offset if DLSS is active.
    *   Offset formula: `(sampleX - 0.5) / RenderWidth, (sampleY - 0.5) / RenderHeight`.
2.  **Texture Manager:**
    *   Apply `mipLodBias` to samplers in `VulkanTextureManager`.
    *   Formula: `log2(RenderWidth / DisplayWidth) - 1.0` (approx).

### Phase 4: The DLSS Pass

1.  **Placement:**
    *   Insert after `deferredLightingFinish()` but *before* `recordTonemappingToSwapchain` (or replace the copy to swapchain).
    *   DLSS usually outputs to a separate resource (Upscaled Color).
2.  **Resource Transitions:**
    *   Transition G-Buffer colors, Depth, and Velocity to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`.
    *   Transition Output Image to `VK_IMAGE_LAYOUT_GENERAL` or `STORAGE` (NGX requirement dependent).
3.  **Command Recording:**
    *   Call `NGX_VULKAN_EVALUATE_DLSS_EXT`.
    *   Inputs:
        *   Source: `sceneHdrImage` (Render Resolution)
        *   Depth: `depthImage`
        *   Motion: `velocityImage`
        *   Output: `displayHdrImage` (Display Resolution - new target needed)
4.  **Post-Process Integration:**
    *   The Post-Process pipeline (Bloom, Tonemap) must now operate on the **Upscaled** image (Display Resolution).
    *   Currently, post-processing happens on `sceneHdrImage` (Render Res).
    *   *Change:* DLSS becomes the bridge. `Scene (Low)` -> `DLSS` -> `Upscaled (High)` -> `PostFX (High)` -> `Swapchain`.

---

## 5. Files to Modify

| File | Changes |
|------|---------|
| `code/graphics/vulkan/VulkanRenderTargets.h` | Add Velocity image; split Render/Display extents. |
| `code/graphics/vulkan/VulkanRenderingSession.h` | Update `DeferredGBufferTarget` definition. |
| `code/graphics/vulkan/VulkanRenderer.cpp` | Initialize NGX; Orchestrate Jitter, Motion Vectors, DLSS Pass. |
| `code/graphics/vulkan/VulkanDevice.cpp` | Enable NV extensions. |
| `code/graphics/vulkan/VulkanTextureManager.cpp` | Apply LOD bias to samplers. |
| `code/graphics/shaders/model.vert/frag` | Calculate and output motion vectors. |
| `code/graphics/render.cpp` | Update projection matrix logic. |

## 6. Challenges & Risks

1.  **Motion Vector Accuracy:** Incorrect MVs cause "ghosting" or "smearing". Dynamic objects (ships) need precise previous-frame matrices.
2.  **Transparent Objects:** Particles/Glass usually don't write to depth/motion vectors. DLSS handles this but might produce artifacts on transparency if not careful.
    *   *Mitigation:* Render transparency *after* DLSS? No, transparency needs to be upscaled too. Usually, transparency is rendered at low-res on top of opaque, then whole image upscaled.
3.  **Pipeline Barriers:** NGX is internal; we must ensure correct barriers before passing resources to it.
4.  **Memory:** Extra G-buffer channel (Velocity) increases VRAM usage.

## 7. Validation

1.  **Debug View:** Implement a debug mode to visualize the Velocity buffer (Red/Green channels).
    *   Static objects should show velocity when camera moves.
    *   Moving objects should show velocity relative to camera.
2.  **Jitter Check:** Disable TAA/DLSS; scene should "shake" by <1 pixel.
