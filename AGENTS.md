# Repository Guidelines

- Adhere to the design philosophy at `docs/DESIGN_PHILOSOPHY.md`
- Read relevant `README.md` files before modifying subsystems

## Project Structure & Modules

- Core engine C++ lives in `code/`; rendering backends under `code/graphics` (e.g., `vulkan`, `opengl`), gameplay logic and subsystems alongside.
- Build entrypoints: `build.ps1`/`build.sh` in root. Utilities: `scripts/` (analysis, formatting), `cmake/` (toolchain helpers), `tools/` (asset binaries), `ci/` (pipeline configs).
- Tests reside in `test/` (unit tests). `Testing/` is CTest output, not test sources. Build output defaults to `build/`.
- Docs and design notes: `docs/` plus `code/graphics/vulkan/README.md` for the Vulkan backend.

### Vulkan Version Requirements

**Minimum: Vulkan 1.4. No fallback paths for older versions.**

Features assumed to be core (do not add extension checks):
- Dynamic rendering (`VK_KHR_dynamic_rendering`) — core in 1.3
- Synchronization2 (`pipelineBarrier2`) — core in 1.3
- Push descriptors (`VK_KHR_push_descriptor`) — core in 1.4

The `supportsExtendedDynamicState{,2,3}` flags gate **optional 1.4+ features**, not version fallbacks. If a feature is gated by these flags, the pipeline works without it — it's an optimization, not a requirement.

**Do not:**
- Investigate "what if runtime is 1.2/1.3" scenarios — it won't run
- Add fallback code paths for missing 1.4 core features
- Treat `supportsExtendedDynamicState` as indicating version compatibility

**Depth formats:** The engine uses `D32SfloatS8Uint` or `D24UnormS8Uint`. Do not add handling for `D16UnormS8Uint` unless hardware that only supports it actually exists and is a target.

### Extended Dynamic State Features

**EDS1 (Extended Dynamic State 1):** Core in Vulkan 1.3. Always available. Includes:
- `eCullMode`, `eFrontFace`, `ePrimitiveTopology`
- `eViewportWithCount`, `eScissorWithCount`
- `eVertexInputBindingStride`, `eDepthTestEnable`, `eDepthWriteEnable`, etc.

**EDS2 (Extended Dynamic State 2):** Core in Vulkan 1.3. Always available. Includes:
- `eRasterizerDiscardEnable`, `eDepthBiasEnable`, `ePrimitiveRestartEnable`

**EDS3 (Extended Dynamic State 3):** Optional. Gated by `supportsExtendedDynamicState3()`. Includes:
- `ePolygonMode`, `eRasterizationSamples`
- `eColorBlendEnable`, `eColorWriteMask`, `eColorBlendEquation`

**The flags mean:**
- `supportsExtendedDynamicState()` / `supportsExtendedDynamicState2()` — Legacy. These are always true on 1.4. Do not add conditionals.
- `supportsExtendedDynamicState3()` — Actually optional. Code paths must work without it.

**Do not:**
- Add conditionals around EDS1/EDS2 states — they're guaranteed
- Investigate "what if device doesn't support eCullMode" — it does
- Treat the existence of `m_supportsExtDyn` flags as evidence they might be false

### Subsystem Documentation (read before modifying)

| Subsystem | Documentation |
|-----------|---------------|
| Memory/Types | `code/globalincs/README.md` |
| Vulkan Backend | `code/graphics/vulkan/README.md`, `docs/VULKAN_CAPABILITY_TOKENS.md` |
| Rendering API | `code/graphics/README.md`, `code/render/README.md` |
| Textures | `code/bmpman/README.md` |
| Models | `code/model/README.md` |
| AI | `code/ai/README.md` |
| Physics | `code/physics/README.md` |
| Missions | `code/mission/README.md` |
| Game Entities | `code/object/README.md`, `code/ship/README.md` |
| Tables/Logic | `code/parse/README.md` |
| Filesystem/IO | `code/cfile/README.md` |

## Build, Test, and Development Commands

### Prerequisites
CMake + Ninja + C++ toolchain (VS on Windows, GCC/Clang on POSIX); Vulkan SDK if building Vulkan.

### Build Targets

| Target | Purpose |
|--------|---------|
| `Freespace2` | Main executable |
| `code` | Engine library only |
| `unittests` | Unit test executable |

### Commands

```bash
# Windows (PowerShell)
./build.ps1                              # Full build
./build.ps1 -Target Freespace2           # Main executable only
./build.ps1 -Target code                 # Engine library only
./build.ps1 -Target unittests -Tests     # Build tests
./build.ps1 -Clean                       # Clean rebuild
./build.ps1 -ConfigureOnly               # CMake configure only

# POSIX
./build.sh                               # Full build
cmake -S . -B build -G Ninja -DFSO_BUILD_WITH_VULKAN=ON -DSHADERS_ENABLE_COMPILATION=ON
cmake --build build --parallel
cmake --build build --target unittests

# Run tests
./build/bin/unittests --gtest_filter="Vulkan*"    # Vulkan tests only
./build/bin/unittests                              # All tests

# Integration tests (require GPU)
export FS2_VULKAN_IT=1  # or $env:FS2_VULKAN_IT="1" on Windows
./build/bin/unittests --gtest_filter="VulkanSubsystems.*"
```

### Build Decision Tree

1. Changed `code/graphics/vulkan/` only? → `./build.ps1 -Target code`
2. Changed shaders? → Ensure `-DSHADERS_ENABLE_COMPILATION=ON` in configure
3. Changed test files? → `./build.ps1 -Target unittests -Tests`
4. Changed core engine or headers? → Full build: `./build.ps1`
5. POSIX equivalents: `cmake --build build --target code` / `cmake --build build --target unittests` / `./build.sh`

## Coding Style & Naming

- C/C++: C++17 minimum (`cxx_std_17`); repo contains C and third-party code with their own standards.
- Formatting: `.clang-format` is authoritative. Run `clang-format` before commit. Do not ask permission.
  - Root: `IndentWidth: 4`, `TabWidth: 4`, `UseTab: ForContinuationAndIndentation`, `ColumnLimit: 120`
  - `code/graphics/vulkan/`: `IndentWidth: 2`, `TabWidth: 2`, `UseTab: Never`, `ColumnLimit: 120`
  - `lib/`: Do not reformat. Third-party code has its own configs.
  - Run `clang-format` only on files you touch; never format `lib/`, `build/`, or `Testing/`.
- Naming:
  - Types: `PascalCase` (`RenderCtx`, `FrameCtx`)
  - Constants/macros: `ALL_CAPS`
  - Functions/variables: `snake_case` (legacy), `camelCase` (new Vulkan code)
  - Handles/indices: suffix with `...Handle`, `...Index`
- Keep headers lean; prefer forward declarations over includes.

## Vulkan Backend Rules (`code/graphics/vulkan/`)

### Required Reading
Before modifying Vulkan code, read:
- `docs/DESIGN_PHILOSOPHY.md` — Type-driven design principles
- `docs/VULKAN_CAPABILITY_TOKENS.md` — Token system reference
- `code/graphics/vulkan/README.md` — Backend overview

### Capability Tokens

Functions require proof tokens. No token, no operation.

| Token | Proves | Created By |
|-------|--------|------------|
| `RecordingFrame` | Frame is recording | `beginRecording()` |
| `FrameCtx` | Active recording + renderer | `currentFrameCtx()` |
| `RenderCtx` | Dynamic rendering is active | `ensureRenderingStarted()` |
| `UploadCtx` | Upload phase is active | Internal (frame start) |
| `DeferredGeometryCtx` | G-buffer pass active | `deferredLightingBegin()` |
| `DeferredLightingCtx` | Lighting pass active | `deferredLightingEnd()` |

**Correct:**
```cpp
void drawThing(const RenderCtx& ctx) {
    ctx.cmd.draw(3, 1, 0, 0);  // ctx proves render pass is active
}
```

**Wrong:**
```cpp
void drawThing() {
    assert(g_renderPassActive);  // Runtime check — type should prove this
    vkCmdDraw(g_commandBuffer, 3, 1, 0, 0);
}
```

### Ring Buffers

Per-frame transient data uses ring buffers. Never use `new`/`malloc` for per-frame resources.

```cpp
// Correct: use ring buffer
auto alloc = frame.uniformBuffer().allocate(size, alignment);
memcpy(alloc.mapped, data, size);

// Wrong: heap allocation in frame loop
auto* data = new UniformData();  // Fragmentation, stalls
```

| Ring Buffer | Size | Purpose |
|-------------|------|---------|
| `uniformBuffer()` | 512 KB | Per-draw uniforms |
| `vertexBuffer()` | 1 MB | Dynamic vertices |
| `stagingBuffer()` | 12 MB | Texture uploads |

### Layout Contracts

`VulkanLayoutContracts.cpp` maps shader types to pipeline layouts. If you modify `VulkanDescriptorLayouts.cpp`, verify the corresponding entry in `getShaderLayoutSpec()`.

### Vulkan Style

- Use `vk::` namespace (Vulkan-Hpp), not C-style `Vk*` types
- Use RAII handles (`vk::UniqueBuffer`), not raw handles with manual cleanup

## Design Philosophy Enforcement

Read `docs/DESIGN_PHILOSOPHY.md` before proposing architectural changes.

### Red Flags — Stop and Propose Architecture Instead

| If your fix... | Then... |
|----------------|---------|
| Adds a retry loop | Ownership is fragmented — restructure |
| Adds a guard for state that "shouldn't happen" | State is representable but invalid — make it unconstructable |
| Adds `bool isInitialized` or lifecycle flag | Object exists before it's valid — fix construction |
| Adds enum variant `Failed`/`Error`/`Uninitialized` | Failure is presence, not absence — use container membership |
| Adds `switch` on state enum to route around invalid cases | Protecting invalid state from being noticed — eliminate the state |

### Decision Procedures

- About to add a `bool` field? → Could this be container membership instead?
- About to add `std::optional<T>`? → Is this domain-optional (real absence) or state-optional (lifecycle stage)?
- About to add a `switch` on state? → Could the cases be separate types?

### Canonical Example

**Wrong (state as data):**
```cpp
struct Texture {
    enum State { Pending, Loaded, Failed } state;
    std::optional<GpuHandle> handle;  // valid iff Loaded
};
```

**Correct (state as location):**
```cpp
std::map<TextureId, UploadRequest> m_pending;
std::map<TextureId, GpuTexture> m_resident;
// absent = unknown, pending = uploading, resident = ready
```

## Agent Tooling

When tools-mcp is available:
- Prefer `Read` over `cat`/`head`/`tail`/`Get-Content`
- Prefer `Search` over `grep`/`rg`
- Prefer `Glob` for finding files
- Prefer `Edit` for modifications
- Prefer `Write` for creating files
- Use `Search` for exact matches (e.g., finding all calls to `foo()`)
If tools-mcp is not available, prefer `rg`/`rg --files` for searching and file discovery.

## Agent Workflow

### Permission Boundaries

| Action | Permission |
|--------|------------|
| Run `clang-format` | Do not ask |
| Build with `-Target code` | Do not ask |
| Run specific test filter | Do not ask |
| Full rebuild (`./build.ps1`) | Ask first if build is slow |
| Run full test suite | Ask first |
| Destructive git commands (`reset`, `restore`) | Ask first |
| Push changes | Ask first |

### Decision Tree

1. Asked to "check" or "verify"? → Read-only investigation. Do not patch.
2. Modifying Vulkan code? → Read `docs/VULKAN_CAPABILITY_TOKENS.md` first.
3. Proposing architectural change? → Read `docs/DESIGN_PHILOSOPHY.md` first.
4. Unsure about call order or frame timing? → Cite file/line anchors (`path/to/file.cpp:123`).
5. Task complete? → Verify relevant targets build when feasible; do not push unless requested.

### Debugging Vulkan Issues

1. Check mundane causes first: uninitialized state, missing init calls, wrong frame index
2. Verify token flow: does the function have the token it needs?
3. Check RenderDoc if draw calls are involved
4. Single-variable hypotheses — change one thing at a time

### Anti-Patterns

| Don't | Why |
|-------|-----|
| Run full workspace build when only one target changed | Slow; use `-Target` |
| Reformat files in `lib/` | Third-party code |
| Use `vkDeviceWaitIdle` in render loop | Kills parallelism; use frame fence |
| Add guards/asserts for "impossible" state | Guards protect invalid state from being noticed |

## Commit Expectations

- Messages: short, imperative summary with type and scope
  - `feat(vulkan): implement ring buffer overflow handling`
  - `fix(bmpman): correct mipmap level calculation`
  - `refactor(render): extract depth attachment logic`
- Add detail lines for context as needed
- Group related changes per commit
```
