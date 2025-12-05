# Bug fix: Off-center reticle (Vulkan)

**Status**: **FIXED**

## Problem

Targeting reticle started centered for ~0.5 seconds, then moved to top-left corner. Other HUD elements remained correctly positioned.

## Root cause

Mid-frame render-to-texture operations (cockpit displays, radar) called `gr_set_viewport()` with small dimensions (e.g., `131x112`), which set `gr_screen.clip_width/clip_height` to those values. When `g3_start_frame()` was later called, it picked up these small values and set `Canvas_width/Canvas_height` accordingly. The `HUD_get_nose_coordinates()` function used `g3_project_vertex()`, which projected using the tiny canvas dimensions, producing coordinates like `(65, 56)` instead of the correct `(1920, 1080)`.

**Call chain that caused the bug**:
1. `gr_set_viewport(0, 2048, 131, 112)` – sets small clip region for render target
2. `g3_start_frame()` – copies `gr_screen.clip_width/height` to `Canvas_width/Canvas_height`
3. `HUD_get_nose_coordinates()` → `g3_project_vertex()` – projects using wrong canvas size
4. Reticle renders at wrong position (top-left)

## Fix

Modified `HUD_get_nose_coordinates()` in `code/hud/hud.cpp` to manually project using `gr_screen.max_w/max_h` (always full screen dimensions) instead of calling `g3_project_vertex()` (which uses potentially-corrupted `Canvas_width/Canvas_height`).

```cpp
// Project vertex manually using full screen dimensions instead of g3_project_vertex(),
// which uses Canvas_width/Canvas_height that may be set to a small render target.
float screen_w = static_cast<float>(gr_screen.max_w);
float screen_h = static_cast<float>(gr_screen.max_h);

float w = 1.0f / v0.world.xyz.z;
x_nose = (screen_w + (v0.world.xyz.x * screen_w * w)) * 0.5f;
y_nose = (screen_h - (v0.world.xyz.y * screen_h * w)) * 0.5f;

float x_center = screen_w * 0.5f;
float y_center = screen_h * 0.5f;

*x = fl2i(x_nose - x_center);
*y = fl2i(y_nose - y_center);
```

## Investigation notes (for future reference)

Failed approaches before finding root cause:
1. Viewport tracking (`gr_vulkan_set_viewport`) – viewport values were correct, not the issue
2. DrawState dirty flag logic – did not fix
3. Scene pass timing – did not fix

The key insight was tracing the log to find `gr_set_viewport(0, 2048, 131, 112)` being called mid-frame, which corrupted the projection state used by HUD calculations.



