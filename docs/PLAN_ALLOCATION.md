# Implementation Plan: Memory Sub-Allocation (Item 2)

## Objective
Implement a memory sub-allocator within `VulkanBufferManager` to solve the "Memory Allocation Limit" risk.
The goal is to replace the current 1-to-1 Buffer-to-Memory allocation strategy with a Page-based aggregation strategy, keeping the number of `vk::DeviceMemory` allocations well below the physical device limit (typically 4096).

## Problem Analysis
*   **Current State:** `VulkanBufferManager::resizeBuffer` calls `m_device.allocateMemoryUnique` for every buffer.
*   **Risk:** Applications using many small buffers (e.g., individual static meshes, particle systems, dynamic UI elements) will quickly exhaust the `maxMemoryAllocationCount` limit, causing crashes or allocation failures.
*   **Location:** `code/graphics/vulkan/VulkanBufferManager.cpp` and `.h`.

## Research: Industry Patterns

Before designing, I surveyed established patterns:

1. **VMA (Vulkan Memory Allocator)** - Uses opaque `VmaAllocation` handles with explicit `vmaDestroyBuffer()`. Not RAII, but industry standard. ([VMA Documentation](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/))

2. **Generational Handles** - Game engines use index+generation pairs to detect use-after-free at O(1) cost. When a slot is reused, its generation increments. Stale handles have mismatched generations. ([Handles vs Pointers](https://floooh.github.io/2018/06/17/handles-vs-pointers.html), [Generational Indices Guide](https://lucassardois.medium.com/generational-indices-guide-8e3c5f7fd594))

3. **Typestate in Rust vs C++** - Rust's move semantics prevent access to moved-from values. C++ allows it, requiring sentinel states. True typestate is impossible in C++. ([Rust Typestate](https://cliffle.com/blog/rust-typestate/))

4. **Intrusive Containers** - Boost.Intrusive provides O(1) container operations without pointer invalidation. ([Boost.Intrusive](https://theboostcpplibraries.com/boost.intrusive))

## Strategy: Generational Handle + RAII Hybrid

Combine RAII (scope manages lifetime) with generational handles (graceful use-after-free detection). This is more robust than pure RAII with nullptr checks.

### Core Concepts

1.  **Memory Page:**
    *   Represents a large single allocation of `vk::DeviceMemory` (e.g., 64MB).
    *   Contains slots for sub-allocations, each with a generation counter.
    *   Uses intrusive list node for O(1) movement between state containers.

2.  **Generational Handle:**
    *   Compact identifier: `{pageIndex, slotIndex, generation}`.
    *   Generation mismatch = stale handle (use-after-free detected).
    *   Sentinel generation (0) = explicitly invalid.

3.  **Sub-Allocation (RAII Wrapper):**
    *   Owns a handle; destructor frees via pool.
    *   Move invalidates source handle (sets generation to sentinel).
    *   All accessors validate handle through pool (graceful failure on misuse).

### Implementation Steps

#### 1. Data Structures

```cpp
// Generational handle - 64 bits total, same size as pointer
struct AllocationHandle {
    uint16_t pageIndex;
    uint16_t slotIndex;
    uint32_t generation;

    static constexpr uint32_t INVALID_GENERATION = 0;
    [[nodiscard]] bool isValid() const { return generation != INVALID_GENERATION; }
};

// Slot within a page - tracks one sub-allocation
struct AllocationSlot {
    vk::DeviceSize offset;
    vk::DeviceSize size;
    uint32_t generation;  // Incremented on each reuse; starts at 1
    bool inUse;
};

// Forward declarations
class MemoryPagePool;

// IMMOVABLE core - destructor frees, no moved-from state possible
class AllocationCore {
    friend class MemoryPagePool;

    AllocationHandle m_handle;
    MemoryPagePool* m_pool;

    AllocationCore(AllocationHandle handle, MemoryPagePool* pool);

public:
    ~AllocationCore();

    // DELETED: No moved-from state can exist
    AllocationCore(AllocationCore&&) = delete;
    AllocationCore& operator=(AllocationCore&&) = delete;
    AllocationCore(const AllocationCore&) = delete;
    AllocationCore& operator=(const AllocationCore&) = delete;

    // Accessors go through pool validation (generational check)
    [[nodiscard]] vk::DeviceMemory memory() const;
    [[nodiscard]] vk::DeviceSize offset() const;
    [[nodiscard]] vk::DeviceSize size() const;
    [[nodiscard]] void* mappedPtr() const;
    [[nodiscard]] AllocationHandle handle() const { return m_handle; }
};

// MOVABLE wrapper - delegates moved-from handling to unique_ptr
class SubAllocation {
    std::unique_ptr<AllocationCore> m_core;

public:
    explicit SubAllocation(std::unique_ptr<AllocationCore> core);

    // Moves are handled by unique_ptr - well-tested, standard behavior
    SubAllocation(SubAllocation&&) = default;
    SubAllocation& operator=(SubAllocation&&) = default;

    SubAllocation(const SubAllocation&) = delete;
    SubAllocation& operator=(const SubAllocation&) = delete;

    // Accessors assert + graceful fallback
    [[nodiscard]] vk::DeviceMemory memory() const;
    [[nodiscard]] vk::DeviceSize offset() const;
    [[nodiscard]] vk::DeviceSize size() const;
    [[nodiscard]] void* mappedPtr() const;

    // Explicit validity check (for conditional logic at boundaries)
    [[nodiscard]] bool isValid() const { return m_core != nullptr; }

    // Consume and release handle (rvalue-qualified: forces std::move)
    [[nodiscard]] AllocationHandle release() &&;
};
```

**Typestate for Pages** - Different types prevent invalid operations:

```cpp
// Base page data - shared by all page states
class MemoryPageData {
protected:
    vk::UniqueDeviceMemory m_memory;
    void* m_mappedBase = nullptr;
    vk::DeviceSize m_totalSize;
    uint32_t m_memoryTypeIndex;
    std::vector<AllocationSlot> m_slots;

    // Free list embedded in slots (indices, not pointers)
    uint16_t m_firstFreeSlot = UINT16_MAX;  // UINT16_MAX = none
    size_t m_usedSlotCount = 0;

public:
    [[nodiscard]] uint32_t memoryTypeIndex() const { return m_memoryTypeIndex; }
    [[nodiscard]] vk::DeviceMemory memory() const { return m_memory.get(); }
};

// Page that CAN accept allocations
class PageWithSpace : public MemoryPageData, public boost::intrusive::list_base_hook<> {
public:
    // This method EXISTS - you can allocate from this page
    [[nodiscard]] std::optional<AllocationHandle> tryAllocate(
        vk::DeviceSize size, vk::DeviceSize alignment, uint16_t pageIndex);

    // Validates handle and returns slot data (or nullopt if generation mismatch)
    [[nodiscard]] std::optional<AllocationSlot*> validateHandle(AllocationHandle h);

    // Returns memory to page. Returns new state.
    enum class AfterFree { StillHasSpace, BecameEmpty };
    AfterFree freeSlot(uint16_t slotIndex);
};

// Page that CANNOT accept allocations (full)
class FullPage : public MemoryPageData, public boost::intrusive::list_base_hook<> {
public:
    // NO tryAllocate method - compile error if you try!

    [[nodiscard]] std::optional<AllocationSlot*> validateHandle(AllocationHandle h);

    // After freeing, page has space again
    AfterFree freeSlot(uint16_t slotIndex);  // Always returns StillHasSpace
};

// Page that is completely empty (can be released)
class EmptyPage : public MemoryPageData, public boost::intrusive::list_base_hook<> {
public:
    // NO tryAllocate - must promote to PageWithSpace first
    // NO validateHandle - no valid handles exist for empty pages

    // Promote to allocatable state
    [[nodiscard]] PageWithSpace promoteToActive();
};
```

**State-as-Location with Intrusive Lists:**

```cpp
class MemoryPagePool {
    // Intrusive lists - O(1) insert/remove, no pointer invalidation
    boost::intrusive::list<PageWithSpace> m_pagesWithSpace;
    boost::intrusive::list<FullPage> m_fullPages;
    boost::intrusive::list<EmptyPage> m_emptyPages;

    // Page storage - owns the actual page objects
    // Using deque: stable addresses, no reallocation invalidation
    std::deque<std::variant<PageWithSpace, FullPage, EmptyPage>> m_pageStorage;

    vk::Device m_device;

public:
    // Returns owned RAII wrapper
    [[nodiscard]] SubAllocation allocate(
        vk::DeviceSize size,
        vk::DeviceSize alignment,
        uint32_t memoryTypeBits,
        bool hostVisible);

    // Handle-based accessors - validate generation internally
    [[nodiscard]] std::optional<vk::DeviceMemory> getMemory(AllocationHandle h) const;
    [[nodiscard]] std::optional<vk::DeviceSize> getOffset(AllocationHandle h) const;
    [[nodiscard]] std::optional<void*> getMappedPtr(AllocationHandle h) const;

    // Free by handle - called by SubAllocation destructor
    void free(AllocationHandle h);

    // For deferred deletion
    void freeDeferred(AllocationHandle h);

    // Release unused memory
    void releaseEmptyPages(size_t keepCount = 1);
};
```

**Updated VulkanBuffer:**

```cpp
struct VulkanBuffer {
    vk::UniqueBuffer buffer;
    SubAllocation allocation;  // RAII - auto-frees on destruction

    BufferType type;
    BufferUsageHint usage;
    bool isPersistentMapped;

    // Accessors delegate to allocation (which validates via pool)
    [[nodiscard]] vk::DeviceSize size() const { return allocation.size(); }
    [[nodiscard]] void* mapped() const { return allocation.mappedPtr(); }
};
```

#### 2. Key Implementation Details

**Immovable Core + Movable Wrapper:**

```cpp
// Core: RAII with no moved-from state (moves deleted)
AllocationCore::~AllocationCore() {
    m_pool->free(m_handle);  // Always valid - no moved-from state exists
}

// Wrapper: Moves delegated to unique_ptr
SubAllocation::SubAllocation(SubAllocation&&) = default;  // unique_ptr handles it

void* SubAllocation::mappedPtr() const {
    // Layer 1: Assert in debug (catches bugs during development)
    assert(m_core && "Use of moved-from SubAllocation");

    // Layer 2: Graceful fallback in release (doesn't crash in production)
    if (!m_core) return nullptr;

    // Layer 3: Generational validation (detects use-after-free)
    return m_core->mappedPtr();  // Delegates to pool validation
}

// Rvalue-qualified: forces caller to write std::move(alloc).release()
AllocationHandle SubAllocation::release() && {
    assert(m_core && "Releasing invalid SubAllocation");
    AllocationHandle h = m_core->handle();
    m_core.reset();  // Prevent destructor from freeing
    return h;
}
```

**Why This Design:**
- `AllocationCore` has NO moved-from state (moves deleted)
- `SubAllocation` uses `unique_ptr` for moves (standard, battle-tested)
- Our code has ZERO sentinel value handling
- Debug builds catch misuse with clear assert message
- Release builds fail gracefully (nullptr return, not crash)
- `release() &&` makes consumption explicit (must write `std::move`)

**Generation Validation:**

```cpp
std::optional<void*> MemoryPagePool::getMappedPtr(AllocationHandle h) const {
    if (!h.isValid() || h.pageIndex >= m_pageStorage.size()) {
        return std::nullopt;
    }

    auto& pageVariant = m_pageStorage[h.pageIndex];
    return std::visit([&](auto& page) -> std::optional<void*> {
        auto slot = page.validateHandle(h);
        if (!slot) return std::nullopt;  // Generation mismatch
        return page.mappedBase() + (*slot)->offset;
    }, pageVariant);
}

std::optional<AllocationSlot*> PageWithSpace::validateHandle(AllocationHandle h) {
    if (h.slotIndex >= m_slots.size()) return std::nullopt;
    auto& slot = m_slots[h.slotIndex];
    if (slot.generation != h.generation) return std::nullopt;  // Stale handle
    if (!slot.inUse) return std::nullopt;  // Already freed
    return &slot;
}
```

**State Transitions via Type Changes:**

```cpp
void MemoryPagePool::free(AllocationHandle h) {
    if (!h.isValid()) return;

    auto& pageVariant = m_pageStorage[h.pageIndex];

    std::visit(overloaded{
        [&](PageWithSpace& page) {
            auto result = page.freeSlot(h.slotIndex);
            if (result == AfterFree::BecameEmpty) {
                m_pagesWithSpace.erase(m_pagesWithSpace.iterator_to(page));
                pageVariant = EmptyPage(std::move(page));  // Typestate transition
                m_emptyPages.push_back(std::get<EmptyPage>(pageVariant));
            }
        },
        [&](FullPage& page) {
            page.freeSlot(h.slotIndex);  // Always becomes PageWithSpace
            m_fullPages.erase(m_fullPages.iterator_to(page));
            pageVariant = PageWithSpace(std::move(page));  // Typestate transition
            m_pagesWithSpace.push_back(std::get<PageWithSpace>(pageVariant));
        },
        [&](EmptyPage&) {
            // Invalid: can't free from empty page (no valid handles exist)
            // This is a logic error, not a runtime state
        }
    }, pageVariant);
}
```

#### 3. Deferred Release for GPU Synchronization

```cpp
struct RetiredAllocation {
    AllocationHandle handle;  // Just data, not ownership
    // Pool frees when this expires
};

// In SubAllocation: release handle for deferred deletion
AllocationHandle SubAllocation::release() {
    AllocationHandle h = m_handle;
    m_handle.generation = AllocationHandle::INVALID_GENERATION;
    return h;
}

// In deleteBuffer:
m_deferredReleases.enqueue(retireSerial,
    RetiredAllocation{buffer.allocation.release()});

// When RetiredAllocation expires:
m_pool.freeDeferred(retired.handle);
```

### Design Principles Analysis

| Principle | Old Design | New Design |
|-----------|------------|------------|
| **Sentinel values** | `m_page = nullptr` | `generation = 0` (same issue, but...) |
| **Use-after-free** | Crashes | Returns nullopt (graceful) |
| **Pointer stability** | `MemoryPage*` can invalidate | Handle indices are stable |
| **State-as-location** | Same-type containers | Different types per state |
| **Compile-time safety** | Can call allocate on any page | FullPage has no allocate method |

**"Locking the Door" - Making Misuse Difficult**

C++ can't prevent access to moved-from values, but that's no excuse for leaving the door wide open. Here's how we make misuse as hard as possible:

**Layer 1: Immovable Core + Movable Wrapper**
```cpp
// Core is immovable - NO moved-from state exists
class AllocationCore {
    AllocationCore(AllocationCore&&) = delete;
    AllocationCore(const AllocationCore&) = delete;
    ~AllocationCore() { m_pool->free(m_handle); }
    // ... handle, pool, accessors ...
};

// Wrapper delegates to unique_ptr's well-tested moved-from handling
class SubAllocation {
    std::unique_ptr<AllocationCore> m_core;
public:
    // All accessors require valid core
    void* mappedPtr() const {
        assert(m_core && "Use of moved-from SubAllocation");
        return m_core->mappedPtr();
    }
};
```

**Layer 2: Debug-Mode Assertions**
```cpp
void* SubAllocation::mappedPtr() const {
    assert(m_core && "Use of moved-from SubAllocation");  // Debug: crash with message
    if (!m_core) return nullptr;  // Release: graceful failure
    return m_core->mappedPtr();
}
```

**Layer 3: Rvalue-Qualified Consumption (Optional)**
```cpp
// Forces caller to write std::move(alloc).release()
[[nodiscard]] AllocationHandle release() && {
    assert(m_core);
    return m_core->releaseHandle();
}
```

**Why This is Better:**
- Moved-from handling delegated to `unique_ptr` (standard, battle-tested)
- Our code has zero sentinel value handling
- Debug builds crash with clear message on misuse
- Release builds fail gracefully (nullptr, not crash)
- Rvalue qualifiers make consumption explicit

This is "locking the door" - not perfect security, but making the wrong thing obviously wrong.

### Migration Plan

1.  **Add Handle Types:** Implement `AllocationHandle`, `AllocationSlot`.
2.  **Add Page Types:** Implement `PageWithSpace`, `FullPage`, `EmptyPage` with intrusive hooks.
3.  **Implement Pool:** Build `MemoryPagePool` with generational validation.
4.  **RAII Wrapper:** Implement `SubAllocation` with handle-based accessors.
5.  **Wire Up:** Replace `VulkanBuffer::memory` with `SubAllocation allocation`.
6.  **Update Deferred Release:** Use `AllocationHandle` for deferred deletion.

### Dependencies

- **Boost.Intrusive** - For O(1) container operations ([Boost.Intrusive docs](https://www.boost.org/libs/intrusive/))
- Or implement simple intrusive list (just next/prev pointers in page)

## Verification

*   **Allocation Count Test:** Create 5000 small buffers (1KB each).
    *   **Expected:** ~1 `vk::DeviceMemory` object (Success).
*   **Use-After-Free Test:** Move from SubAllocation, then access it.
    *   **Expected:** Returns nullopt, does not crash.
*   **Generation Wraparound Test:** Allocate/free same slot 2^32 times.
    *   **Expected:** Slot becomes permanently disabled (generation overflow).
*   **State Transition Test:** Verify pages move between containers correctly.
*   **Compile-Time Safety Test:** Try to call `tryAllocate` on `FullPage`.
    *   **Expected:** Compile error.

## References

- [Vulkan Memory Allocator](https://gpuopen.com/vulkan-memory-allocator/) - AMD's production allocator
- [Handles vs Pointers](https://floooh.github.io/2018/06/17/handles-vs-pointers.html) - Why game engines use handles
- [Generational Indices Guide](https://lucassardois.medium.com/generational-indices-guide-8e3c5f7fd594) - Implementation details
- [Rust Typestate Pattern](https://cliffle.com/blog/rust-typestate/) - Why C++ can't do true typestate
- [C++ slot_map proposal P0661R0](https://open-std.org/jtc1/sc22/wg21/docs/papers/2017/p0661r0.pdf) - Standard proposal
- [Boost.Intrusive](https://theboostcpplibraries.com/boost.intrusive) - Intrusive containers
