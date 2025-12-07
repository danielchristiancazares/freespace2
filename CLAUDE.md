# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Prerequisites: update submodules first
git submodule update --init --recursive

# Configure (basic)
cmake -S . -B build

# Configure with Vulkan and shader compilation
cmake -S . -B build -DFSO_BUILD_WITH_VULKAN=ON -DSHADERS_ENABLE_COMPILATION=ON

# Build
cmake --build build --config Release --parallel

# Windows helper script
powershell -ExecutionPolicy Bypass -File build.ps1

# Run tests
cmake -S . -B build -DFSO_BUILD_TESTS=ON
ctest --output-on-failure --test-dir build
```

## Key CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `FSO_BUILD_WITH_OPENGL` | ON | OpenGL renderer |
| `FSO_BUILD_WITH_VULKAN` | OFF | Vulkan renderer |
| `FSO_BUILD_TESTS` | OFF | Unit tests |
| `FSO_BUILD_FRED2` | ON (Win) | Mission editor |
| `FSO_BUILD_QTFRED` | OFF | Qt-based mission editor |
| `SHADERS_ENABLE_COMPILATION` | OFF | Compile shaders (requires glslc) |

## Project Structure

- `code/` - Core engine source code
  - `graphics/opengl/` - OpenGL renderer backend
  - `graphics/vulkan/` - Vulkan renderer backend (in development)
  - `graphics/shaders/` - GLSL shader sources; `compiled/` contains SPIR-V and generated headers
  - `graphics/util/` - Shared graphics utilities (UniformBufferManager, etc.)
  - `ai/`, `ship/`, `weapon/` - Gameplay systems
  - `scripting/` - Lua scripting integration
- `freespace2/` - Game executable entry point
- `fred2/`, `qtfred/` - Mission editor sources
- `lib/` - Third-party libraries (SDL, imgui, OpenXR, etc.)
- `cmake/` - Build system modules
- `test/` - Unit tests (gtest)

## Rendering Architecture

Two renderer backends share common interfaces in `code/graphics/`:
- **OpenGL** (`gropengl*`): Production renderer, uses legacy shader pipeline
- **Vulkan** (`gr_vulkan*`, `VulkanRenderer`): In development, targets Vulkan 1.4

### Vulkan Manager Classes

The Vulkan renderer uses a modular architecture with dedicated manager classes:
- `VulkanRenderer` - Core renderer orchestration and frame management
- `VulkanFrame` - Per-frame resources (command buffers, ring buffers, sync primitives)
- `VulkanBufferManager` - GPU buffer lifecycle (create, update, resize, map)
- `VulkanTextureManager` - Texture uploads via staging ring buffers, sampler caching
- `VulkanShaderManager` - Shader module loading (filesystem and embedded)
- `VulkanPipelineManager` - Pipeline creation and caching
- `VulkanDescriptorLayouts` - Descriptor set layout management
- `VulkanShaderReflection` - SPIR-V descriptor binding validation

### Graphics API

Common graphics operations are exposed through function pointers in `gr_screen` (see `2d.h`):
- `gr_create_buffer`, `gr_delete_buffer` - Buffer lifecycle
- `gr_update_buffer_data`, `gr_update_buffer_data_offset` - Buffer data upload
- `gr_resize_buffer` - Resize buffer without full recreation
- `gr_map_buffer`, `gr_flush_mapped_buffer` - Persistent mapping support

### Shader Compilation Flow

Shader compilation (`code/shaders.cmake`):
1. `glslc` compiles GLSL to SPIR-V (target: vulkan1.4 for Vulkan-only, vulkan1.2 for cross-backend)
2. `shadertool` post-processes SPIR-V to generate OpenGL GLSL and C++ uniform structs
3. Vulkan-only shaders skip GLSL generation (structs only)

## Coding Conventions

- C++17 throughout
- Follow `.clang-format` for style
- Types/classes: PascalCase; functions/locals: camelCase; constants: ALL_CAPS
- Prefer feature checks over platform `#ifdef`s

## Commit Style

Conventional commits with scope (e.g., `feat(vulkan): add texture manager`). For PRs, describe intent, testing performed (platform, backend), and link related issues.
