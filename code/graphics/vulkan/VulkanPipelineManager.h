/**
 * @file VulkanPipelineManager.h
 * @brief Graphics pipeline caching and creation for the Vulkan renderer.
 *
 * This module provides a thread-safe pipeline manager that creates and caches Vulkan graphics
 * pipelines based on a composite key (PipelineKey). Pipelines are created on-demand when first
 * requested and then cached for subsequent draw calls with matching state.
 *
 * The manager supports two vertex input modes:
 * - **Vertex Attributes**: Traditional vertex input with bindings and attributes derived from
 *   vertex_layout. Used by most shaders (interface, particles, effects).
 * - **Vertex Pulling**: No vertex input attributes; the shader fetches vertex data from storage
 *   buffers using gl_VertexIndex. Used by model shaders (SDR_TYPE_MODEL) for bindless rendering.
 *
 * The manager targets Vulkan 1.4 and uses dynamic rendering (VK_KHR_dynamic_rendering) exclusively.
 * Extended Dynamic State 1 and 2 are assumed to be core features; Extended Dynamic State 3
 * features (color blend enable, color write mask, polygon mode, rasterization samples) are
 * optional and gated by capability flags.
 *
 * @note All public methods are thread-safe.
 *
 * @see VulkanLayoutContracts.h for shader-to-pipeline-layout mapping
 * @see VulkanShaderManager for SPIR-V module loading
 */

#pragma once

#include "VulkanDebug.h"
#include "VulkanLayoutContracts.h"
#include "VulkanShaderManager.h"

#include "graphics/2d.h"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace graphics {
namespace vulkan {

/**
 * @struct PipelineKey
 * @brief Composite key identifying a unique graphics pipeline configuration.
 *
 * PipelineKey captures all pipeline state that is not set dynamically. Two draw calls with
 * identical PipelineKey values can share the same VkPipeline object, reducing pipeline
 * creation overhead and improving GPU state coherence.
 *
 * The key includes:
 * - Shader identification (type, variant flags, module hash)
 * - Render target configuration (color/depth formats, sample count, attachment count)
 * - Blend mode and color write mask
 * - Stencil test configuration (front/back face operations)
 * - Vertex layout hash (for non-vertex-pulling shaders)
 *
 * @note For shaders using vertex pulling (e.g., SDR_TYPE_MODEL), the layout_hash field is
 *       ignored during comparison and hashing since these shaders do not use vertex attributes.
 */
struct PipelineKey {
  /** @brief Shader type from the shader_type enumeration (e.g., SDR_TYPE_MODEL, SDR_TYPE_INTERFACE). */
  shader_type type = SDR_TYPE_NONE;

  /** @brief Shader variant flags for compile-time shader permutations. */
  uint32_t variant_flags = 0;

  /**
   * @brief Hash of the shader modules (vertex + fragment).
   * @note This field is computed internally by getPipeline() and should not be set by callers.
   */
  size_t shader_hash = 0;

  /** @brief Color attachment format for dynamic rendering (e.g., VK_FORMAT_B8G8R8A8_SRGB). */
  VkFormat color_format = VK_FORMAT_UNDEFINED;

  /**
   * @brief Depth attachment format (e.g., VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT).
   * @note Set to VK_FORMAT_UNDEFINED for pipelines that do not use depth testing.
   */
  VkFormat depth_format = VK_FORMAT_UNDEFINED;

  /** @brief MSAA sample count for the render target. */
  VkSampleCountFlagBits sample_count{VK_SAMPLE_COUNT_1_BIT};

  /**
   * @brief Number of color attachments in the render pass.
   * @note Must be at least 1. Typically 1 for forward rendering, 3+ for deferred G-buffer.
   */
  uint32_t color_attachment_count{1};

  /**
   * @brief Alpha blending mode from gr_alpha_blend enumeration.
   * @see gr_alpha_blend in grinternal.h for available modes.
   */
  gr_alpha_blend blend_mode{ALPHA_BLEND_NONE};

  /**
   * @brief Hash of the vertex_layout for pipeline keying.
   * @note Ignored for shaders using vertex pulling (VertexInputMode::VertexPulling).
   *       For vertex attribute shaders, this must match the layout passed to getPipeline().
   */
  size_t layout_hash = 0;

  /**
   * @brief Color write mask as a combination of VK_COLOR_COMPONENT_*_BIT flags.
   * @note Default enables all channels (RGBA).
   */
  uint32_t color_write_mask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

  /** @brief Enable stencil testing. Requires a depth format with stencil component. */
  bool stencil_test_enable = false;

  /** @brief Stencil comparison operation for both front and back faces. */
  VkCompareOp stencil_compare_op = VK_COMPARE_OP_ALWAYS;

  /** @brief Stencil compare mask applied to both reference and buffer values. */
  uint32_t stencil_compare_mask = 0xFF;

  /** @brief Stencil write mask controlling which bits can be written. */
  uint32_t stencil_write_mask = 0xFF;

  /** @brief Stencil reference value used in stencil comparison. */
  uint32_t stencil_reference = 0;

  /** @brief Stencil operation when stencil test fails (front faces). */
  VkStencilOp front_fail_op = VK_STENCIL_OP_KEEP;

  /** @brief Stencil operation when stencil passes but depth fails (front faces). */
  VkStencilOp front_depth_fail_op = VK_STENCIL_OP_KEEP;

  /** @brief Stencil operation when both stencil and depth pass (front faces). */
  VkStencilOp front_pass_op = VK_STENCIL_OP_KEEP;

  /** @brief Stencil operation when stencil test fails (back faces). */
  VkStencilOp back_fail_op = VK_STENCIL_OP_KEEP;

  /** @brief Stencil operation when stencil passes but depth fails (back faces). */
  VkStencilOp back_depth_fail_op = VK_STENCIL_OP_KEEP;

  /** @brief Stencil operation when both stencil and depth pass (back faces). */
  VkStencilOp back_pass_op = VK_STENCIL_OP_KEEP;

  bool operator==(const PipelineKey &other) const {
    if (type != other.type) {
      return false;
    }
    const bool ignoreLayout = usesVertexPulling(type);
    return variant_flags == other.variant_flags && shader_hash == other.shader_hash &&
           color_format == other.color_format && depth_format == other.depth_format &&
           sample_count == other.sample_count && color_attachment_count == other.color_attachment_count &&
           blend_mode == other.blend_mode && color_write_mask == other.color_write_mask &&
           stencil_test_enable == other.stencil_test_enable && stencil_compare_op == other.stencil_compare_op &&
           stencil_compare_mask == other.stencil_compare_mask && stencil_write_mask == other.stencil_write_mask &&
           stencil_reference == other.stencil_reference && front_fail_op == other.front_fail_op &&
           front_depth_fail_op == other.front_depth_fail_op && front_pass_op == other.front_pass_op &&
           back_fail_op == other.back_fail_op && back_depth_fail_op == other.back_depth_fail_op &&
           back_pass_op == other.back_pass_op && (ignoreLayout || layout_hash == other.layout_hash);
  }
};

/**
 * @struct PipelineKeyHasher
 * @brief Hash functor for PipelineKey, enabling use in std::unordered_map.
 *
 * Uses boost-style hash combining (golden ratio constant 0x9e3779b9) to produce
 * well-distributed hash values from all PipelineKey fields. Consistent with
 * PipelineKey::operator==, the layout_hash is excluded for vertex-pulling shaders.
 */
struct PipelineKeyHasher {
  std::size_t operator()(const PipelineKey &key) const {
    std::size_t h = static_cast<std::size_t>(key.type);
    h ^= static_cast<std::size_t>(key.variant_flags + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.shader_hash + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.color_format + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.depth_format + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.sample_count + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.color_attachment_count + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.blend_mode + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.color_write_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_test_enable + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_compare_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_compare_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_write_mask + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.stencil_reference + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.front_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.front_depth_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.front_pass_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.back_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.back_depth_fail_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    h ^= static_cast<std::size_t>(key.back_pass_op + 0x9e3779b9 + (h << 6) + (h >> 2));
    if (!usesVertexPulling(key.type)) {
      h ^= key.layout_hash;
    }
    return h;
  }
};

/**
 * @struct VertexInputState
 * @brief Vulkan vertex input configuration derived from a vertex_layout.
 *
 * Contains the binding descriptions (buffer stride, input rate), attribute descriptions
 * (location, format, offset), and optional divisor descriptions for instanced rendering.
 * This structure is populated by convertVertexLayoutToVulkan() and cached internally
 * by VulkanPipelineManager.
 */
struct VertexInputState {
  /** @brief Vertex buffer bindings with stride and input rate per buffer. */
  std::vector<vk::VertexInputBindingDescription> bindings;

  /** @brief Vertex attributes mapping buffer data to shader input locations. */
  std::vector<vk::VertexInputAttributeDescription> attributes;

  /**
   * @brief Instance rate divisors for bindings with divisor > 1.
   * @note Requires VK_EXT_vertex_attribute_divisor when non-empty.
   */
  std::vector<vk::VertexInputBindingDivisorDescription> divisors;
};

/**
 * @brief Converts an engine vertex_layout to Vulkan vertex input state.
 *
 * Translates the engine's vertex_layout (which describes vertex components, their
 * formats, offsets, and buffer assignments) into Vulkan-compatible binding and
 * attribute descriptions.
 *
 * The function:
 * - Groups vertex components by buffer number into VkVertexInputBindingDescription
 * - Converts each vertex_format_data component to VkVertexInputAttributeDescription
 * - Handles MATRIX4 attributes specially (spans 4 consecutive shader locations)
 * - Detects instanced attributes (divisor > 0) and sets appropriate input rate
 * - Collects divisor > 1 cases into divisor descriptions (requires VK_EXT_vertex_attribute_divisor)
 *
 * Vertex attribute locations follow OpenGL convention:
 * - Location 0: POSITION (POSITION2, POSITION3, POSITION4, SCREEN_POS)
 * - Location 1: COLOR (COLOR3, COLOR4, COLOR4F)
 * - Location 2: TEXCOORD (TEX_COORD2, TEX_COORD4)
 * - Location 3: NORMAL
 * - Location 4: TANGENT
 * - Location 5: MODEL_ID
 * - Location 6: RADIUS
 * - Location 7: UVEC
 * - Location 8-11: MATRIX4 (4 consecutive vec4 locations)
 *
 * @param layout The engine vertex layout to convert.
 * @return VertexInputState containing Vulkan-ready binding and attribute descriptions.
 *
 * @throws std::runtime_error If an unknown vertex format type is encountered.
 */
VertexInputState convertVertexLayoutToVulkan(const vertex_layout &layout);

/**
 * @class VulkanPipelineManager
 * @brief Thread-safe manager for Vulkan graphics pipeline creation and caching.
 *
 * VulkanPipelineManager maintains a cache of VkPipeline objects keyed by PipelineKey.
 * When getPipeline() is called, it either returns an existing cached pipeline or creates
 * a new one. Pipeline creation is expensive, so caching is essential for performance.
 *
 * The manager supports three pipeline layouts corresponding to different rendering paths:
 * - **Standard**: Per-draw push descriptors with global descriptor set (particles, effects, UI)
 * - **Model**: Bindless model descriptor set with push constants (SDR_TYPE_MODEL)
 * - **Deferred**: Push descriptors with G-buffer global set (deferred lighting pass)
 *
 * Dynamic state is used extensively to reduce pipeline permutations:
 * - **Core (always)**: Viewport, scissor, line width, cull mode, front face, primitive topology,
 *   depth test/write enable, depth compare op, stencil test enable
 * - **EDS3 (optional)**: Color blend enable, color write mask, polygon mode, rasterization samples
 *
 * @note All public methods are thread-safe. Pipeline creation happens under mutex lock.
 */
class VulkanPipelineManager {
public:
  /**
   * @brief Constructs the pipeline manager with the given device and configuration.
   *
   * @param device The Vulkan logical device for pipeline creation.
   * @param pipelineLayout Pipeline layout for standard (per-draw push descriptor) shaders.
   * @param modelPipelineLayout Pipeline layout for bindless model rendering.
   * @param deferredPipelineLayout Pipeline layout for deferred lighting shaders.
   * @param pipelineCache Vulkan pipeline cache for accelerating pipeline creation.
   * @param supportsExtendedDynamicState3 True if EDS3 extension is available.
   * @param extDyn3Caps Per-feature capability flags for EDS3 dynamic states.
   * @param supportsVertexAttributeDivisor True if VK_EXT_vertex_attribute_divisor is available.
   * @param dynamicRenderingEnabled Must be true; indicates VK_KHR_dynamic_rendering is in use.
   *
   * @throws std::runtime_error If dynamicRenderingEnabled is false (required feature).
   */
  VulkanPipelineManager(vk::Device device, vk::PipelineLayout pipelineLayout, vk::PipelineLayout modelPipelineLayout,
                        vk::PipelineLayout deferredPipelineLayout, vk::PipelineCache pipelineCache,
                        bool supportsExtendedDynamicState3, const ExtendedDynamicState3Caps &extDyn3Caps,
                        bool supportsVertexAttributeDivisor, bool dynamicRenderingEnabled);

  /**
   * @brief Retrieves or creates a pipeline matching the given key and shader modules.
   *
   * This is the primary API for obtaining pipelines. The method:
   * 1. Validates that the key's layout_hash matches the provided layout (for vertex attribute shaders)
   * 2. Computes a shader_hash from the provided modules and updates the cache key
   * 3. Checks the cache for an existing pipeline with matching key
   * 4. Creates a new pipeline if none exists, caches it, and returns it
   *
   * For shaders using vertex pulling (VertexInputMode::VertexPulling), the layout parameter
   * is ignored and the pipeline is created with empty vertex input state.
   *
   * @param key Pipeline configuration key (all fields except shader_hash should be set by caller).
   * @param modules Compiled SPIR-V shader modules (vertex and fragment).
   * @param layout Vertex layout for shaders using vertex attributes (ignored for vertex pulling).
   * @return VkPipeline handle. The pipeline is owned by the manager; do not destroy it.
   *
   * @throws std::runtime_error If key.layout_hash mismatches layout.hash() for vertex attribute shaders.
   * @throws std::runtime_error If pipeline creation fails.
   *
   * @note Thread-safe. May block briefly while creating a new pipeline.
   */
  vk::Pipeline getPipeline(const PipelineKey &key, const ShaderModules &modules, const vertex_layout &layout);

  /**
   * @brief Builds the list of dynamic states used by all pipelines.
   *
   * Returns the set of VkDynamicState values that will be set dynamically rather than
   * baked into the pipeline. This affects what state must be set via vkCmdSet* commands
   * before each draw call.
   *
   * Core dynamic states (always included):
   * - eViewport, eScissor, eLineWidth
   * - eCullMode, eFrontFace, ePrimitiveTopology
   * - eDepthTestEnable, eDepthWriteEnable, eDepthCompareOp
   * - eStencilTestEnable
   *
   * Extended Dynamic State 3 (conditionally included based on caps):
   * - eColorBlendEnableEXT, eColorWriteMaskEXT
   * - ePolygonModeEXT, eRasterizationSamplesEXT
   *
   * @param supportsExtendedDynamicState3 True if EDS3 extension is available.
   * @param caps Per-feature capability flags for EDS3 states.
   * @return Vector of dynamic state enums for use in VkPipelineDynamicStateCreateInfo.
   */
  static std::vector<vk::DynamicState> BuildDynamicStateList(bool supportsExtendedDynamicState3,
                                                             const ExtendedDynamicState3Caps &caps);

private:
  // --- Device and pipeline configuration ---
  vk::Device m_device;                          ///< Vulkan logical device for pipeline creation.
  vk::PipelineLayout m_pipelineLayout;          ///< Layout for standard per-draw push descriptor shaders.
  vk::PipelineLayout m_modelPipelineLayout;     ///< Layout for bindless model rendering shaders.
  vk::PipelineLayout m_deferredPipelineLayout;  ///< Layout for deferred lighting pass shaders.
  vk::PipelineCache m_pipelineCache;            ///< Vulkan pipeline cache for creation acceleration.

  // --- Thread safety ---
  std::mutex m_mutex;  ///< Protects m_pipelines and m_vertexInputCache from concurrent access.

  // --- Feature capability flags ---
  bool m_supportsExtendedDynamicState3 = false;       ///< True if VK_EXT_extended_dynamic_state3 is available.
  ExtendedDynamicState3Caps m_extDyn3Caps{};          ///< Per-feature caps for EDS3 dynamic states.
  bool m_supportsVertexAttributeDivisor = false;      ///< True if VK_EXT_vertex_attribute_divisor is available.
  bool m_dynamicRenderingEnabled = false;             ///< True if VK_KHR_dynamic_rendering is enabled (required).

  // --- Caches ---
  std::unordered_map<PipelineKey, vk::UniquePipeline, PipelineKeyHasher> m_pipelines;  ///< Pipeline cache by key.
  std::unordered_map<size_t, VertexInputState> m_vertexInputCache;  ///< Vertex input state cache by layout hash.

  /**
   * @brief Creates a new graphics pipeline for the given configuration.
   *
   * Called by getPipeline() when no cached pipeline exists. Constructs the full
   * VkGraphicsPipelineCreateInfo with shader stages, vertex input, rasterization,
   * depth/stencil, blend state, and dynamic state configuration.
   *
   * @param key Pipeline configuration (shader_hash must be set).
   * @param modules SPIR-V shader modules.
   * @param layout Vertex layout (used only if shader uses vertex attributes).
   * @return Newly created pipeline wrapped in vk::UniquePipeline.
   * @throws std::runtime_error On pipeline creation failure.
   */
  vk::UniquePipeline createPipeline(const PipelineKey &key, const ShaderModules &modules, const vertex_layout &layout);

  /**
   * @brief Retrieves or creates cached Vulkan vertex input state for a layout.
   *
   * Converts vertex_layout to VertexInputState on first access and caches the result.
   * Subsequent calls with layouts having the same hash() return the cached state.
   *
   * @param layout Engine vertex layout to convert.
   * @return Reference to cached VertexInputState (valid for manager lifetime).
   */
  const VertexInputState &getVertexInputState(const vertex_layout &layout);
};

} // namespace vulkan
} // namespace graphics
