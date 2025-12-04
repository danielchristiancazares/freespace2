# Bug fix: sampler2D bound to 2D_ARRAY views (VUID-07752)

**Status**: **FIXED**

## Problem

Validation error `VUID-vkCmdDraw-viewType-07752`: shaders declared `sampler2D` but received `VK_IMAGE_VIEW_TYPE_2D_ARRAY` views, causing undefined behavior and rendering corruption.

## Root Cause

`bindMaterialDescriptors()` used a hardcoded list of bindings (0,1,2,3,9,10) to determine which needed array views. This didn't account for different shader types having different expectations per binding.

## Fix

Added `shaderUsesArrayView(shader_type, binding)` in `code/graphics/vulkan/gr_vulkan.cpp` to determine correct view type based on shader type:

```cpp
static bool shaderUsesArrayView(shader_type shaderType, uint32_t binding)
{
    switch (shaderType) {
        case SDR_TYPE_MODEL:
            return (binding <= 3) || binding == 8 || binding == 9 || binding == 10;
        case SDR_TYPE_EFFECT_PARTICLE:
        case SDR_TYPE_EFFECT_DISTORTION:
        case SDR_TYPE_BATCHED_BITMAP:
        case SDR_TYPE_NANOVG:
        case SDR_TYPE_ROCKET_UI:
        case SDR_TYPE_SHIELD_DECAL:
            return binding == 0;
        case SDR_TYPE_DECAL:
            return binding >= 2 && binding <= 4;
        case SDR_TYPE_VIDEO_PROCESS:
            return binding <= 2;
        case SDR_TYPE_COPY:
        case SDR_TYPE_COPY_WORLD:
            return binding == 0;
        default:
            return false;
    }
}
```

Updated `bindMaterialDescriptors()` to use `mat->get_shader_type()` and call `shaderUsesArrayView()` instead of hardcoded binding list.

