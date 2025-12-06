# Repository Guidelines

## Project Structure & Modules
- `code/`: Core engine sources (renderers, gameplay, platform glue); shaders live in `code/graphics/shaders`.
- `freespace2/`: Game executable sources and resources.
- `lib/`: Third-party libraries (OpenGL/Vulkan tooling, SDL, imgui, etc.).
- `parsers/`, `tools/`, `scripts/`: Auxiliary utilities and build helpers.
- `build/` (user-created): Out-of-tree build output; keep sources and build dirs separate.

## Build, Test, and Development Commands
- Configure (example, Vulkan + shader compilation on Windows/macOS/Linux):
  - `cmake -S . -B build -DFSO_BUILD_WITH_VULKAN=ON -DSHADERS_ENABLE_COMPILATION=ON`
- Build:
  - `cmake --build build --config Release --parallel`
  - Windows helper: `pwsh ./build.ps1 -Config Release -Vulkan true -ShaderCompilation true`
- Run (adjust suffix/config as generated): `build/bin/fs2_open_*`
- Tests:
  - `cmake -S . -B build -DFSO_BUILD_TESTS=ON`
  - `cmake --build build --target test` or `ctest --output-on-failure --test-dir build`

## Coding Style & Naming
- C++17 throughout; follow `.clang-format` and `.clang-tidy` (run `clang-format` on touched files).
- Prefer descriptive names; types/classes in PascalCase, functions/locals in camelCase, constants/macros ALL_CAPS.
- Keep platform-conditional code minimal; prefer feature checks over OS ifdefs.

## Shader & Rendering Notes
- Vulkan shaders are compiled to SPIR-V; when `SHADERS_ENABLE_COMPILATION=ON`, `glslc` must be on PATH (or set `GLSLC_PATH`) and `shadertool` available.
- Vulkan-only shaders may skip GLSL generation; OpenGL uses its own shader set (`default-material.*`).
- Toggle render backends via CMake: `FSO_BUILD_WITH_OPENGL`, `FSO_BUILD_WITH_VULKAN`.

## Testing Guidelines
- Enable `FSO_BUILD_TESTS` for unit tests; name tests after the feature under test.
- For renderer-sensitive changes, add a smoke scenario or log the configuration used (backend, GPU).

## Commit & Pull Request Guidelines
- Commits: short imperative subject ("Fix Vulkan surface creation"), scope small and coherent.
- PRs: describe intent, testing performed (platform, backend, config flags), and any assets or tools needed; link issues when applicable. Include screenshots/log snippets for rendering/UI changes.

## Vulkan-Specific Notes
- Target API: Vulkan 1.4. Required device extensions: `VK_KHR_swapchain`, `VK_KHR_push_descriptor`, `VK_KHR_maintenance5`; optional: maintenance6/EDS3/dynamic_rendering_local_read when available.
- Validation: use `VK_EXT_debug_utils` + `VK_LAYER_KHRONOS_validation` (debug builds). Initialize the HPP dispatcher with the device.
- Pipeline cache: loaded/saved as `vulkan_pipeline.cache` in the working directory; safe to delete if corrupted. Ideally key by device UUID/driver.
- Swapchain: handle OUT_OF_DATE/SUBOPTIMAL by recreating swapchain resources on resize/mode changes.
- Memory: prefer VMA for buffer/image allocation; keep OpenGL path unchanged.
