# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Vulkan Renderer Subsystem

This directory contains the Vulkan rendering backend for FreeSpace Open. It implements modern Vulkan 1.4 rendering with fallbacks for 1.2/1.3, using dynamic rendering (no render passes), push descriptors, and bindless textures.

## Building and Testing

Build with Vulkan enabled:
```powershell
.\build.ps1 -Vulkan "true" -Tests "true"
```

Run Vulkan-specific tests:
```powershell
.\build\bin\unittests.exe --gtest_filter="*Vulkan*"
```

Run a single test:
```powershell
.\build\bin\unittests.exe --gtest_filter="VulkanLayoutContracts.Scenario_ModelUsesModelLayoutAndVertexPulling"
```

## Architecture Overview

The Vulkan renderer follows a manager-based architecture where each subsystem owns its resources and the VulkanRenderer orchestrates them.

### Core Components

**VulkanRenderer** (`VulkanRenderer.h/.cpp`) is the main orchestrator. It owns the Vulkan instance, device, swapchain, and coordinates frame submission. Key responsibilities include swapchain management, frame lifecycle (`MAX_FRAMES_IN_FLIGHT = 2` for double-buffered VulkanFrame objects), and deferred rendering infrastructure (G-buffer with 3 attachments). The `FrameLifecycleTracker` (`FrameLifecycleTracker.h`) validates that draw calls only occur during active recording.

**VulkanFrame** (`VulkanFrame.h/.cpp`) encapsulates per-frame resources: command buffer, synchronization primitives (fences, semaphores, timeline semaphores), and ring buffers for uniform/vertex/staging data.

**VulkanPipelineManager** (`VulkanPipelineManager.h/.cpp`) creates and caches pipelines keyed by shader type, formats, blend mode, and vertex layout hash. Uses extensive dynamic state (Vulkan 1.3 core + EXT extensions) to minimize pipeline variants.

**VulkanShaderManager** (`VulkanShaderManager.h/.cpp`) loads SPIR-V modules from embedded data or filesystem. Modules are cached by shader type and variant flags.

**VulkanDescriptorLayouts** (`VulkanDescriptorLayouts.h/.cpp`) defines two pipeline layout paradigms:
- **Standard**: Push descriptors for per-draw uniforms + texture binding
- **Model**: Bindless texture array + storage buffer for vertex pulling + push constants

**VulkanBufferManager** (`VulkanBufferManager.h/.cpp`) manages GPU buffers with deferred deletion (waits 3 frames before destroying retired buffers to avoid use-after-free).

**VulkanTextureManager** (`VulkanTextureManager.h/.cpp`) handles texture uploads (immediate or staged), sampler caching, and texture lifecycle states (Missing -> Queued -> Uploading -> Resident -> Retired), with Failed as an error state.

### Rendering Paradigms

The renderer supports two shader binding strategies defined in `VulkanLayoutContracts.h`:

1. **Standard Layout** (`PipelineLayoutKind::Standard`): Traditional vertex attributes with push descriptors for per-draw uniforms. Used by all shader types except `SDR_TYPE_MODEL` (effects, post-processing, UI, deferred lighting, etc.).

2. **Model Layout** (`PipelineLayoutKind::Model`): Vertex pulling from storage buffer + bindless textures + push constants. Used exclusively by `SDR_TYPE_MODEL` for 3D model rendering. Push constants are 56 bytes (`ModelPushConstants` in `VulkanModelTypes.h`).

### Frame Flow

1. `flip()` ends previous frame, submits it, advances frame index, waits on fence, resets ring buffers, acquires swapchain image, and calls `beginFrame()`
2. `beginFrame()` begins command buffer recording and syncs model descriptors
3. First draw call triggers `ensureRenderingStarted()` which begins dynamic rendering
4. Draw calls record to command buffer, allocating from ring buffers
5. `endFrame()` ends rendering pass
6. Frame submission and present happen in the next `flip()` call

### Ring Buffer System

Each frame has three ring buffers (`VulkanRingBuffer.h`):
- **Uniform ring** (512 KiB): Per-draw uniform data
- **Vertex ring** (1 MiB): Dynamic vertex data
- **Staging ring** (12 MiB): Texture upload staging

Allocations are bump-pointer with configurable alignment. Rings reset at frame start after fence wait.

## Shader Pipeline

Shaders are compiled via `shaders.cmake`:

1. GLSL source in `code/graphics/shaders/` (e.g., `model.vert`, `model.frag`)
2. `glslc` compiles to SPIR-V with target environment:
   - Vulkan-only shaders: `vulkan1.4` (can use demote_to_helper)
   - Cross-backend shaders: `vulkan1.2` (for OpenGL GLSL translation)
3. `shadertool` generates C++ struct headers (`*_structs.h`) from SPIR-V reflection
4. SPIR-V embedded into executable via `target_embed_files`

Shader layout contracts in `VulkanLayoutContracts.cpp` must match shader declarations exactly.

## Key Constants

`VulkanConstants.h` defines:
- `kFramesInFlight = 3`: Frame synchronization window for descriptor pool sizing and texture binding state tracking
- `kMaxBindlessTextures = 1024`: Bindless array size
- `kModelSetsPerPool = 4096 * kFramesInFlight`: Descriptor pool sizing

Note: `MAX_FRAMES_IN_FLIGHT = 2` in `VulkanRenderer.h` is a separate constant controlling double-buffered VulkanFrame objects, distinct from `kFramesInFlight` which sizes pools for worst-case in-flight resource tracking.

## Test Files

Vulkan tests are in `test/src/graphics/`:
- `test_vulkan_shader_layout_contracts.cpp`: Validates shader layout specs
- `test_vulkan_pipeline_manager.cpp`: Pipeline creation tests
- `test_vulkan_frame_lifecycle.cpp`: Frame state machine tests
- `test_vulkan_texture_contract.cpp`: Texture manager invariants
- `test_vulkan_dynstate.cpp`: Dynamic state capability tests
- `test_model_shader_spirv.cpp`: Model shader SPIR-V validation
- `test_vulkan_descriptor_layouts.cpp`: Descriptor layout validation
- `test_vulkan_renderer_shutdown.cpp`: Shutdown sequence tests
- `test_vulkan_shader_manager_model.cpp`: Model shader loading tests
- `test_vulkan_texture_helpers.cpp`: Texture helper function tests

## Common Patterns

### Adding a New Shader Type

1. Add GLSL sources to `code/graphics/shaders/`
2. Register in `code/shaders.cmake` (SHADERS or VULKAN_SHADERS list)
3. Add layout spec in `VulkanLayoutContracts.cpp`
4. Add loading case in `VulkanShaderManager.cpp`
5. Implement render function in `VulkanGraphics.cpp`
6. Wire function pointer in `init_function_pointers()`

### Dynamic State Usage

The renderer uses Vulkan 1.3 dynamic state (viewport, scissor, cull mode, front face, depth test/write/compare, stencil, primitive topology) plus extended dynamic state 3 when available (color blend enable, write mask, polygon mode, rasterization samples). Pipeline creation bakes only the non-dynamic state.

### Descriptor Binding

Standard shaders use `cmd.pushDescriptorSetKHR()` for zero-allocation per-draw binding. Model shaders bind a pre-allocated descriptor set with bindless texture array and vertex buffer, using push constants for per-draw variation.
