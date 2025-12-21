# Implementation Plan: Memory Sub-Allocation (Item 2)

## Objective
Implement a memory sub-allocator within `VulkanBufferManager` to solve the "Memory Allocation Limit" risk.
The goal is to replace the current 1-to-1 Buffer-to-Memory allocation strategy with a Page-based aggregation strategy, keeping the number of `vk::DeviceMemory` allocations well below the physical device limit (typically 4096).

## Problem Analysis
*   **Current State:** `VulkanBufferManager::resizeBuffer` calls `m_device.allocateMemoryUnique` for every buffer.
*   **Risk:** Applications using many small buffers (e.g., individual static meshes, particle systems, dynamic UI elements) will quickly exhaust the `maxMemoryAllocationCount` limit, causing crashes or allocation failures.
*   **Location:** `code/graphics/vulkan/VulkanBufferManager.cpp` and `.h`.

## Strategy: Page-Based Free-List Allocator
Instead of integrating a heavy external library (like VMA) which might require build system changes, we will implement a lightweight **Page-based Free-List Allocator** directly inside `VulkanBufferManager`.

### Core Concepts

1.  **Memory Page:**
    *   Represents a large single allocation of `vk::DeviceMemory` (e.g., 64MB or 128MB).
    *   Properties: `vk::UniqueDeviceMemory`, `memoryTypeIndex`, `totalSize`, `mappedPointer` (if host visible).
    *   Contains a **Free List**: A sorted list of free memory ranges `{offset, size}` within the page.

2.  **Sub-Allocation:**
    *   Buffers are bound to `(Page.memory, PageOffset)`.
    *   The `VulkanBuffer` struct will reference its host Page instead of owning `vk::UniqueDeviceMemory`.

3.  **Coalescing:**
    *   When memory is freed, it is returned to the Page's free list.
    *   Adjacent free blocks are merged to prevent fragmentation.

### Implementation Steps

#### 1. Data Structures (Internal Headers)
Modify `code/graphics/vulkan/VulkanBufferManager.h` to include allocator structures.

```cpp
struct FreeBlock {
    vk::DeviceSize offset;
    vk::DeviceSize size;
};

struct MemoryPage {
    vk::UniqueDeviceMemory memory;
    void* mappedBase = nullptr; // If mapped
    vk::DeviceSize totalSize;
    uint32_t memoryTypeIndex;
    std::vector<FreeBlock> freeList;
    
    // Helpers
    bool tryAllocate(vk::DeviceSize size, vk::DeviceSize alignment, vk::DeviceSize& outOffset);
    void free(vk::DeviceSize offset, vk::DeviceSize size);
};

// Update VulkanBuffer struct
struct VulkanBuffer {
    // ... existing type/usage fields ...
    vk::UniqueBuffer buffer;
    
    // NEW: No longer owns DeviceMemory directly
    int16_t pageIndex = -1; // Index into manager's page list
    vk::DeviceSize pageOffset = 0;
    
    void* mapped = nullptr; // Calculated: page->mappedBase + pageOffset
    size_t size = 0;
};
```

#### 2. Logic: Allocation (`resizeBuffer`)
Refactor `VulkanBufferManager::resizeBuffer`:
1.  **Create Buffer:** Create `vk::Buffer` to query `vk::MemoryRequirements` (size, alignment, typeBits).
2.  **Find Page:**
    *   Iterate through existing `std::vector<MemoryPage>`.
    *   Check compatibility: `memoryTypeIndex` matches, and `tryAllocate` returns true.
    *   `tryAllocate`: Scans `freeList` for a block >= `alignedSize`. Uses "Best Fit" or "First Fit".
3.  **New Page (Fallback):**
    *   If no page fits, allocate a new `MemoryPage`.
    *   Size = `std::max(kDefaultPageSize, requiredSize)`. `kDefaultPageSize` ≈ 64MB.
    *   Map persistently if `HostVisible` usage is requested.
4.  **Bind:**
    *   `m_device.bindBufferMemory(buffer, page.memory.get(), offset)`.
    *   Update `VulkanBuffer` tracking info (`pageIndex`, `pageOffset`).

#### 3. Logic: Deallocation (`deleteBuffer` / `cleanup`)
Refactor `deleteBuffer` and `RetiredBuffer` handling:
*   **Deferred Free:** `RetiredBuffer` currently holds `vk::UniqueDeviceMemory`. It must change to hold allocation info `{ int16_t pageIndex, vk::DeviceSize offset, vk::DeviceSize size }`.
*   **Frame End Processing:**
    *   In `onFrameEnd`, when a `RetiredBuffer` expires:
    *   Call `m_pages[retired.pageIndex].free(retired.offset, retired.size)`.
*   **Page Release:**
    *   After freeing, check if `m_pages[i]` is **100% free** (one free block of `totalSize`).
    *   If yes, and we have > 1 empty pages (hysteresis), destroy the Page to release physical memory.

#### 4. Logic: Coalescing (`MemoryPage::free`)
1.  Insert the freed block `{offset, size}` into `freeList` (sorted by offset).
2.  Iterate the list once:
    *   If `current.offset + current.size == next.offset`, merge them.
    *   `current.size += next.size`. Remove `next`.

### Migration Plan
1.  **Prepare:** Define the constants (`kPageSize = 64 * 1024 * 1024`).
2.  **Modify Structs:** Update `VulkanBuffer` and `RetiredBuffer`. This breaks the build until logic is updated.
3.  **Implement Allocator:** Add `MemoryPage` logic (allocate/free/coalesce).
4.  **Wire Up:** Rewrite `resizeBuffer` to use the allocator.
5.  **Cleanup:** Update `deleteBuffer` and `onFrameEnd` to route through the allocator.

## Verification
*   **Test:** Create a test case allocating 5000 small buffers (1KB each).
*   **Expectation:**
    *   **Old:** 5000 `vk::DeviceMemory` objects (Crash).
    *   **New:** ~1 `vk::DeviceMemory` object (Success).
*   **Validation:** Verify buffer data integrity (mapping works correctly with offsets).

