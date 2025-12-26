# Vulkan Shader Coverage Plan

## Status

VulkanShaderManager implements 22 of 33 shader types. Missing types will throw at runtime (fail-fast).

## Implemented

- SDR_TYPE_MODEL
- SDR_TYPE_POST_PROCESS_MAIN
- SDR_TYPE_POST_PROCESS_BLUR
- SDR_TYPE_POST_PROCESS_BLOOM_COMP
- SDR_TYPE_POST_PROCESS_BRIGHTPASS
- SDR_TYPE_POST_PROCESS_FXAA
- SDR_TYPE_POST_PROCESS_FXAA_PREPASS
- SDR_TYPE_POST_PROCESS_LIGHTSHAFTS
- SDR_TYPE_POST_PROCESS_TONEMAPPING
- SDR_TYPE_POST_PROCESS_SMAA_EDGE
- SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT
- SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING
- SDR_TYPE_DEFERRED_LIGHTING
- SDR_TYPE_PASSTHROUGH_RENDER
- SDR_TYPE_SHIELD_DECAL
- SDR_TYPE_BATCHED_BITMAP
- SDR_TYPE_DEFAULT_MATERIAL
- SDR_TYPE_INTERFACE
- SDR_TYPE_NANOVG
- SDR_TYPE_ROCKET_UI
- SDR_TYPE_COPY
- SDR_TYPE_FLAT_COLOR

## Missing

| Shader Type | Priority | Notes |
|-------------|----------|-------|
| SDR_TYPE_EFFECT_PARTICLE | High | Particle effects |
| SDR_TYPE_EFFECT_DISTORTION | High | Shockwave/distortion effects |
| SDR_TYPE_DECAL | High | Decal rendering |
| SDR_TYPE_DEFERRED_CLEAR | Medium | Deferred pass clear |
| SDR_TYPE_VIDEO_PROCESS | Medium | Video playback (movie player uses VulkanMovieManager) |
| SDR_TYPE_SCENE_FOG | Medium | Scene fog effects |
| SDR_TYPE_VOLUMETRIC_FOG | Medium | Volumetric fog |
| SDR_TYPE_COPY_WORLD | Low | World-space copy |
| SDR_TYPE_MSAA_RESOLVE | Low | MSAA resolve (Vulkan uses native resolve) |
| SDR_TYPE_ENVMAP_SPHERE_WARP | Low | Environment map warping |
| SDR_TYPE_IRRADIANCE_MAP_GEN | Low | PBR irradiance generation |
