# CLAUDE.md

Guidance for Claude Code when working in this repo.

## Read This First
- `CONTEXT.md` (Vulkan status, pitfalls, failed attempts, logging)
- `AGENTS.md` (repo rules and workflows)
- Respect existing uncommitted changes; do not revert user work.
- Add diagnostics before fixes; confirm intent with the user before large code changes.

## Repo Snapshot
- Project: FreeSpace 2 Open fork, version **26.0.0**
- Binary: `fs2_26_0_0.exe`
- Version override: `version_override.cmake` sets 26.0.0
- Key fork tweaks: joystick support removed; default graphics = TAA, SSAO Ultra, auto-exposure on.

## Non-Negotiables
- Run `git submodule update --init --recursive` before any build.
- Keep files ASCII; follow existing brace/format style.
- Use cfile VFS APIs—avoid direct file I/O.
- When touching Vulkan, cross-check `CONTEXT.md` and related docs to avoid repeating failed attempts.

## Build Quickstart
**Windows (default Debug, Vulkan on):**
```powershell
.\build.ps1
```
Common variants: `-Config Release`, `-Vulkan false`, `-Target unittests`, `-Config Release -Vulkan false -Clean false`.

**Linux/macOS:**
```bash
mkdir build && cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DFSO_BUILD_TESTS=ON -DFSO_BUILD_INCLUDED_LIBS=ON ..
ninja -k 20 all
./bin/unittests --gtest_shuffle
```

Manual MSVC configure (examples):
```powershell
cmake -G "Visual Studio 17 2022" -A x64 -DFSO_BUILD_TESTS=ON -DFSO_USE_SPEECH=ON -DFSO_USE_VOICEREC=ON ..
cmake --build . --config Release --target INSTALL
```

Important CMake options: `FSO_BUILD_TESTS`, `FSO_BUILD_FRED2`, `FSO_BUILD_QTFRED`, `FSO_BUILD_TOOLS`, `FSO_BUILD_WITH_FFMPEG`, `FSO_BUILD_WITH_DISCORD`, `FSO_BUILD_WITH_VULKAN`, `FSO_BUILD_WITH_OPENXR`, `FSO_FATAL_WARNINGS`, `FSO_BUILD_APPIMAGE`.

## Test Quickstart
From `build/`:
```bash
./bin/unittests
./bin/unittests --gtest_shuffle
```
Valgrind (Linux Debug): `valgrind --leak-check=full ./bin/unittests --gtest_shuffle`.
Test data: `test/test_data/`.

## Tooling
- Format: `clang-format -i <files>`
- Static analysis: `clang-tidy` (CI treats warnings as errors)
- Prefer `ninja -k 20 all` to surface full error sets.

## Vulkan (active development)
- Dynamic rendering (no VkFramebuffer objects); uses `vkCmdBeginRendering`.
- Pipelines set `renderPass = nullptr` with formats via `pNext`.
- Key classes: `VulkanRenderer`, `VulkanTextureManager`, `VulkanDescriptorManager`, `VulkanPipelineManager`, `VulkanBufferManager`.
- See `CONTEXT.md` for active bugs (scene pass state, red tint) and failed attempts; avoid repeating them.
- For shader defaults: keep `default-material.frag` using `sampler2D` (see CONTEXT “Mistakes to avoid”).

## High-Risk / Large Files
- `code/ai/aicode.cpp`, `code/parse/sexp.cpp`, `code/weapon/weapons.cpp`, `code/ship/ship.cpp`, `code/network/multimsgs.cpp`
- Changes here are wide-impact; minimize churn and add tests.

## Architecture Snapshot
- Directories: `code/` (engine), `freespace2/` (exe entry), `lib/` (third-party), `fred2/`, `wxfred2/`, `qtfred/`, `tools/`, `test/`, `ci/`.
- Core patterns: subsystem init/close/do_frame/reset; object manager factory; table-driven data; event-driven gamestate; Lua-first extensibility.
- Typical frame: state processing → object/AI/physics → collisions → HUD → render_frame → sound → present.

## Workflows
- Pre-commit: Release build with warnings-as-errors, run tests, format, run clang-tidy diff.
- For new subsystems/features, add table parsing + tests; for SEXP edits, be cautious (mission logic is brittle).
- Lua APIs live in `scripting/api/`; add hooks in `scripting/hook_api.h`.

## Platform Notes
- Windows: Visual Studio 2022; Vulkan enabled by default; speech/voicerec unavailable.
- macOS: Vulkan off by default (OpenGL); no OpenXR.
- Linux: SDL2/OpenAL; AppImage via `FSO_BUILD_APPIMAGE`.

## Logs & Paths
- Main log: `%APPDATA%\HardLightProductions\FreeSpaceOpen\data\fs2_open.log`
- Vulkan validation: `vulkan_debug.log` (game directory)
- HDR debug: `vulkan_hdr_debug.txt`

## Additional Resources
- Wiki: https://github.com/scp-fs2open/fs2open.github.com/wiki/Building
- Codebase size: ~449k C++ LOC across 79 subsystems
- Supported platforms: Windows, macOS, Linux, FreeBSD, Solaris
