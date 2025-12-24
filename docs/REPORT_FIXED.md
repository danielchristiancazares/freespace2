# Vulkan Renderer Bug Investigation - Resolved Issues

**Parent Document:** [REPORT.md](REPORT.md)
**Changelog:** [REPORT_CHANGELOG.md](REPORT_CHANGELOG.md)

---

## Summary

| Severity | Resolved |
|----------|----------|
| Critical | 3        |
| High     | 5        |
| Medium   | 1        |
| Total    | 9        |

---

## Resolved Critical Issues

### C4. Immediate Buffer Deletion (Use-After-Free)

**Status:** Resolved
**File:** `VulkanBufferManager.cpp:79-102`
**Source:** GPT-5.2 Pro

**Problem:**
`deleteBuffer()` reset `UniqueBuffer`/`UniqueDeviceMemory` immediately, unlike `updateBufferData` which defers to `m_retiredBuffers`:

```cpp
void VulkanBufferManager::deleteBuffer(gr_buffer_handle handle) {
    // ...
    buffer.buffer.reset();   // Immediate destruction!
    buffer.memory.reset();
    buffer.size = 0;
}
```

**Impact:**
If GPU is still using the buffer (common with multiple frames in flight), immediate deletion causes use-after-free on GPU, validation errors, and driver crashes.

**Fix Applied:**
1. Modified `deleteBuffer()` to move GPU resources to `m_retiredBuffers` instead of immediate destruction
2. Now uses the same deferred deletion pattern as `updateBufferData()` and `resizeBuffer()`
3. Buffer destruction deferred by `FRAMES_BEFORE_DELETE` (3 frames) to ensure GPU has finished using resources
4. Added unit test `test_vulkan_buffer_manager_retirement.cpp` to verify all three destruction paths use deferred deletion

---

### C6. Misaligned Shader Pointer on ARM

**Status:** Resolved
**File:** `VulkanShaderManager.cpp:107-113`
**Source:** Gemini 3 Deep Think

**Problem:**
Filesystem shader loading uses `std::vector<char>` which has 1-byte alignment:

```cpp
std::vector<char> buffer(fileSize);
file.read(buffer.data(), fileSize);
moduleInfo.pCode = reinterpret_cast<const uint32_t*>(buffer.data());
```

Vulkan requires `pCode` to be 4-byte aligned.

**Impact:**
Crash (SIGBUS) on ARM architectures (Mac M-series, Android) or strict x86 modes. The embedded shader path correctly uses `std::vector<uint32_t>`, but filesystem fallback does not.

**Fix Applied:**
1. Changed filesystem shader loading to use `std::vector<uint32_t>` instead of `std::vector<char>`
2. Buffer size calculated as `(fileSize + 3) / 4` to round up to uint32_t boundary
3. File read uses `reinterpret_cast<char*>(buffer.data())` for byte-wise reading
4. `codeSize` preserved as exact file size (not rounded up)
5. Now matches the pattern used by the embedded shader path
6. Added unit test `test_vulkan_shader_alignment.cpp` to verify 4-byte alignment of pCode for various file sizes

---

### C9. Fallback Texture Handle Never Initialized

**Status:** Resolved
**Files:** `VulkanTextureManager.h:145`, `VulkanRenderer.cpp:674-675`
**Source:** Claude Desktop

**Problem:**
Fallback texture handle is initialized to -1 and never set:

```cpp
// VulkanTextureManager.h:145
int m_fallbackTextureHandle = -1;
```

But it's used with an assertion that will always fail:

```cpp
// VulkanRenderer.cpp:674-675
int fallbackHandle = m_textureManager->getFallbackTextureHandle();
Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");
```

**Impact:**
When `writeFallbackDescriptor` is called during texture retirement, assertion fires and crashes. This will happen on the first texture retirement.

**Fix Applied:**
1. Added `createFallbackTexture()` method to `VulkanTextureManager`
2. Creates a 1x1 black RGBA texture during initialization
3. Uses synthetic handle `kFallbackTextureHandle = -1000` to avoid collision with bmpman handles
4. Stores in `m_textures` map and sets `m_fallbackTextureHandle` to the synthetic handle
5. Called from constructor after `createDefaultSampler()`

---

## Resolved High Severity Issues

### H1. Frame Count Constant Mismatch

**Status:** Resolved
**Files:** `VulkanRenderer.h:139`, `VulkanConstants.h:8`
**Source:** GPT-5.2 Pro

**Problem:**
Two constants define frames-in-flight with conflicting values:

| Constant | Value | Location |
|----------|-------|----------|
| `MAX_FRAMES_IN_FLIGHT` | 2 | VulkanRenderer.h:139 |
| `kFramesInFlight` | 3 | VulkanConstants.h:8 |

Renderer uses `MAX_FRAMES_IN_FLIGHT = 2` for frame cycling:
```cpp
m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
```

But texture manager uses `kFramesInFlight = 3`:
```cpp
std::array<bool, kFramesInFlight> descriptorWritten = {false, false, false};
```

**Impact:**
Wasted memory (resources held one frame longer), unused array slots, inconsistent codebase inviting future bugs.

**Root Cause:**
Value `3` likely confused with `kGBufferCount = 3` (VulkanRenderTargets.h:13) which defines G-buffer attachments, unrelated to frame synchronization.

**Fix Applied:**
1. Changed `kFramesInFlight` from 3 to 2 in `VulkanConstants.h`
2. Removed `MAX_FRAMES_IN_FLIGHT` from `VulkanRenderer.h`
3. Added `#include "VulkanConstants.h"` to `VulkanRenderer.h`
4. Updated all references to use `kFramesInFlight`
5. Changed `descriptorWritten` initializer to use value-initialization (`{}`)

---

### H7. Blending Unconditionally Disabled with EDS3

**Status:** Resolved
**Files:** `VulkanGraphics.cpp:899-901, 1146-1148`, `VulkanRenderingSession.cpp:247-249`
**Source:** Gemini 3 Deep Think

**Problem:**
When Extended Dynamic State 3 is supported, blending is forced to FALSE regardless of material settings:

```cpp
if (caps.colorBlendEnable) {
    vk::Bool32 blendEnable = VK_FALSE;  // ALWAYS FALSE!
    cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
}
```

The pipeline key correctly stores `blend_mode` from material, but EDS3 overrides it.

**Verification (Claude Opus 4.5):**
Confirmed bug exists in 3 locations:
1. `VulkanGraphics.cpp:899-901` - `gr_vulkan_render_primitives()`
2. `VulkanGraphics.cpp:1146-1148` - `gr_vulkan_render_primitives_batched()`
3. `VulkanRenderingSession.cpp:247-249` - `applyDynamicState()` default dynamic state setup

All three hardcode `blendEnable = VK_FALSE` while the pipeline key at lines 481, 707, and 1012 correctly captures `material_info->get_blend_mode()`.

**Impact:**
All transparent rendering is broken when EDS3 is supported. Glass, smoke, HUD elements, particles all appear opaque.

**Fix Applied:**
1. Modified `gr_vulkan_render_primitives()` and `gr_vulkan_render_primitives_batched()` to set `blendEnable` based on material blend mode
2. Changed hardcoded `VK_FALSE` to `(material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE`
3. `applyDynamicState()` default remains `VK_FALSE` (correct, as per-draw calls override based on material)
4. Added unit test `test_vulkan_blend_enable.cpp` with 7 test cases verifying all blend modes

---

### H8. Scissor Ignores Clip Region (UI Broken)

**Status:** Resolved
**File:** `VulkanGraphics.cpp:968, 1214`
**Source:** Gemini 3 Deep Think

**Problem:**
`createClipScissor()` exists and correctly reads `gr_screen.clip_*` values, but is NEVER CALLED:

```cpp
// Line 50-60: Correct implementation exists
vk::Rect2D createClipScissor() {
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{
        static_cast<int32_t>(gr_screen.clip_left),
        static_cast<int32_t>(gr_screen.clip_top)};
    // ...
}

// Line 968, 1214: But all render functions use full screen
vk::Rect2D scissor = createFullScreenScissor();
cmd.setScissor(0, 1, &scissor);
```

**Impact:**
UI clipping via `gr_set_clip()` updates `gr_screen.clip_*` but scissor ignores it. UI elements (text boxes, lists, scroll areas) will draw outside their intended bounds.

**Fix Applied (2025-12-13):**
1. Corrected Vulkan clip state semantics to match engine expectations (`clip_left/top` remain 0; Vulkan scissor uses `offset_x/y` + `clip_width/height`).
2. Updated Vulkan draw paths to apply the current clip scissor instead of forcing full-screen scissor.
3. Added a Vulkan unit test covering clip/scissor state derivation.

---

### H9. Buffer Offset Alignment for R8 Textures

**Status:** Resolved (2025-12-13)
**File:** `VulkanTextureManager.cpp:323, 444`
**Source:** Gemini 3 Deep Think

**Problem:**
In the `uploadImmediate()` path (synchronous upload with dedicated staging buffer), texture layers were packed tightly without alignment:

```cpp
// Previous behavior in uploadImmediate()
offset += layerSize;
```

For R8 format (1 byte/pixel) with small dimensions, `layerSize` may not be a multiple of 4.

**Verification (Claude Opus 4.5):**
Line number in original report was incorrect (claimed 242). Actual bug is at lines 391 (data copy loop) and 464 (region generation loop) in `uploadImmediate()`. The ring buffer path (`flushPendingUploads()`) is NOT affected because it uses `optimalBufferCopyOffsetAlignment` from device properties (VulkanRenderer.cpp:92), which ensures each allocation is properly aligned.

**Impact:**
Vulkan spec requires `bufferOffset` in `vkCmdCopyBufferToImage` to be a multiple of 4 bytes. Unaligned offsets in the `uploadImmediate()` path cause validation errors or device loss on strict drivers when uploading R8 texture arrays where `width * height % 4 != 0`.

**Fix Applied (2025-12-13):**
1. Added `buildImmediateUploadLayout()` helper to compute 4-byte aligned per-layer offsets and padded staging size for `uploadImmediate()`.
2. Updated `uploadImmediate()` to use the precomputed offsets for both CPU-side staging writes and `vk::BufferImageCopy::bufferOffset` generation.
3. Added unit test `test_vulkan_texture_upload_alignment.cpp` to lock in the alignment requirement for R8 texture arrays.

---

### H10. Depth Format Selection Has Two Critical Flaws

**Status:** Resolved
**File:** `VulkanRenderTargets.cpp:70-96`
**Source:** Claude Desktop, Claude Opus 4.5

**Problem:**
Two issues in `findDepthFormat()`:

**Flaw 1 - Incomplete capability check:**
Loop only checked `eDepthStencilAttachment`, ignoring `eSampledImage`. But depth buffer is created with BOTH usages and a sample view is created for deferred lighting.

**Flaw 2 - Silent fallback masks failure:**
If NO candidate format supported depth attachment, the code silently returned `eD32Sfloat` without verification, masking a fundamental capability failure.

**Impact:**
- Flaw 1: If selected format supports attachment but not sampling, deferred lighting has undefined behavior
- Flaw 2: If no candidate works, invalid format returned silently causing cryptic failures

**Fix Applied:**
1. Modified `findDepthFormat()` to check for BOTH `eDepthStencilAttachment` AND `eSampledImage` features
2. Replaced silent fallback with `throw std::runtime_error()` - device must support the required features
3. Added unit test `test_vulkan_depth_format_selection.cpp` with 9 test cases covering:
   - Format with both features is selected
   - Format with only attachment throws
   - Format with only sampling throws
   - No suitable format throws (no silent fallback)
   - Candidate preference order is respected
   - Bug H10 regression tests for both flaws

---

## Resolved Medium Severity Issues

### M3. Device Scoring Algorithm Broken

**Status:** Resolved (2025-12-12)
**File:** `VulkanDevice.cpp:188-195`
**Source:** Gemini 3 Deep Think

**Problem:**
Device selection score calculation used the raw `apiVersion` packed integer:

```cpp
score += deviceTypeScore(device.properties.deviceType) * 1000;  // max ~2000
score += device.properties.apiVersion * 100;  // ~420,000,000 for Vulkan 1.4
```

`apiVersion` is a packed integer (~4.2 million for Vulkan 1.4). Multiplied by 100 = ~420 million, completely dwarfing the device type score of at most 2000.

**Impact:**
An integrated GPU with Vulkan 1.4 scored ~421M while a discrete GPU with Vulkan 1.3 scored ~419M. On systems with both (common on laptops), the wrong GPU was selected.

**Fix Applied:**
1. Made device type the dominant factor by multiplying by 1,000,000 instead of 1,000
2. Changed Vulkan version contribution to use only major.minor (ignoring patch), giving a maximum score of ~200
3. Updated `deviceTypeScore()` to return 3 for discrete, 2 for integrated, 1 for virtual, 0 for others
4. Exposed `deviceTypeScore()` and `scoreDevice()` in `VulkanDevice.h` for testing
5. Added unit test `test_vulkan_device_scoring.cpp` with 6 test cases verifying:
   - Discrete GPU beats integrated regardless of Vulkan version
   - Same device type prefers higher Vulkan version
   - Patch version is ignored
   - Integrated beats virtual GPU

Example scores after fix:
- Discrete GPU with Vulkan 1.4: 3,000,104
- Integrated GPU with Vulkan 1.4: 2,000,104
- Discrete GPU now correctly wins on multi-GPU systems

---

## Files Affected by Resolved Issues

| File | Issues |
|------|--------|
| VulkanRenderer.cpp | C9 |
| VulkanRenderer.h | H1 |
| VulkanConstants.h | H1 |
| VulkanBufferManager.cpp | C4 |
| VulkanGraphics.cpp | H7, H8 |
| VulkanRenderTargets.cpp | H10 |
| VulkanRenderingSession.cpp | H7 |
| VulkanTextureManager.cpp | H9 |
| VulkanTextureManager.h | C9 |
| VulkanShaderManager.cpp | C6 |
| VulkanDevice.cpp | M3 |
| VulkanDevice.h | M3 |
