# Vulkan Renderer Bug Report: Negative Scissor Offset Y (-3)

**Date:** 2025-12-14  
**Subsystem:** Vulkan backend clip/scissor handling (`gr_set_clip` → `vkCmdSetScissor`)  
**Status:** Analysis and hypotheses (not yet validated as root cause)

## Summary

The Vulkan backend emits validation warnings indicating that `vkCmdSetScissor()` is called with a negative scissor Y offset (example: `-3`). This violates the Vulkan requirement that scissor offsets be non-negative (VUID `VUID-vkCmdSetScissor-x-00595`), and may lead to undefined behavior depending on driver and validation behavior.

## Evidence

Observed log excerpt (representative):

```text
Vulkan: Vulkan message: [4096] vkCmdSetScissor(): pScissors[0].offset.y (-3) is negative.
The Vulkan spec states: The x and y members of offset member of any element of pScissors must be greater than or equal to 0
```

## Relevant Code Path (Current Implementation)

The Vulkan backend sets the scissor rectangle from the engine’s global `gr_screen` clip state. In Vulkan, the scissor is built as:

- `createClipScissor()` reads `gr_screen.offset_x`, `gr_screen.offset_y`, `gr_screen.clip_width`, and `gr_screen.clip_height`, and uses those as `vk::Rect2D.offset` and `vk::Rect2D.extent` (`code/graphics/vulkan/VulkanGraphics.cpp:51`, `code/graphics/vulkan/VulkanClip.h:16`).
- The scissor is applied frequently, including at frame setup (`code/graphics/vulkan/VulkanGraphics.cpp:103`, `code/graphics/vulkan/VulkanGraphics.cpp:124`) and after clip changes (`code/graphics/vulkan/VulkanGraphics.cpp:250`).

The clip state itself is updated by the Vulkan `gr_set_clip` implementation:

- `gr_set_clip()` calls the active backend’s function pointer (`code/graphics/2d.h:1090`).
- On Vulkan, `gr_vulkan_set_clip()` calls `applyClipToScreen(x, y, w, h, resize_mode)` and then immediately applies `cmd.setScissor(...)` using `createClipScissor()` (`code/graphics/vulkan/VulkanGraphics.cpp:250`, `code/graphics/vulkan/VulkanGraphics.cpp:257`).

Inside `applyClipToScreen()`:

- The input `x` and `y` are clamped to `>= 0` before any resize math (`code/graphics/vulkan/VulkanClip.h:33`).
- If resizing is active, the function calls `gr_resize_screen_pos(&x, &y, &w, &h, resize_mode)` (`code/graphics/vulkan/VulkanClip.h:89`).
- The final computed `y` is assigned into `gr_screen.offset_y` (`code/graphics/vulkan/VulkanClip.h:97`), which later becomes the Vulkan scissor’s `offset.y`.

## Primary Hypothesis (Most Likely)

The negative scissor offset is produced when `applyClipToScreen()` calls `gr_resize_screen_pos()`, and `gr_resize_screen_pos()` computes a negative `y` for certain resize modes, despite the initial `y >= 0` clamp.

For the menu/UI resize path, `gr_resize_screen_pos()` computes:

`y = fl2ir(y * Gr_resize_Y + Gr_menu_offset_Y)` for `GR_RESIZE_MENU` (`code/graphics/2d.cpp:891`, `code/graphics/2d.cpp:901`, `code/math/floating.h:41`).

This means a small negative scissor like `-3` is consistent with `Gr_menu_offset_Y` (or the full expression) being slightly negative at the time `gr_set_clip()` is called.

### Why `Gr_menu_offset_Y` can be negative

`Gr_menu_offset_Y` is computed by `gr_set_screen_scale()` and may become negative in the zoom path:

`Gr_menu_offset_Y = ((center_h - h * Gr_resize_Y) / 2.0f) + gr_screen.center_offset_y` (`code/graphics/2d.cpp:760`, `code/graphics/2d.cpp:767`).

If `h * Gr_resize_Y > center_h`, the expression becomes negative. After rounding (`fl2ir`), small negative integers such as `-3` are plausible (`code/math/floating.h:41`).

## Secondary Hypotheses (Plausible, Not Yet Proven)

### Hypothesis A: Screen-scale state leaks across UI/state transitions

One plausible trigger is a code path that calls `gr_set_screen_scale(..., zoom_w, zoom_h, ...)` (activating the `do_zoom` branch) and fails to call `gr_reset_screen_scale()` afterward, leaving negative `Gr_menu_offset_Y` active when later UI code uses `GR_RESIZE_MENU` with `gr_set_clip()`.

This is a general state-management hypothesis; the evidence to confirm it would be observing `Gr_menu_offset_Y < 0` at the time the scissor is set.

### Hypothesis B: Lua scripting sets screen scale and does not restore it

Lua exposes `Graphics.setScreenScale(...)` and `Graphics.resetScreenScale()` (`code/scripting/api/libs/graphics.cpp:1524`, `code/scripting/api/libs/graphics.cpp:1545`). A script can set a zoomed scale (which may produce a negative menu offset depending on parameters) and forget to reset it, causing later UI clipping to compute negative `gr_screen.offset_y`.

This hypothesis becomes more likely if the bug is mod-dependent or appears only with particular scripted UIs.

### Hypothesis C: ImGui Vulkan backend scissor (less likely)

ImGui’s Vulkan backend does issue scissor commands (`lib/imgui/backends/imgui_impl_vulkan.cpp:546`), but it clamps negative clip coordinates to `0` before calling `vkCmdSetScissor()` (`lib/imgui/backends/imgui_impl_vulkan.cpp:532`). This makes ImGui a less likely source of an exact negative offset unless the clamp logic is bypassed by invalid floating-point values (e.g., NaNs) or other upstream corruption.

### Hypothesis D: Memory corruption of `gr_screen`

If runtime instrumentation shows `gr_screen.offset_y` is never negative when computed, but the scissor still receives a negative offset, then corruption of the `gr_screen` struct (or an unrelated `vkCmdSetScissor` call site) becomes a more credible explanation. At present, this remains a fallback hypothesis.

## Proposed Diagnostics (To Validate Root Cause)

1. Instrument immediately before `cmd.setScissor(...)` in the Vulkan backend to log `gr_screen.offset_y`, `gr_screen.clip_height`, the computed scissor rect, and the caller context (frame setup vs `gr_set_clip`). The primary call sites are `code/graphics/vulkan/VulkanGraphics.cpp:126` and `code/graphics/vulkan/VulkanGraphics.cpp:257`.
2. Instrument inside `applyClipToScreen()` after the `gr_resize_screen_pos()` call to log the computed `(x, y, w, h)` and `resize_mode` (`code/graphics/vulkan/VulkanClip.h:89`, `code/graphics/vulkan/VulkanClip.h:97`).
3. Log the active scale parameters contributing to `GR_RESIZE_MENU`, especially `Gr_resize_Y` and `Gr_menu_offset_Y`, when `gr_resize_screen_pos()` runs (`code/graphics/2d.cpp:858`).
4. If the scissor is negative but `gr_screen.offset_y` is not, add a breakpoint on `vkCmdSetScissor` to identify the originating call site (engine vs ImGui).

## Notes

The OpenGL backend converts from the engine’s top-left UI coordinates to OpenGL’s bottom-left scissor coordinates when not rendering to a texture (`code/graphics/opengl/gropengl.cpp:245`). The Vulkan backend, by contrast, uses `gr_screen.offset_y` directly as the scissor Y offset (`code/graphics/vulkan/VulkanGraphics.cpp:55`) and relies on a flipped viewport (`code/graphics/vulkan/VulkanGraphics.cpp:43`) to match OpenGL-style rendering coordinates. The observed issue is not a coordinate-system mismatch by itself; it is a violation caused by a negative computed offset entering the Vulkan scissor API.
