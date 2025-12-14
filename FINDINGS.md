# Vulkan Texture Renderer Review (So Far)

Scope of this pass (completed before disconnect):
- `code/graphics/vulkan/VulkanTextureManager.h`
- `code/graphics/vulkan/VulkanTextureManager.cpp`
- `code/graphics/vulkan/VulkanRenderer.cpp` (texture descriptor path + model descriptor sync)
- `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

Notes:
- This is **partial**. I have **not** yet reviewed `VulkanGraphics.cpp`, `VulkanFrame.*` (staging ring alignment rules), `VulkanDevice.*` (feature enablement details), shader declarations (sampler types), or the render target paths.
- All snippets below are copied from the repository with approximate line references based on the file reads during the session.

---

## Issue 1 — Fallback handle assertion is incorrect (will always fail if executed)

**File:** `code/graphics/vulkan/VulkanTextureManager.h` (around `kFallbackTextureHandle`) and `code/graphics/vulkan/VulkanRenderer.cpp` (`writeFallbackDescriptor`)

**Snippet (TextureManager constant):**
```cpp
static constexpr int kFallbackTextureHandle = -1000;
```

**Snippet (Renderer assertion):**
```cpp
int fallbackHandle = m_textureManager->getFallbackTextureHandle();
Assertion(fallbackHandle >= 0, "Fallback texture must be initialized");
```

**What the issue is**
- The fallback texture handle is intentionally a **negative sentinel** (`-1000`), but `VulkanRenderer::writeFallbackDescriptor()` asserts `fallbackHandle >= 0`.

**Why it’s a problem**
- If/when retired slots are used (i.e., `writeFallbackDescriptor()` gets called), this assertion will fire even though the fallback texture is initialized correctly.
- This is a “latent crash” that depends on whether the retired-slot path is exercised.

**Suggested fix**
- Change the assertion to check initialization correctly, e.g.:
  - `Assertion(fallbackHandle == VulkanTextureManager::kFallbackTextureHandle, ...)`, or
  - `Assertion(fallbackHandle != -1, ...)`, or
  - store the fallback handle as a non-negative “real” bitmap handle instead of a sentinel key.

---

## Issue 2 — Sampler cache uses a lossy hash key (collisions can return the wrong sampler)

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (around `getOrCreateSampler`)

**Snippet:**
```cpp
const size_t hash = (static_cast<size_t>(key.filter) << 4) ^ static_cast<size_t>(key.address);
auto it = m_samplerCache.find(hash);
if (it != m_samplerCache.end()) {
    return it->second.get();
}
```

**What the issue is**
- The cache key is a single `size_t` computed via shift+XOR, and the map is keyed solely by that value.
- Different `(filter, address)` pairs can collide into the same `hash`.

**Why it’s a problem**
- A collision silently returns a sampler with the wrong parameters (filter/addressing), causing subtle rendering bugs that are hard to diagnose.

**Suggested fix**
- Key the cache by `SamplerKey` directly:
  - `std::unordered_map<SamplerKey, vk::UniqueSampler, SamplerKeyHash> m_samplerCache;`
- Or use a collision-resistant key (e.g., `uint64_t` pack, or `std::pair` with a proper hash), and/or verify equality on collision.

---

## Issue 3 — `queryDescriptor()` can return an “empty” descriptor despite claiming it’s always valid

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`queryDescriptor`) and `code/graphics/vulkan/VulkanRenderer.cpp` (`getTextureDescriptor`)

**Snippet (`getTextureDescriptor` comment + behavior):**
```cpp
// Query descriptor - this never throws and always returns a valid descriptor (possibly fallback)
auto query = m_textureManager->queryDescriptor(bitmapHandle, samplerKey);
return query.info;
```

**Snippet (`queryDescriptor` invalid-handle path):**
```cpp
int baseFrame = bm_get_base_frame(bitmapHandle, nullptr);
if (baseFrame < 0) {
    // Invalid bitmap handle - return fallback
    result.isFallback = true;
    return result;
}
```

**What the issue is**
- For invalid handles (and also if the fallback record isn’t present for some reason), `result.info` is left default-initialized (null `imageView` / null `sampler` / undefined-ish layout).

**Why it’s a problem**
- Passing an empty `vk::DescriptorImageInfo` into a push descriptor write or descriptor set update can trigger validation errors or sampling from a null descriptor (undefined behavior).
- The renderer code is written as if it’s impossible (“always returns a valid descriptor”), which can hide this failure mode.

**Suggested fix**
- Make `queryDescriptor()` always fill `result.info` with a valid fallback descriptor for *all* fallback cases:
  - Ensure fallback texture creation is mandatory and its descriptor info is always used.
  - If fallback isn’t available, explicitly log and return a known-safe “null descriptor” pattern only if the device supports it and the pipeline layout enables it.
- Update/remove the misleading comment in `VulkanRenderer::getTextureDescriptor()`.

---

## Issue 4 — `uploadImmediate()` uses a suspicious/likely-wrong barrier destination stage/access

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`uploadImmediate`)

**Snippet:**
```cpp
vk::ImageMemoryBarrier2 toShader{};
toShader.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
toShader.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
toShader.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
toShader.dstAccessMask = vk::AccessFlagBits2::eMemoryRead;
toShader.oldLayout = vk::ImageLayout::eTransferDstOptimal;
toShader.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
```

**What the issue is**
- The barrier transitions the image to `eShaderReadOnlyOptimal` but uses `dstStageMask = eTransfer` / `dstAccessMask = eMemoryRead` instead of a shader stage + `eShaderRead`.

**Why it’s a problem**
- It’s inconsistent with the other paths (`createFallbackTexture()` and `flushPendingUploads()`) which correctly use fragment shader stage + shader-read access.
- On stricter drivers or under async usage, this can lead to missing/incorrect visibility guarantees for shader sampling.

**Suggested fix**
- Make it consistent with the sampling usage:
  - `dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader` (and/or include other shader stages that sample)
  - `dstAccessMask = vk::AccessFlagBits2::eShaderRead`

---

## Issue 5 — All textures are created with `vk::ImageViewType::e2DArray` (even non-array textures)

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`createFallbackTexture`, `uploadImmediate`, `flushPendingUploads`)

**Snippet (fallback):**
```cpp
viewInfo.viewType = vk::ImageViewType::e2DArray;
viewInfo.subresourceRange.layerCount = 1;
```

**Snippet (upload paths):**
```cpp
viewInfo.viewType = vk::ImageViewType::e2DArray;
viewInfo.subresourceRange.layerCount = layers;
```

**What the issue is**
- Even for a normal 2D texture (`layers == 1`), the view type is `e2DArray`.

**Why it’s a problem**
- Vulkan requires the image view type to be compatible with the shader sampler type. If shaders use `sampler2D` for these bindings, binding an `e2DArray` view is invalid.
- If shaders use `sampler2DArray` everywhere (sampling `layer = 0` for non-array), this may be fine—but it’s a strong assumption that should be verified against the actual GLSL/SPIR-V.

**Suggested fix**
- Use `vk::ImageViewType::e2D` when `layers == 1`, and `vk::ImageViewType::e2DArray` only when `layers > 1`.
- Alternatively, enforce `sampler2DArray` in all relevant shaders and document that as a contract.

---

## Issue 6 — `preloadTexture()` ignores its `isAABitmap` parameter

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp`

**Snippet:**
```cpp
bool VulkanTextureManager::preloadTexture(int bitmapHandle, bool /*isAABitmap*/)
{
    return uploadImmediate(bitmapHandle, false);
}
```

**What the issue is**
- The parameter is ignored and the call hardcodes `false`.

**Why it’s a problem**
- If AA-bitmaps require special handling (format selection, upload sizing, or decoding), the preload path cannot request it.
- At minimum this is misleading API design; at worst it breaks preloading for AA textures.

**Suggested fix**
- Either wire the flag through (`uploadImmediate(bitmapHandle, isAABitmap)`) and make `uploadImmediate()` honor it, or remove the parameter entirely if it’s truly unnecessary.

---

## Issue 7 — 8bpp textures are treated as `R8` without palette expansion (likely wrong for palettized assets)

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`selectFormat` + upload copy logic)

**Snippet (format choice):**
```cpp
if ((bmp.flags & BMP_AABITMAP) || bmp.bpp == 8) {
    return vk::Format::eR8Unorm;
}
```

**Snippet (single-channel upload):**
```cpp
} else if (!compressed && singleChannel) {
    std::memcpy(..., frameBmp->data, layerSize);
}
```

**What the issue is**
- All 8bpp textures are treated as single-channel `R8` and uploaded by direct byte copy.

**Why it’s a problem**
- If the engine’s 8bpp textures include palettized images (common in older assets), `frameBmp->data` is likely indices into a palette, not grayscale intensity. Uploading indices as `R8` produces incorrect colors.
- Even if current content uses 8bpp only for AA-font alpha, the `bmp.bpp == 8` condition is broader than that assumption.

**Suggested fix**
- Distinguish AA-bitmaps (`BMP_AABITMAP`) from palettized textures.
- For palettized sources, expand to RGBA (or at least RGB) on upload and choose an appropriate `vk::Format` (likely `eB8G8R8A8Unorm` given the other paths).
- If the engine guarantees “8bpp == alpha-only” for Vulkan, enforce that with an assertion + fallback instead of silently uploading wrong data.

---

## Issue 8 — Oversized textures are marked `Failed` in the staged upload path (and may be repeatedly re-queued)

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`flushPendingUploads`, `queryDescriptor`)

**Snippet (staging budget check):**
```cpp
// Textures that can never fit in the staging buffer are marked as Failed
if (totalUploadSize > stagingBudget) {
    record.state = TextureState::Failed;
    continue;
}
```

**Snippet (re-queue behavior):**
```cpp
if (record.state == TextureState::Missing || record.state == TextureState::Failed) {
    if (!isUploadQueued(baseFrame)) {
        m_pendingUploads.push_back(baseFrame);
        record.state = TextureState::Queued;
    }
    result.queuedUpload = true;
}
```

**What the issue is**
- Any texture larger than the per-frame staging budget is marked `Failed` (i.e., it will never be uploaded via staging).
- `queryDescriptor()` will re-queue `Failed` textures again, so large textures can churn CPU (queued → failed → queued → …) whenever referenced.

**Why it’s a problem**
- Large textures (e.g., 4K RGBA) are valid and common; treating them as permanent failures can cause missing assets.
- The re-queue loop wastes time every frame those textures are requested.

**Suggested fix**
- Prefer a fallback upload strategy for oversized textures:
  - Call `uploadImmediate()` (dedicated staging) as a fallback when `totalUploadSize > stagingBudget`, or
  - Implement chunked uploads / a separate “large upload” queue.
- Make `Failed` terminal (or back off with a retry timer / reason code) to avoid perpetual re-queue churn.

---

## Issue 9 — Staging allocations can partially succeed then fail, wasting staging capacity that frame

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`flushPendingUploads`)

**Snippet (per-layer allocations):**
```cpp
for (uint32_t layer = 0; layer < layers; ++layer) {
    auto allocOpt = frame.stagingBuffer().try_allocate(layerSize);
    if (!allocOpt) {
        remaining.push_back(baseFrame);
        stagingFailed = true;
        break;
    }
    ...
}
```

**What the issue is**
- The code allocates per layer. If allocation fails mid-texture (or due to fragmentation), earlier layer allocations are already consumed and are not reclaimed until the frame’s staging buffer resets.

**Why it’s a problem**
- A single “almost fits” texture can temporarily starve other texture uploads in the same frame, causing avoidable stalls/deferrals.

**Suggested fix**
- Pre-compute total required staging for all layers and allocate a single contiguous chunk up front (then sub-allocate offsets yourself).
- Or add a “rollback” mechanism in the staging allocator when a multi-part allocation fails.

---

## Issue 10 — Potential `vkCmdCopyBufferToImage` offset alignment risk in the staged path (needs confirmation in `VulkanFrame`)

**File:** `code/graphics/vulkan/VulkanTextureManager.cpp` (`flushPendingUploads`)

**Snippet:**
```cpp
vk::BufferImageCopy region{};
region.bufferOffset = alloc.offset;
...
cmd.copyBufferToImage(frame.stagingBuffer().buffer(), ...);
```

**What the issue is**
- Vulkan requires `VkBufferImageCopy::bufferOffset` to satisfy alignment rules (commonly at least 4 bytes; compressed formats can impose additional constraints).
- Whether `alloc.offset` meets those rules depends entirely on the staging ring allocator’s alignment policy (in `VulkanFrame` / `VulkanRingBuffer`), which I have not yet reviewed in this pass.

**Why it’s a problem**
- If the staging allocator aligns only to `limits.optimalBufferCopyOffsetAlignment` (which can be 1), the staged upload path can emit invalid copy regions and trigger validation errors or device loss on strict drivers.

**Suggested fix**
- Ensure the staging allocator aligns offsets to `max(4, required-format-alignment, limits.optimalBufferCopyOffsetAlignment)` for copy-to-image usage.
- Alternatively, enforce alignment at call sites (round `alloc.offset` up and pad allocations).

---

## Issue 11 — `getTextureDescriptor()` can silently return empty descriptors if the texture manager is missing

**File:** `code/graphics/vulkan/VulkanRenderer.cpp` (`getTextureDescriptor`)

**Snippet:**
```cpp
if (!m_textureManager) {
    vk::DescriptorImageInfo empty{};
    return empty;
}
```

**What the issue is**
- If `m_textureManager` is null, the function returns an empty descriptor without logging or hard failure.

**Why it’s a problem**
- This is likely an unrecoverable renderer state. Returning empty descriptors can lead to confusing downstream crashes/validation errors rather than a clear failure.

**Suggested fix**
- Treat this as a hard error (assert/log + return a known-valid fallback descriptor if possible), or ensure the call sites can never reach this state.

---

## Issue 12 — Device-limit validation for bindless textures is incomplete (only checks one limit)

**File:** `code/graphics/vulkan/VulkanDescriptorLayouts.cpp`

**Snippet:**
```cpp
Assertion(limits.maxDescriptorSetSampledImages >= kMaxBindlessTextures, ...);
```

**What the issue is**
- The code validates `maxDescriptorSetSampledImages` but does not validate other relevant limits, such as:
  - `maxPerStageDescriptorSampledImages`
  - `maxDescriptorSetSamplers` (combined image samplers consume samplers too)
  - potentially `maxPerStageResources` depending on shader usage

**Why it’s a problem**
- A device might pass `maxDescriptorSetSampledImages` but still fail layout creation or pipeline creation due to other limit constraints.
- This could appear as a runtime failure rather than a clear “unsupported device” rejection.

**Suggested fix**
- Add checks for the other relevant limits based on the actual shader stage usage and descriptor types.
- If you rely on descriptor indexing features, ensure the feature+limit checks match the exact layout flags and shader access patterns you use.

---

End of “analysis so far”.
