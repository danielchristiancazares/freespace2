# Invisible Ships: Hypothesis Evaluation

## Architecture Check

Before asserting any bug exists, apply this check:

This codebase is designed so that invalid states are unconstructible. When you find code that "looks wrong," your first hypothesis should be "I'm misunderstanding the invariant", not "this is broken."

For every potential bug you identify, ask:

1. **"What would have to be true for this to NOT be a bug?"** — Then look for evidence of that.
2. **"If this were really broken, what else would be failing?"** — If the expected collateral damage isn't observed, your theory is probably wrong.
3. **"Why would competent engineers have written it this way?"** — Find the reason before assuming there isn't one.

If you cannot articulate a coherent alternative explanation where the code is correct, you haven't understood the system well enough to claim it's broken.

### Hypothesis Template

```markdown
### Hypothesis N: [Brief description]

**Status: PENDING | CONFIRMED | REJECTED**

#### Observation

[What was observed that prompted this hypothesis. Factual, no interpretation.]

#### Hypothesis

[The proposed explanation for the observation.]

#### Architecture Check

1. **What would make this NOT a bug?**
   [Alternative explanation where the code is correct]

2. **Expected collateral damage if true:**
   [What else should be failing if this hypothesis is correct]

3. **Why might engineers have written it this way?**
   [Plausible intentional design rationale]

#### Proposed Instrumentation

[How to gather evidence to test this hypothesis]

- What to log/measure:
- Where to add instrumentation:
- What results would support the hypothesis:
- What results would contradict the hypothesis:

#### Evidence

[Filled in after instrumentation is run. Leave blank or "Pending instrumentation" until data is collected.]

#### Conclusion

[Filled in after evidence is gathered. Use hedged language: "suggests", "appears", "may indicate"]
```

---

## Evaluated Hypotheses

### Hypothesis 1: Redundant G-buffer clears

**Status: CONFIRMED**

#### Evidence from logs

All `beginDeferredPass` calls show the parameter is ignored:
- `clearNonColorBufs: false` passed by caller
- `shouldClearColor: true`, `shouldClearDepth: true`, `shouldClearStencil: true` set internally

The `clearNonColorBufs` parameter is completely ignored in `VulkanRenderingSession.cpp:106-112`:
```cpp
void VulkanRenderingSession::beginDeferredPass(bool /*clearNonColorBufs*/) {
  endActivePass();
  m_shouldClearColor = true;   // Always true - ignores parameter
  m_shouldClearDepth = true;   // Always true - ignores parameter
  m_shouldClearStencil = true; // Always true - ignores parameter
  m_target = std::make_unique<DeferredGBufferTarget>();
}
```

Only one `beginDeferredPass` call per frame observed (lines 788, 791, 794, 797, etc.).

#### Conclusion

The bug exists (parameter ignored), but it does not cause invisible ships because there is only one deferred pass per frame, so no redundant clears occur within a frame.

---

### Hypothesis 2: Shared G-buffer race condition

**Status: REJECTED**

#### Evidence from logs

Frame serial progression shows proper synchronization:

| Log Line | Event | Serial Values |
|----------|-------|---------------|
| 789 | submitSerial | 394, currentFrame=0 |
| 790 | completedSerial | 393, nextSubmitSerial=395, currentFrame=1 |
| 792 | submitSerial | 395, currentFrame=1 |
| 793 | completedSerial | 394, nextSubmitSerial=396, currentFrame=0 |

**Pattern observed:**
- `completedSerial` is always less than `nextSubmitSerial`
- Frames wait for GPU completion before starting
- `beginDeferredPass` is called after the previous frame completes (e.g., line 788 after line 787 shows completion)

#### Conclusion

No race condition detected in the sampled log data. Frames appear to be properly synchronized.

---

### Hypothesis 3: Bindless texture residency lag

**Status: CONFIRMED**

#### Initial Evidence (BEFORE FIX)

Many models rendered with `baseMapIndex: 4294967295` (MODEL_OFFSET_ABSENT).
- Example: texture handle 1229 returned `baseMapIndex: 4294967295`
- Log line 1460: `{"baseTex":1229,"baseMapIndex":4294967295,"hasAbsentIndex":true}`

#### Root Cause of the Bug (NOW FIXED)

`VulkanTextureManager::getBindlessSlotIndex` returned ABSENT due to **slot exhaustion**, not residency timing:

**Problem (NOW FIXED):**
- Bindless texture array has 1024 slots (0-1023), with slot 0 reserved for fallback
- When textures were retired, slots were added to `m_retiredSlots` but never returned to the free pool
- This exhausted all slots, leaving none for new textures

#### Fix Applied

1. Modified `clearRetiredSlotsIfAllFramesUpdated` to return retired slots to `m_freeBindlessSlots`
2. Implemented LRU eviction to handle slot pressure

#### Post-Fix Verification (CURRENT STATE)

**The bindless texture system is now working correctly:**
- Handle 1229 gets slot 1082 (`baseMapIndex: 1082`, NOT 4294967295)
- 3013 free slots remaining (no exhaustion)
- Descriptors are written for handle 1229
- Valid indices are used at render time
- `hasAbsentIndex: true` only appears for optional maps (glow, normal, spec), which aligns with expected behavior when those maps aren't present

#### Conclusion

The slot exhaustion bug was fixed. Textures now receive valid bindless slots. The `baseMapIndex` passed to the shader is correct (1082, not ABSENT). Ships remain invisible after this fix, suggesting the bindless texture system is not the cause of invisible ships.

---

### Hypothesis 4: Models rendering to wrong render target

**Status: REJECTED**

#### Evidence from logs

All model renders show `colorAttachmentCount: 5` (G-buffer), not 1 (swapchain).
- Initial investigation: Log line 1462: `{"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130}`
- 2025-01-17 verification: Multiple log entries confirm consistent G-buffer rendering:
  ```
  {"indexCount":2490,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":24}
  {"indexCount":72,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":50}
  {"depthTest":true,"depthWrite":true,"zMode":3,"colorAttachmentCount":5,"callCount":50}
  ```

**Note:** Some models render to swapchain (`colorAttachmentCount: 1`) with `depthWrite: false`. These appear to be HUD/interface elements.

#### Conclusion

Logs indicate models are rendering to the G-buffer target (5 attachments). Ships appear to be written to the correct render target.

---

### Hypothesis 5: Invalid vertex pulling offsets

**Status: REJECTED**

#### Evidence

All vertex offsets are valid:
- `posOffset: 40`, `stride: 52`, `normalOffset: 8`, etc.
- No MODEL_OFFSET_ABSENT values found for vertex offsets

#### Conclusion

Vertex pulling configuration appears correct based on logged values.

---

### Hypothesis 6: Model uniform matrices are identity/zero

**Status: REJECTED**

#### Evidence

ModelData uniform buffers are bound with valid offsets and sizes.
- Log shows consistent binding: `{"offset":0,"size":1392,"handle":26}`

#### Additional Reasoning (2025-01-17)

**Why matrices are unlikely to be the issue:**
- Ships appear to be written to G-buffer (logs show substantial geometry with `indexCount: 2490`, etc.)
- If matrices were invalid (identity, zero, or causing clipping), we would expect to see:
  - Geometry at wrong positions (not invisible)
  - Clipping would show geometry partially visible or at screen edges
  - Zero/identity matrices would place geometry at origin or wrong scale, not make it invisible
- Since ships appear to be written to G-buffer correctly, the problem may be downstream in the deferred lighting pass

#### Conclusion

Uniform buffers are being bound correctly. Matrix issues would typically cause geometry to appear at wrong positions, not make ships invisible. This suggests the issue may be in the deferred lighting pipeline rather than the geometry pass.

---

### Hypothesis 7: Vertex Heap Descriptor Desynchronization

**Status: REJECTED**

#### Evidence

- No Model Vertex Heap resize events found in logs
- Buffer pointer remains consistent: `"vertexHeapBufferPtr":"00005A000000005A"`

#### Conclusion

Vertex heap descriptor appears stable throughout the sampled frames.

---

### Hypothesis 8: Dynamic State Support Mismatch

**Status: REJECTED**

#### Evidence

Pipelines include both `eDepthTestEnable` and `eDepthWriteEnable`.
- Log line 12: `{"hasDepthTestEnable":true,"hasDepthWriteEnable":true,"dynamicStateCount":14}`

#### Conclusion

Dynamic state appears correctly configured based on logged values.

---

### Hypothesis 9: baseMapIndex not reaching shader

**Status: REJECTED**

#### Evidence

- Handle 1229 gets slot 1082 (`baseMapIndex:1082`)
- Valid slot indices used (not MODEL_OFFSET_ABSENT / 4294967295)
- Descriptors are written for handle 1229
- Rendering uses the correct baseMapIndex at render time

Note: `hasAbsentIndex:true` appears because `glowMapIndex`, `normalMapIndex`, and `specMapIndex` are MODEL_OFFSET_ABSENT. This is consistent with those maps not being present on the model.

#### Conclusion

The bindless texture system appears to be functioning correctly based on logs:
- Slots are assigned
- Descriptors are written
- Valid indices are used at render time

---

### Hypothesis 10: Backface culling / winding order

**Status: REJECTED**

#### Evidence

Forced `cmd.setCullMode(vk::CullModeFlagBits::eNone)` to disable all culling in both `gr_vulkan_render_model` and `gr_vulkan_render_primitives`.

**Result:** Ships remain invisible with culling completely disabled.

**Instrumentation:** Added logging to track original cull mode vs forced mode, but logs were not captured (likely due to sampling rate or code path not executing the logging block).

#### Conclusion

Winding order and culling do not appear to be the issue. Ships remained invisible with culling disabled.

---

### Hypothesis 11: Multiple G-buffer clears per frame wipe geometry

**Status: SUPERSEDED** (see below)

#### Observation

Ships are invisible in the main 3D view. `beginDeferredPass` was reported to be called multiple times per frame.

#### Hypothesis

Each `beginDeferredPass` call triggers a G-buffer clear via `loadOp::eClear`. If clears occur after geometry draws, earlier geometry is wiped, leaving zeros in the position buffer. The deferred shader discards fragments where position equals zero.

#### Architecture Check

1. **What would make this NOT a bug?**
   Multiple passes might be intentional for multi-phase rendering (opaque, decals, transparent). If all clears occur before draws begin, draws survive.

2. **Expected collateral damage if true:**
   All deferred-rendered objects should be invisible, not just ships. Only the last batch of geometry (drawn after final clear) would survive.

3. **Why might engineers have written it this way?**
   The `clearNonColorBufs` parameter suggests clearing behavior was designed to be controllable. Multi-phase deferred rendering legitimately uses multiple passes.

#### Proposed Instrumentation

**A. Clear/Draw Temporal Sequence**
- What to log: Monotonic event counter, event type (GBUFFER_BEGIN/GBUFFER_DRAW/GBUFFER_END), loadOp for BEGIN, indexCount for DRAW
- Where: `beginGBufferRenderingInternal()`, `issueModelDraw()`, `endDeferredGeometry()`
- Supporting result: `loadOp=Clear` events appear AFTER draw events in same frame
- Contradicting result: All clears before all draws, only one BEGIN per frame, or no G-buffer events at all

**B. G-Buffer Content Verification**
- What to log: Position buffer sample at deferred lighting time
- Where: `recordDeferredLighting()`
- Supporting result: Position reads (0,0,0,0) where ships should be
- Contradicting result: Position has valid world-space coordinates, or deferred lighting never runs

#### Evidence

Instrumentation A was implemented. Results from logs:

```
deferredPassCount: 0
No GBUFFER_BEGIN events
No GBUFFER_DRAW events
Only SWAPCHAIN_BEGIN events observed
```

**Deferred rendering is not running at all.** Zero G-buffer passes per frame means G-buffer clears cannot be causing invisible ships—there is no G-buffer being used.

#### Conclusion

This hypothesis is **superseded**. The premise (multiple G-buffer clears) cannot be the cause because deferred rendering is not executing. The instrumentation revealed a more fundamental issue: `light_deferred_enabled()` appears to return false, so the deferred pipeline never starts.

This finding invalidates the earlier "evidence from logs" that claimed to show multiple `beginDeferredPass` calls—that evidence was either from a different configuration, a different code path, or was misinterpreted.

**New hypothesis required:** If deferred is disabled, ships should render via forward rendering. They don't, suggesting a forward rendering bug. See Hypothesis 21.

---

## Investigation Log

### DEBUG Block Removed, Ships Still Invisible (2025-12-17)

**Initial finding:** A DEBUG code block in `model.frag` (lines 53-59) returned early, preventing the production shader logic from executing.

**Fix applied:** Removed the DEBUG block.

**Result after rebuild:** Ships remain invisible.

**Post-fix instrumentation shows:**
- Pipeline sequence correct: Clear → Draw → End → Deferred Lighting
- Many geometry draws per frame
- Deferred lighting runs (lightCount: 2-29)
- Ships still invisible

**Conclusion:** The DEBUG block was a contributing factor that needed removal, but was not the sole cause. Investigation continues with H23 (G-buffer position data may be zero/invalid).

---

### Shader and Pipeline Verification Complete (2025-12-17)

**Shader size analysis:**
- With DEBUG block: 6500 bytes (smaller — early return, optimizer removes dead code)
- Without DEBUG block: 6536 bytes (larger — full texture sampling implementation)
- Current state: Source correct, compiled 6536 bytes, embedded 6536 bytes

**Pipeline sequence verified from logs:**
- G-buffer draws: ~26 per frame
- Deferred lighting: 2-4 lights per frame
- Sequence: Clear → Draw → End → Deferred Lighting → Swapchain
- Layout transitions: G-buffers transition `ColorAttachment` → `ShaderReadOnlyOptimal`
- Descriptor bindings: G-buffer textures bound to deferred lighting descriptor set

**Ruled out:**
- Multiple `beginDeferredPass` calls clearing G-buffer (only 1 per frame)
- Shader DEBUG code (removed, shader is correct)
- Missing deferred lighting execution (runs with lights)
- Missing G-buffer draws (26 per frame)
- Layout transition issues (transitions occur correctly)
- Descriptor binding issues (bindings occur correctly)

**Remaining unknowns:**
- G-buffer content: Is position data valid/non-zero after geometry pass?
- Deferred lighting shader: Can it sample G-buffer textures correctly?
- Position buffer: Does it contain valid view-space positions?
- Deferred shader discard: Is `dot(position, position) < 1.0e-8` discarding valid fragments?

**Conclusion:** Shader and pipeline structure are correct. Issue is likely in G-buffer data content or how the deferred lighting shader reads it.

---

### Detailed Frame Analysis — Frame 197 (2025-12-17)

Complete instrumentation of a single frame confirms correct execution sequence:

```
Line 594: beginDeferredPass (passNum: 1)
Line 595: beginGBufferRenderingInternal (Clear color, Load depth)
Lines 598-624: 27 G-buffer draws (indexCount: 36-2490)
Line 625: endDeferredGeometry (G-buffers in ColorAttachment layout)
Line 627: BIND_DEFERRED_DESCRIPTORS (G-buffer textures bound)
Line 628: DEFERRED_LIGHTING_RECORD (lightCount: 5)
Line 630: DEFERRED_LIGHTING_VIEWPORT (width: 3840, height: 2160, viewportHeight: -2160)
Lines 633-637: Deferred lighting draws
  - AMBIENT (fullscreen, 3 vertices)
  - 3× POINT lights (sphere, 24 indices each)
  - DIRECTIONAL (fullscreen, 3 vertices)
```

**Verified correct:**
- Pipeline sequence: G-buffer → End → Bind descriptors → Deferred lighting
- G-buffer draws: 27 per frame with varying index counts (36-2490)
- Deferred lighting: 5 lights per frame (ambient + 3 point + directional)
- Viewport: 3840×2160 (matches display resolution)
- Viewport height: -2160 (negative Y for Vulkan coordinate system — correct)

**All pipeline stages executing correctly. The issue must be in G-buffer data content or how the deferred shader reads it.**

---

### Earlier Finding: Deferred Rendering Not Running (2025-12-17) — SUPERSEDED

> **Note:** This finding was superseded by the root cause identification above. The instrumentation was likely capturing state before the DEBUG block returned early, or the logging conditions weren't met. With the DEBUG block removed, instrumentation now shows deferred rendering is active (26 draws, 3-4 lights per frame).

Original observation:
```
deferredPassCount: 0
No GBUFFER_BEGIN events
No GBUFFER_DRAW events
Only SWAPCHAIN_BEGIN events observed
```

This led to H21, which is now superseded by the DEBUG block finding.

---

### ~~Key Finding: Draw Calls Issued to G-Buffer~~ (POTENTIALLY INVALID)

> **Warning:** This section documents evidence that may be invalid. Current instrumentation (2025-12-17) shows `deferredPassCount: 0`, meaning no G-buffer rendering is occurring. The logs below claiming `colorAttachmentCount: 5` may have been from a different build, configuration, or misinterpreted. Retain for historical reference but do not rely on this evidence.

~~Logs indicate draw calls are issuing geometry to the G-buffer with parameters that appear correct.~~

#### Evidence from logs (2025-01-17 debugging session) — STATUS UNCERTAIN

**Draw calls to G-buffer:**
- Multiple entries show `colorAttachmentCount: 5` (G-buffer)
- Substantial geometry: `indexCount` values include 2490, 72, 174, 612, 753, etc.
- Log examples:
  ```
  {"indexCount":2490,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":24}
  {"indexCount":72,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":50}
  {"indexCount":174,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":100}
  {"indexCount":612,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":150}
  {"indexCount":753,"colorAttachmentCount":5,"colorFormat":97,"depthFormat":130,"callCount":200}
  ```

**Depth state is correct:**
- When rendering to G-buffer: `depthTest: true`, `depthWrite: true`, `zMode: 3` (ZBUFFER_TYPE_FULL)
- Log examples:
  ```
  {"depthTest":true,"depthWrite":true,"zMode":3,"colorAttachmentCount":5,"callCount":50}
  {"depthTest":true,"depthWrite":true,"zMode":3,"colorAttachmentCount":5,"callCount":100}
  {"depthTest":true,"depthWrite":true,"zMode":3,"colorAttachmentCount":5,"callCount":150}
  {"depthTest":true,"depthWrite":true,"zMode":3,"colorAttachmentCount":5,"callCount":200}
  ```

**Mixed rendering:**
- Some models render to swapchain (`colorAttachmentCount: 1`) with `depthWrite: false`, possibly HUD/interface elements
- Log examples:
  ```
  {"indexCount":1392,"colorAttachmentCount":1,"colorFormat":50,"depthFormat":130,"callCount":133}
  {"depthTest":true,"depthWrite":false,"zMode":1,"colorAttachmentCount":1,"callCount":250}
  ```

**Instrumentation details:**
- Logging added in `issueModelDraw()` (VulkanGraphics.cpp:611) - samples every 50th draw call or large draws (>1000 indices)
- Logging added in `gr_vulkan_render_model()` (VulkanGraphics.cpp:762) - samples every 50th call
- Both log render target state (`colorAttachmentCount`, `colorFormat`, `depthFormat`) and draw parameters (`indexCount`, `depthTest`, `depthWrite`, `zMode`)

#### Conclusion

Logs suggest ships are being drawn to the G-buffer with correct parameters. The geometry pass appears to be working. Possible areas for further investigation:

1. Deferred lighting pass not reading the G-buffer correctly
2. G-buffer being cleared after ships are drawn (before lighting pass)
3. Lighting shader discarding pixels
4. G-buffer image layout transition issue (not readable when sampled)

---

### Critical Experiment: Debug Shader Test (2025-12-17)

#### Experiment 1: Screen-UV Debug Output in model.frag

Modified `model.frag` to output screen-space UV coordinates as color, bypassing all texture sampling:

```glsl
void main() {
    outColor = vec4(gl_FragCoord.x / 3840.0, gl_FragCoord.y / 2160.0, 0.0, 1.0);
    outNormal = vec4(0.0, 0.0, 1.0, 1.0);
    outPosition = vec4(vPosition, 1.0);
    outSpecular = vec4(0.04, 0.04, 0.04, 0.0);
    outEmissive = vec4(0.0);
    return;
}
```

**Result:**
- **HUD Targeting Box**: Shows green gradient (ship geometry visible)
- **Main 3D View**: Ships remain invisible

**Interpretation:** The fragment shader appears to be executing for the targeting box. Ship geometry appears to reach the GPU and produce fragments in that context.

#### Experiment 2: Fullscreen Scissor on Geometry Pass

Attempted fix by forcing fullscreen scissor before model rendering:

```cpp
vk::Rect2D scissor{{0, 0}, gbufferExtent};
cmd.setScissor(0, scissor);
```

**Result:** Everything went black - not just ships, but nebula and HUD too.

**Interpretation:** The scissor change affected more than just ships. This suggests the scissor state interacts with other rendering, though the exact relationship is unclear.

---

### Why Targeting Box Works (Bypasses Deferred Pipeline)

The targeting box renders AFTER `gr_deferred_lighting_finish()` completes:

```
game_render_frame()
    -> obj_render_queue_all()
        -> gr_deferred_lighting_begin()
        -> scene.render_all()  <- Ships to G-buffer
        -> gr_deferred_lighting_end()
        -> gr_deferred_lighting_finish()  <- Deferred lighting to swapchain
    |
game_render_hud()
    -> HudGaugeTargetBox::renderTargetShip()
        -> model_render_immediate()  <- Ships DIRECTLY to swapchain
```

At HUD render time, the render target is **swapchain** (1 color attachment), not G-buffer (5 attachments).

When `model.frag` writes to `outColor` (location 0), only that output goes to the swapchain. Locations 1-4 are silently discarded (no corresponding attachments).

**The targeting box appears to render in forward mode, bypassing the deferred lighting pipeline.**

#### Implication

The targeting box working while main view ships are invisible suggests:
1. Model vertex/fragment shaders can execute correctly
2. Ship geometry can produce valid fragments
3. The difference in visibility may be related to the deferred lighting pass

---

## Candidates for Investigation

The following hypotheses have not been tested. They are candidates for future investigation if the root cause remains unfound.

### Hypothesis 12: G-buffer layout transition failure

**Status: PENDING**

G-buffer images not transitioned to `SHADER_READ_ONLY_OPTIMAL` before deferred pass samples them, causing samples to return zero or undefined values.

**Instrumentation:**
- In `endDeferredGeometry` / `transitionGBufferToShaderRead`, verify the barrier is issued
- Log all barriers/transitions for each G-buffer image:
  - `oldLayout`/`newLayout`, `src`/`dst` stage masks, `src`/`dst` access masks, and aspect
  - Log which layout the engine thinks the image is in at deferred bind time

**Interpretation:**
- If you never transition from attachment layout to shader-read layout (or barriers have wrong stages/access), sampling can silently return zeros/undefined

---

### Hypothesis 13: Deferred pass binding wrong G-buffer textures

**Status: PENDING**

The deferred lighting shader samples from wrong/stale descriptors, not the G-buffer that was just written.

**Instrumentation:** Log which descriptor set / image views are bound when deferred pass runs. Compare with actual G-buffer image views.

---

### Hypothesis 14: Fullscreen deferred pass fails but small regions work

**Status: PENDING**

When scissor was small (targeting box region), ship geometry was visible. When scissor is fullscreen, ships are invisible. This may indicate something about the fullscreen path differs.

Possible causes:
- Swapchain target not properly activated for fullscreen
- ensureRenderingStarted() fails at fullscreen
- Viewport/scissor state corruption

**Instrumentation:** Add logging to `recordDeferredLighting` to verify:
- `ensureRenderingStarted()` activates correct target
- Viewport and scissor are fullscreen
- Pipeline is bound
- Draw calls are issued

---

### Hypothesis 15: Deferred fullscreen pass isn't producing any fragments

**Status: PENDING**

The deferred lighting pass may not be executing at all due to incorrect state (viewport/scissor zero, pipeline not bound, wrong render target, early return).

**Instrumentation:**
- Wrap the deferred lighting draw with an occlusion query (or pipeline statistics query if enabled)
- Log `samplesPassed` (or `fragmentShaderInvocations`)

**Interpretation:**
- `0` = the pass effectively draws nothing (scissor/viewport zero, wrong render area, pipeline not bound, draw not issued, etc.)
- Non-zero = the pass runs

---

### Hypothesis 16: Deferred pass writes to the wrong target

**Status: PENDING**

The deferred lighting pass may be rendering to a different image than the one being presented.

**Instrumentation:**
- At "begin swapchain rendering" time, log the exact `VkImage`/`VkImageView` chosen as the color attachment for deferred finish, plus the image you actually present
- Add a one-frame unconditional clear of that attachment (magenta) using the same command buffer/path

**Interpretation:**
- No magenta visible would suggest rendering is not reaching the presented image, or the pass is not executing

---

### Hypothesis 17: Viewport/scissor corruption between passes

**Status: PENDING**

Viewport or scissor may be corrupted between the geometry pass and deferred lighting pass.

**Instrumentation:**
- Right before every draw in (a) geometry pass and (b) deferred lighting pass, log current viewport + scissor (`x`/`y`/`w`/`h`) and render target extent
- Include a "frame id" and "pass id"

**Interpretation:**
- Values like (`w==0 || h==0`), negative, or smaller-than-expected would suggest viewport/scissor state as a contributing factor

---

### Hypothesis 18: Deferred samples wrong images (stale frame or wrong views)

**Status: PENDING**

The deferred lighting pass may be sampling from stale G-buffer images from a previous frame or wrong image views.

**Instrumentation:**
- When building/updating the deferred descriptor set, log the bound `VkImageView` handles (and underlying `VkImage`) for each G-buffer sampler
- Separately log the current frame's G-buffer attachment views/images

**Interpretation:**
- A mismatch would indicate the deferred pass is sampling from a different G-buffer than expected

---

### Hypothesis 19: Shader logic discards everything (e.g., position == 0)

**Status: PENDING**

The deferred lighting shader may be discarding all fragments due to invalid G-buffer data (e.g., position == vec4(0,0,0,0) check).

**Instrumentation Options:**

**Option A (no shader modification):**
- Add a debug path that writes discard-reason counters via an SSBO atomic (e.g., `discard_position_zero++`, `discard_depth_invalid++`)

---

### Hypothesis 20: Blending / color write mask / attachment format mismatch

**Status: REJECTED**

The deferred lighting pass may be running correctly but the output is invisible due to blending state, color write mask, or format mismatches.

#### Evidence

**Pipeline creation logging:**
```
ADDITIVE: blendMode:"ADDITIVE", blendEnable:true, writeMask:"RGBA", colorFormat:50
NONE: blendMode:"NONE", blendEnable:false, writeMask:"RGBA", colorFormat:50
```

**Render target format verification:**
```
colorFormat:50, pipelineColorFormat:50, formatMatch:"true"
```

**Runtime blend state:**
```
AMBIENT: blendEnable:false
DIRECTIONAL: blendEnable:true
writeMask:15 = 0xF = RGBA (all components enabled)
```

#### Conclusion

Blend state, write mask, and attachment format are all correct:
- ADDITIVE pipeline: blend enabled, RGBA write mask
- NONE pipeline (ambient): blend disabled, RGBA write mask
- Format 50 matches between pipeline and render target
- Write mask 0xF enables all RGBA components

The deferred output should be visible if data is being written. The issue is upstream — likely G-buffer content (H23).

---

### Hypothesis 21: Deferred rendering disabled, forward rendering broken

**Status: SUPERSEDED**

#### Observation

Instrumentation from H11 appeared to show:
- `deferredPassCount: 0` for all frames
- No `GBUFFER_BEGIN` or `GBUFFER_DRAW` events

#### Hypothesis

`light_deferred_enabled()` returns false, causing the deferred pipeline to not execute.

#### Evidence

This hypothesis was based on instrumentation that showed no deferred events. However, subsequent investigation revealed:

1. A DEBUG code block in `model.frag` was returning early, writing incorrect G-buffer data
2. After removing the DEBUG block, instrumentation shows deferred rendering is active (26 draws, 3-4 lights per frame)
3. The earlier instrumentation results may have been affected by the DEBUG block's early return or logging conditions

#### Conclusion

This hypothesis is superseded. The root cause was identified as the DEBUG block in `model.frag`, not deferred rendering being disabled. See "Root Cause Identified" in Investigation Log.

---

### Hypothesis 22: DEBUG block in model.frag writes incorrect G-buffer data

**Status: REJECTED** (contributing factor, not root cause)

#### Observation

Ships invisible in main 3D view. HUD targeting box shows ships. Debug shader modifications from earlier experiments remained in the shader.

#### Hypothesis

A DEBUG code block in `model.frag` (lines 53-59) returns early, bypassing the production shader logic. This incorrect G-buffer data causes the deferred lighting shader to produce incorrect results or discard fragments.

#### Architecture Check

1. **What would make this NOT a bug?**
   The DEBUG block might have been intentionally left for testing. However, production builds should not include debug code that alters rendering output.

2. **Expected collateral damage if true:**
   All deferred-rendered objects would have incorrect lighting or be invisible—consistent with observed symptoms.

3. **Why might engineers have written it this way?**
   The DEBUG block was added during earlier debugging (see "Critical Experiment: Debug Shader Test" in Investigation Log) to test whether the fragment shader was executing. It was not removed after the experiment.

#### Evidence

- DEBUG block identified in `model.frag` lines 53-59
- Block returns early, preventing production shader from running
- After removal:
  - Instrumentation shows correct sequence: Clear → Draw → End → Deferred Lighting
  - Many geometry draws per frame observed
  - Deferred lighting runs (lightCount: 2-29)
  - **Ships remain invisible**

#### Conclusion

The DEBUG block was a contributing factor that needed removal, but it was not the root cause. After removal and rebuild, ships remain invisible despite correct pipeline sequencing. The issue persists, indicating additional causes. See H23.

#### Possible Remaining Issues (identified during continued investigation)

1. **Shader binary not embedded** - SPIR-V may have been recompiled but not embedded in executable
2. **G-buffer layout transition** - Images may not be in correct layout for sampling
3. **Descriptor binding** - G-buffer textures may not be bound correctly to deferred shader
4. **Deferred shader discard condition** - `dot(position, position) < 1.0e-8` discards fragments where position is near-zero

---

### Hypothesis 23: G-buffer position data is zero/invalid, triggering deferred shader discard

**Status: PENDING**

#### Observation

After removing the DEBUG block from `model.frag`:
- Pipeline sequence is correct (Clear → Draw → End → Deferred Lighting)
- Many geometry draws occur per frame
- Deferred lighting runs (lightCount: 2-29)
- Ships remain invisible

The deferred lighting shader (`deferred.frag`) contains:
```glsl
if (dot(position, position) < 1.0e-8) {
    discard;
}
```

#### Hypothesis

The G-buffer position attachment contains zero or near-zero values when sampled by the deferred shader. This triggers the discard condition, causing all ship fragments to be discarded.

Possible causes for zero position data:
1. **Shader binary not embedded** - The recompiled `model.frag` SPIR-V may not be included in the executable
2. **G-buffer layout transition** - Position attachment not transitioned to `SHADER_READ_ONLY_OPTIMAL` before sampling
3. **Descriptor binding** - Wrong image view bound to position sampler
4. **Attachment write failure** - Position output not reaching the attachment

#### Architecture Check

1. **What would make this NOT a bug?**
   The discard condition is intentional—it skips pixels with no geometry. If the position buffer correctly contains zeros for empty pixels, the discard is correct behavior. The bug would be that pixels WITH geometry also have zero positions.

2. **Expected collateral damage if true:**
   All deferred-rendered geometry would be invisible (discarded). This matches observed symptoms.

3. **Why might engineers have written it this way?**
   The discard condition optimizes the deferred pass by skipping empty pixels. The assumption is that the G-buffer position attachment contains valid world-space positions for rendered geometry.

#### Proposed Instrumentation

**A. Verify shader binary is current**
- What to check: Timestamp of embedded SPIR-V vs source file
- Where: Build system / embedded resource
- Supporting result: Old timestamp indicates stale binary
- Contradicting result: Timestamps match

**B. Verify G-buffer layout transition**
- What to log: `oldLayout` and `newLayout` for position attachment at `endDeferredGeometry()`
- Where: `VulkanRenderingSession::transitionGBufferForSampling()` or equivalent
- Supporting result: Transition missing or incorrect (not `eShaderReadOnlyOptimal`)
- Contradicting result: Correct transition to `eShaderReadOnlyOptimal`

**C. Verify descriptor binding**
- What to log: Image view handles bound to deferred descriptor set vs actual G-buffer attachment views
- Where: `bindDeferredGlobalDescriptors()`
- Supporting result: Mismatch between bound views and G-buffer views
- Contradicting result: Views match

**D. Sample G-buffer content**
- What to log: Read back a pixel from position attachment after geometry pass
- Where: After `endDeferredGeometry()`, before deferred lighting
- Supporting result: Position is (0,0,0,0) where ships should be
- Contradicting result: Position has valid world-space coordinates

#### Evidence

**Instrumentation results (Frame 197 analysis):**
- Layout transitions: Verified. G-buffers transition `ColorAttachment` → `ShaderReadOnlyOptimal`
- Descriptor bindings: Verified. G-buffer textures bound to deferred lighting descriptor set
- Pipeline sequence: Verified. Clear → Draw → End → Deferred Lighting → Swapchain
- G-buffer draws: 27 per frame with index counts 36-2490
- Deferred lighting draws: 5 lights (ambient + 3 point + directional)
- Viewport: 3840×2160, viewportHeight -2160 (correct for Vulkan)

**Still pending:**
- G-buffer content sampling (is position non-zero where ships are drawn?)
- Deferred shader texture read verification

#### Conclusion

Pipeline execution is verified correct. All stages run in proper sequence with correct viewport and scissor settings. The remaining unknown is whether the G-buffer contains valid position data. If position data is zero or near-zero, the deferred shader's discard condition would trigger, causing all fragments to be discarded.

---

## Bugs Fixed During Investigation

1. **Bindless slot leak** - Retired slots were never returned to the free pool. Fixed by returning slots in `clearRetiredSlotsIfAllFramesUpdated`.
2. **Slot exhaustion** - Implemented LRU eviction to handle high texture counts.
3. **G-buffer clear flags** - `beginDeferredPass` ignored `clearNonColorBufs` parameter. Fixed to respect the parameter. *(Status: Deferred is now confirmed running; fix may be relevant.)*
4. **G-buffer color clearing** - Only clears color on first deferred pass call, preserves on subsequent calls. *(Status: Deferred is now confirmed running; fix may be relevant.)*
5. **Multiple G-buffer clears per frame** - Fixed with `m_gbufferClearedThisFrame` flag. *(Status: Deferred is now confirmed running; fix may be relevant.)*
6. **Missing `light_deferred_enabled()` check** - Vulkan `gr_vulkan_deferred_lighting_begin()` was missing the early return when deferred is disabled. Added to match OpenGL behavior.
7. **DEBUG block in model.frag** - Debug code from earlier experiment remained in shader, writing incorrect G-buffer data and returning early. Removed to restore production shader logic. *(Contributing factor; ships still invisible after removal.)*
8. **VulkanShaderManager loads from disk, not embedded** - `VulkanShaderManager` loads shaders from `code/graphics/shaders/compiled/` at runtime instead of using embedded resources. This bypasses the shader embedding system entirely. *(Bug to fix; not root cause of invisible ships.)*

---

## Appears Working (Based on Logs)

- Bindless texture system (slots, descriptors, indices)
- Vertex data (valid offsets and strides)
- Texture descriptors (written and bound)
- Culling state (disabled culling did not resolve invisibility)
- Deferred pipeline structure (clears before draws, 27 draws per frame, 5 lights per frame)
- G-buffer render target selection (confirmed after DEBUG block removal)
- G-buffer layout transitions (`ColorAttachment` → `ShaderReadOnlyOptimal`)
- Deferred descriptor bindings (G-buffer textures bound correctly)
- Viewport and scissor (3840×2160, viewportHeight -2160)
- Blend state (ADDITIVE enabled for lights, NONE for ambient)
- Write mask (0xF = RGBA, all components enabled)
- Attachment format (colorFormat 50 matches pipeline and render target)

---

## Summary

| Hypothesis | Status | Causing Invisible Ships? |
|------------|--------|--------------------------|
| 1. Redundant G-buffer clears | CONFIRMED | No |
| 2. Shared G-buffer race condition | REJECTED | No |
| 3. Bindless texture residency lag | CONFIRMED | No |
| 4. Wrong render target | REJECTED | No |
| 5. Invalid vertex pulling offsets | REJECTED | No |
| 6. Identity/zero matrices | REJECTED | No |
| 7. Vertex heap descriptor desync | REJECTED | No |
| 8. Dynamic state mismatch | REJECTED | No |
| 9. baseMapIndex not reaching shader | REJECTED | No |
| 10. Backface culling / winding order | REJECTED | No |
| 11. Multiple G-buffer clears | SUPERSEDED | No (premise invalidated) |
| 12. G-buffer layout transition failure | REJECTED | No (transitions verified) |
| 13. Deferred pass wrong descriptors | REJECTED | No (bindings verified) |
| 14. Fullscreen deferred path broken | NOT TESTED | Unknown |
| 15. Deferred pass not producing fragments | NOT TESTED | Unknown |
| 16. Deferred writes to wrong target | NOT TESTED | Unknown |
| 17. Viewport/scissor corruption | REJECTED | No (3840×2160 verified) |
| 18. Deferred samples wrong images | NOT TESTED | Unknown |
| 19. Shader discards everything | NOT TESTED | Unknown |
| 20. Blending/write mask mismatch | REJECTED | No (blend/mask/format verified) |
| 21. Deferred disabled, forward broken | SUPERSEDED | No (premise invalidated) |
| 22. DEBUG block in model.frag | REJECTED | No (contributing factor, not root cause) |
| **23. G-buffer position zero/invalid** | **PENDING** | **Primary candidate** |

**Current Status:** Shader and pipeline verified correct. Layout transitions, descriptor bindings, blend state, write mask, and format all confirmed working. Ships remain invisible. Investigation focuses on H23: G-buffer content verification — does the position buffer contain valid data, or is it zero/invalid triggering the deferred shader's discard condition?
