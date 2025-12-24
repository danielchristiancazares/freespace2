# Vulkan Pipeline Commit Message Fixes

This document lists all the commit message fixes needed for the Vulkan pipeline work.

## Issues Found

1. **Duplicate messages**: Commit `0f75add42` has the wrong message (should be about deferred lighting infrastructure)
2. **Missing body text**: Several commits have no body or inconsistent formatting
3. **Multiple subjects**: Some commits have multiple subjects merged together
4. **Format inconsistencies**: Some commits don't follow the `<type>(<scope>): <imperative>` format with bulleted lists

## Commits to Fix

### 1. 0f75add42 - WRONG MESSAGE (off by one)
**Current**: `refactor(vulkan): split renderer into device/session layers and add UI shaders`  
**Should be**: `feat(vulkan): add deferred lighting infrastructure`

**New message**:
```
feat(vulkan): add deferred lighting infrastructure

Add deferred lighting shaders and infrastructure before implementing the full pass.

- Add deferred.vert/frag shaders and compiled SPIR-V
- Add VulkanDeferredLights{.h,.cpp} with light UBO structures
- Add deferred pipeline layout to descriptor layouts
- Update shader manager and CMake shader lists
- Add memcpy overloads for const/void* in pstypes.h
- Add documentation for deferred lighting patches and stub render functions
```

### 2. c86e49347 - Missing body
**Current**: `fix(vulkan): bindless texture slots and swapchain no-depth loadOp` (no body)

**New message**:
```
fix(vulkan): bindless texture slots and swapchain no-depth loadOp

- Fix bindless texture slot management and cleanup
- Fix swapchain no-depth loadOp configuration
- Update Vulkan README with accurate module documentation
- Remove stale REFACTOR.md file
```

### 3. 0851ab406 - Missing body
**Current**: `fix(vulkan): sync2 present barrier and VulkanFrame reset` (no body)

**New message**:
```
fix(vulkan): sync2 present barrier and VulkanFrame reset

- Fix sync2 present barrier stage mask configuration
- Fix VulkanFrame command pool reset handling for VULKAN_HPP_NO_EXCEPTIONS
```

### 4. 190a7ea4e - Missing body
**Current**: `refactor(vulkan): require UploadCtx for texture flush` (no body)

**New message**:
```
refactor(vulkan): require UploadCtx for texture flush

- Require UploadCtx for texture flush operations to prevent uploads from draw paths
- Ensure upload operations are properly gated by UploadCtx
```

### 5. 711281ebe - Missing body
**Current**: `refactor(vulkan): improve validation logs and drop dead shader TU` (no body)

**New message**:
```
refactor(vulkan): improve validation logs and drop dead shader TU

- Improve Vulkan validation logging clarity
- Remove dead shader translation unit
```

### 6. b3657cc08 - Missing body
**Current**: `feat(vulkan): stage uploads for device-local static buffers` (no body)

**New message**:
```
feat(vulkan): stage uploads for device-local static buffers

- Stage uploads for device-local static buffers to improve performance
- Optimize buffer upload path for static content using staging buffers
```

### 7. a8ee888ab - Inconsistent format
**Current**: Has body but not in bullet format

**New message**:
```
fix(vulkan): deferred lighting trails

- Preserve pre-deferred scene color for trail rendering
- Ensure scene color is available after deferred lighting pass
```

### 8. 9ec9ffa67 - Inconsistent format
**Current**: Has mixed format (some bullets, some not)

**New message**:
```
feat(vulkan): implement bitmap render targets

- Add bmpman RTT support (bm_make_render_target/bm_set_render_target) backed by VulkanTextureManager and VulkanRenderingSession BitmapTarget
- Honor engine viewport/scissor changes via gf_set_viewport and scissor updates on clip changes
- Stop session from overriding viewport
- Fix scissor clamp and RTT handle reuse
- Stub envmap/irrmap hooks and fix bm_free_data
- Update QA/remediation docs and Vulkan README
```

### 9. c37958dc3 - Inconsistent format
**Current**: Has body but not in bullet format

**New message**:
```
fix(vulkan): HUD alpha, 16bpp unpack, and dynamic buffer orphaning

- Orphan dynamic/streaming buffer updates to prevent corruption
- Fix AA bitmap coverage issues
- Fix 16bpp texture unpacking
```

### 10. 4039c8e94 - Missing body
**Current**: `fix(vulkan): gr_update_texture uploads` (no body)

**New message**:
```
fix(vulkan): gr_update_texture uploads

- Fix texture upload handling in gr_update_texture
- Ensure uploads are properly queued and flushed
```

### 11. 3aa94d43f - Multiple subjects merged
**Current**: Has multiple refactor messages merged together

**New message**:
```
refactor(vulkan): make render pass session-owned and enforce RenderCtx

- Replace per-draw RenderScope RAII with a session-owned active pass ended at boundaries
- Introduce RenderCtx token (ensureRenderingStarted) and update Vulkan draw paths + docs
- Replace deferred lighting enum with typestate tokens
- Use move-only DeferredGeometryCtx/DeferredLightingCtx tokens to enforce begin/end/finish ordering
- Thread RenderCtx into model draw helper and make draw paths use RenderCtx cmd
- Gate deferred lighting recording on RenderCtx
- Hide RecordingFrame cmd outside VulkanRenderer
```

### 12. 25552b06b - Multiple subjects merged
**Current**: Has multiple refactor messages merged together

**New message**:
```
refactor(vulkan): make TextureId non-forgeable and improve bindless slots

- Replace sentinel-based TextureId with an optional factory and update call sites to construct IDs only from validated base-frame handles
- Make texture state container-based
- Make bindless slots always valid
- Drop partially-bound bindless requirement
- Reserve default bindless slots for model material textures
- Remove absent-texture sentinel routing by reserving slots for base/normal/spec defaults and always sampling in model.frag
```

### 13. 43eed3432 - Multiple subjects merged
**Current**: Has multiple fix/refactor messages merged together

**New message**:
```
perf(vulkan): batch model descriptor writes

- Have beginModelDescriptorSync build a slot list and update binding 0/1 in a single vkUpdateDescriptorSets call via updateModelDescriptors
- Remove unused per-slot write helpers
- Update model bindless descriptors by dirty slots
- Replace MODEL_OFFSET_ABSENT with attrib mask
- Wire gr_update_transform_buffer and bind a dynamic SSBO for batched submodel matrices (MODEL_SDR_FLAG_TRANSFORM)
- Extend model push constants for MODEL_ID offset and apply orient transforms in the Vulkan model vertex shader
- Sample bindless textures as 2D arrays (layer 0)
- Apply sRGB->linear for base/emissive
- Decode DXT5nm-style normal maps
- Allow mip sampling by removing maxLod clamp
- Initialize matrix state during init
```

### 14. 755c9a4a8 - Multiple subjects merged
**Current**: Has multiple fix/refactor messages merged together

**New message**:
```
fix(vulkan): make bindless slot eviction serial-safe

- Track lastUsedSerial per texture and only evict bindless slots once the GPU has completed that serial
- Bindings now mark textures used on descriptor binds
- Make texture retirement drop cache records right away and move GPU handles into the serial-gated deferred release queue
- Avoid long-lived retired records blocking re-requests while keeping in-flight descriptor users safe
- Add VulkanRenderer-constructible UploadCtx and require it for VulkanTextureUploader::flushPendingUploads
- Prevent upload recording from being invoked from draw paths
- Make VulkanTextureManager upload/retire helpers private and expose flushPendingUploads only via the UploadCtx-gated VulkanTextureUploader
```

### 15. 9dea62e78 - Inconsistent format
**Current**: Has body but not in bullet format

**New message**:
```
refactor(vulkan): split texture bindings from uploader

- Introduce TextureId and a bindings/uploader boundary so draw code can queue uploads without recording GPU work
- Update descriptor request APIs to be command-buffer-free and route model bindless indices through VulkanRenderer helpers
- Remove the unused Uploading/markUploadsCompleted path
- Update docs to match current behavior (uploads are recorded during beginFrame on the primary command buffer)
```

### 16. a84621042 - Format cleanup
**Current**: Has body but could be better formatted

**New message**:
```
refactor(vulkan): split renderer into device/session layers and add UI shaders

Vulkan renderer refactor and new UI/batched rendering paths.

- Introduce VulkanDevice, VulkanRenderTargets, and update VulkanFrame/session flow (swapchain, dynamic rendering, per-frame resources, pipeline cache, serial info)
- Adjust descriptor/pool sizing and device-limit validation; unify kFramesInFlight = 2
- Add SDR_TYPE_INTERFACE and batched-bitmap shaders (GL/Vulkan); wire through materials (material_set_*), gr_aabitmap_list, and GL shader uniforms
- Fix dynamic-state issues: EDS3 blend enable now reflects material, applies current clip scissor, validates pipeline vs attachment/vertex inputs, and sets depth/cull/front-face dynamically
- OpenGL: guard null timer-query pointers and add GLAD diagnostics; add interface shader plumbing
- Buffer manager now defers GPU resource deletion; minor cleanup/renames, README refresh, and .gitignore
```

### 17. e6cefeca6 - Multiple subjects merged
**Current**: Has multiple docs messages merged together

**New message**:
```
docs(vulkan): update architecture documentation

- Add Vulkan renderer bug investigation report
- Document GPT-5.2 Pro analysis findings (5 critical, 4 high, 3 medium severity issues)
- Track ring buffer wrap, descriptor pool explosion, and layout tracking problems
- Include build.sh for macOS builds
- Update bug report and remediation plan
- Align remediation plan with state-as-location
- Refresh Vulkan QA review and remediation plan
- Add and update module READMEs
- Update design philosophy with reading guidelines
```

## How to Apply Fixes

1. Make sure you're on the correct branch and have no uncommitted changes
2. Run: `git rebase -i 95b27e4c3^`
3. For each commit listed above, change `pick` to `reword`
4. When prompted for each commit, replace the message with the corrected version above
5. After rebase completes, force push: `git push --force-with-lease`

## Summary

- **Total commits to fix**: 17
- **Most critical**: Commit `0f75add42` has completely wrong message (off by one error)
- **Format issues**: 13 commits need body text or format standardization
- **Multiple subjects**: 5 commits have multiple subjects that should be consolidated

