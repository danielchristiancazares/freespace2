# Bug fix: `samplerCube` bound to non-cube view

**Status**: **FIXED**

## Problem

`gr_vulkan_calculate_irrmap` updated envmap binding 4 with `getImageView()` (2D/array view) even though the shader expects `samplerCube`, triggering viewType validation errors and incorrect sampling.

## Root Cause

1. `VulkanTexture::create()` never set `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` flag
2. `createRenderTarget()` computed cube flags but didn't pass them to `create()`
3. No cube view (`VK_IMAGE_VIEW_TYPE_CUBE`) was ever created
4. Irradiance generation bound a 2D/2D_ARRAY view to a samplerCube descriptor

## Fix

1. Added `vk::ImageCreateFlags flags` parameter to `VulkanTexture::create()`
2. Set `imageInfo.flags = flags` when creating the image
3. Create `m_cubeImageView` with `VK_IMAGE_VIEW_TYPE_CUBE` when `cubeCompatible && arrayLayers >= 6`
4. Added `getCubeImageView()` accessor
5. Updated `createRenderTarget()` and `createTexture()` to pass cube flags for 6-layer cubemaps
6. Changed `gr_vulkan_calculate_irrmap()` to use `getCubeImageView()` for envmap binding
7. Added Vulkan layout qualifiers to `irrmap-f.sdr` shader

