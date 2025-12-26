#include "VulkanLayoutContracts.h"

#include "globalincs/pstypes.h"

#include <array>

namespace graphics {
namespace vulkan {

namespace {

constexpr ShaderLayoutSpec makeSpec(shader_type type, const char *name, PipelineLayoutKind layout,
                                    VertexInputMode vertexInput) {
  return ShaderLayoutSpec{type, name, layout, vertexInput};
}

// Explicit, ordered mapping from shader_type to pipeline layout + vertex input mode.
// This makes it trivial to see which shaders use the model bindless layout vs. the
// standard per-draw push descriptor layout.
constexpr std::array<ShaderLayoutSpec, NUM_SHADER_TYPES> buildSpecs() {
  using PL = PipelineLayoutKind;
  using VI = VertexInputMode;

  return {
      makeSpec(SDR_TYPE_MODEL, "SDR_TYPE_MODEL", PL::Model, VI::VertexPulling),
      makeSpec(SDR_TYPE_EFFECT_PARTICLE, "SDR_TYPE_EFFECT_PARTICLE", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_EFFECT_DISTORTION, "SDR_TYPE_EFFECT_DISTORTION", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_MAIN, "SDR_TYPE_POST_PROCESS_MAIN", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_BLUR, "SDR_TYPE_POST_PROCESS_BLUR", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_BLOOM_COMP, "SDR_TYPE_POST_PROCESS_BLOOM_COMP", PL::Standard,
               VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_BRIGHTPASS, "SDR_TYPE_POST_PROCESS_BRIGHTPASS", PL::Standard,
               VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_FXAA, "SDR_TYPE_POST_PROCESS_FXAA", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_FXAA_PREPASS, "SDR_TYPE_POST_PROCESS_FXAA_PREPASS", PL::Standard,
               VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_LIGHTSHAFTS, "SDR_TYPE_POST_PROCESS_LIGHTSHAFTS", PL::Standard,
               VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_TONEMAPPING, "SDR_TYPE_POST_PROCESS_TONEMAPPING", PL::Standard,
               VI::VertexAttributes),
      makeSpec(SDR_TYPE_DEFERRED_LIGHTING, "SDR_TYPE_DEFERRED_LIGHTING", PL::Deferred, VI::VertexAttributes),
      makeSpec(SDR_TYPE_DEFERRED_CLEAR, "SDR_TYPE_DEFERRED_CLEAR", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_VIDEO_PROCESS, "SDR_TYPE_VIDEO_PROCESS", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_PASSTHROUGH_RENDER, "SDR_TYPE_PASSTHROUGH_RENDER", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_SHIELD_DECAL, "SDR_TYPE_SHIELD_DECAL", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_BATCHED_BITMAP, "SDR_TYPE_BATCHED_BITMAP", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_DEFAULT_MATERIAL, "SDR_TYPE_DEFAULT_MATERIAL", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_INTERFACE, "SDR_TYPE_INTERFACE", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_NANOVG, "SDR_TYPE_NANOVG", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_DECAL, "SDR_TYPE_DECAL", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_SCENE_FOG, "SDR_TYPE_SCENE_FOG", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_VOLUMETRIC_FOG, "SDR_TYPE_VOLUMETRIC_FOG", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_ROCKET_UI, "SDR_TYPE_ROCKET_UI", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_COPY, "SDR_TYPE_COPY", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_COPY_WORLD, "SDR_TYPE_COPY_WORLD", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_MSAA_RESOLVE, "SDR_TYPE_MSAA_RESOLVE", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_SMAA_EDGE, "SDR_TYPE_POST_PROCESS_SMAA_EDGE", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT, "SDR_TYPE_POST_PROCESS_SMAA_BLENDING_WEIGHT", PL::Standard,
               VI::VertexAttributes),
      makeSpec(SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING, "SDR_TYPE_POST_PROCESS_SMAA_NEIGHBORHOOD_BLENDING",
               PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_ENVMAP_SPHERE_WARP, "SDR_TYPE_ENVMAP_SPHERE_WARP", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_IRRADIANCE_MAP_GEN, "SDR_TYPE_IRRADIANCE_MAP_GEN", PL::Standard, VI::VertexAttributes),
      makeSpec(SDR_TYPE_FLAT_COLOR, "SDR_TYPE_FLAT_COLOR", PL::Standard, VI::VertexAttributes),
  };
}

constexpr std::array<ShaderLayoutSpec, NUM_SHADER_TYPES> kShaderLayoutSpecs = buildSpecs();

} // namespace

const ShaderLayoutSpec &getShaderLayoutSpec(shader_type type) {
  Assertion(type >= 0 && type < NUM_SHADER_TYPES, "Invalid shader_type %d", type);
  return kShaderLayoutSpecs[static_cast<size_t>(type)];
}

const SCP_vector<ShaderLayoutSpec> &getShaderLayoutSpecs() {
  static const SCP_vector<ShaderLayoutSpec> specs(kShaderLayoutSpecs.begin(), kShaderLayoutSpecs.end());
  return specs;
}

bool usesVertexPulling(shader_type type) {
  return getShaderLayoutSpec(type).vertexInput == VertexInputMode::VertexPulling;
}

PipelineLayoutKind pipelineLayoutForShader(shader_type type) { return getShaderLayoutSpec(type).pipelineLayout; }

} // namespace vulkan
} // namespace graphics
