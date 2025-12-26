#include "VulkanDeferredLights.h"
#include "VulkanFrame.h"
#include "graphics/light.h"
#include "graphics/matrix.h"
#include "lighting/lighting.h"
#include "lighting/lighting_profiles.h"
#include "model/modelrender.h"
#include "osapi/outwnd.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>

extern matrix4 gr_view_matrix;
extern matrix4 gr_projection_matrix;

namespace graphics::vulkan {

namespace {

// Synthetic type for ambient light in shader (matches lighting.sdr LT_AMBIENT)
constexpr int LT_AMBIENT_SHADER = 4;

uint32_t uploadUBO(VulkanFrame &frame, const void *data, size_t size, uint32_t alignment) {
  auto alloc = frame.uniformBuffer().allocate(size, alignment);
  memcpy(alloc.mapped, data, size);
  return static_cast<uint32_t>(alloc.offset);
}

void pushLightDescriptors(vk::CommandBuffer cmd, vk::PipelineLayout layout, vk::Buffer buffer, uint32_t matrixOffset,
                          uint32_t lightOffset) {
  vk::DescriptorBufferInfo matrixInfo{};
  matrixInfo.buffer = buffer;
  matrixInfo.offset = matrixOffset;
  matrixInfo.range = sizeof(DeferredMatrixUBO);

  vk::DescriptorBufferInfo lightInfo{};
  lightInfo.buffer = buffer;
  lightInfo.offset = lightOffset;
  lightInfo.range = sizeof(DeferredLightUBO);

  std::array<vk::WriteDescriptorSet, 2> writes{};

  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].pBufferInfo = &matrixInfo;

  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[1].pBufferInfo = &lightInfo;

  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, layout,
                           0, // set 0
                           writes);
}

} // namespace

void FullscreenLight::record(const DeferredDrawContext &ctx, vk::Buffer fullscreenVB) const {
  // Ambient uses blend-off pipeline to overwrite undefined swapchain
  vk::Pipeline pipe = isAmbient ? ctx.ambientPipeline : ctx.pipeline;
  ctx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipe);

  if (ctx.dynamicBlendEnable) {
    vk::Bool32 blendEnable = isAmbient ? VK_FALSE : VK_TRUE;
    ctx.cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
  }

  pushLightDescriptors(ctx.cmd, ctx.layout, ctx.uniformBuffer, matrixOffset, lightOffset);

  vk::DeviceSize offset = 0;
  ctx.cmd.bindVertexBuffers(0, 1, &fullscreenVB, &offset);

  ctx.cmd.draw(3, 1, 0, 0);
}

void SphereLight::record(const DeferredDrawContext &ctx, vk::Buffer sphereVB, vk::Buffer sphereIB,
                         uint32_t indexCount) const {
  ctx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pipeline);

  if (ctx.dynamicBlendEnable) {
    vk::Bool32 blendEnable = VK_TRUE;
    ctx.cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
  }

  pushLightDescriptors(ctx.cmd, ctx.layout, ctx.uniformBuffer, matrixOffset, lightOffset);

  vk::DeviceSize offset = 0;
  ctx.cmd.bindVertexBuffers(0, 1, &sphereVB, &offset);
  ctx.cmd.bindIndexBuffer(sphereIB, 0, vk::IndexType::eUint32);

  ctx.cmd.drawIndexed(indexCount, 1, 0, 0, 0);
}

void CylinderLight::record(const DeferredDrawContext &ctx, vk::Buffer cylinderVB, vk::Buffer cylinderIB,
                           uint32_t indexCount) const {
  ctx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pipeline);

  if (ctx.dynamicBlendEnable) {
    vk::Bool32 blendEnable = VK_TRUE;
    ctx.cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
  }

  pushLightDescriptors(ctx.cmd, ctx.layout, ctx.uniformBuffer, matrixOffset, lightOffset);

  vk::DeviceSize offset = 0;
  ctx.cmd.bindVertexBuffers(0, 1, &cylinderVB, &offset);
  ctx.cmd.bindIndexBuffer(cylinderIB, 0, vk::IndexType::eUint32);

  ctx.cmd.drawIndexed(indexCount, 1, 0, 0, 0);
}

// ============================================================
// BOUNDARY CODE - conditionals on engine type acceptable here
// ============================================================

std::vector<DeferredLight> buildDeferredLights(VulkanFrame &frame, vk::Buffer uniformBuffer, const matrix4 &viewMatrix,
                                               const matrix4 &projMatrix, uint32_t uniformAlignment) {
  std::vector<DeferredLight> result;
  result.reserve(Lights.size() + 1);

  namespace ltp = lighting_profiles;
  auto lp = ltp::current();

  // Synthetic ambient light - MUST BE FIRST (uses blend-off to initialize swapchain)
  {
    FullscreenLight ambient{};
    ambient.isAmbient = true;

    vm_matrix4_set_identity(&ambient.matrices.modelViewMatrix);
    vm_matrix4_set_identity(&ambient.matrices.projMatrix);

    ambient.light.lightType = LT_AMBIENT_SHADER;

    vec3d ambientColor{};
    gr_get_ambient_light(&ambientColor);
    ambient.light.diffuseLightColor[0] = ambientColor.xyz.x;
    ambient.light.diffuseLightColor[1] = ambientColor.xyz.y;
    ambient.light.diffuseLightColor[2] = ambientColor.xyz.z;

    ambient.light.scale[0] = 1.f;
    ambient.light.scale[1] = 1.f;
    ambient.light.scale[2] = 1.f;
    ambient.light.enable_shadows = 0;
    ambient.light.sourceRadius = 0.f;

    ambient.matrixOffset = uploadUBO(frame, &ambient.matrices, sizeof(ambient.matrices), uniformAlignment);
    ambient.lightOffset = uploadUBO(frame, &ambient.light, sizeof(ambient.light), uniformAlignment);
    result.push_back(ambient);
  }

  for (const auto &src : Lights) {
    float intensity = (Lighting_mode == lighting_mode::COCKPIT)
                          ? lp->cockpit_light_intensity_modifier.handle(src.intensity)
                          : src.intensity;

    DeferredLightUBO lightData{};
    lightData.diffuseLightColor[0] = src.r * intensity;
    lightData.diffuseLightColor[1] = src.g * intensity;
    lightData.diffuseLightColor[2] = src.b * intensity;
    lightData.sourceRadius = src.source_radius;
    lightData.enable_shadows = 0;

    if (src.type == Light_Type::Directional) {
      FullscreenLight l{};
      l.isAmbient = false;

      vm_matrix4_set_identity(&l.matrices.modelViewMatrix);
      vm_matrix4_set_identity(&l.matrices.projMatrix);

      lightData.lightType = LT_DIRECTIONAL;

      // Transform direction to view space
      vec4 lightDir4{};
      lightDir4.xyzw.x = -src.vec.xyz.x;
      lightDir4.xyzw.y = -src.vec.xyz.y;
      lightDir4.xyzw.z = -src.vec.xyz.z;
      lightDir4.xyzw.w = 0.0f;

      vec4 viewDir{};
      vm_vec_transform(&viewDir, &lightDir4, &viewMatrix);

      lightData.lightDir[0] = viewDir.xyzw.x;
      lightData.lightDir[1] = viewDir.xyzw.y;
      lightData.lightDir[2] = viewDir.xyzw.z;

      lightData.scale[0] = 1.f;
      lightData.scale[1] = 1.f;
      lightData.scale[2] = 1.f;

      l.light = lightData;
      l.matrixOffset = uploadUBO(frame, &l.matrices, sizeof(l.matrices), uniformAlignment);
      l.lightOffset = uploadUBO(frame, &l.light, sizeof(l.light), uniformAlignment);
      result.push_back(l);
    } else if (src.type == Light_Type::Point) {
      SphereLight l{};

      float radius = (Lighting_mode == lighting_mode::COCKPIT)
                         ? lp->cockpit_light_radius_modifier.handle(MAX(src.rada, src.radb))
                         : MAX(src.rada, src.radb);

      // Build model-view matrix: translation to light position
      matrix4 model{};
      vm_matrix4_set_identity(&model);
      // Set translation manually (no vm_matrix4_set_translation exists)
      model.a1d[12] = src.vec.xyz.x;
      model.a1d[13] = src.vec.xyz.y;
      model.a1d[14] = src.vec.xyz.z;

      vm_matrix4_x_matrix4(&l.matrices.modelViewMatrix, &viewMatrix, &model);
      l.matrices.projMatrix = projMatrix;

      lightData.lightType = LT_POINT;
      lightData.lightRadius = radius;

      float meshScale = radius * 1.05f;
      lightData.scale[0] = meshScale;
      lightData.scale[1] = meshScale;
      lightData.scale[2] = meshScale;

      l.light = lightData;
      l.matrixOffset = uploadUBO(frame, &l.matrices, sizeof(l.matrices), uniformAlignment);
      l.lightOffset = uploadUBO(frame, &l.light, sizeof(l.light), uniformAlignment);
      result.push_back(l);
    } else if (src.type == Light_Type::Cone) {
      SphereLight l{};

      float radius = (Lighting_mode == lighting_mode::COCKPIT)
                         ? lp->cockpit_light_radius_modifier.handle(MAX(src.rada, src.radb))
                         : MAX(src.rada, src.radb);

      matrix4 model{};
      vm_matrix4_set_identity(&model);
      model.a1d[12] = src.vec.xyz.x;
      model.a1d[13] = src.vec.xyz.y;
      model.a1d[14] = src.vec.xyz.z;

      vm_matrix4_x_matrix4(&l.matrices.modelViewMatrix, &viewMatrix, &model);
      l.matrices.projMatrix = projMatrix;

      lightData.lightType = LT_CONE;
      lightData.lightRadius = radius;
      lightData.coneAngle = src.cone_angle;
      lightData.coneInnerAngle = src.cone_inner_angle;
      lightData.dualCone = (src.flags & LF_DUAL_CONE) ? 1 : 0;

      // Transform cone direction to view space
      vec4 coneDir4{};
      coneDir4.xyzw.x = src.vec2.xyz.x;
      coneDir4.xyzw.y = src.vec2.xyz.y;
      coneDir4.xyzw.z = src.vec2.xyz.z;
      coneDir4.xyzw.w = 0.0f;

      vec4 viewConeDir{};
      vm_vec_transform(&viewConeDir, &coneDir4, &viewMatrix);

      // Normalize
      float len = sqrtf(viewConeDir.xyzw.x * viewConeDir.xyzw.x + viewConeDir.xyzw.y * viewConeDir.xyzw.y +
                        viewConeDir.xyzw.z * viewConeDir.xyzw.z);
      if (len > 0.0001f) {
        lightData.coneDir[0] = viewConeDir.xyzw.x / len;
        lightData.coneDir[1] = viewConeDir.xyzw.y / len;
        lightData.coneDir[2] = viewConeDir.xyzw.z / len;
      }

      float meshScale = radius * 1.05f;
      lightData.scale[0] = meshScale;
      lightData.scale[1] = meshScale;
      lightData.scale[2] = meshScale;

      l.light = lightData;
      l.matrixOffset = uploadUBO(frame, &l.matrices, sizeof(l.matrices), uniformAlignment);
      l.lightOffset = uploadUBO(frame, &l.light, sizeof(l.light), uniformAlignment);
      result.push_back(l);
    } else if (src.type == Light_Type::Tube) {
      CylinderLight l{};

      float radius =
          (Lighting_mode == lighting_mode::COCKPIT) ? lp->cockpit_light_radius_modifier.handle(src.radb) : src.radb;

      // Tube goes from vec2 to vec
      vec3d tubeDir{};
      vm_vec_sub(&tubeDir, &src.vec, &src.vec2);
      float length = vm_vec_mag(&tubeDir);
      if (length > 0.0001f) {
        vm_vec_normalize(&tubeDir);
      }

      // Build orientation matrix that aligns local -Z with tube direction
      matrix orient{};
      vec3d negDir = tubeDir;
      vm_vec_negate(&negDir);
      vm_vector_2_matrix(&orient, &negDir, nullptr, nullptr);

      // Build model matrix with translation at tube start (vec2) and rotation
      matrix4 model{};
      vec3d start = src.vec2;
      vm_matrix4_set_transform(&model, &orient, &start);

      vm_matrix4_x_matrix4(&l.matrices.modelViewMatrix, &viewMatrix, &model);
      l.matrices.projMatrix = projMatrix;

      lightData.lightType = LT_TUBE;
      lightData.lightRadius = radius;

      // Scale: radius for X/Y, length for Z
      lightData.scale[0] = radius * 1.05f;
      lightData.scale[1] = radius * 1.05f;
      lightData.scale[2] = length;

      l.light = lightData;
      l.matrixOffset = uploadUBO(frame, &l.matrices, sizeof(l.matrices), uniformAlignment);
      l.lightOffset = uploadUBO(frame, &l.light, sizeof(l.light), uniformAlignment);
      result.push_back(l);
    }
  }

  return result;
}

} // namespace graphics::vulkan
