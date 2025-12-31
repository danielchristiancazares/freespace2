# Codex Guidelines

## Agent Workflow
- Ensure you adhere to `docs/DESIGN_PHILOSOPHY.md` before proposing fixes or architectural changes. Your proposals will get rejected if you don't.
- If you change or write new tests, rebuild and run them using `./test.ps1 -Rebuild`
- If you modify core engine or headers, do a full build: `./build.ps1`

### Permission Boundaries
- Don't ask to run `./build.ps1` or `./test.ps1`, just do it.
- Running specific test filter: Don't ask. Run as needed.
- Full rebuild (`./build.ps1`): Don't ask. Run after finishing a code change.
- Running full test suite (./test.ps1 -Rebuild: Don't ask. Run it after finishing a task.
- Commits: Confirm with user first
- Pushing changes: Don't ask. Always do it after a commit.
- Destructive git commands (`git reset`, `git restore`, `git checkout`): ALWAYS ask

### tools-mcp Usage
- Prefer `mcp__tools-mcp__Read` over `cat`/`Get-Content`.
- Prefer `mcp__tools-mcp_Search` over `rg`/`grep`.
- Prefer `mcp__tools-mcp__Glob` for finding files.
- Prefer `mcp__tools-mcp__Edit` for modifications and `Write` for new files.
- Prefer `apply_patch`; use `mcp__tools-mcp__Edit` as an alternative.
- Use `mcp__tools-mcp__Outline` to quickly understand what’s in a file and where to jump next.
- If `mcp__tools-mcp` is unavailable, fall back to built in tooling and command line.

### RAG Tool Usage
- Prefer tools-mcp `Search` (ripgrep) when you have an exact token/string (symbol name, error text, build flag, `#define`, config key).
- Prefer `mcp__rag__search_code` when you *don't* have an exact token and need semantic/codebase-wide discovery (e.g., "where is swapchain recreated?", "similar descriptor update patterns", "who owns this VkImage?").
- Run `mcp__rag__index_repo` at the start of a session, when you expect cross-cutting code archaeology, or after big repo changes (branch switch/pull, mass renames/moves, large refactors) that could make search results stale.
- Re-run `mcp__rag__index_repo` when `mcp__rag__search_code` results are obviously missing/stale or still pointing at old paths.
- Keep RAG queries narrow: include subsystem/path hints ("deferred lighting", "post processing"), the object type (`VkImage`, `DescriptorSet`), and the behavior ("recreate on resize", "transition layout").
- Indexing has a built-in filter for just code files. Don't move files around to avoid indexing them, it's already handled by the tool.

### Commit Expectations
- Messages: short, imperative summary with type and scope.
  - `feat(vulkan): implement ring buffer overflow handling`
  - `fix(bmpman): correct mipmap level calculation`
  - `refactor(render): extract depth attachment logic`
- Group related changes per commit.

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
./build.ps1 -Clean              # Clean rebuild
./test.ps1 -Rebuild             # Build and run tests
```
## Coding Style
- Language: C++17 minimum (`cxx_std_17`) unless a subsystem states otherwise.
- Formatting: `./build.ps1` will automatically run `.clang-format` so don't waste time fixing indentation.
- Naming:
  - Types: `PascalCase` (e.g. `RenderCtx`, `FrameCtx`)
  - Constants/macros: `ALL_CAPS`
  - Functions/variables: `snake_case` (legacy), `camelCase` (new Vulkan code)
  - Handles/indices: suffix with `...Handle`, `...Index`
- Headers: prefer forward declarations over includes; keep headers lean.
