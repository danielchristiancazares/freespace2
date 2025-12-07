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

Shader compilation flow (`code/shaders.cmake`):
1. `glslc` compiles GLSL to SPIR-V (target: vulkan1.4 for Vulkan-only, vulkan1.2 for cross-backend)
2. `shadertool` post-processes SPIR-V to generate OpenGL GLSL and C++ uniform structs
3. Vulkan-only shaders skip GLSL generation (structs only)

## Coding Conventions

- C++17 throughout
- Follow `.clang-format` for style
- Types/classes: PascalCase; functions/locals: camelCase; constants: ALL_CAPS
- Prefer feature checks over platform `#ifdef`s

## Commit Style

Short imperative subject (e.g., "Fix Vulkan surface creation"). For PRs, describe intent, testing performed (platform, backend), and link related issues.
