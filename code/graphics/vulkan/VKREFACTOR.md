Below is a refactor plan that’s meant to be **incremental**, keeps the current external `gr_*` API mostly intact, and directly targets **four** goals:
* **Each object owns the state it needs to make decisions**
* **No retry loops / no exception-based control flow for coordination**
* **Invalid states unrepresentable**
* **Ownership boundaries aligned with Vulkan’s resource model**

---

## 0) Snapshot the current coordination problems (what we’re fixing)

These are the “pressure points” that drive the refactor shape:

### A. Texture residency is coordinated via exceptions + “end pass and retry”

* `VulkanTextureManager::getDescriptor(...)` requires the caller to pass `renderPassActive` and explicitly “refuses to return dummy fallbacks”, so the *texture manager* can’t decide safely without *render-session state passed in*. 
* When a texture isn’t resident and a render pass is active, the texture manager throws to force the renderer to end rendering and retry. 
* `VulkanRenderer::getTextureDescriptor` catches that exception, ends the render pass, flushes uploads, and restarts rendering. 

This violates:

* “each object owns state it needs” (texture manager needs render state passed in),
* “no exception-based control flow for coordination” (exceptions are being used as state signals),
* and creates “implicit retry” behavior.

### B. Upload budget / ring-buffer exhaustion uses exceptions and can wrap inside a frame

* `VulkanTextureManager::flushPendingUploads` may catch exceptions from staging allocation, then changes record state and defers. 
* `VulkanRingBuffer::allocate` can wrap “within a frame” (explicit FIXME) and throws on exhaustion. 

Wrapping within a frame makes “overwriting in-flight data” representable, i.e. *invalid states are representable*.

### C. Model descriptor set design currently pushes toward huge pools + confusing ownership

* `kModelSetsPerPool = 4096 * kFramesInFlight`  and the model pool allocates `kModelSetsPerPool * kMaxBindlessTextures` combined image sampler descriptors. 
* `validateDeviceLimits` checks `maxDescriptorSetStorageBuffers >= kModelSetsPerPool` which is not the right interpretation of that limit (you only bind **1** storage buffer per set at binding 0). 

Also: texture-manager tracks “descriptorWritten per frame”, which is really *descriptor-set state*, not texture-resource state. 

### D. Render-session state is spread across flags and external “knowledge”

`VulkanRenderingSession` uses booleans like `m_renderPassActive` and mode switching logic. 
Additionally, swapchain layout transitions always start from `eUndefined`, while load-op may be `eLoad` depending on “clear flags” — that’s a representable invalid combo unless you guarantee clear. 

---

## 1) Target architecture (ownership boundaries that mirror Vulkan)

This is the ownership map the plan converges to:

### Device-lifetime owners

* **`VulkanDevice`**: instance/device/queues/swapchain images & views, pipeline cache. (Already mostly correct.) 
* **`VulkanDescriptorLayouts`**: set layouts + descriptor pools, nothing else. 
* **`VulkanShaderManager` + reflection validator**: owns shader modules and validates their bindings against layout contracts. (You already have a reflection API surface.) 

### Frame-lifetime owners (one per frame-in-flight)

* **`VulkanFrame`**: command pool/buffer, semaphores/fence/timeline, ring buffers (uniform/vertex/staging), and *per-frame descriptor sets* that are safe to mutate after its fence wait. 

### Resource managers (multi-frame lifetime, but “free when serial completes”)

* **`VulkanBufferManager`**: owns VkBuffer/VkDeviceMemory and their lifetimes; retirement keyed by a renderer-provided completion serial instead of “magic frame counts”.
* **`VulkanTextureManager`**: owns VkImage/VkImageView/VkSampler handles, plus texture streaming state. It should **not** own per-frame descriptor-set write tracking.

### Render orchestration

* **`VulkanRenderingSession`**: owns *rendering state machine* and attachment layout tracking (swapchain/gbuffer/depth). No other subsystem should need to ask “is renderpass active?”. 
* **`VulkanRenderer`**: “glue”: frame acquisition/submission + tells subsystems when a new frame starts, when a serial completes, and provides accessors.

---

## 2) Core refactor principle: capability-based APIs (make invalid states unrepresentable)

Right now the texture path passes `bool renderPassActive` and throws if it’s wrong. 

Replace this with **capability objects** that can only exist when it’s legal to do something.

### Example: `UploadContext` is only created when it’s legal to record copies/barriers

```cpp
struct UploadContext {
  VulkanFrame& frame;
  vk::CommandBuffer cmd;      // valid + begun
  uint32_t frameIndex;        // ring index
  // access to staging allocator etc.
};
```

Only `VulkanRenderer`/`VulkanRenderingSession` can create an `UploadContext`, and **only when no rendering is active**. That makes “upload during rendering” unrepresentable *by construction*.

---

## 3) Phase-by-phase refactor plan

### Phase 1 — Eliminate exception-driven coordination in texture residency (highest ROI)

**Goal coverage:** No exception-based control flow; texture manager owns its decisions; invalid states harder to represent.

#### 1.1 Replace `VulkanTextureManager::getDescriptor(...)` with a non-throwing query

Current signature includes `renderPassActive` and throws for mid-pass requests.  

Propose two functions:

1. **Always safe, never uploads:**

```cpp
struct TextureDescriptorQuery {
  vk::DescriptorImageInfo info;  // always valid
  bool isFallback;               // true if not resident
  bool queuedUpload;             // whether we queued work
};

TextureDescriptorQuery queryDescriptor(int bitmapHandle,
                                       const SamplerKey& samplerKey);
```

2. **Explicit “do uploads now” function** (only callable with capability):

```cpp
void flushUploads(UploadContext& up);
```

How `queryDescriptor` behaves:

* If resident → returns real `imageView/sampler`.
* If not resident → queues upload request **and returns fallback descriptor** (no throw).
  (You already have a fallback texture handle and descriptor helper. )

This removes:

* the “throw to force caller behavior”, and
* the “end renderpass and retry” path in `VulkanRenderer::getTextureDescriptor`. 

#### 1.2 Move “same-frame upload if safe” to a single explicit place (optional but nice)

If you want to preserve the current behavior where early-in-frame first use can upload immediately, do it **without retries**:

* At the moment you’re *about to start rendering* (i.e., before `beginRendering`), call `textureManager.flushUploads(uploadCtx)` once.
* If the draw requests textures *before* rendering has begun, those requests will be queued and then flushed right before beginRendering.

This removes state coordination from random call sites and makes it deterministic.

You already have a single “start rendering” chokepoint via `ensureRenderingStarted`. 
So the refactor is:

* `getTextureDescriptor()` becomes pure query + queue.
* `ensureRenderingStarted()` does “flush queued uploads if any” *before* transitioning into active rendering.

#### 1.3 Replace staging allocation exceptions with `try_allocate`

Right now upload flushing catches exceptions from staging allocation. 
And the underlying ring buffer can wrap within a frame and throws. 

Refactor `VulkanRingBuffer`:

* Remove intra-frame wrapping entirely (it’s explicitly suspect). 
* Provide:

```cpp
std::optional<RingAlloc> try_allocate(size_t size, size_t alignment);
size_t remaining() const;
```

Then `flushUploads`:

* Iterates queued textures.
* For each, compute upload size.
* If `uploadSize > stagingCapacity` → mark texture `FailedStreaming` and keep fallback (no assertion crash). (Today it hard-asserts on “too big”.) 
* If `try_allocate` fails → keep queued for later.

No exceptions, no implicit retries.

#### 1.4 Normalize texture state so “Resident implies view+layout ready”

Today, state and resource fields can contradict (state Resident but view missing; layout not shader-read triggers transitions and can throw).  

Refactor `TextureRecord` into a sum type:

```cpp
struct Missing { ... };
struct Queued { ... };
struct Resident { VulkanTexture gpu; /* gpu.imageView guaranteed non-null */ };
struct Failed { ... };
struct Retired { ... };

using TextureState = std::variant<Missing, Queued, Resident, Failed, Retired>;
```

Now “Resident but no imageView” is unrepresentable.

---

### Phase 2 — Fix model descriptor set architecture and pool sizing (remove huge pool pressure + clarify ownership)

**Goal coverage:** ownership boundaries, invalid states unrepresentable, fewer “global flags”.

#### 2.1 Decide: per-frame model set (recommended first) vs split sets (later)

Right now:

* You do a per-frame descriptor sync path (`beginModelDescriptorSync`) in `VulkanRenderer`. 
* But model drawing also allocates per-draw model sets and updates them. 
* Plus `setModelUniformBinding` updates `frame.modelDescriptorSet`, so it assumes a per-frame model set is what’s bound. 

**Recommended near-term**: commit to **one model descriptor set per frame-in-flight**, bound for every model draw in that frame.

Changes:

* Remove/stop using “per-draw allocateModelDescriptorSet” in the model draw path (keep helper available if needed elsewhere). 
* Ensure the bound set for model draws is `frame.modelDescriptorSet`, which is what `setModelUniformBinding` writes to. 

This collapses lots of confusion and makes the state consistent.

#### 2.2 Shrink the model descriptor pool

Currently the model pool sizes scale by `kModelSetsPerPool = 4096*kFramesInFlight`. 
and then `descriptorCount = kModelSetsPerPool * kMaxBindlessTextures`. 

If you allocate only **kFramesInFlight** model sets, pool sizing becomes:

* storage buffers: `kFramesInFlight`
* uniform dynamic: `kFramesInFlight`
* combined image sampler: `kFramesInFlight * kMaxBindlessTextures`

This is still big-ish but orders of magnitude smaller than the current 8M-ish descriptor count implied by the current constants.

Also fix `validateDeviceLimits`:

* Keep `maxDescriptorSetSampledImages >= kMaxBindlessTextures` if the model set truly contains that many. 
* Replace the storage-buffer check: it should be `>= 1` (or >= the count of storage buffers in the model set layout), not `>= kModelSetsPerPool`. 

#### 2.3 Move “descriptorWritten per frame” out of texture manager (ownership fix)

Texture manager currently tracks per-frame descriptor-set write status. 
That’s descriptor-set ownership bleeding into the texture system.

Introduce a `BindlessTextureSetUpdater` owned by the renderer/frame layer:

* Owns `vk::DescriptorSet modelSet` (or one per frame).
* Tracks “dirty slots” for that set (bitset or small vector).
* Writes fallback descriptors for retired slots.

The texture manager then exposes:

* “is resident?” and “descriptor image info for handle or fallback”.

This aligns boundaries:

* Texture manager owns texture resources and streaming state.
* Descriptor updater owns descriptor set write state.

---

### Phase 3 — Render-session typestate: make renderpass-active transitions unrepresentable

**Goal coverage:** invalid states unrepresentable, fewer “global booleans”, better ownership.

#### 3.1 Replace `m_renderPassActive + m_activeMode + m_pendingMode` with a state variant

Today `ensureRenderingActive` relies on boolean flags and mode comparisons. 

Refactor to:

```cpp
struct Idle { RenderMode pending; };
struct RenderingSwapchain { uint32_t imageIndex; };
struct RenderingGBuffer { /* maybe attachments */ };

using RenderState = std::variant<Idle, RenderingSwapchain, RenderingGBuffer>;
```

Then:

* `beginFrame()` sets state to `Idle{ pending = Swapchain }` (or deferred based on your rules).
* `ensureRenderingActive()` transitions from `Idle → RenderingX`.
* `endRendering()` transitions `RenderingX → Idle`.

Now you can *only* call “rendering-only operations” when state is RenderingX (checked by variant).

#### 3.2 Track attachment layouts as state, not inferred defaults

You already have layout transitions in `beginFrame/endFrame` and in gbuffer transitions, but the swapchain transition uses `oldLayout = eUndefined` even when load-op could be `eLoad`. 

Make swapchain layout state explicit:

* Keep an array `swapchainImageLayout[imageCount]` in `VulkanRenderingSession`.
* After present, mark it `ePresentSrcKHR`.
* Before rendering:

  * If you will clear, you *can* transition from `eUndefined` **only if you also guarantee clear** (otherwise it’s a representable invalid state).
  * Otherwise transition from `ePresentSrcKHR`.

This removes the “clear flags + layout mismatch” invalid combo.

#### 3.3 Move `depthInitialized` into an image state tracker

Render targets currently keep `m_depthInitialized` and session consults it to pick `oldLayout`. 
Instead:

* Make each owned image carry its own “current layout” (swapchain images in session, gbuffer/depth in render-targets or a shared tracker).
* Expose `RenderTargets::transitionDepth(cmd, newLayout)` that uses and updates its own layout state.

Now the object that owns the image also owns its layout state.

---

### Phase 4 — Resource retirement and deletion based on “completion serial”, not frame-count guesses

**Goal coverage:** clear Vulkan-style lifetime boundaries; invalid states unrepresentable (no UAF by design).

#### 4.1 Unify on a single monotonic “GPU serial”

You already track a `m_submitSerial` and record submit info per frame. 
Formalize:

* `FrameSerial` increments once per submit.
* Each resource retirement records `safeSerial = currentSubmitSerial`.
* You maintain `completedSerial` from fences/timeline.

Then:

* BufferManager deletion uses `completedSerial` not “FRAMES_BEFORE_DELETE=3”. 
* TextureManager’s pending destructions use the same serial, not `m_currentFrame + kFramesInFlight` (which currently relies on a `setCurrentFrame` that isn’t obviously wired into the renderer loop). 

This mirrors Vulkan’s model: “resource can be destroyed once the GPU has passed point X”.

#### 4.2 Make handles generational (buffers/textures)

For buffers, you currently store handle indices and can erase entries; that makes stale handles representable.

Introduce:

```cpp
struct Handle { uint32_t index; uint32_t generation; };
```

* Store generation per slot.
* Increment generation on delete.
* Every lookup checks generation.

Now “use-after-free handle” is unrepresentable as a successful lookup.

---

### Phase 5 — Make shader/layout mismatch states unrepresentable (reflection + contracts)

**Goal coverage:** invalid states unrepresentable; clearer boundaries.

You already have a reflection API surface declared (`VulkanShaderReflection.h`). 
And you have “layout contracts” mapping `shader_type → PipelineLayoutKind/VertexLayoutKind` in `VulkanLayoutContracts`. 

Implement and enforce:

* On shader load, reflect descriptor sets/bindings from SPIR-V.
* Validate against expected contract:

  * Standard shaders must only use set 0 push bindings 0..2 and set 1 global (if needed).
  * Model shaders must match the model set layout (binding 0 SSBO, binding 1 bindless array, binding 2 dynamic UBO). 

Fail fast at init (or disable that shader path) rather than letting runtime bind mismatches exist.

---

## 4) Practical “implementation order” I’d actually do (min risk, max payoff)

If you want the shortest path to your goals without a huge rewrite:

1. **Texture path:** make `getDescriptor` non-throwing + fallback, delete the renderer try/catch end-pass-retry logic. 
2. **Ring buffer:** implement `try_allocate` + remove intra-frame wrap; update texture flushing to use it. 
3. **Model descriptors:** commit to per-frame model set; remove per-draw allocation; shrink pool + fix device-limit checks.
4. **Render-session typestate:** replace boolean flags with variant state; track swapchain layout correctly.
5. **Serial-based retire/free:** unify around submit serial and completed serial; fix buffer/texture deletion.
6. **Shader reflection validation:** wire reflection + contracts to make layout mismatches impossible.

---

## 5) Expected behavioral changes (and how to control them)

### Textures

* Without “end pass and retry”, **a texture first used mid-pass will render as fallback until next safe flush**.

  * Mitigation: preloading (you already have `preloadTexture`). 
  * Mitigation: flush queued uploads at “render mode transitions” (end deferred geometry, etc.) when rendering is idle.

### Validation / correctness

* The big win: you remove a whole class of hard-to-debug “works unless a texture appears late” behavior and replace it with deterministic fallback + streaming.

---

## 6) What “done” looks like against your goals

### Each object owns the state it needs

* Texture manager no longer asks for `renderPassActive`; it just queues and reports “fallback or resident”.
* Rendering session owns rendering/pass state and image layout state.
* Descriptor updater owns descriptor write tracking.

### No retry loops / no exception-based coordination

* No `throw`/`catch` in the hot path to force pass restarts. 
* No “attempt upload, throw, retry”.

### Invalid states unrepresentable

* Ring buffer cannot wrap within a frame. 
* Texture “Resident” implies “has image view + correct layout” via variant state.
* Render session “upload allowed” is representable only by possession of `UploadContext`.

### Clear ownership boundaries matching Vulkan

* Frames own per-frame mutable GPU state (cmd buffers, per-frame sets).
* Managers own resource objects and retire them via a GPU serial.
* Session owns attachment/layout transitions.
