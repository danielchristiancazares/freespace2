#pragma once

#include "VulkanDebug.h"

#include "graphics/2d.h"

#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <variant>
#include <vector>

namespace graphics::vulkan {

class VulkanRenderer;
class VulkanFrame;

// CPU-side packed light parameters. The shader is responsible for interpreting
// type-specific fields (type is for shader math, not CPU dispatch).
struct DeferredLightUBO {
    // Transform for volume geometry. For fullscreen lights, this can be identity.
    matrix4 mvp;

    // xyz = position (view space), w = radius
    vec4 position_radius;
    // xyz = direction (view space), w = type (ambient/directional/point/cone/tube)
    vec4 direction_type;
    // rgb = color, a = intensity
    vec4 color_intensity;
    // x = coneInnerCos, y = coneOuterCos, z = tubeLength, w = sourceRadius
    vec4 params;
};

struct DeferredLightingSharedState;

struct FullscreenLight {
    DeferredLightUBO ubo;
    void record(vk::CommandBuffer cmd, VulkanFrame& frame, const DeferredLightingSharedState& shared);
};

struct SphereLight {
    DeferredLightUBO ubo;
    void record(vk::CommandBuffer cmd, VulkanFrame& frame, const DeferredLightingSharedState& shared);
};

struct CylinderLight {
    DeferredLightUBO ubo;
    void record(vk::CommandBuffer cmd, VulkanFrame& frame, const DeferredLightingSharedState& shared);
};

using DeferredLight = std::variant<FullscreenLight, SphereLight, CylinderLight>;

// Owns everything needed to render the deferred lighting pass.
class VulkanDeferredLightingPass {
  public:
    struct Mesh {
        gr_buffer_handle vertex = gr_buffer_handle::invalid();
        gr_buffer_handle index = gr_buffer_handle::invalid();
        uint32_t indexCount = 0;
        vertex_layout layout;
    };

    explicit VulkanDeferredLightingPass(VulkanRenderer& renderer);
    ~VulkanDeferredLightingPass();

    VulkanDeferredLightingPass(const VulkanDeferredLightingPass&) = delete;
    VulkanDeferredLightingPass& operator=(const VulkanDeferredLightingPass&) = delete;
    VulkanDeferredLightingPass(VulkanDeferredLightingPass&&) = delete;
    VulkanDeferredLightingPass& operator=(VulkanDeferredLightingPass&&) = delete;

    // Records the lighting pass. The caller provides a set of already-categorized lights.
    void record(vk::CommandBuffer cmd, VulkanFrame& frame, std::vector<DeferredLight>& lights);

  private:
    VulkanRenderer& m_renderer;

    // Descriptor set layout/pool/set for per-light dynamic offsets.
    vk::UniqueDescriptorSetLayout m_lightSetLayout;
    vk::UniqueDescriptorPool m_lightPool;
    vk::DescriptorSet m_lightSet = nullptr;
    vk::UniquePipelineLayout m_pipelineLayout;

    Mesh m_sphere;
    Mesh m_cylinder;

    void createLightDescriptors();
    void createMeshes();
    Mesh createUnitSphereMesh(uint32_t slices, uint32_t stacks);
    Mesh createUnitCylinderMesh(uint32_t slices);

    friend struct FullscreenLight;
    friend struct SphereLight;
    friend struct CylinderLight;
    friend struct DeferredLightingSharedState;
};

struct DeferredLightingSharedState {
    VulkanDeferredLightingPass& pass;
    vk::Pipeline fullscreenPipeline = nullptr;
    vk::Pipeline volumePipeline = nullptr;
};

} // namespace graphics::vulkan
