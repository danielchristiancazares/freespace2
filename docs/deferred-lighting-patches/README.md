# Deferred Lighting Pass Implementation Notes

This directory contains draft implementation files for the Vulkan deferred lighting pass.
The code is approximately 60% complete and requires fixes before it will compile.

## Files

- `VulkanDeferredLightingPass.h` - Header with light types and pass class
- `VulkanDeferredLightingPass.cpp` - Implementation (needs API fixes)

## Known Issues to Fix

### 1. VulkanRenderer API Mismatches

The code assumes APIs that don't exist on VulkanRenderer. Fix by using the actual accessors:

| GPT Assumed | Actual API |
|-------------|------------|
| `m_renderer.device()` | `m_renderer.vulkanDevice()->device()` |
| `m_renderer.getDescriptorLayouts().globalLayout()` | `m_renderer.descriptorLayouts()->globalLayout()` |
| `m_renderer.extDyn3Caps()` | `m_renderer.vulkanDevice()->extDyn3Caps()` |
| `m_renderer.getPipelineWithLayout()` | Does not exist - needs to be added or use existing `getPipeline()` |

### 2. Missing VulkanRenderer Methods

These methods need to be added to VulkanRenderer.h/.cpp:

```cpp
// In VulkanRenderer.h public section:
void queueDeferredLight(DeferredLight light);
void recordDeferredLighting(vk::CommandBuffer cmd, VulkanFrame& frame);

// In VulkanRenderer.h private section:
std::unique_ptr<VulkanDeferredLightingPass> m_deferredLighting;
std::vector<DeferredLight> m_deferredLights;
```

### 3. VulkanShaderManager Update

Add case for SDR_TYPE_DEFERRED_LIGHTING in getModules():

```cpp
case shader_type::SDR_TYPE_DEFERRED_LIGHTING: {
    const auto vertPath = fs::path(m_shaderRoot) / "deferred.vert.spv";
    const auto fragPath = fs::path(m_shaderRoot) / "deferred.frag.spv";
    return {loadIfMissing(m_vertexModules, vertPath.string()),
            loadIfMissing(m_fragmentModules, fragPath.string())};
}
```

### 4. VulkanGraphics.cpp Update

Replace the stub in `gr_vulkan_deferred_lighting_finish()`:

```cpp
void gr_vulkan_deferred_lighting_finish()
{
    if (!renderer_instance) {
        return;
    }
    VulkanFrame* frame = renderer_instance->getCurrentRecordingFrame();
    if (!frame) {
        return;
    }
    vk::CommandBuffer cmd = frame->commandBuffer();

    renderer_instance->bindDeferredGlobalDescriptors();
    renderer_instance->setPendingRenderTargetSwapchain();
    renderer_instance->recordDeferredLighting(cmd, *frame);
}
```

### 5. Pipeline Layout Incompatibility

The deferred lighting pass uses a different pipeline layout than the standard pipeline:
- Set 0: Per-light dynamic UBO (not push descriptors)
- Set 1: Global G-buffer samplers (same as existing)

This requires either:
1. Adding a `getPipelineWithLayout()` method to VulkanPipelineManager, OR
2. Creating pipelines directly in VulkanDeferredLightingPass

### 6. Light Building

The code has no integration with the engine's light system. Need to add a function
(likely in VulkanGraphics.cpp) that converts engine `light` structs to `DeferredLight`
variants and calls `renderer_instance->queueDeferredLight()`.

## Integration Order

1. Fix VulkanRenderer accessor methods
2. Add shader manager case
3. Add VulkanRenderer members and methods
4. Initialize m_deferredLighting in VulkanRenderer::initialize()
5. Update VulkanGraphics.cpp stub
6. Add light conversion code
7. Test compile
