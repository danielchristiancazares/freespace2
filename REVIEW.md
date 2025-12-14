# Vulkan Texture Renderer Review - Verification Analysis

This document contains adversarial verification of issues identified in FINDINGS.md. Each issue was rigorously tested for validity by attempting to disprove it.

---

## Issue 1 — Fallback handle assertion is incorrect

**Original Claim**: Assertion `fallbackHandle >= 0` will always fail because `kFallbackTextureHandle = -1000`.

### Evidence

**VulkanTextureManager.h:153**
```cpp
static constexpr int kFallbackTextureHandle = -1000;
```

**VulkanTextureManager.h:184** (uninitialized sentinel)
```cpp
int m_fallbackTextureHandle = -1;
```

**VulkanTextureManager.cpp:239** (after createFallbackTexture)
```cpp
m_fallbackTextureHandle = kFallbackTextureHandle;  // Sets to -1000
```

**VulkanRenderer.cpp:608-609** (the problematic assertion)
```cpp
int fallbackHandle = m_textureManager->getFallbackTextureHandle();
Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");
```

### Disproof Attempts

**Attempt 1: Is writeFallbackDescriptor ever called?**

The call site is VulkanRenderer.cpp:548-550:
```cpp
for (uint32_t slot : m_textureManager->getRetiredSlots()) {
    writeFallbackDescriptor(frame.modelDescriptorSet, slot);
}
```

This executes when `getRetiredSlots()` is non-empty, populated by `retireTexture()`. The method `retireTexture` is not called within VulkanRenderer.cpp but is public API intended for external use. If texture retirement is never used, this bug is latent.

**Attempt 2: Does the code work despite the bad assertion?**

Tracing past the assertion:
```cpp
vk::DescriptorImageInfo info = m_textureManager->getTextureDescriptorInfo(
    fallbackHandle, samplerKey);  // fallbackHandle = -1000
```

In `getTextureDescriptorInfo` (VulkanTextureManager.cpp:917):
```cpp
auto it = m_textures.find(textureHandle);  // Finds -1000
```

The fallback texture IS stored at key -1000 (line 238) and IS marked Resident (line 237). The lookup succeeds and returns valid descriptor info.

**Finding**: If `Assertion` is debug-only (common pattern), release builds work correctly. The assertion is wrong, but the underlying storage logic is sound.

**Attempt 3: What should the assertion check?**

The member `m_fallbackTextureHandle` is initialized to -1 (uninitialized sentinel) and set to -1000 (initialized). The check `>= 0` fails for both states. Correct checks would be:
```cpp
Assertion(fallbackHandle == kFallbackTextureHandle, "...");
// or
Assertion(fallbackHandle != -1, "...");
```

### Verdict: BUG CONFIRMED

- The assertion logic is definitively wrong
- Severity is conditional: debug builds crash on texture retirement; release builds likely work
- The correct fix is to change the assertion condition, not the handle value

---

## Issue 2 — Sampler cache uses a lossy hash key

**Original Claim**: Different `(filter, address)` pairs can hash to the same value, returning the wrong sampler.

### Evidence

**VulkanTextureManager.cpp:244-248**
```cpp
const size_t hash = (static_cast<size_t>(key.filter) << 4) ^ static_cast<size_t>(key.address);
auto it = m_samplerCache.find(hash);
if (it != m_samplerCache.end()) {
    return it->second.get();
}
```

The map is `std::unordered_map<size_t, vk::UniqueSampler>` — keyed by hash, not by `SamplerKey`.

### Disproof Attempts

**Attempt 1: Enumerate all possible hash values**

vk::Filter values (Vulkan core):
- `eNearest` = 0
- `eLinear` = 1
- `eCubicEXT` = 2 (extension)

vk::SamplerAddressMode values:
- `eRepeat` = 0
- `eMirroredRepeat` = 1
- `eClampToEdge` = 2
- `eClampToBorder` = 3
- `eMirrorClampToEdge` = 4

Hash computation for all combinations:

| filter | address | hash = (filter << 4) ^ address |
|--------|---------|-------------------------------|
| 0 | 0 | 0 |
| 0 | 1 | 1 |
| 0 | 2 | 2 |
| 0 | 3 | 3 |
| 0 | 4 | 4 |
| 1 | 0 | 16 |
| 1 | 1 | 17 |
| 1 | 2 | 18 |
| 1 | 3 | 19 |
| 1 | 4 | 20 |
| 2 | 0 | 32 |
| 2 | 1 | 33 |
| 2 | 2 | 34 |
| 2 | 3 | 35 |
| 2 | 4 | 36 |

**All 15 hashes are unique. No collisions exist.**

**Attempt 2: Mathematical proof of collision impossibility**

For a collision: `(f1 << 4) ^ a1 = (f2 << 4) ^ a2` where `(f1, a1) != (f2, a2)`

Rearranging: `a1 ^ a2 = (f1 ^ f2) << 4`

Left side maximum: max XOR of values in {0,1,2,3,4} is `4 ^ 3 = 7` (bits 0-2).

Right side values: multiples of 16 (0, 16, 32, 48...).

For equality when f1 != f2: need `a1 ^ a2 >= 16`, but max is 7. **Impossible.**

For f1 = f2: need `a1 ^ a2 = 0`, meaning `a1 = a2`. Keys are identical, not a collision.

**The bit ranges are disjoint**: filter occupies bits 4+ (values 0, 16, 32...), address occupies bits 0-2 (values 0-7 max). XOR of disjoint bit ranges cannot produce collisions.

**Attempt 3: Future enum growth**

For collisions with expanded enums, address values would need to reach 16+ to overlap with filter bits. Vulkan core enums are stable since 1.0. Extensions use values >= 1000000000 (different collision domain, rarely used for samplers).

### Verdict: NOT A BUG

The hash function is mathematically collision-free for all valid Vulkan enum values. The bit ranges are disjoint, making XOR a valid combine function for this specific use case. The original analysis was overly cautious but mathematically incorrect.

---

## Issue 11 — getTextureDescriptor() can silently return empty descriptors

**Original Claim**: Returns invalid empty descriptors when texture manager is null, causing Vulkan validation errors.

### Evidence

**VulkanRenderer.cpp:394-398**
```cpp
if (!m_textureManager) {
    vk::DescriptorImageInfo empty{};
    return empty;
}
```

**VulkanTextureManager.cpp:791-796** (related issue in queryDescriptor)
```cpp
int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
if (baseFrame < 0) {
    // Invalid bitmap handle - return fallback
    result.isFallback = true;
    return result;  // result.info is DEFAULT (empty)!
}
```

### Disproof Attempts

**Attempt 1: Is m_textureManager ever null after initialization?**

VulkanRenderer::initialize() at lines 60-63:
```cpp
m_textureManager = std::make_unique<VulkanTextureManager>(...);
```

After successful initialization, `m_textureManager` is guaranteed non-null. The null check is defensive programming for pre-init calls or partial init failure.

**Finding**: In normal operation, this branch is rarely taken. However, if taken, it returns invalid data.

**Attempt 2: Does queryDescriptor always return valid fallback info?**

The early return at line 795 (for `baseFrame < 0`) sets `result.isFallback = true` but leaves `result.info` default-initialized (null imageView, null sampler).

The fallback lookup at lines 818-823:
```cpp
auto fallbackIt = m_textures.find(kFallbackTextureHandle);
if (fallbackIt != m_textures.end() && fallbackIt->second.gpu.imageView) {
    result.info.imageView = fallbackIt->second.gpu.imageView.get();
    result.info.sampler = getOrCreateSampler(samplerKey);
    result.info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
}
```

This code is **only reached for non-resident textures**, not for invalid handles (which return early).

**Finding**: The comment "return fallback" is incorrect — invalid bitmap handles bypass fallback descriptor assignment entirely.

**Attempt 3: Can baseFrame < 0 happen in practice?**

`bm_get_base_frame` returns -1 for invalid handles in FreeSpace2's bitmap manager. This occurs with:
- Uninitialized handles
- Freed/unloaded textures
- Handle array indexing errors

The code structure (checking for it, commenting about fallback) indicates this is an expected edge case.

**Attempt 4: What happens with empty descriptors downstream?**

In `updateModelDescriptors` (VulkanRenderer.cpp:502-513):
```cpp
vk::DescriptorImageInfo info = getTextureDescriptor(handle, frame, cmd, samplerKey);
imageInfos.push_back(info);
writes.push_back({set, 1, arrayIndex, 1, vk::DescriptorType::eCombinedImageSampler,
    &imageInfos.back(), nullptr, nullptr});
// ...
m_vulkanDevice->device().updateDescriptorSets(writes, {});
```

No validation of `info.imageView` before use. An empty descriptor is written to the descriptor set.

When sampled:
- Vulkan validation layers error
- Undefined behavior on some drivers
- Possible GPU hang

**Attempt 5: Do callers validate results?**

All callers of `getTextureDescriptor` use the result directly without checking validity.

### Verdict: BUG CONFIRMED

The original claim about `!m_textureManager` is technically correct but unlikely to manifest (defensive code).

The more significant bug is in `queryDescriptor`: the early return for invalid bitmap handles does NOT populate fallback descriptor info, despite the comment claiming otherwise. This violates the documented contract ("always returns a valid descriptor") and creates descriptors that cause Vulkan validation errors.

---

## Summary

| Issue | Verdict | Confidence | Notes |
|-------|---------|------------|-------|
| 1 - Fallback assertion | **Bug** | High | Wrong assertion condition; works in release if Assertion compiles out |
| 2 - Sampler hash | **Not a bug** | Very High | Mathematically proven collision-free for all valid Vulkan enums |
| 11 - Empty descriptor | **Bug** | High | queryDescriptor invalid-handle path bypasses fallback; violates contract |

## Recommended Fixes

### Issue 1
```cpp
// VulkanRenderer.cpp:609
// Change from:
Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");
// To:
Assertion(fallbackHandle == VulkanTextureManager::kFallbackTextureHandle,
    "Fallback texture must be initialized");
```

### Issue 11
```cpp
// VulkanTextureManager.cpp:791-796
// Change from:
if (baseFrame < 0) {
    result.isFallback = true;
    return result;
}
// To:
if (baseFrame < 0) {
    result.isFallback = true;
    auto fallbackIt = m_textures.find(kFallbackTextureHandle);
    if (fallbackIt != m_textures.end() && fallbackIt->second.gpu.imageView) {
        result.info.imageView = fallbackIt->second.gpu.imageView.get();
        result.info.sampler = getOrCreateSampler(samplerKey);
        result.info.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    }
    return result;
}
```
