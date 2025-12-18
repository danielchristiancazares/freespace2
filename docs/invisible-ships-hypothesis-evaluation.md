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

### H23 Follow-up Reveals Transform Issue (2025-12-18)

Multi-color diagnostic shader test produced unexpected results:

**Test setup:**
- Modified `deferred.frag` to output distinct colors based on G-buffer validity
- Discard for empty sky (no G-buffer data)
- Magenta for missing position, Orange for missing normal, Yellow for missing albedo
- Cyan for all G-buffer data valid

**Observations:**
1. One large ship (destroyer/station) appeared CYAN when player turned 180°
2. Ship was visually behind player, but game logic placed it in front
3. All other ships remained invisible at any viewing angle
4. No magenta/orange/yellow visible for ships — only cyan or nothing

**Interpretation:**
- G-buffer data IS valid for at least one ship (cyan confirms all components present)
- The issue is not data corruption but incorrect transform/position
- Ships are rendered at mirrored/inverted screen positions
- Smaller ships likely rendered entirely off-screen due to same transform error

**New hypothesis:** View matrix Z inversion (H33) — see hypothesis section for details.

---

### Reproducible Position Anomaly Test (2025-12-18)

**Test procedure:**
1. Load mission, immediately turn off engines
2. Turn 180° away from known station location
3. Observe screen for station geometry
4. Turn back toward actual station location
5. Move camera toward screen edges

**Observations:**
- Station geometry visible when facing away from its actual position
- Station geometry not visible when facing toward its actual position
- Geometry reappears at screen edges
- Behavior is reproducible across multiple attempts

**Possible interpretations:**

The observed pattern is consistent with several potential causes:
1. CPU-side frustum culling returning inverted results
2. View matrix Z sign error causing objects to transform to opposite positions
3. Near/far plane configuration issue
4. Projection matrix sign error

**Next step:** Investigate frustum culling logic and view matrix construction to identify the source of the position anomaly.

---

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

**Status: REJECTED** (G-buffer data is valid; issue is transform-related)

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

#### Evidence

**Initial test (2025-12-18):**

Modified `deferred.frag` to output magenta when position is zero instead of discarding. Result was indeterminate (magenta on magenta background).

**Follow-up test with multi-color diagnostic (2025-12-18):**

Modified `deferred.frag` to output distinct colors:
```glsl
// Empty sky (no G-buffer data) - discard
if (!hasPosition && !hasNormal && !hasDiffuse) { discard; }
// Geometry present but position missing - MAGENTA
if (!hasPosition) { fragOut0 = vec4(1.0, 0.0, 1.0, 1.0); return; }
// Geometry present but normal missing - ORANGE
if (!hasNormal) { fragOut0 = vec4(1.0, 0.5, 0.0, 1.0); return; }
// Geometry present but albedo black - YELLOW
if (!hasDiffuse) { fragOut0 = vec4(1.0, 1.0, 0.0, 1.0); return; }
// All G-buffer data valid - CYAN
fragOut0 = vec4(0.0, 1.0, 1.0, 1.0); return;
```

**Results:**
- **One large ship (destroyer/station)**: Appeared CYAN when player turned 180°
- **All other ships**: Remained invisible (no diagnostic color visible)
- **Position anomaly**: The visible ship appeared BEHIND the player (visible only when turning around), despite game logic placing it in front

#### Interpretation

1. **G-buffer data IS valid** for at least one ship (cyan output confirms position, normal, and albedo all present)
2. **Position values are WRONG** — ship renders behind player when it should be in front
3. **Other ships not visible** — either rendered completely off-screen due to same transform issue, or draw calls not issued

This pattern suggests a **view matrix or coordinate system issue** rather than G-buffer content corruption. See H33.

#### Conclusion

**REJECTED as root cause.** G-buffer data is valid (cyan confirms all components present). The issue is that position values place geometry at incorrect screen locations, not that position data is zero/invalid.

---

### Hypothesis 24: Deferred lighting calculation produces zero output

**Status: PENDING** — Candidate (depends on H23 follow-up)

#### Observation

After removing the DEBUG block from `model.frag`:
- Pipeline sequence is correct
- Many geometry draws occur per frame
- Deferred lighting runs
- Ships remain invisible

H23 test was inconclusive for ship pixels. If ships have valid position data, the lighting calculation may be producing zero output.

#### Hypothesis

One or more terms in the deferred lighting calculation is zero, causing the final output to be zero:

```glsl
outRgb = computeLighting(...) * diffuseLightColor * attenuation * area_norm;
```

Possible zero terms:
1. **`diffuseLightColor`** — Light color uniform is zero or not set
2. **`attenuation`** — Distance attenuation calculates to zero (light too far, radius wrong)
3. **`area_norm`** — Area normalization term is zero
4. **`computeLighting()` result** — Lighting function returns zero (NdotL=0, specular=0, diffuse=0)
5. **`diffColor`** — G-buffer color is zero (texture sampling failed)
6. **`normal`** — G-buffer normal is invalid, causing NdotL=0

#### Architecture Check

1. **What would make this NOT a bug?**
   The lighting could legitimately be zero if lights are too far away, pointing wrong direction, or disabled. However, this would affect all deferred geometry, not just ships.

2. **Expected collateral damage if true:**
   All deferred-rendered objects would be dark/invisible. This matches observed symptoms.

3. **Why might engineers have written it this way?**
   The lighting calculation is standard PBR. Each term has a physical meaning. Zero output typically indicates missing or invalid input data.

#### Proposed Instrumentation

**A. Constant output test**
- What to do: Replace final output with constant yellow `vec4(1,1,0,1)` after lighting calculation
- Where: End of `main()` in `deferred.frag`
- Supporting result: Ships appear yellow → lighting path executes, something in calculation is zero
- Contradicting result: Ships still invisible → something prevents reaching end of main()

**B. Term-by-term output**
- What to do: Output each term as color to identify which is zero:
  - `fragOut0 = vec4(diffuseLightColor, 1.0);` — Is light color valid?
  - `fragOut0 = vec4(attenuation, attenuation, attenuation, 1.0);` — Is attenuation valid?
  - `fragOut0 = vec4(diffColor, 1.0);` — Is G-buffer color valid?
  - `fragOut0 = vec4(normal * 0.5 + 0.5, 1.0);` — Is G-buffer normal valid?
  - `fragOut0 = vec4(NdotL, NdotL, NdotL, 1.0);` — Is N·L valid?

#### Evidence

Pending instrumentation. Requires H23 follow-up test to confirm shader executes for ship pixels.

#### Conclusion

Pending. This hypothesis is contingent on H23 follow-up confirming that ship pixels have valid position data and the deferred shader executes for them. If H23 follow-up shows cyan ships, H24 becomes the primary candidate.

---

### Hypothesis 26: Float16 G-buffer position overflow

**Status: PENDING**

#### Observation

Ships are invisible. G-buffer format is `VK_FORMAT_R16G16B16A16_SFLOAT` (16-bit float). Maximum representable value for Float16 is approximately 65,504.

#### Hypothesis

If the G-buffer stores world-space coordinates and ships are located beyond ±65,504 units from the origin:

1. Position stored in G-buffer overflows to infinity
2. `LightVector = LightPos - Position` → `LightPos - Inf = -Inf`
3. `Attenuation = 1.0 / dot(LightVector, LightVector)` → `1.0 / Inf = 0.0`
4. All lighting terms multiply by zero, producing black output

Note: The `dot(position, position) < 1.0e-8` check would return false for infinity (not triggering the discard), allowing the shader to continue with corrupted data.

#### Architecture Check

1. **What would make this NOT a bug?**
   If the G-buffer stores view-space positions (relative to camera), values are bounded by the view frustum and overflow is unlikely.

2. **Expected collateral damage if true:**
   - Objects near world origin (0,0,0) would render correctly
   - Objects far from origin would be black/invisible
   - Objects at threshold (~30k-60k units) would show banding artifacts

3. **Why might engineers have written it this way?**
   Float16 saves memory bandwidth. World-space storage may have been inherited from a different coordinate system or overlooked during format selection.

#### Proposed Instrumentation

**A. Coordinate space verification**
- Check `model.frag` to determine if `vPosition` is view-space or world-space
- Check vertex shader to see what space positions are output in

**B. Infinity detection in shader**
```glsl
if (isinf(position.x) || isinf(position.y) || isinf(position.z)) {
    fragOut0 = vec4(1.0, 0.0, 0.0, 1.0);  // Red = overflow detected
    return;
}
```

**C. Teleport test**
- Move ship/camera to world origin (0,0,0) and observe if ships become visible

#### Evidence

Pending instrumentation.

#### Conclusion

Pending. Requires verification of coordinate space used in G-buffer and overflow detection test.

---

### Hypothesis 27: Depth test enabled on fullscreen lighting pass

**Status: PENDING**

#### Observation

Ships appear invisible (nebula visible through them) rather than as black silhouettes. This distinction suggests the deferred lighting shader may not execute for ship pixels at all.

#### Hypothesis

The fullscreen deferred lighting passes (AMBIENT, DIRECTIONAL) have depth testing incorrectly enabled with `VK_COMPARE_OP_LESS_OR_EQUAL`.

Mechanism:
1. Fullscreen quad is drawn at Z=1.0 (far plane)
2. Ship geometry wrote depth Z < 1.0 during G-buffer pass
3. Depth test: `1.0 <= ship_depth` fails
4. Fragment shader is culled before execution
5. Pixel retains previous value (cleared background or nebula)

This would cause ships to appear transparent/invisible rather than black.

#### Architecture Check

1. **What would make this NOT a bug?**
   Depth testing is intentionally disabled for fullscreen passes. Only volumetric light passes (point/spot light spheres) require depth testing.

2. **Expected collateral damage if true:**
   All deferred geometry would be invisible against any background that was rendered before the deferred pass.

3. **Why might engineers have written it this way?**
   Point light pipelines require depth testing for light volume culling. This configuration may have been incorrectly copied to fullscreen pass pipelines.

#### Proposed Instrumentation

**A. Pipeline state inspection**
- Log `VkPipelineDepthStencilStateCreateInfo` for AMBIENT and DIRECTIONAL pipelines
- Specifically check `depthTestEnable` and `depthCompareOp`

**B. Force disable depth test**
- Set `depthTestEnable = VK_FALSE` for fullscreen lighting passes
- Observe if ships become visible (as black silhouettes or lit)

#### Evidence

Pending instrumentation.

#### Conclusion

Pending. H23 follow-up test will help disambiguate: if ships remain invisible with cyan output forced, shader is not executing (supports H27).

---

### Hypothesis 28: Coordinate space mismatch between G-buffer and lights

**Status: PENDING**

#### Observation

Ships are invisible. Position data may be valid but lighting produces zero output.

#### Hypothesis

The G-buffer stores positions in one coordinate space (e.g., view-space) but light uniforms are provided in a different space (e.g., world-space), or vice versa.

Example:
- G-buffer position: view-space `(0, 0, 10)` (10 units in front of camera)
- Light position: world-space `(10000, 5000, 3000)`
- Distance calculation: `length(lightPos - fragPos)` = ~11,000 units
- Attenuation: `1.0 / (11000²)` ≈ 0.000000008 ≈ 0

Even nearby lights would appear infinitely far away due to the space mismatch.

#### Architecture Check

1. **What would make this NOT a bug?**
   Both G-buffer positions and light positions are consistently in the same coordinate space (typically view-space for deferred rendering).

2. **Expected collateral damage if true:**
   All deferred lighting would be near-zero. No objects would be lit correctly.

3. **Why might engineers have written it this way?**
   Coordinate space conventions may differ between the geometry pass (model.frag) and the lighting pass (deferred.frag). Porting from OpenGL or another renderer may have introduced inconsistencies.

#### Proposed Instrumentation

**A. Verify coordinate spaces**
- In `model.frag`: determine what space `vPosition` is in (check vertex shader output)
- In `deferred.frag`: determine what space light positions are expected in (check uniform buffer layout)

**B. Visualize distance**
```glsl
float dist = length(lightPos.xyz - position.xyz);
fragOut0 = vec4(vec3(dist * 0.001), 1.0);  // White = huge distance
```
If ships appear white (huge distance) when near a light, spaces are mismatched.

#### Evidence

Pending instrumentation.

#### Conclusion

Pending. Requires verification of coordinate spaces in both geometry and lighting shaders.

---

### Hypothesis 29: Zero normals from invalid vertex data

**Status: PENDING**

#### Observation

Deferred lighting executes for ship pixels but produces zero/near-zero output. Position buffer contains valid data. Skybox renders correctly with lighting.

#### Hypothesis

The normal term in the lighting calculation is zero or invalid because ship model vertex normals are not being correctly fetched or transformed in the deferred geometry pass, leading to dot product of zero in lighting equations (`NdotL = 0`).

#### Architecture Check

1. **What would make this NOT a bug?**
   An intentional optimization or shader variant where normals are discarded for certain materials, or a dynamic state where normals are computed on-the-fly in the lighting shader rather than stored in G-buffer.

2. **Expected collateral damage if true:**
   Any lit geometry relying on normals (specular highlights on other models, particles) would also be unlit or incorrectly lit. Skybox normals would fail similarly if using the same path—but skybox renders correctly, which may contradict this hypothesis.

3. **Why might engineers have written it this way?**
   To support flat-shaded or unlit materials efficiently by allowing absent normals (via MODEL_OFFSET_ABSENT sentinel), reducing vertex buffer size for performance.

#### Proposed Instrumentation

**A. Visualize G-buffer normals**
```glsl
// In deferred.frag
fragOut0 = vec4(normal * 0.5 + 0.5, 1.0);  // Remap [-1,1] to [0,1] for visualization
return;
```
- Ships appear grey (0.5, 0.5, 0.5) → normals are zero
- Ships appear colored → normals are valid

**B. Check normal offset in vertex pulling**
- Log `normalOffset` value in model draw calls
- Verify it's not MODEL_OFFSET_ABSENT

#### Evidence

Pending instrumentation.

#### Conclusion

Pending.

---

### Hypothesis 30: Attenuation factor clamped to zero

**Status: PENDING**

#### Observation

Deferred lighting path runs but outputs zero. Valid positions confirmed, but ships invisible while skybox (potentially using ambient-only path) shows.

#### Hypothesis

The attenuation term computes to zero for ship pixels because light positions/distances in DeferredLightUBO are incorrectly set relative to ship positions, causing all lights to be out-of-range for finite-attenuation models.

Mechanism:
- Light radius set too small
- Light positions incorrect
- Attenuation formula produces near-zero for actual distances

#### Architecture Check

1. **What would make this NOT a bug?**
   Ships are intentionally placed beyond max light radius for a scene-specific effect, or attenuation is overridden per-light via shader flags.

2. **Expected collateral damage if true:**
   Point/tube lights would fail to illuminate nearby objects like particles or debris. Distant skybox would be unaffected if using ambient-only path, matching observation.

3. **Why might engineers have written it this way?**
   To enforce physically-based falloff, preventing over-brightness in dense scenes, with attenuation computed in shader for flexibility across light types.

#### Proposed Instrumentation

**A. Visualize attenuation**
```glsl
// In deferred.frag, after computing attenuation
fragOut0 = vec4(attenuation, attenuation, attenuation, 1.0);
return;
```
- Ships appear black → attenuation is zero
- Ships appear grey/white → attenuation is valid

**B. Log light data**
- Log light positions, radii, and attenuation parameters from DeferredLightUBO
- Compare with ship positions

#### Evidence

Pending instrumentation.

#### Conclusion

Pending.

---

### Hypothesis 31: Diffuse color albedo is black/zero

**Status: PENDING**

#### Observation

Lighting calculation yields zero despite valid positions and execution. Ships invisible, but skybox (possibly using different material) visible.

#### Hypothesis

The `diffColor` term (albedo from G-buffer color attachment) is zero/black because base map textures for ships are not bound or are fully transparent/black in the deferred geometry pass, multiplying lighting to zero.

Possible causes:
- Texture not bound to bindless slot
- Texture sampling returns black (wrong UV, mipmap issue)
- Texture format mismatch
- Alpha channel causing discard

#### Architecture Check

1. **What would make this NOT a bug?**
   Ships use a special "invisible" material where albedo is zero for stealth effects, or color is modulated post-lighting in a separate pass.

2. **Expected collateral damage if true:**
   Unlit ships would still show if using emissive/glow maps, but they're invisible. Other textured geometry (UI elements) would also be black if sharing the bindless texture path.

3. **Why might engineers have written it this way?**
   To allow runtime material overrides via push constants (e.g., flags skipping baseMapIndex), enabling efficient cloaking or damage effects without texture swaps.

#### Proposed Instrumentation

**A. Visualize G-buffer albedo**
```glsl
// In deferred.frag
vec4 albedo = texture(gBufferColor, texCoord);
fragOut0 = vec4(albedo.rgb, 1.0);
return;
```
- Ships appear black → albedo is zero (texture issue)
- Ships appear textured → albedo is valid

**B. Force albedo in model.frag**
```glsl
// In model.frag
outColor = vec4(1.0, 0.0, 0.0, 1.0);  // Force red albedo
```
- Ships appear red in deferred → albedo reaching G-buffer
- Ships still invisible → issue elsewhere

#### Evidence

Pending instrumentation.

#### Conclusion

Pending.

---

### Hypothesis 32: computeLighting() returns zero due to missing lights

**Status: PENDING**

#### Observation

Zero lighting output in deferred path for ships. Position data valid, but no visible contribution from lights.

#### Hypothesis

The `computeLighting()` function (or equivalent in shader) sums to zero because no lights are being built or uploaded to DeferredLightUBO for the frame, possibly due to a failure in `buildDeferredLights()` filtering out all ship-relevant lights.

Possible causes:
- Light culling incorrectly excludes all lights
- Light count is zero in UBO
- Light data not uploaded (missing barrier, wrong buffer)

#### Architecture Check

1. **What would make this NOT a bug?**
   A per-frame optimization culls lights outside view frustum, and ships are in a lightless region intentionally.

2. **Expected collateral damage if true:**
   All deferred-lit geometry would be dark, including skybox if it uses the same light list. But skybox shows, suggesting skybox bypasses deferred lights (uses ambient-only or separate path).

3. **Why might engineers have written it this way?**
   To minimize UBO uploads by dynamically building light variants (Fullscreen/Sphere/Cylinder) only for active lights, reducing GPU bandwidth in light-heavy scenes.

#### Proposed Instrumentation

**A. Log light count**
- In `recordDeferredLighting()`, log the number of lights being rendered
- Already partially instrumented: logs show "lightCount: 5" per frame

**B. Visualize light contribution**
```glsl
// In deferred.frag
fragOut0 = vec4(diffuseLightColor, 1.0);
return;
```
- Ships appear black → light color is zero
- Ships appear colored → light color is valid

**C. Force constant light**
```glsl
// In deferred.frag
vec3 forcedLight = vec3(1.0, 1.0, 1.0);
fragOut0 = vec4(albedo.rgb * forcedLight, 1.0);
return;
```
- Ships appear textured → lighting calculation is the issue
- Ships still invisible → issue is upstream (albedo, position, or shader not executing)

#### Evidence

Partial evidence exists: Frame 197 analysis shows "lightCount: 5" per frame (ambient + 3 point + directional). Lights appear to be present.

#### Conclusion

Pending. Light count appears non-zero based on existing logs, but light data validity not yet confirmed.

---

### Hypothesis 33: View matrix Z inversion causes geometry to render behind camera

**Status: TESTING**

#### Observation

H23 follow-up test (2025-12-18) with multi-color diagnostic shader revealed:

1. One large ship (destroyer/station) appeared CYAN when player turned 180°
2. The ship was visually located behind the player, but game logic (physics, targeting) placed it in front
3. All other ships remained invisible — no diagnostic colors visible at any angle
4. The visible geometry may be from HUD targeting box rendering, not main 3D view

**Additional observation (2025-12-18):**

Reproducible test with clean shaders (no diagnostic code):
1. Load mission, turn off engines, do 180° turn
2. Station geometry visible at location opposite to its actual position
3. Turn toward actual station location — geometry no longer visible
4. Move camera to screen edge — geometry reappears at edge

This behavior is consistent with an inverted frustum or view transform, though other causes remain possible.

#### Hypothesis

The view matrix used in the Vulkan geometry pass has an inverted Z axis (or equivalent coordinate system error), causing:

1. Objects in front of the camera to transform to negative Z in view space
2. Projection to place these objects behind the near plane (culled) or behind the camera
3. Large objects to partially span the mirrored frustum, appearing visible when player turns around
4. Smaller objects to be entirely off-screen in the mirrored space

This would explain why:
- Physics/game logic has correct positions (uses different transform path)
- Visual rendering shows objects at mirrored locations
- Only one large object is visible (spans enough space to appear in mirrored frustum)
- Smaller ships are completely invisible (mirrored position is fully off-screen)

#### Architecture Check

1. **What would make this NOT a bug?**
   The coordinate system might be intentionally different for Vulkan (right-hand vs left-hand, Y-up vs Z-up). However, this would typically be handled in the projection matrix, not the view matrix, and would affect all rendering uniformly.

2. **Expected collateral damage if true:**
   - All 3D geometry would be mirrored/inverted
   - HUD elements rendered in screen space would be unaffected
   - Forward-rendered objects (targeting box) might use a different matrix path and render correctly

3. **Why might engineers have written it this way?**
   The view matrix construction in `matrix.cpp:30-47` negates position and forward vector:
   ```cpp
   vm_vec_copy_scale(&scaled_pos, pos, -1.0f);
   vm_vec_scale(&scaled_orient.vec.fvec, -1.0f);
   ```
   This is standard for OpenGL-style view matrix. If Vulkan expects different conventions (or if there's a sign error elsewhere), the result would be inverted geometry.

#### Proposed Instrumentation

**A. Position-Z visualization (ACTIVE)**

Modified `deferred.frag` to visualize position Z sign:
```glsl
float zSign = position.z < 0.0 ? 1.0 : 0.0;
float depth = clamp(abs(position.z) / 1000.0, 0.0, 1.0);
fragOut0 = vec4(zSign, depth, 1.0, 1.0);
```

**Interpretation:**
| Color | Meaning |
|-------|---------|
| Purple/Magenta (R=1, B=1) | Z is negative — geometry behind camera in view space |
| Cyan/Teal (G+B, no R) | Z is positive — geometry in front of camera |
| Brighter green component | Farther from camera |
| No blue | No geometry at that pixel |

**Expected results if H33 is correct:**
- Ships show purple/magenta (negative Z) when they should show cyan (positive Z)
- Large destroyer shows purple but is still visible due to size spanning frustum

**B. Compare OpenGL and Vulkan view matrices**
- Log `gr_view_matrix` values at render time for both backends
- Check for sign differences in Z-related components

**C. Force identity view matrix**
- Temporarily set view matrix to identity in Vulkan path
- If ships appear at screen center, view matrix is the issue

#### Evidence

**Instrumentation results (2025-12-18):**

Log comparison of GPU vs CPU view matrices:

| Source | Forward Vector (fvec) | Z Sign |
|--------|----------------------|--------|
| GPU `gr_view_matrix` | `{x:0.012859, y:0.135177, z:-0.990738}` | Negative |
| CPU `View_matrix` | `{x:-0.012741, y:-0.135188, z:0.990738}` | Positive |

Difference magnitude: 2.277441 (vectors point in opposite directions along Z)

Object position in view space from log:
```
viewPos: {x:-12.196, y:-26.455, z:-16.950}
```
Objects transformed with GPU matrix have negative Z.

**Relevant code paths:**

1. `create_view_matrix()` in `matrix.cpp:38` negates the forward vector:
   ```cpp
   vm_vec_scale(&scaled_orient.vec.fvec, -1.0f);
   ```

2. `g3_set_view_matrix()` in `3dsetup.cpp` assigns the matrix directly without negation.

3. Frustum culling in `3dmath.cpp` uses `z < MIN_Z` to check if objects are behind camera.

**Observations:**

- GPU view matrix (used for rendering) has negative Z forward
- CPU View_matrix (used for frustum culling) has positive Z forward
- This mismatch means objects passing CPU frustum culling (positive Z = in front) may have negative Z in GPU space
- The relationship between these two matrices and the observed rendering behavior requires further analysis

#### Attempted Fix #1 (2025-12-18)

Changes applied:
1. `3dsetup.cpp`: Negate forward vector in `g3_set_view_matrix()`
2. `3dmath.cpp`: Change frustum culling from `z < MIN_Z` to `z > -MIN_Z`

**Result: FAILED**

- Controls became inverted
- Ship still renders at wrong position
- Ship still disappears when facing actual location

The attempted fix did not resolve the issue and introduced new problems.

#### Attempted Fix #2 (2025-12-18)

Re-analysis of logs showed:
- Objects have negative Z in view space (e.g., `z:-3133.889`)
- Negative Z is forward in OpenGL convention
- Original `z < MIN_Z` check marks negative Z (in front) as behind — incorrect

Changes applied:
1. Reverted `g3_set_view_matrix()` negation from Fix #1
2. Changed frustum culling from `z < MIN_Z` to `z > MIN_Z` in `3dmath.cpp` (lines 41 and 154)

Rationale: If negative Z is forward, then:
- Negative Z = in front of camera (should pass culling)
- Positive Z = behind camera (should be culled)
- `z > MIN_Z` marks positive Z as behind

**Result: FAILED**

- OpenGL geometry now disappearing
- Frustum culling code in `3dmath.cpp` is shared between OpenGL and Vulkan
- Changing culling logic breaks the working OpenGL backend

This indicates the original culling logic (`z < MIN_Z`) was correct for OpenGL. The issue is Vulkan-specific and should not be fixed by modifying shared culling code.

#### Attempted Fix #3 (2025-12-18)

Additional frustum plane changes applied:
- Changed `x > z` to `x > -z`, `x < -z` to `x < z`, etc.
- Attempting to align frustum checks with "negative Z forward" convention

**Result: FAILED**

Objects continued to disappear. Analysis went in circles with Cursor questioning its own changes.

#### Key Insight

User observation: In the initial v1 Vulkan implementation, objects rendered correctly (though with wrong colors). No core engine logic (including frustum culling) was modified in v1.

**This rules out frustum culling as the cause.** If culling were broken, v1 Vulkan wouldn't have rendered objects at all.

#### Conclusion

The frustum culling investigation was a dead end. All changes to `3dmath.cpp` and `3dsetup.cpp` have been reverted.

The issue must be in Vulkan-specific code that changed since v1, not in shared engine infrastructure. Future investigation should focus on:
1. What changed in Vulkan rendering path since v1
2. Vulkan-specific matrix handling or shader differences
3. Descriptor binding or resource management changes

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
| 23. G-buffer position zero/invalid | INCONCLUSIVE | Unknown (test indeterminate for ships) |
| 24. Lighting calculation produces zero | PENDING | Unknown (depends on H23 follow-up) |
| 26. Float16 position overflow | PENDING | Unknown |
| 27. Depth test on fullscreen lighting pass | PENDING | Unknown (depends on H23 follow-up) |
| 28. Coordinate space mismatch | PENDING | Unknown |
| 29. Zero normals from vertex data | PENDING | Unknown |
| 30. Attenuation clamped to zero | PENDING | Unknown |
| 31. Diffuse color albedo is black | PENDING | Unknown |
| 32. Missing lights in UBO | PENDING | Partially contradicted (lightCount: 5 observed) |
| 33. View matrix Z inversion | REJECTED | No (frustum culling ruled out) |

**Current Status:** H33 frustum culling investigation was a dead end. User confirmed v1 Vulkan rendered objects correctly without modifying core engine logic. Changes to shared culling code broke OpenGL and did not fix Vulkan. All changes reverted.

**Next Step:** Investigate what changed in Vulkan-specific code since v1 that could cause the position anomaly.
