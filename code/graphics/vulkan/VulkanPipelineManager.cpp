/**
 * @file VulkanPipelineManager.cpp
 * @brief Implementation of Vulkan graphics pipeline management.
 *
 * This file implements the VulkanPipelineManager class and supporting utilities
 * for creating and caching Vulkan graphics pipelines. Key implementation details:
 *
 * ## Vertex Format Mapping
 * Engine vertex formats (vertex_format_data) are mapped to Vulkan formats via
 * VERTEX_FORMAT_MAP. Each format has an assigned shader location following OpenGL
 * conventions for compatibility with existing shaders.
 *
 * ## Pipeline Creation
 * Pipelines are created using VkGraphicsPipelineCreateInfo with:
 * - Dynamic rendering (VK_KHR_dynamic_rendering) instead of render passes
 * - Extensive use of dynamic state to minimize pipeline permutations
 * - Support for both vertex attribute and vertex pulling input modes
 *
 * ## Thread Safety
 * All mutable state (pipeline cache, vertex input cache) is protected by m_mutex.
 * Pipeline creation occurs under lock but is infrequent after initial warmup.
 *
 * @see VulkanPipelineManager.h for public API documentation
 */

#include "VulkanPipelineManager.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace graphics {
namespace vulkan {

// =============================================================================
// Vertex Format Mapping
// =============================================================================

/**
 * @brief Maps an engine vertex format to Vulkan format and shader location.
 *
 * @var format Vulkan format for the vertex attribute (e.g., eR32G32B32Sfloat)
 * @var location Shader input location (matches OpenGL attribute locations)
 * @var componentCount Number of components (used for documentation/debugging)
 */
struct VertexFormatMapping {
  vk::Format format;
  uint32_t location;
  uint32_t componentCount;
};

// Location mapping follows OpenGL convention:
// 0 = POSITION, 1 = COLOR, 2 = TEXCOORD, 3 = NORMAL, 4 = TANGENT, etc.
static const std::unordered_map<vertex_format_data::vertex_format, VertexFormatMapping> VERTEX_FORMAT_MAP = {
    {vertex_format_data::POSITION4, {vk::Format::eR32G32B32A32Sfloat, 0, 4}},
    {vertex_format_data::POSITION3, {vk::Format::eR32G32B32Sfloat, 0, 3}},
    {vertex_format_data::POSITION2, {vk::Format::eR32G32Sfloat, 0, 2}},
    {vertex_format_data::SCREEN_POS, {vk::Format::eR32G32Sfloat, 0, 2}},
    {vertex_format_data::COLOR3, {vk::Format::eR8G8B8Unorm, 1, 3}},
    {vertex_format_data::COLOR4, {vk::Format::eR8G8B8A8Unorm, 1, 4}},
    {vertex_format_data::COLOR4F, {vk::Format::eR32G32B32A32Sfloat, 1, 4}},
    {vertex_format_data::TEX_COORD2, {vk::Format::eR32G32Sfloat, 2, 2}},
    {vertex_format_data::TEX_COORD4, {vk::Format::eR32G32B32A32Sfloat, 2, 4}},
    {vertex_format_data::NORMAL, {vk::Format::eR32G32B32Sfloat, 3, 3}},
    {vertex_format_data::TANGENT, {vk::Format::eR32G32B32A32Sfloat, 4, 4}},
    {vertex_format_data::MODEL_ID, {vk::Format::eR32Sfloat, 5, 1}},
    {vertex_format_data::RADIUS, {vk::Format::eR32Sfloat, 6, 1}},
    {vertex_format_data::UVEC, {vk::Format::eR32G32B32Sfloat, 7, 3}},
    {vertex_format_data::MATRIX4, {vk::Format::eR32G32B32A32Sfloat, 8, 4}},
};

// Vulkan allows gaps in vertex attribute locations - a layout with position (0) and
// texcoord (2) but no color (1) is valid. The shader simply won't receive data for
// unused locations. Validation layer warnings about mismatched locations indicate
// shader/layout incompatibility, not an invalid layout.

// =============================================================================
// Shader Module Hashing
// =============================================================================

/**
 * @brief Extracts a numeric identifier from a VkShaderModule handle.
 *
 * On 64-bit platforms, VkShaderModule is a pointer-like opaque handle.
 * On 32-bit platforms with non-dispatchable handles, it's a 64-bit integer.
 * This function normalizes both cases to std::uintptr_t for hashing.
 *
 * @param module The Vulkan shader module handle.
 * @return A numeric value suitable for hashing.
 */
static std::uintptr_t shaderModuleId(vk::ShaderModule module) {
  const auto handle = static_cast<VkShaderModule>(module);
#if defined(VK_USE_64_BIT_PTR_DEFINES)
  return reinterpret_cast<std::uintptr_t>(handle);
#else
  return static_cast<std::uintptr_t>(handle);
#endif
}

/**
 * @brief Computes a hash from vertex and fragment shader module handles.
 *
 * Uses boost-style hash combining to produce a single hash value from both
 * shader module handles. This hash is used as part of the PipelineKey to
 * distinguish pipelines using different shader variants.
 *
 * @param modules Struct containing vertex and fragment shader modules.
 * @return Combined hash value.
 */
static size_t hashShaderModules(const ShaderModules &modules) {
  std::size_t h = shaderModuleId(modules.vert);
  h ^= shaderModuleId(modules.frag) + 0x9e3779b9 + (h << 6) + (h >> 2);
  return h;
}

// =============================================================================
// Blend State Configuration
// =============================================================================

/**
 * @brief Creates a Vulkan color blend attachment state from engine blend mode.
 *
 * Translates the engine's gr_alpha_blend enumeration to Vulkan blend factors
 * and operations. Each blend mode corresponds to a specific blending equation:
 *
 * - ALPHA_BLEND_NONE: No blending (1*Src + 0*Dst)
 * - ALPHA_BLEND_ADDITIVE: Pure additive (1*Src + 1*Dst)
 * - ALPHA_BLEND_ALPHA_ADDITIVE: Alpha-weighted additive (Alpha*Src + 1*Dst)
 * - ALPHA_BLEND_ALPHA_BLEND_ALPHA: Standard alpha blend (Alpha*Src + (1-Alpha)*Dst)
 * - ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR: Alpha + inverse source color (Alpha*Src + (1-SrcColor)*Dst)
 * - ALPHA_BLEND_PREMULTIPLIED: Premultiplied alpha (1*Src + (1-Alpha)*Dst)
 *
 * @param mode Engine blend mode to translate.
 * @param colorWriteMask Channel write mask (RGBA components).
 * @return Configured VkPipelineColorBlendAttachmentState.
 */
static vk::PipelineColorBlendAttachmentState buildBlendAttachment(gr_alpha_blend mode,
                                                                  vk::ColorComponentFlags colorWriteMask) {
  vk::PipelineColorBlendAttachmentState state{};
  state.colorWriteMask = colorWriteMask;

  switch (mode) {
  case ALPHA_BLEND_NONE:
    state.blendEnable = VK_FALSE;
    break;
  case ALPHA_BLEND_ADDITIVE:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = vk::BlendFactor::eOne;
    state.dstColorBlendFactor = vk::BlendFactor::eOne;
    state.colorBlendOp = vk::BlendOp::eAdd;
    state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    state.alphaBlendOp = vk::BlendOp::eAdd;
    break;
  case ALPHA_BLEND_ALPHA_ADDITIVE:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    state.dstColorBlendFactor = vk::BlendFactor::eOne;
    state.colorBlendOp = vk::BlendOp::eAdd;
    state.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
    state.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    state.alphaBlendOp = vk::BlendOp::eAdd;
    break;
  case ALPHA_BLEND_ALPHA_BLEND_ALPHA:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    state.colorBlendOp = vk::BlendOp::eAdd;
    state.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
    state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    state.alphaBlendOp = vk::BlendOp::eAdd;
    break;
  case ALPHA_BLEND_ALPHA_BLEND_SRC_COLOR:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
    state.colorBlendOp = vk::BlendOp::eAdd;
    state.srcAlphaBlendFactor = vk::BlendFactor::eSrcAlpha;
    state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcColor;
    state.alphaBlendOp = vk::BlendOp::eAdd;
    break;
  case ALPHA_BLEND_PREMULTIPLIED:
    state.blendEnable = VK_TRUE;
    state.srcColorBlendFactor = vk::BlendFactor::eOne;
    state.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    state.colorBlendOp = vk::BlendOp::eAdd;
    state.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    state.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    state.alphaBlendOp = vk::BlendOp::eAdd;
    break;
  default:
    state.blendEnable = VK_FALSE;
    break;
  }

  return state;
}

// =============================================================================
// Format Utilities
// =============================================================================

/**
 * @brief Checks if a Vulkan format includes a stencil component.
 *
 * Used to validate that stencil operations are only configured when the
 * depth attachment format actually supports stencil. Also used to set the
 * stencilAttachmentFormat in VkPipelineRenderingCreateInfo.
 *
 * @param format The depth/stencil format to check.
 * @return True if format has stencil component (D24S8, D32S8), false otherwise.
 */
static bool formatHasStencil(vk::Format format) {
  switch (format) {
  case vk::Format::eD32SfloatS8Uint:
  case vk::Format::eD24UnormS8Uint:
    return true;
  default:
    return false;
  }
}

// =============================================================================
// Vertex Layout Conversion
// =============================================================================

VertexInputState convertVertexLayoutToVulkan(const vertex_layout &layout) {
  VertexInputState result;

  // Step 1: Group vertex components by their buffer number.
  // Each buffer becomes a separate VkVertexInputBindingDescription.
  std::unordered_map<size_t, std::vector<const vertex_format_data *>> componentsByBuffer;

  for (size_t i = 0; i < layout.get_num_vertex_components(); ++i) {
    const vertex_format_data *component = layout.get_vertex_component(i);
    componentsByBuffer[component->buffer_number].push_back(component);
  }

  // Track bindings that need instance rate divisors > 1 (requires VK_EXT_vertex_attribute_divisor)
  std::unordered_map<uint32_t, uint32_t> divisorsByBinding;

  // Step 2: Create binding and attribute descriptions for each buffer.
  for (const auto &[bufferNum, components] : componentsByBuffer) {
    if (components.empty()) {
      continue;
    }

    size_t stride = layout.get_vertex_stride(bufferNum);

    // Create binding description for this buffer
    vk::VertexInputBindingDescription binding{};
    binding.binding = static_cast<uint32_t>(bufferNum);
    binding.stride = static_cast<uint32_t>(stride);
    binding.inputRate = vk::VertexInputRate::eVertex;

    // Check if any component in this buffer uses instancing.
    // If so, the entire binding becomes per-instance.
    for (const auto *comp : components) {
      if (comp->divisor != 0) {
        binding.inputRate = vk::VertexInputRate::eInstance;
        // Divisor == 1 means "advance once per instance" which is core Vulkan.
        // Divisor > 1 means "advance once every N instances" and requires the extension.
        if (comp->divisor > 1) {
          divisorsByBinding[binding.binding] = static_cast<uint32_t>(comp->divisor);
        }
        break;
      }
    }

    result.bindings.push_back(binding);

    // Create attribute descriptions for each component in this buffer.
    // Each component maps to a shader input location via VERTEX_FORMAT_MAP.
    for (const auto *component : components) {
      auto it = VERTEX_FORMAT_MAP.find(component->format_type);
      Assertion(it != VERTEX_FORMAT_MAP.end(), "Unknown vertex format type %d - add to VERTEX_FORMAT_MAP",
                static_cast<int>(component->format_type));
      if (it == VERTEX_FORMAT_MAP.end()) {
        throw std::runtime_error("Unknown vertex format type; add to VERTEX_FORMAT_MAP.");
      }

      const auto &mapping = it->second;

      // MATRIX4 is special: it consumes 4 consecutive shader locations (8, 9, 10, 11)
      // since each mat4 column is passed as a separate vec4 attribute.
      if (component->format_type == vertex_format_data::MATRIX4) {
        for (uint32_t row = 0; row < 4; ++row) {
          vk::VertexInputAttributeDescription attr{};
          attr.binding = static_cast<uint32_t>(component->buffer_number);
          attr.location = mapping.location + row;  // Locations 8, 9, 10, 11
          attr.format = vk::Format::eR32G32B32A32Sfloat;
          attr.offset = static_cast<uint32_t>(component->offset + row * 16);  // 16 bytes per column
          result.attributes.push_back(attr);
        }
      } else {
        // Standard single-location attribute
        vk::VertexInputAttributeDescription attr{};
        attr.binding = static_cast<uint32_t>(component->buffer_number);
        attr.location = mapping.location;
        attr.format = mapping.format;
        attr.offset = static_cast<uint32_t>(component->offset);
        result.attributes.push_back(attr);
      }
    }
  }

  // Step 3: Collect divisor descriptions for bindings that need the extension.
  for (const auto &kv : divisorsByBinding) {
    vk::VertexInputBindingDivisorDescription desc{};
    desc.binding = kv.first;
    desc.divisor = kv.second;
    result.divisors.push_back(desc);
  }

  return result;
}

// =============================================================================
// VulkanPipelineManager Implementation
// =============================================================================

VulkanPipelineManager::VulkanPipelineManager(vk::Device device, vk::PipelineLayout pipelineLayout,
                                             vk::PipelineLayout modelPipelineLayout,
                                             vk::PipelineLayout deferredPipelineLayout, vk::PipelineCache pipelineCache,
                                             bool supportsExtendedDynamicState3,
                                             const ExtendedDynamicState3Caps &extDyn3Caps,
                                             bool supportsVertexAttributeDivisor, bool dynamicRenderingEnabled)
    : m_device(device), m_pipelineLayout(pipelineLayout), m_modelPipelineLayout(modelPipelineLayout),
      m_deferredPipelineLayout(deferredPipelineLayout), m_pipelineCache(pipelineCache),
      m_supportsExtendedDynamicState3(supportsExtendedDynamicState3), m_extDyn3Caps(extDyn3Caps),
      m_supportsVertexAttributeDivisor(supportsVertexAttributeDivisor),
      m_dynamicRenderingEnabled(dynamicRenderingEnabled) {
  // Dynamic rendering is required - we don't support traditional render passes.
  // This allows pipelines to be created without knowing the render pass ahead of time.
  if (!m_dynamicRenderingEnabled) {
    throw std::runtime_error("Vulkan dynamicRendering feature must be enabled when using renderPass=VK_NULL_HANDLE.");
  }
}

// Targeting Vulkan 1.4: Extended Dynamic State 1/2 are core and always available for this engine.
// Extended Dynamic State 3 remains optional and is gated by supportsExtendedDynamicState3 + per-feature caps.
std::vector<vk::DynamicState> VulkanPipelineManager::BuildDynamicStateList(bool supportsExtendedDynamicState3,
                                                                           const ExtendedDynamicState3Caps &caps) {
  std::vector<vk::DynamicState> dynamicStates = {
      vk::DynamicState::eViewport,          vk::DynamicState::eScissor,          vk::DynamicState::eLineWidth,
      vk::DynamicState::eCullMode,          vk::DynamicState::eFrontFace,        vk::DynamicState::ePrimitiveTopology,
      vk::DynamicState::eDepthTestEnable,   vk::DynamicState::eDepthWriteEnable, vk::DynamicState::eDepthCompareOp,
      vk::DynamicState::eStencilTestEnable,
  };

  if (supportsExtendedDynamicState3) {
    if (caps.colorBlendEnable) {
      dynamicStates.push_back(vk::DynamicState::eColorBlendEnableEXT);
    }
    if (caps.colorWriteMask) {
      dynamicStates.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (caps.polygonMode) {
      dynamicStates.push_back(vk::DynamicState::ePolygonModeEXT);
    }
    if (caps.rasterizationSamples) {
      dynamicStates.push_back(vk::DynamicState::eRasterizationSamplesEXT);
    }
  }

  return dynamicStates;
}

vk::Pipeline VulkanPipelineManager::getPipeline(const PipelineKey &key, const ShaderModules &modules,
                                                const vertex_layout &layout) {
  // Enforce layout contract in all builds: if the shader uses vertex attributes, the key's
  // layout_hash must match the supplied layout (mismatches are treated as a hard error).
  const auto &layoutSpec = getShaderLayoutSpec(key.type);
  if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
    const size_t expectedHash = layout.hash();
    if (key.layout_hash != expectedHash) {
      throw std::runtime_error("PipelineKey.layout_hash mismatches provided vertex_layout for VertexAttributes shader");
    }
  }

  PipelineKey cacheKey = key;
  cacheKey.shader_hash = hashShaderModules(modules);

  // Pipelines are cached by PipelineKey.
  std::lock_guard<std::mutex> guard(m_mutex);
  auto it = m_pipelines.find(cacheKey);
  if (it != m_pipelines.end()) {
    return it->second.get();
  }

  auto pipeline = createPipeline(cacheKey, modules, layout);
  auto handle = pipeline.get();
  m_pipelines.emplace(cacheKey, std::move(pipeline));
  return handle;
}

const VertexInputState &VulkanPipelineManager::getVertexInputState(const vertex_layout &layout) {
  size_t layoutHash = layout.hash();
  auto it = m_vertexInputCache.find(layoutHash);
  if (it != m_vertexInputCache.end()) {
    return it->second;
  }

  auto state = convertVertexLayoutToVulkan(layout);
  auto result = m_vertexInputCache.emplace(layoutHash, std::move(state));
  return result.first->second;
}

vk::UniquePipeline VulkanPipelineManager::createPipeline(const PipelineKey &key, const ShaderModules &modules,
                                                         const vertex_layout &layout) {
  // -------------------------------------------------------------------------
  // Shader Stages
  // -------------------------------------------------------------------------
  std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages{};
  shaderStages[0].stage = vk::ShaderStageFlagBits::eVertex;
  shaderStages[0].module = modules.vert;
  shaderStages[0].pName = "main";

  shaderStages[1].stage = vk::ShaderStageFlagBits::eFragment;
  shaderStages[1].module = modules.frag;
  shaderStages[1].pName = "main";

  // -------------------------------------------------------------------------
  // Vertex Input State
  // -------------------------------------------------------------------------
  // Two modes:
  // - VertexPulling: Empty vertex input; shader fetches from storage buffer via gl_VertexIndex
  // - VertexAttributes: Traditional vertex input with bindings and attributes from layout
  vk::PipelineVertexInputStateCreateInfo vertexInput{};
  vk::PipelineVertexInputDivisorStateCreateInfo divisorInfo{};
  const auto &layoutSpec = getShaderLayoutSpec(key.type);
  const bool useVertexPulling = layoutSpec.vertexInput == VertexInputMode::VertexPulling;

  if (useVertexPulling) {
    // Vertex pulling: no vertex input attributes. Data is fetched from a storage
    // buffer in the vertex shader using gl_VertexIndex.
    vertexInput.vertexBindingDescriptionCount = 0;
    vertexInput.pVertexBindingDescriptions = nullptr;
    vertexInput.vertexAttributeDescriptionCount = 0;
    vertexInput.pVertexAttributeDescriptions = nullptr;
  } else {
    const VertexInputState &vertexInputState = getVertexInputState(layout);

    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputState.bindings.size());
    vertexInput.pVertexBindingDescriptions = vertexInputState.bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputState.attributes.size());
    vertexInput.pVertexAttributeDescriptions = vertexInputState.attributes.data();

    if (m_supportsVertexAttributeDivisor && !vertexInputState.divisors.empty()) {
      divisorInfo.vertexBindingDivisorCount = static_cast<uint32_t>(vertexInputState.divisors.size());
      divisorInfo.pVertexBindingDivisors = vertexInputState.divisors.data();
      vertexInput.pNext = &divisorInfo;
    } else if (!vertexInputState.divisors.empty() && !m_supportsVertexAttributeDivisor) {
      throw std::runtime_error(
          "vertexAttributeInstanceRateDivisor not enabled but divisor > 1 requested in vertex layout.");
    }
  }

  // -------------------------------------------------------------------------
  // Input Assembly State
  // -------------------------------------------------------------------------
  // Topology is set dynamically via vkCmdSetPrimitiveTopology, but we need a default.
  vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  // -------------------------------------------------------------------------
  // Viewport State
  // -------------------------------------------------------------------------
  // Viewport and scissor are dynamic state, so we only specify counts here.
  vk::PipelineViewportStateCreateInfo viewportState{};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  // -------------------------------------------------------------------------
  // Rasterization State
  // -------------------------------------------------------------------------
  // Cull mode, front face, polygon mode, and line width are dynamic state.
  // These are default values that may be overridden at draw time.
  vk::PipelineRasterizationStateCreateInfo rasterizer{};
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = vk::PolygonMode::eFill;
  rasterizer.lineWidth = 1.0f;
  rasterizer.cullMode = vk::CullModeFlagBits::eBack;
  rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
  rasterizer.depthBiasEnable = VK_FALSE;

  // -------------------------------------------------------------------------
  // Multisample State
  // -------------------------------------------------------------------------
  vk::PipelineMultisampleStateCreateInfo multisampling{};
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = static_cast<vk::SampleCountFlagBits>(key.sample_count);

  // -------------------------------------------------------------------------
  // Color Blend State
  // -------------------------------------------------------------------------
  if (key.color_attachment_count == 0) {
    throw std::runtime_error("PipelineKey.color_attachment_count must be at least 1.");
  }

  auto colorWriteMask = static_cast<vk::ColorComponentFlags>(key.color_write_mask);
  auto colorBlendAttachment = buildBlendAttachment(key.blend_mode, colorWriteMask);

  // Replicate the same blend state for all color attachments (MRT)
  std::vector<vk::PipelineColorBlendAttachmentState> attachments(key.color_attachment_count, colorBlendAttachment);

  vk::PipelineColorBlendStateCreateInfo colorBlending{};
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.attachmentCount = static_cast<uint32_t>(attachments.size());
  colorBlending.pAttachments = attachments.data();

  // -------------------------------------------------------------------------
  // Dynamic State
  // -------------------------------------------------------------------------
  // Dynamic state reduces pipeline permutations by deferring state to command buffer.
  auto dynamicStates = BuildDynamicStateList(m_supportsExtendedDynamicState3, m_extDyn3Caps);

  vk::PipelineDynamicStateCreateInfo dynamicState{};
  dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
  dynamicState.pDynamicStates = dynamicStates.data();

  // -------------------------------------------------------------------------
  // Render Target Formats (Dynamic Rendering)
  // -------------------------------------------------------------------------
  vk::Format colorFormat = static_cast<vk::Format>(key.color_format);
  vk::Format depthFormat = static_cast<vk::Format>(key.depth_format);
  std::vector<vk::Format> colorFormats(key.color_attachment_count, colorFormat);

  // -------------------------------------------------------------------------
  // Validation
  // -------------------------------------------------------------------------
  Assertion(!colorFormats.empty(), "colorFormats must not be empty");
  if (layoutSpec.vertexInput == VertexInputMode::VertexAttributes) {
    // When using vertex attributes, require Location 0 (position). Other locations are shader-dependent
    // and may legitimately be absent (e.g., Interface/NanoVG use locations 0 and 2 with no color).
    const VertexInputState &vertexInputState = getVertexInputState(layout);
    bool hasLoc0 = false;
    for (const auto &a : vertexInputState.attributes) {
      if (a.location == 0)
        hasLoc0 = true;
    }
    Assertion(hasLoc0, "Vertex input pipeline created without Location 0 attribute");
    if (!hasLoc0) {
      throw std::runtime_error("VertexAttributes pipeline created without Location 0 attribute.");
    }
  }

  // -------------------------------------------------------------------------
  // Depth/Stencil State
  // -------------------------------------------------------------------------
  // Depth test/write enable and compare op are dynamic state in EDS1.
  // Stencil operations are baked into the pipeline from PipelineKey.
  vk::PipelineDepthStencilStateCreateInfo depthStencil{};
  if (key.stencil_test_enable && !formatHasStencil(depthFormat)) {
    throw std::runtime_error("Stencil test enabled but render target depth format has no stencil component.");
  }
  if (depthFormat != vk::Format::eUndefined) {
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = vk::CompareOp::eLessOrEqual;
  } else {
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
  }
  depthStencil.depthBoundsTestEnable = VK_FALSE;

  depthStencil.stencilTestEnable = key.stencil_test_enable ? VK_TRUE : VK_FALSE;
  depthStencil.front.compareOp = static_cast<vk::CompareOp>(key.stencil_compare_op);
  depthStencil.front.compareMask = key.stencil_compare_mask;
  depthStencil.front.writeMask = key.stencil_write_mask;
  depthStencil.front.reference = key.stencil_reference;
  depthStencil.front.failOp = static_cast<vk::StencilOp>(key.front_fail_op);
  depthStencil.front.depthFailOp = static_cast<vk::StencilOp>(key.front_depth_fail_op);
  depthStencil.front.passOp = static_cast<vk::StencilOp>(key.front_pass_op);
  depthStencil.back.compareOp = static_cast<vk::CompareOp>(key.stencil_compare_op);
  depthStencil.back.compareMask = key.stencil_compare_mask;
  depthStencil.back.writeMask = key.stencil_write_mask;
  depthStencil.back.reference = key.stencil_reference;
  depthStencil.back.failOp = static_cast<vk::StencilOp>(key.back_fail_op);
  depthStencil.back.depthFailOp = static_cast<vk::StencilOp>(key.back_depth_fail_op);
  depthStencil.back.passOp = static_cast<vk::StencilOp>(key.back_pass_op);

  // -------------------------------------------------------------------------
  // Dynamic Rendering Info (VK_KHR_dynamic_rendering)
  // -------------------------------------------------------------------------
  // Instead of a VkRenderPass, we specify attachment formats directly.
  // This allows pipeline creation without knowing the render pass ahead of time.
  vk::PipelineRenderingCreateInfo renderingInfo{};
  renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
  renderingInfo.pColorAttachmentFormats = colorFormats.data();
  renderingInfo.depthAttachmentFormat = depthFormat;
  renderingInfo.stencilAttachmentFormat = formatHasStencil(depthFormat) ? depthFormat : vk::Format::eUndefined;

  // -------------------------------------------------------------------------
  // Pipeline Assembly
  // -------------------------------------------------------------------------
  vk::GraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
  pipelineInfo.pStages = shaderStages.data();
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;

  // Select pipeline layout based on shader type's layout contract
  switch (layoutSpec.pipelineLayout) {
  case PipelineLayoutKind::Model:
    pipelineInfo.layout = m_modelPipelineLayout;
    break;
  case PipelineLayoutKind::Standard:
    pipelineInfo.layout = m_pipelineLayout;
    break;
  case PipelineLayoutKind::Deferred:
    pipelineInfo.layout = m_deferredPipelineLayout;
    break;
  }

  // Use dynamic rendering (no render pass)
  pipelineInfo.renderPass = VK_NULL_HANDLE;
  pipelineInfo.pNext = &renderingInfo;

  // -------------------------------------------------------------------------
  // Pipeline Creation
  // -------------------------------------------------------------------------
  auto pipelineResult = m_device.createGraphicsPipelineUnique(m_pipelineCache, pipelineInfo);
  if (pipelineResult.result != vk::Result::eSuccess) {
    throw std::runtime_error("Failed to create Vulkan graphics pipeline.");
  }

  return std::move(pipelineResult.value);
}

} // namespace vulkan
} // namespace graphics
