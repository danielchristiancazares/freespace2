# Vulkan Remediation Plan (Bulletproof Refactor)

Date: 2025-12-21

Scope: Remediate issues identified in `docs/QA_REVIEW.md` for `code/graphics/vulkan/`, prioritizing correctness and making
invalid states non-representable via typestate, RAII, and type-driven design.

---

## 0) Target Invariants (Non-Negotiable)

1) No "descriptor request" API may record GPU work (no barriers/copies/allocations). It may only:
   - return an already-valid descriptor (possibly fallback), and/or
   - queue work for a later, explicitly safe phase.
2) No Vulkan resource (`VkImage`, `VkImageView`, `VkBuffer`, etc.) may be destroyed while any in-flight submission may still
   reference it (push descriptors included).
3) "Upload allowed" vs "rendering active" must be enforced by types/ownership, not comments.
4) Frame lifecycle exposes one monotonic "GPU completed" signal used for all deferred destruction.

---

## 1) Foundation: Frame/Timeline + Deferred Destruction (DONE)

### 1.1 One source of truth for GPU completion (DONE)

- A monotonic `m_completedSerial` is tracked from FIFO fence waits.
- `m_completedSerial` is passed to `VulkanTextureManager::collect()` and `VulkanBufferManager::collect()`.

Note: the current completion signal relies on FIFO frame recycling. If the renderer ever moves away from that assumption,
prefer a global timeline semaphore as the source of truth.

### 1.2 Defer destruction by serial (RAII-safe) (DONE)

- `DeferredReleaseQueue` implemented (`code/graphics/vulkan/VulkanDeferredRelease.h`).
- Managers enqueue moved RAII handles with a `retireSerial` and destroy them after `collect(completedSerial)`.

### 1.3 Fix frame reuse: exactly one reset path (DONE)

- Frame "reuse prep" happens exactly once per recycle.
- `completedSerial` is populated from a real completion point, not a stub value.

---

## 2) Texture System Rewrite (DONE For Current Architecture)

### 2.1 Split into components (DONE)

- `VulkanTextureBindings`: draw-path API, no command buffer access, returns fallback on miss.
- `VulkanTextureUploader`: upload-phase API, requires `UploadCtx` token.
- `VulkanTextureManager`: internal state; upload flush is not callable from draw code.

### 2.2 State as Location (DONE)

Per `docs/DESIGN_PHILOSOPHY.md`, texture state is container membership:

- `m_residentTextures`: `unordered_map<int, ResidentTexture>` - presence = resident
- `m_pendingUploads`: `vector<int>` - presence = queued
- `m_unavailableTextures`: `unordered_map<int, UnavailableTexture>` - presence = permanently unavailable (domain-real)
- `m_bindlessSlots`: `unordered_map<int, uint32_t>` - presence = has slot assigned
- `m_pendingBindlessSlots`: `unordered_set<int>` - retry slot assignment at frame start
- `m_pendingRetirements`: `unordered_set<int>` - defer slot reuse to upload phase (frame-start safe point)

No state enum. No `std::variant`. Transitions are moves between containers.

### 2.3 Enforce upload-only-before-rendering via typed contexts (PARTIAL)

- `UploadCtx` implemented: private constructor, `friend class VulkanRenderer`.
- Upload recording requires `UploadCtx`; upload during rendering becomes a compile-time error.
- `RenderCtx` not yet implemented (see Section 4).

### 2.4 Bindless slot semantics: slot always points at something valid (DONE)

- Slot 0 reserved for fallback.
- `getBindlessSlotIndex()` returns a valid slot index (returns 0 on pressure/unavailable).
- Non-resident slots sample fallback until upload completes.
- Model bindless descriptor array is written with fallback first, then patched with resident textures.
- The model bindless binding does not use `vk::DescriptorBindingFlagBits::ePartiallyBound` (all descriptors are written).

### 2.5 Eviction/deletion policy is serial-safe by construction (DONE baseline)

- `lastUsedSerial` tracked per texture.
- Eviction only if `lastUsedSerial <= completedSerial`.
- Resident eviction and slot reuse only at upload phase (frame-start safe point).
- Mid-frame: only reclaim non-resident slot mappings (safe because those slots already point to fallback that frame).
- `deleteTexture()` defers retirement to upload phase (`m_pendingRetirements`) to prevent mid-frame slot reuse.

---

## 3) Rendering Session Lifetime (NEXT)

Refactor `VulkanRenderingSession` so the active dynamic-rendering pass lifetime is owned by the frame/session (not by a
temporary scope returned from helper calls).

Options:
- Store `std::optional<ActivePass>` on `VulkanFrame` and manage it explicitly at boundaries, or
- Make `VulkanRenderingSession` own it and end it on target switches/endFrame.

Acceptance:
- It's always clear (and enforceable) whether rendering is active.

---

## 4) Typestate Enforcement (NEXT)

### 4.1 `RenderCtx` token

Introduce `RenderCtx` (owns `RenderScope`, proves rendering is active):
- Only constructible when dynamic rendering is started.
- Passed to draw functions as proof of phase.
- Prevents upload calls during rendering (compile-time).

### 4.2 Migrate draw APIs

- `gr_vulkan_render_primitives()` receives `RenderCtx&`.
- `gr_vulkan_render_model()` receives `RenderCtx&`.
- Texture binding functions accept `RenderCtx&` for LRU tracking (and to make "draw path" explicit in signatures).

---

## 5) Performance Cleanup (NEXT)

### 5.1 Frame-owned pass lifetime

Move from per-draw `RenderScope` RAII to frame-owned pass lifetime. End the pass explicitly at target switch or frame end.

### 5.2 Dirty-slot-only descriptor updates

Track which bindless slots changed since last frame and write only dirty slots instead of rewriting the full array.

---

## 6) Cleanups (DONE / LOW)

1) Hard-coded debug file IO removed from Vulkan hot paths.
2) Validation callback logs via `vkprintf`.
3) `VulkanFrame::reset()` compiles in current configuration.
4) `VulkanShaderReflection.cpp` remains (low priority).

---

## Milestone Summary

| Milestone | Description | Status |
|-----------|-------------|--------|
| 1 | Foundation: serial tracking, deferred destruction, frame reuse | DONE |
| 2 | Texture system: state-as-location, fallback-first, phase safety | DONE (UploadCtx) |
| 3 | Bulletproof lifetime: serial-safe eviction, deferred retirement | DONE (baseline) |
| 4 | Typestate enforcement: RenderCtx, compile-time phase checking | NEXT |
| 5 | Performance: frame-owned passes, dirty-slot descriptors | NEXT |
