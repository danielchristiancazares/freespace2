# Vulkan Integration Tests

This document describes the test suites for the Vulkan renderer subsystems. The tests validate behavioral invariants using lightweight "Fake" state machines and, optionally, real GPU integration tests.

---

## Table of Contents

1. [Overview](#overview)
2. [Prerequisites](#prerequisites)
3. [Test Categories](#test-categories)
4. [Testing Philosophy](#testing-philosophy)
5. [Fake State Machine Pattern](#fake-state-machine-pattern)
6. [Unit Test Suites](#unit-test-suites)
   - [Depth Attachment Switching](#depth-attachment-switching)
   - [Post-Effects Semantics](#post-effects-semantics)
   - [Texture Render Targets](#texture-render-targets)
7. [Additional Unit Tests](#additional-unit-tests)
8. [GPU Integration Tests](#gpu-integration-tests)
9. [Relationship to Production Code](#relationship-to-production-code)
10. [Running the Tests](#running-the-tests)
11. [Adding New Tests](#adding-new-tests)
12. [Troubleshooting](#troubleshooting)
13. [Design Rationale](#design-rationale)
14. [Related Documentation](#related-documentation)

---

## Overview

The Vulkan test suite provides comprehensive coverage of the renderer subsystems through two distinct testing approaches:

| Test Type | File Prefix | GPU Required | Purpose |
|-----------|-------------|--------------|---------|
| Unit Tests | `test_vulkan_*.cpp` | No | Validate state machine logic using Fake classes |
| Integration Tests | `it_vulkan_*.cpp` | Yes | Validate real GPU resource management |

Unit tests run on any machine (including CI without graphics hardware) and execute in milliseconds. Integration tests require a Vulkan-capable device and validate actual GPU behavior.

---

## Prerequisites

**Build Requirements:**

- CMake 3.14 or later
- C++17 compatible compiler
- GoogleTest (included as submodule)
- Vulkan SDK (for `FSO_BUILD_WITH_VULKAN=ON`)

**Build Configuration:**

Vulkan tests are conditionally compiled. Enable with:

```bash
cmake -DFSO_BUILD_WITH_VULKAN=ON ..
```

The test executable links against:
- `gtest` - GoogleTest framework
- `code` - Main FreeSpace2 code library
- `Vulkan::Vulkan` - Vulkan SDK (when enabled)

**Runtime Requirements (Integration Tests Only):**

- Vulkan-capable GPU with driver installed
- Set `FS2_VULKAN_IT=1` environment variable to enable GPU tests

---

## Test Categories

### Unit Tests (Fake State Machines)

Files matching `test_vulkan_*.cpp` use the Fake State Machine pattern. These tests:

- Run without a GPU
- Execute in milliseconds
- Validate behavioral invariants
- Use simplified C++ classes that mirror production logic

### GPU Integration Tests

Files matching `it_vulkan_*.cpp` use real Vulkan resources. These tests:

- Require a Vulkan device
- Create actual GPU resources (buffers, textures, pipelines)
- Validate driver-specific behavior
- Run with Vulkan validation layers enabled

---

## Testing Philosophy

The Vulkan unit tests focus on **behavioral invariants** rather than implementation details. Each test suite:

1. **Isolates a specific subsystem** - Tests one logical concern without dependencies on the full Vulkan stack
2. **Uses Fake state machines** - Lightweight C++ classes that replicate production state transitions
3. **Validates observable behavior** - Tests what the system does, not how it does it
4. **Documents invariants** - Test names and assertions serve as executable specifications

This approach enables:
- Fast test execution (no GPU required)
- Clear failure diagnostics
- Documentation of expected behavior
- Safe refactoring with confidence

---

## Fake State Machine Pattern

Each unit test file defines a `Fake*` class that mirrors the relevant portion of a production component:

```cpp
// Example pattern from test_vulkan_depth_attachment_switch.cpp
class FakeDepthAttachmentSession {
  public:
    void beginFrame();              // Mirrors VulkanRenderingSession::beginFrame()
    void useMainDepthAttachment();  // Mirrors production depth selection
    void ensureRendering();         // Mirrors ensureRendering()

    // Observability methods for assertions
    bool renderingActive() const;
    DepthAttachment selectedDepthAttachment() const;
    int passStartCount() const;

  private:
    // State that mirrors production implementation
    DepthAttachment m_depthAttachment;
    bool m_activePass;
    int m_passStartCount;
};
```

**Key Characteristics:**

| Aspect | Description |
|--------|-------------|
| No Vulkan calls | Pure C++ state transitions |
| Simplified types | Enums and structs instead of `vk::` types |
| Observable state | Public accessors for test assertions |
| Production logic | Core state machine logic replicated from source |

---

## Unit Test Suites

### Depth Attachment Switching

**File:** `test/src/graphics/test_vulkan_depth_attachment_switch.cpp`

**Purpose:** Validates depth attachment switching in `VulkanRenderingSession`. The session supports two depth attachments for OpenGL post-processing parity:

| Attachment | Content | Usage |
|------------|---------|-------|
| Main depth | Scene depth (ships, weapons, effects) | Standard 3D rendering |
| Cockpit depth | Cockpit-only depth | Cockpit effects depth-tested against cockpit geometry |

**Architectural Invariant:**
> Depth attachment selection must end any active render pass (attachment change) and subsequent `ensureRendering()` must use the newly selected depth attachment.

This mirrors the OpenGL pattern where `gr_zbuffer_save()` and `gr_zbuffer_restore()` manage separate depth buffers for cockpit rendering.

#### Test Cases

| Test Name | Validates |
|-----------|-----------|
| `FrameStart_SelectsMainDepth` | Frame initialization defaults to main depth |
| `SwitchToSame_IsNoop` | Switching to already-selected attachment does not end pass |
| `SwitchDifferentAttachment_EndsPass` | Changing depth attachment ends active render pass |
| `EnsureRendering_UsesSelectedAttachment` | New pass uses the currently selected attachment |
| `CockpitWorkflow_FullSequence` | Complete save/draw/restore workflow |
| `MultipleSwitches_TrackCorrectly` | Multiple switches within a frame track correctly |
| `FrameBoundary_ResetsToMain` | New frame resets to main depth attachment |
| `PassCount_WithDepthSwitching` | Each switch and re-start creates a new pass |

#### Cockpit Workflow Sequence

The full cockpit rendering workflow tested by `CockpitWorkflow_FullSequence`:

```
1. Render scene with main depth
   |-- useMainDepthAttachment()
   |-- ensureRendering()
   +-- [draw scene geometry]

2. Save zbuffer (copies main -> cockpit)
   |-- saveZBuffer()
   +-- (pass ends for transfer)

3. Clear main depth and render cockpit
   |-- useMainDepthAttachment()
   |-- ensureRendering()
   +-- [draw cockpit geometry]

4. Switch to cockpit depth for effects
   |-- useCockpitDepthAttachment()
   |-- ensureRendering()
   +-- [draw cockpit effects depth-tested against cockpit-only depth]

5. Restore zbuffer
   +-- restoreZBuffer()
```

---

### Post-Effects Semantics

**File:** `test/src/graphics/test_vulkan_post_effects_semantics.cpp`

**Purpose:** Validates post-effects processing logic from `VulkanRenderer::endSceneTexture()`. Tests ensure correct effect enablement semantics and identity defaults.

**Architectural Invariant:**
> An effect is enabled if `(always_on || intensity != default_intensity)`. If no effects are enabled, identity defaults are used.

This maintains parity with the OpenGL post-processing pipeline.

#### Effect Enablement Rules

| Condition | Result |
|-----------|--------|
| `always_on = true` | Effect is enabled regardless of intensity |
| `intensity != default_intensity` | Effect is enabled |
| `intensity == default_intensity` AND `always_on = false` | Effect is disabled |

#### Identity Defaults

When no effects are enabled, the following identity values are applied:

```cpp
post.saturation = 1.0f;   // No saturation change
post.brightness = 1.0f;   // No brightness change
post.contrast = 1.0f;     // No contrast change
post.film_grain = 0.0f;   // No film grain
post.tv_stripes = 0.0f;   // No TV stripes
post.cutoff = 0.0f;       // No cutoff
post.dither = 0.0f;       // No dither
post.noise_amount = 0.0f; // No noise
post.tint = {0, 0, 0};    // No tint
```

#### Supported Effect Types

| Uniform Type | Data Type | Notes |
|--------------|-----------|-------|
| `NoiseAmount` | float | Static noise overlay |
| `Saturation` | float | Color saturation multiplier |
| `Brightness` | float | Brightness multiplier |
| `Contrast` | float | Contrast adjustment |
| `FilmGrain` | float | Animated film grain effect |
| `TvStripes` | float | CRT scanline effect |
| `Cutoff` | float | Brightness cutoff threshold |
| `Dither` | float | Dithering amount |
| `Tint` | RGB | Color tint overlay |
| `CustomEffectVEC3A` | RGB | Mission script custom effect |
| `CustomEffectFloatA` | float | Mission script custom effect |
| `CustomEffectVEC3B` | RGB | Mission script custom effect |
| `CustomEffectFloatB` | float | Mission script custom effect |

#### Test Cases

| Test Name | Validates |
|-----------|-----------|
| `NoEffects_IdentityDefaults` | Empty effect list preserves identity values |
| `EffectAtDefaultIntensity_NotActive` | Effect with intensity == default is disabled |
| `EffectIntensityDiffers_IsActive` | Effect with intensity != default is enabled |
| `AlwaysOnEffect_ActiveRegardless` | always_on flag overrides intensity check |
| `MixedEffects_OnlyEnabledApplied` | Only enabled effects modify post_data |
| `AllEffectTypes_Processed` | All uniform types are handled correctly |
| `InvalidEffectType_Ignored` | Invalid type does not crash, sets doPostEffects |
| `EffectOrder_LastWriteWins` | Later effects override earlier for same type |
| `FloatComparison_ExactEquality` | Uses exact != comparison (not epsilon) |
| `TintRgb_AppliedCorrectly` | RGB values copied to all three channels |

---

### Texture Render Targets

**File:** `test/src/graphics/test_vulkan_texture_render_target.cpp`

**Purpose:** Validates bitmap render target (RTT) operations in `VulkanTextureManager`. Render targets enable features like environment mapping, dynamic cockpit displays, and procedural textures.

**Architectural Invariant:**
> Render target creation must register the target for the given bmpman base-frame handle, track extent/format/mip levels correctly, provide valid attachment views for rendering, and support layout transitions between attachment and shader-read states.

#### Render Target Record Structure

```cpp
struct RenderTargetRecord {
    uint32_t width;           // Texture width
    uint32_t height;          // Texture height
    uint32_t format;          // Vulkan format (e.g., R8G8B8A8Unorm)
    uint32_t mipLevels;       // Mipmap chain length
    uint32_t layers;          // 1 for 2D, 6 for cubemap
    bool isCubemap;           // Cubemap flag
    bool faceViewsValid[6];   // Per-face attachment view validity
};
```

#### Image Layout States

| Layout | Usage |
|--------|-------|
| `Undefined` | Initial state / deleted |
| `ColorAttachment` | Active rendering target |
| `ShaderReadOnly` | Sampling in shaders |

#### Mipmap Level Calculation

Mip levels are calculated based on the maximum dimension:

```
mipLevels = floor(log2(max(width, height))) + 1
```

| Dimensions | Mip Levels |
|------------|------------|
| 1x1 | 1 |
| 2x2 | 2 |
| 256x256 | 9 |
| 512x512 | 10 |
| 300x200 | 9 (max=300) |

The `kFlagNoMipmaps` flag forces a single mip level.

#### Cubemap Support

Cubemap render targets (`kFlagCubemap`) create 6 layers with valid attachment views for each face:

| Face Index | Direction |
|------------|-----------|
| 0 | +X (Right) |
| 1 | -X (Left) |
| 2 | +Y (Up) |
| 3 | -Y (Down) |
| 4 | +Z (Front) |
| 5 | -Z (Back) |

#### Test Cases

| Test Name | Validates |
|-----------|-----------|
| `Create_Basic2D` | Basic 2D render target creation |
| `Create_Cubemap` | Cubemap with 6 valid face views |
| `Create_NoMipmaps` | kFlagNoMipmaps results in single mip |
| `Create_ZeroSize_Rejected` | Zero width/height rejected |
| `HasRenderTarget_NotFound` | Non-existent handle returns false |
| `AttachmentView_InvalidFace` | Invalid face indices rejected |
| `LayoutTransition_AttachmentToShaderRead` | Layout transitions work correctly |
| `LayoutTransition_NonExistent` | Transitions on non-existent targets fail |
| `Delete_RemovesTarget` | Deletion removes target and resets layout |
| `MultipleTargets_Independent` | Multiple targets do not interfere |
| `MipLevelCalc_PowerOfTwo` | Correct mip levels for power-of-two sizes |
| `MipLevelCalc_NonPowerOfTwo` | Correct mip levels for non-power-of-two |
| `TransitionCount_Tracked` | Layout transitions are counted |
| `FaceView_2D_OnlyFace0Valid` | 2D targets only have face 0 valid |

---

## Additional Unit Tests

The test suite includes many additional unit tests beyond the three documented in detail above. These are organized by subsystem:

### Pipeline and Shader Tests

| File | Purpose |
|------|---------|
| `test_vulkan_pipeline_manager.cpp` | Vertex layout conversion, pipeline key generation |
| `test_vulkan_shader_manager_model.cpp` | Shader compilation and caching model |
| `test_vulkan_shader_alignment.cpp` | Uniform buffer std140 alignment |
| `test_vulkan_shader_layout_contracts.cpp` | Descriptor set layout contracts |
| `test_vulkan_descriptor_layouts.cpp` | Descriptor pool and set allocation |

### Buffer Management Tests

| File | Purpose |
|------|---------|
| `test_vulkan_buffer_manager_retirement.cpp` | Buffer retirement and reuse |
| `test_vulkan_buffer_zero_size.cpp` | Zero-size buffer allocation handling |
| `test_vulkan_dynstate.cpp` | Dynamic state management |

### Texture Tests

| File | Purpose |
|------|---------|
| `test_vulkan_texture_contract.cpp` | Texture lifecycle contracts |
| `test_vulkan_texture_helpers.cpp` | Texture format conversion utilities |
| `test_vulkan_texture_upload_alignment.cpp` | Texture upload row alignment |
| `test_vulkan_fallback_texture.cpp` | Missing texture fallback behavior |

### Frame Lifecycle Tests

| File | Purpose |
|------|---------|
| `test_vulkan_frame_lifecycle.cpp` | Frame begin/end state machine |
| `test_vulkan_recordingframe_sealed.cpp` | Command buffer sealing |
| `test_vulkan_scene_texture_lifecycle.cpp` | Scene texture acquire/release |
| `test_vulkan_swapchain_acquire.cpp` | Swapchain image acquisition retry logic |

### Rendering State Tests

| File | Purpose |
|------|---------|
| `test_vulkan_clip_scissor.cpp` | Clip rectangle and scissor state |
| `test_vulkan_blend_enable.cpp` | Blend state management |
| `test_vulkan_render_target_state.cpp` | Render target state tracking |
| `test_vulkan_clear_ops_oneshot.cpp` | One-shot clear operation semantics |

### Device and Initialization Tests

| File | Purpose |
|------|---------|
| `test_vulkan_device_scoring.cpp` | GPU device selection scoring |
| `test_vulkan_depth_format_selection.cpp` | Depth buffer format selection |
| `test_vulkan_renderer_shutdown.cpp` | Renderer cleanup on shutdown |
| `test_vulkan_deferred_release.cpp` | Deferred GPU resource release |

### Post-Processing Tests

| File | Purpose |
|------|---------|
| `test_vulkan_post_process_targets.cpp` | Post-processing render target management |
| `test_vulkan_perdraw_bindings.cpp` | Per-draw uniform bindings |

### Specialized Feature Tests

| File | Purpose |
|------|---------|
| `test_vulkan_decal_instancing.cpp` | Instanced decal rendering with matrix4 transforms |

---

## GPU Integration Tests

Integration tests validate behavior with real Vulkan resources. These require a GPU and are disabled by default.

### Enabling Integration Tests

Set the environment variable before running:

```bash
# Linux/macOS
export FS2_VULKAN_IT=1

# Windows (PowerShell)
$env:FS2_VULKAN_IT = "1"

# Windows (cmd)
set FS2_VULKAN_IT=1
```

### Integration Test Files

| File | Purpose |
|------|---------|
| `it_vulkan_subsystems.cpp` | Full subsystem validation with real GPU resources |
| `it_vulkan_model_present.cpp` | Model rendering and presentation |

### Integration Test Requirements

- Vulkan-capable GPU
- Vulkan driver installed
- SDL2 for window creation
- Tests create actual windows and render frames

---

## Relationship to Production Code

The Fake classes mirror specific portions of the production implementation:

| Test Class | Production Class | Functionality Tested |
|------------|-----------------|---------------------|
| `FakeDepthAttachmentSession` | `VulkanRenderingSession` | Depth attachment selection and pass management |
| `FakePostEffectsProcessor` | `VulkanRenderer::endSceneTexture()` | Post-effect enablement and application |
| `FakeTextureManagerRTT` | `VulkanTextureManager` | Render target lifecycle and layout transitions |

**Source File References:**

| Production File | Relevant Test Files |
|-----------------|---------------------|
| `code/graphics/vulkan/VulkanRenderingSession.cpp` | `test_vulkan_depth_attachment_switch.cpp`, `test_vulkan_frame_lifecycle.cpp` |
| `code/graphics/vulkan/VulkanRenderer.cpp` | `test_vulkan_post_effects_semantics.cpp`, `test_vulkan_renderer_shutdown.cpp` |
| `code/graphics/vulkan/VulkanTextureManager.cpp` | `test_vulkan_texture_render_target.cpp`, `test_vulkan_texture_contract.cpp` |
| `code/graphics/vulkan/VulkanPipelineManager.cpp` | `test_vulkan_pipeline_manager.cpp` |
| `code/graphics/vulkan/VulkanBufferManager.cpp` | `test_vulkan_buffer_manager_retirement.cpp`, `test_vulkan_buffer_zero_size.cpp` |

---

## Running the Tests

### Building Tests

```bash
# Configure with Vulkan support
cmake -B build -DFSO_BUILD_WITH_VULKAN=ON

# Build the test executable
cmake --build build --target unittests
```

### Running All Vulkan Unit Tests

```bash
# Run all Vulkan tests (unit tests only, no GPU required)
./build/bin/unittests --gtest_filter="Vulkan*"

# With CTest
ctest -R Vulkan --output-on-failure
```

### Running Specific Test Suites

```bash
# Depth attachment tests
./build/bin/unittests --gtest_filter="VulkanDepthAttachmentSwitch.*"

# Post-effects tests
./build/bin/unittests --gtest_filter="VulkanPostEffectsSemantics.*"

# Texture render target tests
./build/bin/unittests --gtest_filter="VulkanTextureRenderTarget.*"

# Pipeline tests
./build/bin/unittests --gtest_filter="VulkanPipelineManager.*"
```

### Running GPU Integration Tests

```bash
# Enable integration tests
export FS2_VULKAN_IT=1

# Run integration tests
./build/bin/unittests --gtest_filter="VulkanSubsystems.*"
```

### Verbose Output

```bash
# Show test names as they run
./build/bin/unittests --gtest_filter="Vulkan*" --gtest_print_time=1

# List all available Vulkan tests without running
./build/bin/unittests --gtest_filter="Vulkan*" --gtest_list_tests
```

---

## Adding New Tests

### Creating a New Unit Test

1. **Create the test file** in `test/src/graphics/` with the naming convention `test_vulkan_<feature>.cpp`

2. **Define a Fake class** that mirrors the relevant production logic:

```cpp
// test_vulkan_my_feature.cpp
#include <gtest/gtest.h>

namespace {

class FakeMyFeature {
  public:
    // Mirror production methods
    void doSomething();

    // Add observability for assertions
    int getSomeState() const;

  private:
    // Mirror relevant production state
    int m_state = 0;
};

} // namespace

TEST(VulkanMyFeature, DescriptiveTestName)
{
    FakeMyFeature fake;
    fake.doSomething();
    EXPECT_EQ(fake.getSomeState(), expectedValue);
}
```

3. **Register the file** in `test/src/source_groups.cmake`:

```cmake
if(FSO_BUILD_WITH_VULKAN)
add_file_folder("Graphics"
    # ... existing files ...
    graphics/test_vulkan_my_feature.cpp
)
endif()
```

4. **Document the test** in this file if it validates a significant behavioral invariant

### Test Naming Conventions

- **Test suite name**: `Vulkan<ComponentName>` (e.g., `VulkanDepthAttachmentSwitch`)
- **Test name**: `<Scenario>_<ExpectedBehavior>` (e.g., `FrameStart_SelectsMainDepth`)

### Keeping Fakes in Sync

When modifying production code:

1. Identify which Fake class mirrors the changed functionality
2. Update the Fake to reflect new state machine logic
3. Add tests for new behavioral invariants
4. Ensure existing tests still pass (or update them if invariants changed intentionally)

---

## Troubleshooting

### Tests Not Found

**Symptom:** `--gtest_filter="Vulkan*"` returns no tests

**Cause:** Vulkan tests not compiled

**Solution:** Ensure `FSO_BUILD_WITH_VULKAN=ON` in CMake configuration

### Integration Tests Skipped

**Symptom:** `VulkanSubsystems.*` tests report as skipped

**Cause:** `FS2_VULKAN_IT` environment variable not set

**Solution:** Set `export FS2_VULKAN_IT=1` before running

### Linker Errors

**Symptom:** Undefined references to Vulkan symbols

**Cause:** Vulkan SDK not found or not linked

**Solution:** Ensure Vulkan SDK is installed and `VULKAN_SDK` environment variable is set

### Test Failures After Production Changes

**Symptom:** Previously passing tests fail after code changes

**Diagnosis Steps:**

1. Read the test assertion message - it describes the expected invariant
2. Check if the invariant intentionally changed
3. If intentional, update the Fake class to match new behavior
4. If unintentional, the production change introduced a regression

### Fake State Mismatch

**Symptom:** Tests pass but production code behaves differently

**Cause:** Fake class drifted from production implementation

**Solution:** Audit the Fake class against the production source file and sync the state machine logic

---

## Design Rationale

### Why Fake State Machines?

1. **No GPU dependency** - Tests run on any machine, including CI without graphics hardware
2. **Deterministic** - No timing-dependent behavior or driver variance
3. **Fast** - Millisecond execution time per test
4. **Focused** - Each test validates exactly one invariant
5. **Maintainable** - Changes to production code require corresponding Fake updates

### Why Not Mock the Vulkan API?

Mocking `vk::*` types would:
- Require extensive boilerplate for each Vulkan call
- Test implementation details (which Vulkan calls are made)
- Miss behavioral bugs that occur in state machine logic
- Be fragile to internal refactoring

The Fake pattern tests **what** the system does (state transitions, computed values) rather than **how** it communicates with Vulkan.

### Two-Tier Testing Strategy

| Tier | Tests | Purpose |
|------|-------|---------|
| Unit (Fakes) | 30+ test files | Validate logic without GPU |
| Integration | 2 test files | Validate real GPU behavior |

Most invariants are tested at the unit level. Integration tests catch driver-specific issues and validate the full rendering pipeline.

---

## Related Documentation

For deeper understanding of the Vulkan renderer architecture, see:

| Document | Description |
|----------|-------------|
| [VULKAN_ARCHITECTURE.md](VULKAN_ARCHITECTURE.md) | Overall Vulkan renderer design |
| [VULKAN_RENDER_PASS_STRUCTURE.md](VULKAN_RENDER_PASS_STRUCTURE.md) | Render pass organization |
| [VULKAN_POST_PROCESSING.md](VULKAN_POST_PROCESSING.md) | Post-processing pipeline details |
| [VULKAN_DESCRIPTOR_SETS.md](VULKAN_DESCRIPTOR_SETS.md) | Descriptor set management |
| [VULKAN_PIPELINE_MANAGEMENT.md](VULKAN_PIPELINE_MANAGEMENT.md) | Pipeline creation and caching |
| [VULKAN_SYNCHRONIZATION.md](VULKAN_SYNCHRONIZATION.md) | Synchronization primitives |
| [VULKAN_ERROR_HANDLING.md](VULKAN_ERROR_HANDLING.md) | Error handling patterns |

---

## Test File Reference

### Documented Unit Tests (Detailed Coverage)

| File | Lines | Test Count | Purpose |
|------|-------|------------|---------|
| `test_vulkan_depth_attachment_switch.cpp` | ~248 | 8 | Depth attachment state machine |
| `test_vulkan_post_effects_semantics.cpp` | ~442 | 10 | Post-effects processing |
| `test_vulkan_texture_render_target.cpp` | ~384 | 14 | Render target management |

### All Vulkan Test Files

| File | Category |
|------|----------|
| `test_vulkan_blend_enable.cpp` | Rendering State |
| `test_vulkan_buffer_manager_retirement.cpp` | Buffer Management |
| `test_vulkan_buffer_zero_size.cpp` | Buffer Management |
| `test_vulkan_clear_ops_oneshot.cpp` | Rendering State |
| `test_vulkan_clip_scissor.cpp` | Rendering State |
| `test_vulkan_decal_instancing.cpp` | Specialized Features |
| `test_vulkan_deferred_release.cpp` | Device/Initialization |
| `test_vulkan_depth_attachment_switch.cpp` | Frame Lifecycle |
| `test_vulkan_depth_format_selection.cpp` | Device/Initialization |
| `test_vulkan_descriptor_layouts.cpp` | Pipeline/Shader |
| `test_vulkan_device_scoring.cpp` | Device/Initialization |
| `test_vulkan_dynstate.cpp` | Buffer Management |
| `test_vulkan_fallback_texture.cpp` | Texture |
| `test_vulkan_frame_lifecycle.cpp` | Frame Lifecycle |
| `test_vulkan_perdraw_bindings.cpp` | Post-Processing |
| `test_vulkan_pipeline_manager.cpp` | Pipeline/Shader |
| `test_vulkan_post_effects_semantics.cpp` | Post-Processing |
| `test_vulkan_post_process_targets.cpp` | Post-Processing |
| `test_vulkan_recordingframe_sealed.cpp` | Frame Lifecycle |
| `test_vulkan_render_target_state.cpp` | Rendering State |
| `test_vulkan_renderer_shutdown.cpp` | Device/Initialization |
| `test_vulkan_scene_texture_lifecycle.cpp` | Frame Lifecycle |
| `test_vulkan_shader_alignment.cpp` | Pipeline/Shader |
| `test_vulkan_shader_layout_contracts.cpp` | Pipeline/Shader |
| `test_vulkan_shader_manager_model.cpp` | Pipeline/Shader |
| `test_vulkan_swapchain_acquire.cpp` | Frame Lifecycle |
| `test_vulkan_texture_contract.cpp` | Texture |
| `test_vulkan_texture_helpers.cpp` | Texture |
| `test_vulkan_texture_render_target.cpp` | Texture |
| `test_vulkan_texture_upload_alignment.cpp` | Texture |
| `it_vulkan_model_present.cpp` | GPU Integration |
| `it_vulkan_subsystems.cpp` | GPU Integration |
