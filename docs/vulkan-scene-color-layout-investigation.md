# Vulkan Scene Color Image Layout Investigation

## Status: IN PROGRESS

## Problem Summary

Validation errors occur during frame rendering related to scene color image layout mismatches:

```
VUID-vkCmdDraw-None-09600: vkQueueSubmit(): pSubmits[0] command buffer VkCommandBuffer 0xc1f39f0
expects VkImage 0x190000000019 ... to be in layout VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
--instead, current layout is VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
```

## Root Cause Analysis

### Scene Color Image Lifecycle

The scene color image transitions through these layouts each frame:

1. **Frame Start (beginScenePass)**: `UNDEFINED` or `SHADER_READ_ONLY` -> `COLOR_ATTACHMENT_OPTIMAL`
2. **Rendering**: Used as color attachment
3. **Frame End (endScenePass)**: `COLOR_ATTACHMENT_OPTIMAL` -> `SHADER_READ_ONLY_OPTIMAL` (for blit sampling)
4. **Next Frame**: Should use `SHADER_READ_ONLY` as oldLayout, but was using `UNDEFINED`

### Partial Fix Applied

Added `m_sceneColorInShaderReadLayout` tracking flag to VulkanRenderer:

- Set to `true` in `endScenePass()` after transitioning to `SHADER_READ_ONLY`
- Used in `beginScenePass()` to select correct `oldLayout` for barrier
- Reset to `false` in `createSceneFramebuffer()` when image is recreated

**Result**: Scene passes now correctly use `oldLayout=SHADER_READ_ONLY` on frame 2+.

### Remaining Issue

Validation errors STILL occur during **direct passes** (menu/UI rendering to swapchain):

- Direct passes render to swapchain, NOT scene framebuffer
- Direct passes should NOT touch the scene color image
- Yet validation reports a descriptor references scene color with expected `COLOR_ATTACHMENT_OPTIMAL`

## Key Observations

1. **We never write descriptors with `COLOR_ATTACHMENT_OPTIMAL`**:
   - `updateCombinedImageSampler()` always uses `SHADER_READ_ONLY_OPTIMAL`
   - Only render pass attachments use `COLOR_ATTACHMENT_OPTIMAL`

2. **The error occurs at vkQueueSubmit, not vkCmdDraw**:
   - Validation checks descriptor bindings at submit time
   - Some descriptor references an image with wrong expected layout

3. **Possible causes**:
   - Stale descriptor set being reused from previous frame
   - Descriptor pool aliasing issue
   - Validation layer tracking confusion across multiple command buffer submissions

## Files Modified

### VulkanRenderer.h
- Added `m_sceneColorInShaderReadLayout` tracking flag

### VulkanRenderer.cpp
- `beginScenePass()`: Uses tracked state to select correct oldLayout
- `endScenePass()`: Sets tracking flag after layout transition
- `createSceneFramebuffer()`: Resets tracking flag
- `flip()`: Added diagnostic logging for layout state

### VulkanTexture.cpp
- Added diagnostic logging for cubemap uploads
- Fixed `createFromImageViews()` call to pass external images for barriers

### VulkanFramebuffer.cpp/h
- Extended `createFromImageViews()` to accept external image handles
- Needed for proper barrier recording on render target framebuffers

## Next Steps

1. **Add descriptor binding diagnostics**:
   - Log which descriptor sets are bound during direct passes
   - Track if any descriptor references scene color image view

2. **Investigate descriptor pool reuse**:
   - Check if freed descriptors are being reused before GPU completes
   - May need per-frame descriptor pools

3. **Check blit descriptor lifecycle**:
   - Blit descriptor references scene color image
   - Verify it's not bound during direct passes

4. **Validate layout transition completeness**:
   - Ensure all barriers complete before next submission
   - Check for missing synchronization between passes

## Related Issues

- Upload command buffer reuse (FIXED)
- External framebuffer image access for RT barriers (FIXED)
- Descriptor set freed same frame as bound (PENDING)

## Diagnostic Output Format

```
flip LAYOUT STATE entry #N sceneColorInShaderRead=X sceneImg=Y scene=Z direct=W
beginScenePass LAYOUT FIX image=X isScene=Y inShaderRead=Z oldLayout=LAYOUT
endScenePass LAYOUT TRACK set m_sceneColorInShaderReadLayout=true image=X
```
