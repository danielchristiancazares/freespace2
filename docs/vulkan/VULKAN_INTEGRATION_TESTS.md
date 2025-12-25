# Vulkan Integration Tests - Behavioral State Machine Testing

This document describes the integration test suites for the Vulkan renderer subsystems. These tests validate critical behavioral invariants using lightweight "Fake" state machines that mirror the production implementation logic.

---

## Table of Contents

1. [Testing Philosophy](#testing-philosophy)
2. [Fake State Machine Pattern](#fake-state-machine-pattern)
3. [Test Suites](#test-suites)
   - [Depth Attachment Switching](#depth-attachment-switching)
   - [Post-Effects Semantics](#post-effects-semantics)
   - [Texture Render Targets](#texture-render-targets)
4. [Relationship to Production Code](#relationship-to-production-code)
5. [Test Files Reference](#test-files-reference)

---

## Testing Philosophy

The Vulkan integration tests focus on **behavioral invariants** rather than implementation details. Each test suite:

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

Each test file defines a `Fake*` class that mirrors the relevant portion of a production component:

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

## Test Suites

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

## Relationship to Production Code

The Fake classes mirror specific portions of the production implementation:

| Test Class | Production Class | Functionality Tested |
|------------|-----------------|---------------------|
| `FakeDepthAttachmentSession` | `VulkanRenderingSession` | Depth attachment selection and pass management |
| `FakePostEffectsProcessor` | `VulkanRenderer::endSceneTexture()` | Post-effect enablement and application |
| `FakeTextureManagerRTT` | `VulkanTextureManager` | Render target lifecycle and layout transitions |

**Source File References:**

| Production File | Relevant Test File |
|-----------------|-------------------|
| `code/graphics/vulkan/VulkanRenderingSession.cpp` | `test_vulkan_depth_attachment_switch.cpp` |
| `code/graphics/vulkan/VulkanRenderer.cpp` | `test_vulkan_post_effects_semantics.cpp` |
| `code/graphics/vulkan/VulkanTextureManager.cpp` | `test_vulkan_texture_render_target.cpp` |

---

## Test Files Reference

| File | Lines | Test Count | Purpose |
|------|-------|------------|---------|
| `test/src/graphics/test_vulkan_depth_attachment_switch.cpp` | ~250 | 8 | Depth attachment state machine |
| `test/src/graphics/test_vulkan_post_effects_semantics.cpp` | ~443 | 10 | Post-effects processing |
| `test/src/graphics/test_vulkan_texture_render_target.cpp` | ~385 | 14 | Render target management |

---

## Running the Tests

Build and run with CTest or GoogleTest runner:

```bash
# Build tests
cmake --build build --target vulkan_tests

# Run all Vulkan integration tests
ctest -R Vulkan --output-on-failure

# Run specific test suite
./build/test/vulkan_tests --gtest_filter="VulkanDepthAttachmentSwitch.*"
./build/test/vulkan_tests --gtest_filter="VulkanPostEffectsSemantics.*"
./build/test/vulkan_tests --gtest_filter="VulkanTextureRenderTarget.*"
```

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

### Keeping Fakes in Sync

When modifying production code:

1. Identify which Fake class mirrors the changed functionality
2. Update the Fake to reflect new state machine logic
3. Add tests for new behavioral invariants
4. Ensure existing tests still pass (or update them if invariants changed intentionally)

The Fake implementations serve as executable documentation of expected behavior.
