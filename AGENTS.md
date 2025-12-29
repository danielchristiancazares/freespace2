# Codex Guidelines

You will be working primarily on the Vulkan backend unless the user says otherwise.

## Agent Workflow

## Repository Map
- Rendering backends: `code/graphics/` (`vulkan/`, `opengl/`, etc.)
- Build entrypoints: `build.ps1`, `build.sh`
- Tests (sources): `test/` (unit tests)
- Test output: `Testing/` (CTest output; not source)
- Build output (default): `build/`
- Vulkan documentation: `docs/` (plus Vulkan backend docs under `code/graphics/vulkan/`)

## Vulkan Docs (Read Based on Change)
- Frame lifecycle/sync: `docs/VULKAN_FRAME_LIFECYCLE.md`, `docs/VULKAN_SYNCHRONIZATION.md`
- Render targets/dynamic rendering: `docs/VULKAN_RENDER_PASS_STRUCTURE.md`, `docs/VULKAN_POST_PROCESSING.md`
- Pipelines/shaders/layouts: `docs/VULKAN_PIPELINE_MANAGEMENT.md`, `docs/VULKAN_PIPELINE_USAGE.md`
- Descriptors/bindless/textures: `docs/VULKAN_DESCRIPTOR_SETS.md`, `docs/VULKAN_TEXTURE_BINDING.md`, `docs/VULKAN_TEXTURE_RESIDENCY.md`
- Buffers/uniforms/alignment: `docs/VULKAN_DYNAMIC_BUFFERS.md`, `docs/VULKAN_UNIFORM_BINDINGS.md`, `docs/VULKAN_UNIFORM_ALIGNMENT.md`
- Deferred lighting/G-buffer: `docs/VULKAN_DEFERRED_LIGHTING_FLOW.md`
- UI/HUD/2D rendering: `docs/VULKAN_2D_PIPELINE.md`, `docs/VULKAN_HUD_RENDERING.md`
- Device/swapchain init: `docs/VULKAN_DEVICE_INIT.md`, `docs/VULKAN_SWAPCHAIN.md`
- Testing/error/perf: `docs/VULKAN_INTEGRATION_TESTS.md`, `docs/VULKAN_ERROR_HANDLING.md`, `docs/VULKAN_PERFORMANCE_OPTIMIZATION.md`

## Build / Test Commands
### Prerequisites
- CMake + Ninja + C++ toolchain (VS on Windows, GCC/Clang on POSIX)
- Vulkan SDK if building Vulkan

### Windows (PowerShell)
```powershell
./build.ps1                     # Full build
./build.ps1 -Target Freespace2  # Main executable only
./build.ps1 -Target code        # Engine library only
./build.ps1 -Clean              # Clean rebuild
./build.ps1 -ConfigureOnly      # CMake configure only
./test.ps1 -Rebuild             # Build and run tests
```

### POSIX
```sh
./build.sh
cmake -S . -B build -G Ninja -DFSO_BUILD_WITH_VULKAN=ON -DSHADERS_ENABLE_COMPILATION=ON
cmake --build build --parallel
cmake --build build --target unittests
```

### Run Tests
```sh
./build/bin/unittests --gtest_filter="Vulkan*"     # Vulkan tests only
./build/bin/unittests                               # All tests
```

### Integration Tests (Require GPU)
```sh
export FS2_VULKAN_IT=1  # Windows: $env:FS2_VULKAN_IT="1"
./build/bin/unittests --gtest_filter="VulkanSubsystems.*"
```

## Coding Style
- Language: C++17 minimum (`cxx_std_17`) unless a subsystem states otherwise.
- Formatting: `.clang-format` is authoritative; run `clang-format` on files you touch (no permission needed).
- Do not reformat: `lib/` (third-party), `build/` (output), `Testing/` (output).
- Formatting rules (high level):
  - Default/root: `IndentWidth: 4`, `TabWidth: 4`, `UseTab: ForContinuationAndIndentation`, `ColumnLimit: 120`
  - Vulkan: `code/graphics/vulkan/` uses `IndentWidth: 2`, `UseTab: Never`, `ColumnLimit: 120`
- Naming:
  - Types: `PascalCase` (e.g. `RenderCtx`, `FrameCtx`)
  - Constants/macros: `ALL_CAPS`
  - Functions/variables: `snake_case` (legacy), `camelCase` (new Vulkan code)
  - Handles/indices: suffix with `...Handle`, `...Index`
- Headers: prefer forward declarations over includes; keep headers lean.

## Agent Workflow
- Ensure you adhere to `docs/DESIGN_PHILOSOPHY.md` before proposing fixes or architectural changes. Your proposals will get rejected if you don't.
- If you change or write new tests, rebuild and run them using `./test.ps1 -Rebuild`
- If you modify core engine or headers, do a full build: `./build.ps1`

### Tooling Preferences
- Prefer tools-mcp `Read` over `cat`/`Get-Content`.
- Prefer tools-mcp `Search` over `rg`/`grep`.
- Prefer tools-mcp `Glob` for finding files.
- Prefer tools-mcp `Edit` for modifications and `Write` for new files.
- Prefer `apply_patch`; use tools-mcp `Edit` as an alternative.
- Use tools-mcp `Outline` to quickly understand what’s in a file and where to jump next.
- If tools-mcp is unavailable, use `rg`/`rg --files`.

### Permission Boundaries
- Running `clang-format`: Do not ask; do it as needed.
- Building with `-Target code`: Do not ask; do it as needed.
- Running specific test filter: Do not ask; do it as needed.
- Full rebuild (`./build.ps1`): Do not ask; do it as needed.
- Running full test suite (./test.ps1 -Rebuild: Do not ask; do it as needed.
- Commit working: Ask
- Pushing changes: Do not ask; do it as needed.
- Destructive git commands (`git reset`, `git restore`, `git checkout`): ALWAYS ask

### Commit Expectations
- Messages: short, imperative summary with type and scope.
  - `feat(vulkan): implement ring buffer overflow handling`
  - `fix(bmpman): correct mipmap level calculation`
  - `refactor(render): extract depth attachment logic`
- Add detail lines for context as needed.
- Group related changes per commit.
