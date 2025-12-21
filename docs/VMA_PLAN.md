### VMA Migration Plan (No Gaps)

This plan migrates Vulkan buffer/image allocations to Vulkan Memory Allocator (VMA) with **no semantic changes** to higher-level renderer behavior:

- VMA is used for allocation/free only (VMA does not record uploads or barriers).
- Existing staging + copy + synchronization behavior stays the same unless explicitly called out.
- Existing “write mapped memory, no flush” behavior is preserved by **requiring HOST_COHERENT** memory where the current code relies on it.
- “No gaps” in this plan means **no remaining `VkDeviceMemory` allocate/bind sites** for buffers/images in the Vulkan backend: every buffer/image allocation path migrates to `vmaCreateBuffer` / `vmaCreateImage` (including one-off staging buffers used by the texture manager).

Scope: Vulkan backend sources under `code/graphics/vulkan/` plus the build file list update needed to compile one new `.cpp` when Vulkan is enabled.

---

## Phase 0: Add VMA as a build dependency

**Goal:** Ensure `vk_mem_alloc.h` is available and compiled exactly once with `VMA_IMPLEMENTATION`.

1. Add VMA to the tree
   - Vendor VMA as a header-only dependency under `code/` so it is already on the include path (CMake sets `CODE_HEADERS` to `${CMAKE_SOURCE_DIR}/code`).
   - Recommended location: `code/graphics/vulkan/vk_mem_alloc.h`.
   - Include it consistently as `#include "graphics/vulkan/vk_mem_alloc.h"`.

2. Create a single “VMA implementation TU”
   - VMA configuration macros must be **consistent across all translation units** that include `vk_mem_alloc.h`. To prevent accidental config drift:
     - Add `code/graphics/vulkan/VmaConfig.h` that defines the required VMA macros (see below).
     - In every `.cpp` that includes `graphics/vulkan/vk_mem_alloc.h`, include `VmaConfig.h` immediately before it.
   - Add a new translation unit: `code/graphics/vulkan/VulkanVma.cpp`, containing only:
     ```cpp
     #define VMA_IMPLEMENTATION
     #include "graphics/vulkan/VmaConfig.h"
     #include "graphics/vulkan/vk_mem_alloc.h"
     ```
   - Rule: **no other file defines `VMA_IMPLEMENTATION`.**
   - Add `graphics/vulkan/VulkanVma.cpp` to the Vulkan source list in `code/source_groups.cmake` under the existing `if (FSO_BUILD_WITH_VULKAN)` block.

3. Add VMA forward declarations for header hygiene (do not redefine VMA handle typedefs)
   - **Do not** define `VmaAllocator` / `VmaAllocation` in a forward header: `vk_mem_alloc.h` defines them via `VK_DEFINE_HANDLE(...)`, and redeclaring them (even as identical typedefs/aliases) will cause compile-time conflicts in any `.cpp` that includes both the module header and `vk_mem_alloc.h`.
   - Add `code/graphics/vulkan/VmaFwd.h` that forward-declares the underlying opaque structs in the **global namespace** and provides non-conflicting pointer aliases for use in headers:
     ```cpp
     struct VmaAllocator_T;
     using VmaAllocatorPtr = VmaAllocator_T*;

     struct VmaAllocation_T;
     using VmaAllocationPtr = VmaAllocation_T*;
     ```
   - Rule:
     - Headers that store allocator/allocation pointers use `VmaAllocatorPtr` / `VmaAllocationPtr` and include only `VmaFwd.h`.
     - `.cpp` files that call VMA APIs include `graphics/vulkan/vk_mem_alloc.h`.

4. Add a minimal VMA config header used everywhere VMA is included
   - Add `code/graphics/vulkan/VmaConfig.h` containing only project-level VMA config macros (and no `VMA_IMPLEMENTATION`):
     ```cpp
     #pragma once

     // This backend uses VK_NO_PROTOTYPES + dynamic dispatch.
     #define VMA_STATIC_VULKAN_FUNCTIONS 0
     #define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
     ```
   - Rule: include `VmaConfig.h` immediately before `vk_mem_alloc.h` in every `.cpp` that calls VMA.

---

## Phase 1: VMA allocator lifetime (VulkanDevice)

**Goal:** Create/destroy one `VmaAllocator` with device lifetime and make it available to managers.

1. `code/graphics/vulkan/VulkanDevice.h`
   - Include `code/graphics/vulkan/VmaFwd.h`.
   - Add `VmaAllocatorPtr m_allocator = nullptr;`.
   - Add `VmaAllocatorPtr allocator() const { return m_allocator; }`.
   - Store Vulkan loader entry points needed by VMA dynamic dispatch:
     - Add members for `PFN_vkGetInstanceProcAddr` and `PFN_vkGetDeviceProcAddr` (or one plus a resolved cached copy of the other).

2. `code/graphics/vulkan/VulkanDevice.cpp`
   - Include `graphics/vulkan/VmaConfig.h` then `graphics/vulkan/vk_mem_alloc.h` (do not define `VMA_IMPLEMENTATION` here).
   - Persist the loader function pointer used today to initialize Vulkan-Hpp dispatch:
     - Store `PFN_vkGetInstanceProcAddr` obtained from SDL (`SDL_Vulkan_GetVkGetInstanceProcAddr()` in the existing code) into the new member.
     - Resolve/store `PFN_vkGetDeviceProcAddr` via `vkGetInstanceProcAddr` (this build uses `VK_NO_PROTOTYPES`, so do not rely on exported global symbols):
       - First try `m_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceProcAddr")`.
       - If that returns null, try `m_vkGetInstanceProcAddr(static_cast<VkInstance>(*m_instance), "vkGetDeviceProcAddr")`.
   - In `VulkanDevice::initialize()`:
     - Create the allocator **after** `createLogicalDevice()` succeeds and **before** any VMA-allocated resources are created (safe location: immediately before `createSwapchain()`).
     - Populate `VmaVulkanFunctions` with only:
       - `vkGetInstanceProcAddr`
       - `vkGetDeviceProcAddr`
     - Populate `VmaAllocatorCreateInfo` with:
       - `instance`, `physicalDevice`, `device` as raw `Vk*` handles (use `static_cast<VkInstance>(...)` / `static_cast<VkPhysicalDevice>(...)` / `static_cast<VkDevice>(...)` as needed with Vulkan-Hpp).
       - `pVulkanFunctions = &vulkanFunctions`
       - `vulkanApiVersion = m_properties.apiVersion`
     - Call `vmaCreateAllocator`. On failure, return `false` and keep `m_allocator == nullptr`.
   - In `VulkanDevice::shutdown()`:
     - After `m_device->waitIdle()` and before device teardown, call `vmaDestroyAllocator(m_allocator)` (if non-null) and set it to null.

**Lifetime rule:** Any object that owns VMA allocations (buffers/images) must be destroyed before `VulkanDevice` destroys `m_allocator`.

**Type/interop rule (minimize churn):**
- VMA APIs are C-style and use `Vk*` handles, but Vulkan-Hpp handle wrappers are binary-compatible with `Vk*`.
- Keep public interfaces that currently return `vk::Buffer` / `vk::Image` stable where possible; internally store raw `VkBuffer` / `VkImage` and wrap on return (e.g. `return vk::Buffer{m_buffer};`) to avoid cascading changes.

---

## Phase 2: Ring buffers (VulkanRingBuffer) + frame plumbing (VulkanFrame)

**Goal:** Replace manual buffer + VkDeviceMemory allocation for per-frame ring buffers with VMA, preserving “mapped + coherent, no flush required” behavior.

1. `code/graphics/vulkan/VulkanRingBuffer.h`
   - Include `code/graphics/vulkan/VmaFwd.h`.
   - Replace `vk::UniqueBuffer` + `vk::UniqueDeviceMemory` with:
     - `VkBuffer m_buffer = VK_NULL_HANDLE;`
     - `VmaAllocationPtr m_allocation = nullptr;`
     - `VmaAllocatorPtr m_allocator = nullptr;`
   - Keep `void* m_mapped` and ring bookkeeping.
   - Change constructor signature to accept `VmaAllocatorPtr` instead of `(vk::Device, memoryProps, ...)`.
   - Add destructor and explicit move operations (copy remains deleted).
   - Preserve the existing outward-facing API shape:
     - If callers currently consume `vk::Buffer`, keep `vk::Buffer buffer() const` and return a wrapped `vk::Buffer{m_buffer}`.

2. `code/graphics/vulkan/VulkanRingBuffer.cpp`
   - Include `graphics/vulkan/VmaConfig.h` then `graphics/vulkan/vk_mem_alloc.h`.
   - Replace creation with `vmaCreateBuffer` using:
     - `VkBufferCreateInfo` mirroring existing size/usage.
     - `VmaAllocationCreateInfo`:
       - `flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`
       - `usage = VMA_MEMORY_USAGE_AUTO` (selection is still constrained by `requiredFlags` below)
       - `requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`
     - Save `VmaAllocationInfo::pMappedData` to `m_mapped`.
   - Destructor:
     - If `m_buffer != VK_NULL_HANDLE`, call `vmaDestroyBuffer(m_allocator, m_buffer, m_allocation)` and null out fields.

3. `code/graphics/vulkan/VulkanFrame.h` / `code/graphics/vulkan/VulkanFrame.cpp`
   - Update `VulkanFrame` constructor to accept `VmaAllocatorPtr`.
   - Construct `m_uniformRing`, `m_vertexRing`, `m_stagingRing` from that allocator.

4. Call-site plumbing
   - In `VulkanRenderer::createFrames()`, pass `m_vulkanDevice->allocator()` to the `VulkanFrame` constructor.

---

## Phase 3: General buffers (VulkanBufferManager)

**Goal:** Migrate buffers managed by `VulkanBufferManager` to VMA while keeping current behavior (host-visible, mapped, coherent).

1. `code/graphics/vulkan/VulkanBufferManager.h`
   - Include `code/graphics/vulkan/VmaFwd.h`.
   - Update structs to store:
     - `VkBuffer buffer = VK_NULL_HANDLE;`
     - `VmaAllocationPtr allocation = nullptr;`
     - `void* mapped = nullptr;`
   - Add `VmaAllocatorPtr m_allocator = nullptr;`.
   - Remove `vk::PhysicalDeviceMemoryProperties` and the manual `findMemoryType()` logic (VMA selects memory).
   - Change constructor signature to accept `VmaAllocatorPtr` (keep `vk::Device`/queues if still required for non-allocation responsibilities).
   - Maintain existing outward-facing API return types (`vk::Buffer`) by wrapping raw `VkBuffer` handles when returning.
   - Add `~VulkanBufferManager()` that destroys all VMA-owned buffers (typically by calling `cleanup()`). Today the class relies on RAII (`vk::Unique*`) for shutdown; after migration, the destructor must actively free VMA allocations to avoid allocator leak reports.

2. `code/graphics/vulkan/VulkanBufferManager.cpp`
   - Include `graphics/vulkan/VmaConfig.h` then `graphics/vulkan/vk_mem_alloc.h`.
   - Replace manual create/allocate/bind with `vmaCreateBuffer`.
   - Preserve “memcpy without explicit flush” behavior by using:
     - `flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT`
     - `usage = VMA_MEMORY_USAGE_AUTO` (selection is still constrained by `requiredFlags` below)
     - `requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`
   - When creating/resizing:
     - Capture `VmaAllocationInfo::pMappedData` and store it in `buffer.mapped`.
     - Remove all direct `vkMapMemory`/`vkUnmapMemory` usage; mapping lifetime is managed by VMA and the plan relies on persistent mapping.
   - Keep `flushMappedBuffer()` as:
     - no-op if not mapped, else `vmaFlushAllocation(m_allocator, allocation, offset, size)` (safe even if coherent; future-proofs if coherency constraints change later).
   - Deferred deletion:
     - Store `{VkBuffer, VmaAllocationPtr}` in `m_retiredBuffers`.
     - When retiring is complete, destroy via `vmaDestroyBuffer(m_allocator, buffer, allocation)`.
   - `cleanup()` destroys all live buffers via VMA before clearing containers.

3. Call-site plumbing
   - Update `VulkanRenderer::initialize()` to construct `VulkanBufferManager` with `m_vulkanDevice->allocator()`.

---

## Phase 4: Textures (VulkanTextureManager)

**Goal:** Allocate texture images with VMA while keeping existing upload logic (staging ring buffer + `copyBufferToImage`) intact.

1. `code/graphics/vulkan/VulkanTextureManager.h`
   - Include `code/graphics/vulkan/VmaFwd.h`.
   - In `VulkanTexture`, replace:
     - `vk::UniqueImage image;` -> `VkImage image = VK_NULL_HANDLE;`
     - `vk::UniqueDeviceMemory memory;` -> `VmaAllocationPtr allocation = nullptr;`
   - Keep `vk::UniqueImageView imageView` and borrowed `vk::Sampler sampler`.
   - Add `VmaAllocatorPtr m_allocator = nullptr;` to the manager.
   - Change constructor signature to accept `VmaAllocatorPtr` (keep `vk::Device`/queues if still used for command submission or sampler creation).
   - Add `~VulkanTextureManager()` that releases all VMA-owned images/buffers (typically by calling `cleanup()`). Today the class relies on RAII (`vk::Unique*`) for shutdown; after migration, the destructor must actively free VMA allocations to avoid allocator leak reports.

2. `code/graphics/vulkan/VulkanTextureManager.cpp`
   - Include `graphics/vulkan/VmaConfig.h` then `graphics/vulkan/vk_mem_alloc.h`.
   - Add a single helper responsible for correct destruction order, e.g. `destroyTextureGpu(VulkanTexture&)`:
     1) `imageView.reset()` (and reset any other views referencing the image)
     2) if `image != VK_NULL_HANDLE`: `vmaDestroyImage(m_allocator, image, allocation)`
     3) null out `image`/`allocation`
   - Migrate **all** current image allocation sites to VMA (must cover every path that creates/binds `vk::Image` today):
     - `createFallbackTexture()`
     - `uploadImmediate()`
     - The main upload path in `flushPendingUploads()` where `record.gpu.image` is currently created/bound.
   - Use `vmaCreateImage` with device-local preference:
     - `VmaAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`
     - Keep `requiredFlags` empty for device-local images unless the existing code has a concrete requirement.
   - Do not assume “textures must be dedicated allocations” by default; only add `VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT` if there is a measured need.
   - Staging (no gaps):
     - Leave `flushPendingUploads()` staging as the per-frame `VulkanRingBuffer` (migrated in Phase 2).
     - Migrate one-off staging buffers used by `createFallbackTexture()` and `uploadImmediate()` to VMA as well:
       - Use `vmaCreateBuffer` with `VMA_MEMORY_USAGE_AUTO_PREFER_HOST` plus `HOST_VISIBLE|HOST_COHERENT` requirements.
       - Keep the existing “map → memcpy → unmap” behavior (or switch to VMA persistent mapping for these staging buffers only if it does not change semantics).
   - Destruction paths:
     - `deleteTexture(int bitmapHandle)` must call `destroyTextureGpu(record.gpu)` before erasing.
     - `cleanup()` must iterate all textures and destroy GPU resources before clearing.
     - `processPendingDestructions(uint64_t completedSerial)` must destroy GPU resources before erasing.
   - Remove texture-manager `findMemoryType()` (VMA replaces it).

3. Call-site plumbing
   - Update `VulkanRenderer::initialize()` to construct `VulkanTextureManager` with `m_vulkanDevice->allocator()`.

---

## Phase 5: Render targets (VulkanRenderTargets)

**Goal:** Migrate depth + G-buffer images to VMA with explicit view-before-image destruction on both resize and shutdown.

1. `code/graphics/vulkan/VulkanRenderTargets.h`
   - Include `code/graphics/vulkan/VmaFwd.h`.
   - Replace image/memory ownership:
     - Depth: `VkImage m_depthImage = VK_NULL_HANDLE;` + `VmaAllocationPtr m_depthAllocation = nullptr;`
     - G-buffer: `std::array<VkImage, kGBufferCount> m_gbufferImages{}` + `std::array<VmaAllocationPtr, kGBufferCount> m_gbufferAllocations{}`
   - Keep:
     - `vk::UniqueImageView` members
     - `vk::UniqueSampler` members
   - Add `VmaAllocatorPtr m_allocator = nullptr;`.
   - Add `~VulkanRenderTargets()` and/or an explicit `destroy()` helper used by both destructor and resize.

2. `code/graphics/vulkan/VulkanRenderTargets.cpp`
   - Include `graphics/vulkan/VmaConfig.h` then `graphics/vulkan/vk_mem_alloc.h`.
   - Store allocator (from `VulkanDevice::allocator()`).
   - Create images via `vmaCreateImage` (device-local preference):
     - `VmaAllocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE`
   - Destruction (shared helper used by both destructor and resize) must be:
     1) Reset all image views (`m_depthImageView`, `m_depthSampleView`, every `m_gbufferViews[i]`).
     2) Destroy depth image via `vmaDestroyImage`.
     3) Destroy each G-buffer image via `vmaDestroyImage`.
     4) Null out handles/allocations.
   - `resize()` must call the same destruction helper before recreating; reassignment is no longer sufficient once images are raw handles.

---

## Validation checklist (targeted, minimal)

- Build succeeds with VMA compiled exactly once (no multiple-definition errors).
- No header introduces VMA types in a namespace that doesn’t match VMA’s global C API.
- No Vulkan-backend code path still uses manual `VkDeviceMemory` allocate/bind for buffers/images (sanity check: grep for `allocateMemoryUnique`, `bindBufferMemory`, `bindImageMemory` in `code/graphics/vulkan/`).
- All VMA-owned images/buffers are destroyed with correct ordering relative to dependent Vulkan objects:
  - All `VkImageView` destroyed/reset before `vmaDestroyImage`.
- Ring buffers and buffer manager preserve current “mapped + coherent” assumptions (no new flush requirements introduced by default).
