# Gemini Changes Summary

## Files Modified

### 1. VulkanDescriptorLayouts.h
- Changed `allocateGlobalSet()` to `allocateGlobalSets(uint32_t count)`
- Removed `vk::DescriptorPool modelDescriptorPool() const`
- Removed `vk::DescriptorSet allocateModelDescriptorSet()`
- Removed `vk::UniqueDescriptorPool m_modelDescriptorPool`

### 2. VulkanDescriptorLayouts.cpp
- Pool size changed: `descriptorCount = 4` to `descriptorCount = 4 * kFramesInFlight`
- Pool maxSets changed: `maxSets = 1` to `maxSets = kFramesInFlight`
- `allocateGlobalSet()` renamed to `allocateGlobalSets(uint32_t count)` and returns vector
- Removed model descriptor pool creation (lines ~157-173)
- Removed `allocateModelDescriptorSet()` function

### 3. VulkanRenderer.h
- Changed `vk::DescriptorSet m_globalDescriptorSet{}` to `std::vector<vk::DescriptorSet> m_globalDescriptorSets`
- Changed `allocateModelDescriptorSet()` to `allocateModelDescriptorSet(VulkanFrame& frame)`
- Implementation now calls `frame.allocateDescriptorSet(m_descriptorLayouts->modelSetLayout())`

### 4. VulkanRenderer.cpp
- Line ~946: `allocateGlobalSet()` to `allocateGlobalSets(kFramesInFlight)`
- Line ~1390: `&m_globalDescriptorSet` to `&m_globalDescriptorSets[m_currentFrame]`
- Line ~1609: `m_globalDescriptorSet` to `m_globalDescriptorSets[m_currentFrame]`
- Line ~1625: `m_globalDescriptorSet` to `m_globalDescriptorSets[m_currentFrame]`
- Line ~1865: `m_descriptorLayouts->allocateModelDescriptorSet()` to `allocateModelDescriptorSet(frame)`

### 5. VulkanFrame.h
- Added `vk::DescriptorSet allocateDescriptorSet(vk::DescriptorSetLayout layout)`
- Added `vk::UniqueDescriptorPool m_descriptorPool`

### 6. VulkanFrame.cpp
- Added `#include "VulkanConstants.h"`
- Added descriptor pool creation after command buffer allocation (~20 lines)
- Added `m_device.resetDescriptorPool(m_descriptorPool.get())` in `reset()`
- Added `allocateDescriptorSet()` implementation

### 7. VulkanGraphics.cpp
- Line ~605: `allocateModelDescriptorSet()` to `allocateModelDescriptorSet(frame)`

## To Revert

The core architectural change was moving the model descriptor pool from VulkanDescriptorLayouts (shared) to VulkanFrame (per-frame). This is actually a reasonable change for avoiding descriptor set aliasing across frames, but may have broken things depending on how the existing code was structured.

Key reversals needed:
1. Restore single `m_globalDescriptorSet` in VulkanRenderer
2. Restore `allocateGlobalSet()` (singular) in VulkanDescriptorLayouts
3. Restore model descriptor pool in VulkanDescriptorLayouts
4. Remove descriptor pool from VulkanFrame
5. Restore original `allocateModelDescriptorSet()` signature
