# Repository Guidelines

## Project Structure & Module Organization
- `code/` – Core engine sources (graphics, input, sound, gameplay); shaders under `code/graphics/shaders`; Vulkan path under `code/graphics/vulkan/`; OpenGL under `code/graphics/opengl/`.
- `freespace2/` – Game executable sources and resources.
- `lib/` – Third-party libraries (OpenGL/Vulkan tooling, SDL, imgui, etc.).
- `parsers/`, `tools/`, `scripts/` – Auxiliary utilities and build helpers.
- `test/` – GoogleTest-based unit and integration tests (see `test/src/graphics/` for renderer tests).
- `docs/` – Design notes and phase breakdowns (e.g., Vulkan renderer phases).
- `build/` – CMake build tree (out-of-source). Binaries land in `build/bin/<Config>/`. Keep sources and build dirs separate.
- Assets and game data live under the game install (e.g., `C:\Program Files (x86)\Steam\steamapps\common\Freespace 2`)

## Build, Test, and Development Commands
- Configure:
  - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` (add `-DSCP_RELEASE_LOGGING=ON` for release logs).
  - Vulkan + shader compilation example: `cmake -S . -B build -DFSO_BUILD_WITH_VULKAN=ON -DSHADERS_ENABLE_COMPILATION=ON`.
- Build:
  - `build.ps1`
  - `cmake --build build --config Release --parallel`
  - Windows helper with options: `./build.ps1 -Config Release -Vulkan true -ShaderCompilation true`
  - Build tests: `./build.ps1 -Config Debug -Target unittests` (script auto-enables `-DFSO_BUILD_TESTS=ON` when targeting `unittests`; use `-Tests true` to force it otherwise)
- Run (adjust suffix/config as generated): `build/bin/fs2_open_*`
- Tests:
  - Configure with `-DFSO_BUILD_TESTS=ON` (use `build.ps1 -Tests true` to set it).
  - Build the suite: `./build.ps1 -Config Debug -Target unittests` (auto-enables tests).
  - Run via CTest: `ctest -C Debug -R unittests --output-on-failure` (from `build/`; pass gtest filters after `--`).
  - Direct run example (PowerShell): `./build/bin/Debug/unittests.exe --gtest_filter=*VulkanPipelineDynamicState*`.

## Coding Style & Naming Conventions
- C++17 with project `.clang-format` and `.clang-tidy`; run `clang-format` on touched files.
- Prefer existing naming: `PascalCase` for types/classes, `camelCase` for functions/vars in C++; constants/macros ALL_CAPS.
- Logging via `mprintf`/`vk_logf`. New files should include header guards or `#pragma once`; keep ASCII.
- Keep platform-conditional code minimal; prefer feature checks over OS ifdefs.

## Vulkan Renderer Architecture

### Manager Classes
Use existing managers and helper APIs in `code/graphics/vulkan/` instead of ad hoc Vulkan calls:
- `VulkanRenderer` – Core renderer, device/swapchain management, frame orchestration
- `VulkanFrame` – Per-frame resources: command pool/buffer, ring buffers (uniform 512KB, vertex 1MB, staging 12MB), sync primitives
- `VulkanBufferManager` – Buffer lifecycle (create, update, resize, map, flush)
- `VulkanTextureManager` – GPU texture uploads via staging ring buffers, sampler caching, compressed format support
- `VulkanShaderManager` – Shader module loading from filesystem or embedded `def_files`
- `VulkanPipelineManager` – Pipeline creation, caching, dynamic state configuration
- `VulkanDescriptorLayouts` – Descriptor set layouts and pipeline layout management
- `VulkanShaderReflection` – SPIR-V reflection for descriptor binding validation

### Graphics API (cross-backend)
Common buffer operations exposed in `gr_screen` (`2d.h`):
- `gr_create_buffer`, `gr_delete_buffer` – Buffer lifecycle
- `gr_update_buffer_data`, `gr_update_buffer_data_offset` – Data upload
- `gr_resize_buffer` – Resize without full recreation (OpenGL/Vulkan/stub)
- `gr_map_buffer`, `gr_flush_mapped_buffer` – Persistent mapping

## Shader & Rendering Notes
- Toggle render backends via CMake: `FSO_BUILD_WITH_OPENGL`, `FSO_BUILD_WITH_VULKAN`.
- Vulkan shaders are compiled to SPIR-V; when `SHADERS_ENABLE_COMPILATION=ON`, `glslc` must be on PATH (or set `GLSLC_PATH`) and `shadertool` available. `SHADERTOOL_PATH` overrides detection. `SHADER_FORCE_PREBUILT` skips tool checks/compilation and uses prebuilts; `SHADER_DEBUG_INFO` gates `-g`.
- Vulkan-only shaders may skip GLSL generation; OpenGL uses its own shader set (`default-material.*`). Compiled shaders output to the build dir (`generated_shaders`); prebuilts live under `code/graphics/shaders/compiled` for fallback/OpenGL.
- Target API: Vulkan 1.4+. Required device extensions: `VK_KHR_swapchain`, `VK_KHR_push_descriptor`, `VK_KHR_maintenance5`; optional: maintenance6/EDS3/dynamic_rendering_local_read when available.
- Extended Dynamic State: EDS1 is core in Vulkan 1.3 (always available on 1.4 baseline). EDS2/EDS3 require extension/feature checks. Feature queries for all three are performed during device selection.
- Validation: use `VK_EXT_debug_utils` + `VK_LAYER_KHRONOS_validation` (debug builds). Initialize the HPP dispatcher with the device.
- Pipeline cache: loaded/saved as `vulkan_pipeline.cache` in the working directory; safe to delete if corrupted. Ideally key by device UUID/driver.
- Swapchain: handle OUT_OF_DATE/SUBOPTIMAL by recreating swapchain resources on resize/mode changes.
- Memory: prefer VMA for buffer/image allocation; keep OpenGL path unchanged.

## Testing Guidelines
- Favor test-first development for non-trivial work: write the expected-behavior test(s) up front, even if they initially fail, and drive the fixes until they pass.
- Use GoogleTest (see `test/src/graphics`). Prefer BDD-ish naming like `Scenario_Given_When_Then`; keep assertions focused and behavior-oriented.
- Enable `FSO_BUILD_TESTS` for unit tests; name tests after the feature under test.
- When touching Vulkan/graphics, add/adjust tests if possible and run `ctest` in the affected configuration. A temporarily failing build is acceptable while newly added tests are red.
- For renderer-sensitive changes, add a smoke scenario or log the configuration used (backend, GPU).
- For crashes/hangs, capture `vulkan_debug.log` and stdout/stderr from the built exe to verify fixes.

## Commit & Pull Request Guidelines
- Commits: conventional commit format with scope (e.g., `feat(vulkan): add texture manager`). Group related changes; avoid mixing style-only edits with logic changes.
- PRs: describe intent, reproduction steps, and validation (tests run, logs). Link issues/threads if applicable and include screenshots/log snippets for rendering or crash fixes.

## Agent-Specific Notes
- On session start, if a `CONTEXT.md` file exists at the repository root, read it fully before performing any other actions.
- Keep `CONTEXT.md` up to date for the areas it covers (currently the Vulkan backend):
  - When you change the architecture or behavior of systems described there (API version, dynamic rendering, key managers, etc.).
  - When you fix a non-trivial crash/bug or discover a pitfall that future LLM sessions should avoid (add to "Crash fix" or "Mistakes to avoid" sections).
  - When you add/remove important files, tools, or workflows that should be reflected in its "File map" or "Testing & debugging" sections.
- Do not revert user changes. Avoid destructive git commands. Log and test before handing off.
- When running `git show`/`git diff` in this repo, always pass `--no-pager` (or set `GIT_PAGER=cat`) so automated tooling is not blocked by the interactive pager/"spacebar to continue" prompt.

# **CRITICAL - NEVER SKIP**
**ALWAYS ADD DIAGNOSTICS OR FAILING TESTS BEFORE IMPLEMENTING FIXES**
**NEVER IMPLEMENT CODE CHANGES BEFORE CONFIRMING WITH THE USER**
