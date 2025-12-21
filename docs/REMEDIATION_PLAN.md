# Vulkan Remediation Plan (Bulletproof Refactor)

Date: 2025-12-21

Scope: Remediate issues identified in `docs/QA_REVIEW.md` for `code/graphics/vulkan/`, prioritizing
correctness and making invalid states non-representable via typestate, RAII, and type-driven design.

---

## Current State (Local Changes As Of 2025-12-21)

- `VulkanRenderer::getTextureDescriptor()` no longer flushes uploads while dynamic rendering may be active; on miss it
  queues the upload and returns a fallback descriptor.
- Frame reuse now tracks a real `m_completedSerial` and runs `prepareFrameForReuse()` exactly once per recycled frame.
- Hard-coded "agent log" file IO was removed from Vulkan hot paths.
- Texture eviction/deletion was changed to defer destruction (serial-gated) instead of immediate RAII teardown.
- Resource retirement during `beginFrame()` is now guarded against the *upcoming* submit serial (prevents premature destruction before the frame completes).
- Bindless slot eviction only considers textures whose `lastUsedSerial <= completedSerial` (no eviction of in-flight textures).
- Cache eviction now drops cache state immediately and moves GPU handles into the deferred release queue (no long-lived `Retired` records blocking re-requests).

## 0) Target Invariants (Non-Negotiable)

1) No “descriptor request” API may record GPU work (no barriers/copies/allocations). It may only:
   - return an already-valid descriptor (possibly fallback), and/or
   - queue work for a later, explicitly safe phase.
2) No Vulkan resource (`VkImage`, `VkImageView`, `VkBuffer`, etc.) may be destroyed while any in-flight submission
   may still reference it (push descriptors included).
3) “Upload allowed” vs “rendering active” must be enforced by types/ownership, not comments.
4) Frame lifecycle exposes one monotonic “GPU completed” signal used for *all* deferred destruction.

---

## 1) Foundation: Frame/Timeline + Deferred Destruction (Prerequisite)

### 1.1 One source of truth for GPU completion

Add `VulkanSubmitTimeline` (owned by `VulkanRenderer`):
- `uint64_t nextSerial()` increments per submit.
- `uint64_t completedSerial()` monotonically increases.
- Update `completedSerial()` from either:
  - **Preferred:** a global timeline semaphore (signal per submit, query counter each frame), or
  - **Fallback:** fence-ordered completion (only if frames are strictly recycled FIFO).

Deliverable:
- A single `completedSerial` value is available at frame begin/recycle and is meaningful (not always `0`).

### 1.2 Defer destruction by serial (RAII-safe)

Add `DeferredReleaseQueue`:
- `enqueue(retireSerial, MoveOnlyResource)` (or type-erased callable).
- `collect(completedSerial)` destroys anything whose `retireSerial <= completedSerial`.

Migrate to it:
- `VulkanTextureManager` (or replacement) image/view destruction.
- `VulkanBufferManager` “retired buffers” (replace `FRAMES_BEFORE_DELETE` with serial-based safety).

### 1.3 Fix frame reuse: exactly one reset path

Refactor `VulkanRenderer` frame lifecycle so:
- Frame “reuse prep” (reset, buffer manager `onFrameEnd`, texture GC) happens exactly once per recycle.
- `AvailableFrame.completedSerial` is populated based on a real completion point, not a stub.

Acceptance for section 1:
- Serial-based destruction is wired end-to-end and used by at least one subsystem.

---

## 2) Texture System Rewrite (Make “Flush While Rendering” Impossible)

### 2.1 Split into three components

Replace the current “all-in-one” texture manager with:

1) **TextureCache** (pure state, no command buffer)
   - Maps `TextureId` (base frame) -> `TextureRecord`.
   - Owns persistent GPU objects via RAII handles (prefer `std::shared_ptr<GpuTexture>`).
   - Implements slot assignment policy (bindless) and eviction decisions.

2) **TextureUploader** (records GPU work; upload phase only)
   - Consumes queued requests + staging allocator.
   - Produces `GpuTexture` objects + a list of “dirty slots” needing descriptor writes.

3) **TextureBindings** (what draw code uses)
   - `bind2D(TextureId, SamplerKey, RenderCtx)` returns a descriptor (fallback if needed) and a keepalive token.
   - `bindlessIndex(TextureId, RenderCtx)` returns a stable slot index and queues upload if needed.

Key rule:
- Bindings never flush uploads; they only queue work.

### 2.2 Typestate makes invalid states non-representable

Replace `TextureState` + nullable members with:
`std::variant<Missing, Queued, Resident, Failed, Retiring>`, where:
- `Resident` contains `std::shared_ptr<GpuTexture>` (never null).
- `Retiring` contains GPU object + `retireSerial`.

This prevents “Resident but no imageView”, etc.

### 2.3 Enforce upload-only-before-rendering via typed contexts

Introduce phase-typed contexts:
- `UploadCtx` (command buffer recording, dynamic rendering not started)
- `RenderCtx` (dynamic rendering active)

Only `UploadCtx` can call:
- `TextureUploader::recordUploads(UploadCtx&)`
- `TextureBindings::flushDirtyBindlessDescriptors(UploadCtx&)`

Only `RenderCtx` is passed into draw calls.

Result:
- “Flush while rendering” becomes a compile-time error.

### 2.4 Bindless slot semantics: slot always points at something valid

Change bindless handling so:
- Slot 0 is reserved fallback (black).
- A texture gets a stable slot on first request (even if not resident yet).
- Until resident, its slot descriptor points at fallback/default.
- When upload completes, the slot is marked dirty and updated at frame start (upload phase).

This fixes model path issues where only an index is requested (no upload occurs).

### 2.5 RAII lifetime for push descriptors: per-frame keepalive

Add a `FrameKeepAlive` container to `VulkanFrame`:
- `std::vector<std::shared_ptr<void>> keepAlive;` (or a typed wrapper).

Every bound texture returns a keepalive token stored in the current frame.
- Cache eviction drops cache references, but in-flight frames keep the GPU object alive.

### 2.6 Eviction policy becomes serial-safe by construction

Track `lastUsedSerial` per texture.
Evict only if `lastUsedSerial <= completedSerial`. If no safe eviction exists:
- Do not evict; use fallback for that request and try later.

Acceptance for section 2:
- No path in draw code can record texture uploads mid-pass.
- Texture destruction/eviction can’t free resources referenced by in-flight frames.

---

## 3) Rendering Session Lifetime (Stability + Simpler Rules)

Refactor `VulkanRenderingSession` so the active dynamic-rendering pass lifetime is owned by the frame/session
(not by “temporary scope” returned from helper calls).

Options:
- Store `std::optional<ActivePass>` on `VulkanFrame` and manage it explicitly at boundaries, or
- Make `VulkanRenderingSession` own it and end it on target switches/endFrame.

Acceptance:
- It’s always clear (and enforceable) whether rendering is active.

---

## 4) Remove Footguns / Cleanups

1) Remove or gate all hard-coded “agent log” file IO in hot paths:
   - `code/graphics/vulkan/VulkanGraphics.cpp`
   - `code/graphics/vulkan/VulkanRenderer.cpp`
   - `code/graphics/vulkan/VulkanTextureManager.cpp`
2) Validation callback must log useful messages (warnings/errors) via `vkprintf`:
   - `code/graphics/vulkan/VulkanDevice.cpp`
3) Fix `VulkanFrame::reset()` so it compiles in both exception/no-exception Vulkan-Hpp modes:
   - `code/graphics/vulkan/VulkanFrame.cpp`
4) Remove or implement dead `VulkanShaderReflection.cpp` (currently header duplicate).

---

## 5) Concrete Execution Milestones (Breaking Allowed)

### Milestone 1: Make current code “safe-ish” and unblock refactor
- Remove lazy flush from `VulkanRenderer::getTextureDescriptor()` (never record uploads there).
- Add `VulkanSubmitTimeline` + `DeferredReleaseQueue`.
- Fix frame reuse so `completedSerial` is real; remove double reuse-prep.

### Milestone 2: Introduce new texture components (fallback-first)
- Add `TextureId` + new `TextureCache/TextureUploader/TextureBindings` skeleton.
- Primitive path: always return a valid descriptor (fallback on miss) and queue upload.
- Model path: request stable bindless slot (queues upload) instead of returning `MODEL_OFFSET_ABSENT` silently.

### Milestone 3: Bulletproof lifetime + eviction
- Add per-frame keepalive, wire it through both primitive and model binding code.
- Use serial-based retirement for textures; remove immediate erases.
- Implement safe eviction (`lastUsedSerial <= completedSerial`).

### Milestone 4: Typestate enforcement
- Introduce `UploadCtx`/`RenderCtx` and migrate APIs so invalid call order cannot compile.
- Add asserts as backstop (debug builds).

### Milestone 5: Rendering session simplification + perf cleanup
- Move to frame-owned pass lifetime.
- Descriptor updates become “dirty slot only” instead of rewriting all slots every frame.

---

## 6) Verification Strategy

- Add unit tests for:
  - slot assignment + safe eviction logic
  - typestate compilation boundaries (where applicable)
- Add “validation hard-fail” option for debug:
  - Debug utils callback logs and can assert on `ERROR` severity.
- Add runtime assertion backstops:
  - Upload recording asserts “rendering not active” even if typestate should prevent it.
