# AGENTS.md (Agent Guidelines)

## Read First (Always)
- Follow `docs/DESIGN_PHILOSOPHY.md` for architecture and "state as location" design.
- Before editing a subsystem, read its `README.md` (see "Subsystem READMEs" below).

## Repository Map
- Engine C/C++: `code/`
- Rendering backends: `code/graphics/` (`vulkan/`, `opengl/`, etc.)
- Build entrypoints: `build.ps1`, `build.sh`
- Build/config helpers: `scripts/`, `cmake/`, `ci/`, `tools/`
- Tests (sources): `test/` (unit tests)
- Test output: `Testing/` (CTest output; not source)
- Build output (default): `build/`
- Docs/design notes: `docs/` (plus Vulkan backend docs under `code/graphics/vulkan/`)

## Subsystem READMEs (Read Before Modifying)
- Design/Architecture: `docs/DESIGN_PHILOSOPHY.md`
- Memory/Types: `code/globalincs/README.md`
- Rendering API: `code/graphics/README.md`, `code/render/README.md`
- Vulkan backend: `code/graphics/vulkan/README.md`, `docs/VULKAN_CAPABILITY_TOKENS.md`
- Textures: `code/bmpman/README.md`
- Models: `code/model/README.md`
- AI: `code/ai/README.md`
- Physics: `code/physics/README.md`
- Missions: `code/mission/README.md`
- Game entities: `code/object/README.md`, `code/ship/README.md`
- Tables/Logic: `code/parse/README.md`
- Filesystem/IO: `code/cfile/README.md`

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
./build.ps1                              # Full build
./build.ps1 -Target Freespace2           # Main executable only
./build.ps1 -Target code                 # Engine library only
./build.ps1 -Target unittests -Tests     # Build tests
./build.ps1 -Clean                       # Clean rebuild
./build.ps1 -ConfigureOnly               # CMake configure only
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

### Build Decision Tree
1. Only `code/graphics/vulkan/` changed -> `./build.ps1 -Target code`
2. Shaders changed -> ensure `-DSHADERS_ENABLE_COMPILATION=ON` in configure
3. Tests changed -> `./build.ps1 -Target unittests -Tests`
4. Core engine/headers changed -> full build: `./build.ps1`

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

## Vulkan Backend Rules (`code/graphics/vulkan/`)
### Required Reading (Before Any Vulkan Change)
- `docs/DESIGN_PHILOSOPHY.md`
- `docs/VULKAN_CAPABILITY_TOKENS.md`
- `code/graphics/vulkan/README.md`

### Vulkan Version Requirements
- Minimum supported Vulkan: 1.4. No fallback paths for older versions.
  - Dynamic rendering (`VK_KHR_dynamic_rendering`) is core in 1.3
  - Synchronization2 (`pipelineBarrier2`) is core in 1.3
  - Push descriptors (`VK_KHR_push_descriptor`) is core in 1.4
- `supportsExtendedDynamicState{,2,3}` gates optional 1.4+ features (optimizations only), not version fallbacks.
- `supportsExtendedDynamicState()` / `supportsExtendedDynamicState2()` are legacy and always true on Vulkan 1.4.
  - Do not add conditionals on these.
- `supportsExtendedDynamicState3()` is actually optional; code paths must work without it.
- Shader build output mentioning `vulkan1.2` is normal.
  - Cross-backend shaders compile to SPIR-V with target env `vulkan1.2` for OpenGL GLSL translation compatibility.
  - Vulkan-only shaders compile to `vulkan1.4`. This is about shader toolchain compatibility, not the engine's runtime Vulkan minimum.
  - See `code/shaders.cmake` (`GLSLC_TARGET_ENV_COMPAT` vs `GLSLC_TARGET_ENV_VULKAN`).
- Extended Dynamic State (EDS) flags: treat EDS1/2 as legacy API surface; EDS3 is the only optional feature gate.
  - EDS3 use must be gated by `supportsExtendedDynamicState3()` plus the per-feature caps (e.g., `colorWriteMask`, `polygonMode`).
  - If you touch the EDS flags or their plumbing, keep docs/tests in sync with code:
    - `code/graphics/vulkan/VulkanDevice.h` exposes `supportsExtendedDynamicState{,2,3}()`.
    - `code/graphics/vulkan/VulkanDevice.cpp` currently only queries/sets EDS3 caps in `queryDeviceCapabilities(...)`.
    - `docs/VULKAN_DEVICE_INIT.md` documents the intended semantics.
### Render Pass & Target Ownership
- Dynamic rendering is owned by `VulkanRenderingSession`; do not call `vkCmdBeginRendering`/`vkCmdEndRendering` directly.
- All target switches go through `request*Target()` or deferred helpers; they end active passes automatically.
- After any target change or `suspendRendering()`, reacquire a new `RenderCtx` via `ensureRenderingStarted()`.
- Do not hold `RenderCtx` across target switches or frame boundaries; acquire close to the draw.

### Capability Tokens (Type Proves State)
Functions require proof tokens. No token, no operation.

| Token | Proves | Created By |
|-------|--------|------------|
| `RecordingFrame` | Frame is recording | `beginRecording()` |
| `FrameCtx` | Active recording + renderer | `currentFrameCtx()` |
| `RenderCtx` | Dynamic rendering is active | `ensureRenderingStarted()` |
| `UploadCtx` | Upload phase is active | Internal (frame start) |
| `DeferredGeometryCtx` | G-buffer pass active | `deferredLightingBegin()` |
| `DeferredLightingCtx` | Lighting pass active | `deferredLightingEnd()` |

Correct:
```cpp
void drawThing(const RenderCtx& ctx) {
    ctx.cmd.draw(3, 1, 0, 0); // ctx proves rendering is active
}
```

Wrong:
```cpp
void drawThing() {
    assert(g_renderPassActive); // runtime check; type should prove this
    vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
}
```

### Ring Buffers (Per-Frame Transients)
Per-frame transient data uses ring buffers. Never use `new`/`malloc` for per-frame resources.

```cpp
// Correct: use ring buffer
auto alloc = frame.uniformBuffer().allocate(size, alignment);
memcpy(alloc.mapped, data, size);

// Wrong: heap allocation in frame loop
auto* data = new UniformData(); // fragmentation, stalls
```

| Ring buffer | Size | Purpose |
|-------------|------|---------|
| `uniformBuffer()` | 512 KB | Per-draw uniforms |
| `vertexBuffer()` | 1 MB | Dynamic vertices |
| `stagingBuffer()` | 12 MB | Texture uploads |

### Layout Contracts
- `VulkanLayoutContracts.cpp` maps shader types to pipeline layouts.
- If you modify `VulkanDescriptorLayouts.cpp`, verify the corresponding entry in `getShaderLayoutSpec()`.

### Vulkan Style
- Prefer `vk::` (Vulkan-Hpp) types over raw `Vk*`.
- Prefer RAII handles (e.g. `vk::UniqueBuffer`) over manual cleanup.

### Pipeline/Descriptor Contracts
- Build `PipelineKey` from the current `RenderCtx.targetInfo` (formats, attachment count, sample count).
- Recompute `PipelineKey` after any target switch; do not reuse stale `RenderCtx.targetInfo`.
- For vertex-attribute shaders, always set `PipelineKey.layout_hash = vertex_layout.hash()`.
- Push descriptors are mandatory for Standard/Deferred layouts; do not add fallback paths.
- Bindless texture arrays are fully populated (fallback-filled); do not enable partially-bound descriptors.
- When changing shader types/layouts: update `VulkanLayoutContracts.cpp`, `VulkanDescriptorLayouts.cpp`, and relevant docs in `docs/`.

### Texture/Bindless Rules
- Use `TextureId::tryFromBaseFrame()`; treat invalid handles as absence (no sentinel values).
- Bindless slots 0-3 are reserved defaults; update `VulkanConstants.h` + `docs/VULKAN_TEXTURE_BINDING.md` if you change them.
- Draw-path code should go through `VulkanTextureBindings`; upload-path code through `VulkanTextureUploader`.
- Domain-invalid inputs live in `m_permanentlyRejected`; do not add retry loops.

### Buffer/Upload Rules
- Per-frame data uses ring buffers; never `new`/`malloc` in the frame loop.
- Ring buffers do not wrap; use `try_allocate()` when exhaustion is possible and respect alignment.
- Dynamic/Streaming buffers orphan on update; do not hand-roll sync.

### Threading
- Vulkan backend is single-threaded; tokens and managers are not thread-safe.

## Design Philosophy Enforcement
Ensure you adhere to `docs/DESIGN_PHILOSOPHY.md` before proposing fixes or architectural changes. Your proposals will get rejected if you don't.

Canonical example (state as location):
```cpp
// Wrong: state as data
struct Texture {
    enum State { Pending, Loaded, Failed } state;
    std::optional<GpuHandle> handle; // valid iff Loaded
};

// Correct: state as location
std::map<TextureId, UploadRequest> m_pending;
std::map<TextureId, GpuTexture> m_resident;
// absent = unknown, pending = uploading, resident = ready
```

## Agent Workflow (When Running Via Tools)
### Tooling Preferences
- Prefer tools-mcp `Read` over `cat`/`Get-Content`.
- Prefer tools-mcp `Search` over `rg`/`grep`.
- Prefer tools-mcp `Glob` for finding files.
- Prefer tools-mcp `Edit` for modifications and `Write` for new files.
- If tools-mcp is unavailable, use `rg`/`rg --files`.

### Permission Boundaries
| Action | Permission |
|--------|------------|
| Run `clang-format` | Do not ask |
| Build with `-Target code` | Do not ask |
| Run specific test filter | Do not ask |
| Full rebuild (`./build.ps1`) | Do not ask|
| Run full test suite (./test.ps1 -Rebuild | Do not ask |
| Destructive git commands (`reset`, `restore`) | ALWAYS ask |
| Push changes | Do not ask |

### Execution Decision Tree
1. Asked to "check" "review" or "verify"? -> Read-only investigation; do not patch.
2. Modifying Vulkan? -> Read `docs/VULKAN_CAPABILITY_TOKENS.md` first.
3. Proposing architecture? -> Read `docs/DESIGN_PHILOSOPHY.md` first.

## Commit Expectations
- Messages: short, imperative summary with type and scope.
  - `feat(vulkan): implement ring buffer overflow handling`
  - `fix(bmpman): correct mipmap level calculation`
  - `refactor(render): extract depth attachment logic`
- Add detail lines for context as needed.
- Group related changes per commit.
