# Implementation Plan: Batch Model Descriptor Updates

## Objective
Reduce CPU overhead and driver calls per frame by batching model descriptor updates into a single `vkUpdateDescriptorSets` call, rather than updating one descriptor at a time.

## Problem Analysis
Currently, `VulkanRenderer::beginModelDescriptorSync` iterates over every resident texture (potentially hundreds) and calls `vkUpdateDescriptorSets` individually for each via `writeTextureDescriptor`.
-   **Impact:** `vkUpdateDescriptorSets` is a high-overhead driver call. Invoking it hundreds of times per frame causes significant CPU time consumption on the driver thread.
-   **Location:** `code/graphics/vulkan/VulkanRenderer.cpp`

## Implementation Steps

### 1. Refactor `VulkanRenderer::beginModelDescriptorSync`
Replace the per-descriptor `updateDescriptorSets(1, ...)` calls with a single batched `updateDescriptorSets(writes, {})` call.

*   **Containers:**
    *   `std::vector<vk::WriteDescriptorSet> writes`: Stores all write operations for the frame.
    *   `std::vector<vk::DescriptorImageInfo> imageInfos`: Stores texture descriptor structures referenced by `writes` via pointers.
    *   `std::vector<vk::DescriptorBufferInfo> bufferInfos`: Stores buffer descriptor structures referenced by `writes` via pointers.

    **Pointer stability note:** `vk::WriteDescriptorSet` stores raw pointers to `vk::DescriptorImageInfo` / `vk::DescriptorBufferInfo`. Using `std::vector` is fine as long as you `reserve()` enough upfront so the vectors do not reallocate while building `writes`. This repo already uses the `vector + reserve + &back()` pattern for descriptor batching (e.g. `updateModelDescriptors()`), so prefer the same approach here for consistency. If you cannot confidently reserve an upper bound, then use a pointer-stable container (e.g. `std::deque`) instead.

*   **Logic Flow:**
    1.  **Vertex Heap (Binding 0):**
        *   Construct `vk::DescriptorBufferInfo`, push to `bufferInfos`, then create a `vk::WriteDescriptorSet` whose `pBufferInfo` points at `&bufferInfos.back()`.
    2.  **Fallback UBO (Binding 2):**
        *   Repeat the same `bufferInfos` + pointer write logic for the dynamic UBO binding.
    3.  **Textures (Binding 1):**
        *   Iterate `m_textureManager->allTextures()`.
        *   Filter for `Resident` state and valid `arrayIndex`.
        *   Retrieve `vk::DescriptorImageInfo` via `m_textureManager->getTextureDescriptorInfo` using the same sampler behavior as `writeTextureDescriptor` (repeat + linear).
        *   Push to `imageInfos`, then create a write whose `pImageInfo` points at `&imageInfos.back()`.
    4.  **Retired Slots (Binding 1):**
        *   Iterate `m_textureManager->getRetiredSlots()`.
        *   Retrieve fallback texture info using the same sampler behavior as `writeFallbackDescriptor` (repeat + nearest).
        *   Push to `imageInfos` / `writes` to overwrite stale slots with the fallback texture.
    5.  **Execute:**
        *   Call `m_vulkanDevice->device().updateDescriptorSets(writes, {})` exactly once.
    6.  **Retired slot lifecycle (preserve existing call):**
        *   After the batch update, call `m_textureManager->clearRetiredSlotsIfAllFramesUpdated(frameIndex)`.
        *   This existing call is essential: it ensures retired slots are only returned to the free pool after all per-frame descriptor sets have had fallback textures written. Omitting it will break slot recycling.
    7.  **Reserve sizing (important):**
        *   Precompute counts so `reserve()` is sufficient to avoid reallocations:
            *   `bufferInfos.reserve(2)` (binding 0 + binding 2).
            *   `imageInfos.reserve(residentTextureWrites + retiredSlotWrites)`.
            *   `writes.reserve(2 + residentTextureWrites + retiredSlotWrites)`.

### 2. Cleanup `VulkanRenderer::flip`
*   The current implementation of `flip()` contains a loop that checks for `newlyResident` textures and writes them individually.
*   Since `beginModelDescriptorSync()` already writes all resident textures each frame today, and will continue to do so after batching, the `flip()` loop is redundant.
*   **Action:** Remove the redundant descriptor update loop in `flip()` to avoid double-writes.
*   **Bookkeeping:** If the `flip()` loop is removed, also ensure `m_newlyResidentTextures` is cleared elsewhere (or delete the tracking entirely). Today it is cleared in `flip()`, so removing that loop without relocating the clear will leave stale entries around (even if they no longer drive descriptor updates).

### 3. Deprecate/Inline Helper Functions
The following single-update helper functions will likely be removed or inlined into the batched loop:
*   `writeVertexHeapDescriptor`
*   `writeTextureDescriptor`
*   `writeFallbackDescriptor`

## Expected Outcome
*   Reduction in driver overhead / CPU frame time.
*   Elimination of validation warnings related to frequent descriptor updates (if any).
*   Cleaner code structure for frame start synchronization.
