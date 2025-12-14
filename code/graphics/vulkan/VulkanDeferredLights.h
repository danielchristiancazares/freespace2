#pragma once

#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "globalincs/pstypes.h"
#include "math/vecmat.h"

namespace graphics::vulkan {

class VulkanFrame;
class VulkanRenderer;

// Must match deferred.vert layout(set=0, binding=0)
struct alignas(16) DeferredMatrixUBO {
    matrix4 modelViewMatrix;
    matrix4 projMatrix;
};

// Must match deferred.vert/frag layout(set=0, binding=1)
// Using std140 layout rules: vec3 takes 16 bytes, following scalar packs into padding
struct alignas(16) DeferredLightUBO {
    float diffuseLightColor[3];
    float coneAngle;

    float lightDir[3];
    float coneInnerAngle;

    float coneDir[3];
    uint32_t dualCone;

    float scale[3];
    float lightRadius;

    int32_t lightType;
    uint32_t enable_shadows;
    float sourceRadius;
    float _pad;
};

struct DeferredDrawContext {
    vk::CommandBuffer cmd;
    vk::PipelineLayout layout;
    vk::Buffer uniformBuffer;
    vk::Pipeline pipeline;
    vk::Pipeline ambientPipeline;  // blend disabled for first pass
};

struct FullscreenLight {
    DeferredMatrixUBO matrices;
    DeferredLightUBO light;
    uint32_t matrixOffset;
    uint32_t lightOffset;
    bool isAmbient;

    void record(const DeferredDrawContext& ctx, vk::Buffer fullscreenVB) const;
};

struct SphereLight {
    DeferredMatrixUBO matrices;
    DeferredLightUBO light;
    uint32_t matrixOffset;
    uint32_t lightOffset;

    void record(const DeferredDrawContext& ctx,
                vk::Buffer sphereVB, vk::Buffer sphereIB, uint32_t indexCount) const;
};

struct CylinderLight {
    DeferredMatrixUBO matrices;
    DeferredLightUBO light;
    uint32_t matrixOffset;
    uint32_t lightOffset;

    void record(const DeferredDrawContext& ctx,
                vk::Buffer cylinderVB, vk::Buffer cylinderIB, uint32_t indexCount) const;
};

using DeferredLight = std::variant<FullscreenLight, SphereLight, CylinderLight>;

// Boundary: engine lights -> variants. Conditionals live here only.
std::vector<DeferredLight> buildDeferredLights(
    VulkanFrame& frame,
    vk::Buffer uniformBuffer,
    const matrix4& viewMatrix,
    const matrix4& projMatrix,
    uint32_t uniformAlignment);

} // namespace graphics::vulkan
