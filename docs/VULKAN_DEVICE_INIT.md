# Vulkan Device Initialization

This document describes how FSO initializes Vulkan, selects a physical device, and creates the logical device with required features. Understanding this flow is essential for debugging driver compatibility issues and extending Vulkan support.

## Overview

Device initialization follows a strict sequence:

1. **Instance creation** - Vulkan entry point with validation layers
2. **Surface creation** - Platform-specific window surface via SDL
3. **Physical device selection** - GPU enumeration, scoring, and validation
4. **Logical device creation** - Queues, extensions, and feature chain
5. **Swapchain creation** - Presentation images and views

All initialization logic lives in `code/graphics/vulkan/VulkanDevice.cpp`.

## Minimum Vulkan Version

FSO requires **Vulkan 1.4** as the minimum API version. This is checked at both instance and device levels:

```cpp
const gameversion::version MinVulkanVersion(1, 4, 0, 0);
```

Devices reporting `apiVersion < VK_API_VERSION_1_4` are rejected during physical device selection. This requirement exists because FSO depends on features promoted to core in Vulkan 1.4, particularly push descriptors.

## Instance Creation

### Entry Point Initialization

FSO uses SDL to obtain the Vulkan entry point:

```cpp
const auto vkGetInstanceProcAddr =
    reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
```

This initializes the vulkan.hpp dynamic dispatcher, which handles all Vulkan function pointer resolution.

### Required Extensions

Instance extensions are queried from SDL based on the windowing system:

```cpp
SDL_Vulkan_GetInstanceExtensions(window, &count, extensions.data());
```

These typically include `VK_KHR_surface` and platform-specific extensions like `VK_KHR_win32_surface`.

### Validation Layers

When `FSO_DEBUG` is defined or `-graphics_debug_output` is passed, FSO enables:

| Layer/Extension | Purpose |
|-----------------|---------|
| `VK_LAYER_KHRONOS_validation` | Validation layer for error checking |
| `VK_EXT_debug_utils` | Debug messenger for validation callbacks |

The debug callback (`debugReportCallback`) implements rate-limiting to prevent log spam from repeated validation messages. Errors are logged first 10 times, warnings first 3 times, then periodically every 50/200 occurrences respectively.

### Apple Portability

On macOS, FSO enables `VK_KHR_portability_enumeration` if available:

```cpp
#if defined(__APPLE__) && defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    createInfo.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
#endif
```

This is required for MoltenVK to enumerate properly.

## Physical Device Selection

### Selection Criteria

Devices are rejected if they fail any of these checks:

| Criterion | Requirement |
|-----------|-------------|
| Device type | Must be discrete, integrated, or virtual GPU (not CPU or "other") |
| Graphics queue | At least one queue with `VK_QUEUE_GRAPHICS_BIT` |
| Present queue | At least one queue supporting surface presentation |
| Required extensions | All extensions in `RequiredDeviceExtensions` |
| Swapchain support | At least one surface format and present mode |
| API version | `apiVersion >= VK_API_VERSION_1_4` |
| Push descriptors | `features14.pushDescriptor == VK_TRUE` |
| Descriptor indexing | See feature requirements below |
| Dynamic rendering | `features13.dynamicRendering == VK_TRUE` |

### Required Device Extensions

```cpp
const SCP_vector<const char*> RequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,       // Presentation
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, // Per-draw descriptor updates
    VK_KHR_MAINTENANCE_5_EXTENSION_NAME,   // Various fixes/improvements
};
```

### Optional Device Extensions

```cpp
const SCP_vector<const char*> OptionalDeviceExtensions = {
    VK_KHR_MAINTENANCE_6_EXTENSION_NAME,           // Additional maintenance
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,// Dynamic blend/polygon mode
    VK_EXT_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, // Attachment reads
    VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,// Instance rate divisor fallback
};
```

Optional extensions are enabled if present but do not affect device suitability.

### Device Scoring

When multiple suitable devices exist, FSO scores them to pick the best:

```cpp
uint32_t scoreDevice(const PhysicalDeviceValues& device) {
    uint32_t score = 0;
    // Device type is dominant (discrete > integrated > virtual)
    score += deviceTypeScore(device.properties.deviceType) * 1000000;
    // Vulkan version as tiebreaker
    uint32_t major = VK_VERSION_MAJOR(device.properties.apiVersion);
    uint32_t minor = VK_VERSION_MINOR(device.properties.apiVersion);
    score += major * 100 + minor;
    return score;
}
```

Device type scores:
- Discrete GPU: 3,000,000
- Integrated GPU: 2,000,000
- Virtual GPU: 1,000,000
- CPU/Other: 0 (rejected anyway)

The highest-scoring device is selected.

## Queue Family Selection

FSO requires two queue capabilities:

1. **Graphics queue** - Must support `VK_QUEUE_GRAPHICS_BIT`
2. **Present queue** - Must support surface presentation (checked via `vkGetPhysicalDeviceSurfaceSupportKHR`)

Queue selection iterates through queue families and picks the first matching family for each capability:

```cpp
for (const auto& queue : values.queueProperties) {
    if (!values.graphicsQueueIndex.initialized &&
        queue.queueFlags & vk::QueueFlagBits::eGraphics) {
        values.graphicsQueueIndex.index = i;
    }
    if (!values.presentQueueIndex.initialized &&
        values.device.getSurfaceSupportKHR(i, surface.get())) {
        values.presentQueueIndex.index = i;
    }
}
```

If graphics and present are different families, swapchain images use `VK_SHARING_MODE_CONCURRENT`. If same family (common case), images use `VK_SHARING_MODE_EXCLUSIVE` for better performance.

## Feature Chain (pNext Chaining)

FSO queries device features using a chained `VkPhysicalDeviceFeatures2` structure. This is the only way to access Vulkan 1.1+ features.

### Query Chain Structure

```
VkPhysicalDeviceFeatures2
    pNext -> VkPhysicalDeviceVulkan11Features
        pNext -> VkPhysicalDeviceVulkan12Features
            pNext -> VkPhysicalDeviceVulkan13Features
                pNext -> VkPhysicalDeviceVulkan14Features
                    pNext -> VkPhysicalDeviceExtendedDynamicStateFeaturesEXT
                        pNext -> VkPhysicalDeviceExtendedDynamicState2FeaturesEXT
                            pNext -> VkPhysicalDeviceExtendedDynamicState3FeaturesEXT
                                pNext -> VkPhysicalDevicePushDescriptorPropertiesKHR
```

The query populates all structures in one call:

```cpp
dev.getFeatures2(&feats);
```

### Enable Chain for Device Creation

When creating the logical device, a similar chain is built with **fresh structures** to avoid stale `pNext` pointers:

```cpp
vk::PhysicalDeviceFeatures2 enabledFeatures;
vk::PhysicalDeviceVulkan11Features enabled11 = deviceValues.features11;
vk::PhysicalDeviceVulkan12Features enabled12 = deviceValues.features12;
// ... copy values, then rebuild chain
enabled11.pNext = &enabled12;
enabled12.pNext = &enabled13;
enabled13.pNext = &enabled14;
// ...
enabledFeatures.pNext = &enabled11;
```

**Critical**: Always reset `pNext` pointers after copying feature structures. Copied structures retain their original `pNext` values, which can cause crashes or validation errors.

## Required Features

### Vulkan 1.1 Features

| Feature | Field | Why Required |
|---------|-------|--------------|
| Sampler YCbCr conversion | `samplerYcbcrConversion` | Video texture format support |

### Vulkan 1.2 Features (Descriptor Indexing)

These features enable bindless texture rendering for models:

| Feature | Field | Why Required |
|---------|-------|--------------|
| Non-uniform indexing | `shaderSampledImageArrayNonUniformIndexing` | Index texture arrays dynamically in shaders |
| Runtime arrays | `runtimeDescriptorArray` | Variable-length descriptor arrays |

Validation is performed by `ValidateModelDescriptorIndexingSupport()`:

```cpp
bool ValidateModelDescriptorIndexingSupport(const vk::PhysicalDeviceVulkan12Features& features12) {
    return features12.shaderSampledImageArrayNonUniformIndexing &&
           features12.runtimeDescriptorArray;
}
```

### Vulkan 1.3 Features

| Feature | Field | Why Required |
|---------|-------|--------------|
| Dynamic rendering | `dynamicRendering` | Renderpass-less pipeline creation |

Dynamic rendering is **mandatory**. FSO does not use traditional render passes.

### Vulkan 1.4 Features

| Feature | Field | Why Required |
|---------|-------|--------------|
| Push descriptors | `pushDescriptor` | Per-draw descriptor updates without allocation |
| Vertex attribute divisor | `vertexAttributeInstanceRateDivisor` | Instance-rate vertex attributes (optional, has fallback) |

Push descriptors are validated by:

```cpp
bool ValidatePushDescriptorSupport(const vk::PhysicalDeviceVulkan14Features& features14) {
    return features14.pushDescriptor == VK_TRUE;
}
```

## Optional Features

### Extended Dynamic State (EDS)

Extended dynamic state allows setting pipeline state at draw time rather than pipeline creation time, reducing pipeline permutations.

**EDS1** (`VK_EXT_extended_dynamic_state`):
- Promoted to Vulkan 1.3 core
- Always available on Vulkan 1.4+ devices
- Tracked via `m_supportsExtDyn` (currently always true)

**EDS2** (`VK_EXT_extended_dynamic_state_2`):
- Tracked via `m_supportsExtDyn2`
- Not currently used by FSO

**EDS3** (`VK_EXT_extended_dynamic_state_3`):
- Extension-only (not promoted to core)
- Per-feature capability tracking required

### ExtendedDynamicState3Caps Structure

Since EDS3 features are individually optional, FSO tracks them separately:

```cpp
struct ExtendedDynamicState3Caps {
    bool colorBlendEnable = false;       // vkCmdSetColorBlendEnableEXT
    bool colorWriteMask = false;         // vkCmdSetColorWriteMaskEXT
    bool polygonMode = false;            // vkCmdSetPolygonModeEXT
    bool rasterizationSamples = false;   // vkCmdSetRasterizationSamplesEXT
};
```

These are queried from `VkPhysicalDeviceExtendedDynamicState3FeaturesEXT`:

```cpp
m_extDyn3Caps.colorBlendEnable = extDyn3Feats.extendedDynamicState3ColorBlendEnable;
m_extDyn3Caps.colorWriteMask = extDyn3Feats.extendedDynamicState3ColorWriteMask;
m_extDyn3Caps.polygonMode = extDyn3Feats.extendedDynamicState3PolygonMode;
m_extDyn3Caps.rasterizationSamples = extDyn3Feats.extendedDynamicState3RasterizationSamples;
```

Pipeline code should check these flags before using dynamic state:

```cpp
if (device.extDyn3Caps().polygonMode) {
    // Use vkCmdSetPolygonModeEXT
} else {
    // Bake into pipeline
}
```

### Vertex Attribute Divisor

FSO prefers the Vulkan 1.4 core feature but falls back to the extension:

```cpp
m_supportsVertexAttributeDivisor = m_features14.vertexAttributeInstanceRateDivisor;

if (!m_supportsVertexAttributeDivisor &&
    /* extension present */) {
    // Query extension feature
    m_supportsVertexAttributeDivisor = divisorFeats.vertexAttributeInstanceRateDivisor;
}
```

## Device Limits

### minUniformBufferOffsetAlignment

This limit determines the alignment requirement for dynamic uniform buffer offsets:

```cpp
size_t VulkanDevice::minUniformBufferOffsetAlignment() const {
    return m_properties.limits.minUniformBufferOffsetAlignment;
}
```

**Usage**: When using dynamic uniform buffers, all offsets must be multiples of this value. Typical values range from 16 to 256 bytes depending on the GPU.

See `VULKAN_UNIFORM_ALIGNMENT.md` for detailed alignment requirements.

### Vertex Buffer Alignment

FSO uses a default vertex buffer alignment of `sizeof(float)` (4 bytes):

```cpp
m_vertexBufferAlignment = static_cast<uint32_t>(sizeof(float));
```

This is conservative and works with all standard vertex formats.

### Push Descriptor Limits

The `VkPhysicalDevicePushDescriptorPropertiesKHR` structure is queried but currently only used for capability verification. The `maxPushDescriptors` limit determines how many descriptors can be pushed per command.

## Logical Device Creation

### Queue Creation

Unique queue family indices are collected, and one queue is requested from each:

```cpp
const std::set<uint32_t> familyIndices{
    deviceValues.graphicsQueueIndex.index,
    deviceValues.presentQueueIndex.index
};

for (auto index : familyIndices) {
    queueInfos.emplace_back(vk::DeviceQueueCreateFlags(), index, 1, &queuePriority);
}
```

Queue priority is set to 1.0 (maximum) for both queues.

### Extension Enablement

Required extensions are always enabled. Optional extensions are enabled if present:

```cpp
std::vector<const char*> enabledExtensions(RequiredDeviceExtensions.begin(),
                                            RequiredDeviceExtensions.end());
for (const auto& opt : OptionalDeviceExtensions) {
    if (/* extension present in device */) {
        enabledExtensions.push_back(opt);
    }
}
```

### Device Creation Call

```cpp
vk::DeviceCreateInfo deviceCreate;
deviceCreate.pQueueCreateInfos = queueInfos.data();
deviceCreate.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
deviceCreate.pEnabledFeatures = nullptr;  // Using pNext chain instead
deviceCreate.pNext = &enabledFeatures;    // Feature chain head
deviceCreate.ppEnabledExtensionNames = enabledExtensions.data();
deviceCreate.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());

m_device = deviceValues.device.createDeviceUnique(deviceCreate);
```

**Note**: `pEnabledFeatures` is null because features are passed via the `pNext` chain. Using both would be invalid.

## Post-Creation Setup

After device creation:

1. **Dispatcher initialization**: Device-level function pointers are loaded
   ```cpp
   VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device.get());
   ```

2. **Queue retrieval**: Graphics and present queues are obtained
   ```cpp
   m_graphicsQueue = m_device->getQueue(graphicsQueueIndex, 0);
   m_presentQueue = m_device->getQueue(presentQueueIndex, 0);
   ```

3. **Property storage**: Device properties and memory properties are cached
   ```cpp
   m_properties = deviceValues.properties;
   m_memoryProperties = m_physicalDevice.getMemoryProperties();
   ```

4. **Feature sanitization**: Enabled feature structures are stored with `pNext` cleared to prevent dangling pointers

5. **Capability query**: `queryDeviceCapabilities()` checks EDS3 and vertex attribute divisor support

6. **Pipeline cache**: Loaded from disk or created empty (see next section)

## Pipeline Cache

FSO persists compiled pipelines to disk to improve load times on subsequent runs.

### Cache File Format

The cache file (`vulkan_pipeline.cache`) uses a custom header followed by Vulkan cache data:

```cpp
struct CacheHeader {
    uint32_t headerLength;               // sizeof(CacheHeader)
    uint32_t headerVersion;              // VK_PIPELINE_CACHE_HEADER_VERSION_ONE
    uint32_t vendorID;                   // From device properties
    uint32_t deviceID;                   // From device properties
    uint8_t pipelineCacheUUID[VK_UUID_SIZE]; // From device properties
};
```

### Cache Validation

On load, the header is validated against the current device. If vendorID, deviceID, or UUID mismatch, the cache is discarded:

```cpp
if (header->vendorID == m_properties.vendorID &&
    header->deviceID == m_properties.deviceID &&
    memcmp(header->pipelineCacheUUID, m_properties.pipelineCacheUUID, VK_UUID_SIZE) == 0) {
    // Use cached data
}
```

This prevents using stale cache data after driver updates or when switching GPUs.

## Shutdown

Device shutdown waits for idle, saves the pipeline cache, and lets RAII destroy handles:

```cpp
void VulkanDevice::shutdown() {
    if (!m_device) return;

    m_device->waitIdle();
    savePipelineCache();
    m_swapchainImages.clear();
    // RAII handles destroyed in reverse declaration order
}
```

## Troubleshooting

### "No suitable Vulkan device found"

Check:
1. GPU supports Vulkan 1.4
2. Graphics drivers are up to date
3. Required extensions are available
4. Device has graphics + present queue support

### Validation layer spam

The debug callback rate-limits repeated messages. If you see "(repeated; suppressing further duplicates)", the underlying issue should be fixed rather than ignored.

### Feature not available

If a required feature check fails, ensure your GPU and drivers support:
- Dynamic rendering (most GPUs since ~2019)
- Descriptor indexing (most GPUs since ~2018)
- Push descriptors (requires Vulkan 1.4 or extension)

### Cache invalidation

If shader compilation is slow despite having a cache file, the cache may be invalid due to:
- Driver update (UUID changed)
- Different GPU (vendorID/deviceID mismatch)
- Corrupted file

Delete `vulkan_pipeline.cache` to force regeneration.

## Files Reference

| File | Purpose |
|------|---------|
| `VulkanDevice.h` | Device class declaration, `PhysicalDeviceValues` struct |
| `VulkanDevice.cpp` | All initialization logic |
| `VulkanDebug.h` | `ExtendedDynamicState3Caps`, logging helpers |
| `VulkanModelValidation.h` | Feature validation functions |
