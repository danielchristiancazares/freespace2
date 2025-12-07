# Core Definitions & Memory — `code/globalincs`

This directory provides the foundational types, memory management, and global constants used throughout the engine. Every subsystem depends on these files.

## Critical Files

### 1. Memory Management
*   **`vmallocator.h`**: Defines `SCP_vector`, `SCP_string`, `SCP_map`, etc.
    *   **Usage:** Prefer `SCP_vector` over `std::vector` for all engine code. It adds bounds-checking safety and uses the engine's memory tracking.
    *   **Includes:** `globalincs/vmallocator.h`
*   **`memory/memory.h`**: Defines `vm_malloc` and `vm_free`.
    *   **Behavior:** `vm_malloc` wraps `std::malloc` but **crashes on Out-Of-Memory (OOM)** via `memory::out_of_memory()`. This "fail-fast" behavior is intentional.
    *   **Safety:** Do not mix `vm_malloc/vm_free` with `new/delete` or `std::malloc/std::free`.
    *   **Smart Pointers:** Use `SCP_vm_unique_ptr<T>` for C-style arrays allocated with `vm_malloc`.

### 2. Global Types (`pstypes.h`)
This is the "pre-compiled header" equivalent for the engine's type system.
*   **Fixed-Point Math:** `typedef int fix;` (Legacy fixed-point type, still used in some physics/AI code).
*   **Vectors:** `vec3d` (float[3]), `vec4` (float[4]), `uv_pair`.
    *   *Note:* These are POD structs, not classes. Helper functions are in `code/math/vecmat.h`.
*   **Matrices:** `matrix` (3x3 row-major) and `matrix4` (4x4 column-major).
*   **Assertions:** `Assert(expr)` and `Int3()` (debug break).
    *   **Policy:** Never comment out an Assert. If it fires, the state is invalid.

### 3. Engine Constants (`globals.h`)
Defines the hard limits of the engine.
*   `MAX_SHIPS` (500)
*   `MAX_OBJECTS` (5000)
*   `MAX_WEAPONS` (3000)
*   `MAX_PLAYERS` (12)

### 4. System Variables (`systemvars.h`)
Global state flags and detail levels.
*   **Game Mode:** `Game_mode` (e.g., `GM_MULTIPLAYER`, `GM_NORMAL`).
*   **Detail Levels:** `detail_levels` struct controls rendering fidelity (nebula, particles, lighting).
*   **Time:** `Missiontime`, `Frametime` (fixed-point seconds).

## Key Design Patterns

### The "VM" Allocator Strategy
The engine uses a split memory strategy:
1.  **C-Style Buffers:** `vm_malloc` (Legacy file parsing, bitmap data).
2.  **C++ Objects:** `new` / `delete`.
3.  **Containers:** `SCP_vector` (uses `std::allocator`, but wrapped for debug checks).

**Vulkan Interaction:** The Vulkan backend (via VMA) allocates GPU memory directly, bypassing `vm_malloc`. However, staging buffers (CPU-side data prep) often use `SCP_vector` or `vm_malloc`.

### Type-Safe Globals
Wherever possible, the engine is moving towards strongly-typed enums and `flagset<T>` instead of raw bitmasks.
*   **Old:** `#define FLAG_A (1<<0)`
*   **New:** `enum class Flags : ubyte { A, B }; flagset<Flags> myFlags;`

## Common Pitfalls
*   **Mixing Allocators:** Passing a `vm_malloc` pointer to `delete`, or a `new` pointer to `vm_free` will corrupt the heap.
*   **Vector Bounds:** `SCP_vector` asserts on out-of-bounds access. This is stricter than `std::vector::operator[]`.
*   **Matrix Order:** `matrix` is **row-major**. `matrix4` is **column-major** (OpenGL style). Be careful when converting.
