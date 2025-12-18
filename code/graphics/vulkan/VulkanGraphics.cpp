
#include "VulkanGraphics.h"

#include "VulkanModelTypes.h"
#include "VulkanRenderer.h"
#include "VulkanPipelineManager.h"
#include "VulkanVertexTypes.h"
#include "VulkanDebug.h"
#include "VulkanClip.h"
#include <fstream>
#include <chrono>
#include <atomic>

#include "backends/imgui_impl_sdl.h"
#include "backends/imgui_impl_vulkan.h"
#include "graphics/2d.h"
#include "graphics/grstub.h"
#include "graphics/material.h"
#include "graphics/matrix.h"
#include "graphics/util/uniform_structs.h"
#include "graphics/tmapper.h"
#include "mod_table/mod_table.h"
#include "cmdline/cmdline.h"
#include "globalincs/version.h"
#include "VulkanRenderer.h"
#include "lighting/lighting.h"

extern transform_stack gr_model_matrix_stack;
extern matrix4 gr_view_matrix;
extern matrix4 gr_model_view_matrix;
extern matrix4 gr_projection_matrix;

#define BMPMAN_INTERNAL
#include "bmpman/bm_internal.h"

namespace graphics::vulkan {

// #region agent log
uint32_t g_gbufferDrawCountThisFrame = 0;

void resetGbufferDrawCount() {
  g_gbufferDrawCountThisFrame = 0;
}

void logDebugEventDraw(const char* eventType, const char* location, const char* message, uint32_t indexCount, uint32_t colorAttachmentCount) {
  const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  static std::atomic<uint64_t> eventCounter{0};
  const uint64_t seq = eventCounter.fetch_add(1) + 1;
  
  std::ofstream logFile(R"(c:\Users\danie\Documents\freespace2\.cursor\debug.log)", std::ios::app);
  if (logFile.is_open()) {
    logFile << R"({"id":"log_)" << seq << R"(","timestamp":)" << timestamp 
            << R"(,"location":")" << location << R"(","message":")" << message 
            << R"(","data":{"indexCount":)" << indexCount 
            << R"(,"colorAttachmentCount":)" << colorAttachmentCount
            << R"(,"drawNum":)" << (++g_gbufferDrawCountThisFrame)
            << R"(},"sessionId":"debug-session","runId":"run2","hypothesisId":"A"})" << "\n";
    logFile.close();
  }
}
// #endregion agent log

namespace {
std::unique_ptr<VulkanRenderer> renderer_instance;
VulkanFrame* g_currentFrame = nullptr;  // Injected by setup_frame, used by render functions

enum class DeferredBoundaryState { Idle, InGeometry, AwaitFinish };
DeferredBoundaryState g_deferredBoundaryState = DeferredBoundaryState::Idle;


const char* deferredBoundaryStateName(DeferredBoundaryState state)
{
  switch (state) {
  case DeferredBoundaryState::Idle:
    return "Idle";
  case DeferredBoundaryState::InGeometry:
    return "InGeometry";
  case DeferredBoundaryState::AwaitFinish:
    return "AwaitFinish";
  default:
    return "Unknown";
  }
}

// Helper functions
vk::Viewport createFullScreenViewport()
{
  vk::Viewport viewport{};
  viewport.x = 0.f;
  viewport.y = static_cast<float>(gr_screen.max_h);
  viewport.width = static_cast<float>(gr_screen.max_w);
  viewport.height = -static_cast<float>(gr_screen.max_h);
  viewport.minDepth = 0.f;
  viewport.maxDepth = 1.f;
  return viewport;
}

  vk::Rect2D createClipScissor()
  {
    const auto clip = getClipScissorFromScreen(gr_screen);
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D{clip.x, clip.y};
    scissor.extent = vk::Extent2D{clip.width, clip.height};
    return scissor;
  }

vk::PrimitiveTopology convertPrimitiveType(primitive_type prim_type)
{
  switch (prim_type) {
  case PRIM_TYPE_POINTS:
    return vk::PrimitiveTopology::ePointList;
  case PRIM_TYPE_LINES:
    return vk::PrimitiveTopology::eLineList;
  case PRIM_TYPE_LINESTRIP:
    return vk::PrimitiveTopology::eLineStrip;
  case PRIM_TYPE_TRIS:
    return vk::PrimitiveTopology::eTriangleList;
  case PRIM_TYPE_TRISTRIP:
    return vk::PrimitiveTopology::eTriangleStrip;
  case PRIM_TYPE_TRIFAN:
    return vk::PrimitiveTopology::eTriangleFan;
  default:
    return vk::PrimitiveTopology::eTriangleList;
  }
}

vk::SamplerAddressMode convertTextureAddressing(int addressing)
{
  switch (addressing) {
  case TMAP_ADDRESS_CLAMP:
    return vk::SamplerAddressMode::eClampToEdge;
  case TMAP_ADDRESS_MIRROR:
    return vk::SamplerAddressMode::eMirroredRepeat;
  case TMAP_ADDRESS_WRAP:
  default:
    return vk::SamplerAddressMode::eRepeat;
  }
}

vk::CompareOp convertComparisionFunction(ComparisionFunction compare)
{
  switch (compare) {
  case ComparisionFunction::Always:
    return vk::CompareOp::eAlways;
  case ComparisionFunction::Never:
    return vk::CompareOp::eNever;
  case ComparisionFunction::Less:
    return vk::CompareOp::eLess;
  case ComparisionFunction::LessOrEqual:
    return vk::CompareOp::eLessOrEqual;
  case ComparisionFunction::Greater:
    return vk::CompareOp::eGreater;
  case ComparisionFunction::GreaterOrEqual:
    return vk::CompareOp::eGreaterOrEqual;
  case ComparisionFunction::Equal:
    return vk::CompareOp::eEqual;
  case ComparisionFunction::NotEqual:
    return vk::CompareOp::eNotEqual;
  default:
    return vk::CompareOp::eAlways;
  }
}

vk::StencilOp convertStencilOperation(StencilOperation op)
{
  switch (op) {
  case StencilOperation::Keep:
    return vk::StencilOp::eKeep;
  case StencilOperation::Zero:
    return vk::StencilOp::eZero;
  case StencilOperation::Replace:
    return vk::StencilOp::eReplace;
  case StencilOperation::Increment:
    return vk::StencilOp::eIncrementAndClamp;
  case StencilOperation::Decrement:
    return vk::StencilOp::eDecrementAndClamp;
  case StencilOperation::IncrementWrap:
    return vk::StencilOp::eIncrementAndWrap;
  case StencilOperation::DecrementWrap:
    return vk::StencilOp::eDecrementAndWrap;
  case StencilOperation::Invert:
    return vk::StencilOp::eInvert;
  default:
    return vk::StencilOp::eKeep;
  }
}

uint32_t convertColorWriteMask(const bvec4& mask)
{
  uint32_t out = 0;
  if (mask.x) out |= VK_COLOR_COMPONENT_R_BIT;
  if (mask.y) out |= VK_COLOR_COMPONENT_G_BIT;
  if (mask.z) out |= VK_COLOR_COMPONENT_B_BIT;
  if (mask.w) out |= VK_COLOR_COMPONENT_A_BIT;
  return out;
}

// Stub implementations for unimplemented functions
gr_buffer_handle gr_vulkan_create_buffer(BufferType type, BufferUsageHint usage)
{
  Assertion(renderer_instance != nullptr, "Vulkan renderer must be initialized before createBuffer");
  return renderer_instance->createBuffer(type, usage);
}

// Begin a new frame for rendering and set initial dynamic state.
// Called immediately after flip() via gr_setup_frame() per API contract.
// Injects g_currentFrame for use by render functions.
    void gr_vulkan_setup_frame()
    {
      Assertion(renderer_instance != nullptr, "setup_frame called without renderer");

  // Inject frame into module context - render functions will use g_currentFrame
  g_currentFrame = renderer_instance->getCurrentRecordingFrame();
  Assertion(g_currentFrame != nullptr, "setup_frame called outside frame recording");

  // Reset per-frame uniform bindings (optional will be empty at frame start)
  g_currentFrame->resetPerFrameBindings();

    vk::CommandBuffer cmd = g_currentFrame->commandBuffer();
    Assertion(cmd, "Frame has no valid command buffer");

    g_deferredBoundaryState = DeferredBoundaryState::Idle;

    // DO NOT start the render pass here - allow gr_clear to set clear flags first.
    // The render pass will start lazily when the first draw or clear occurs.

      // Viewport: full-screen with Vulkan Y-flip (y = height, height = -height)
    vk::Viewport viewport = createFullScreenViewport();
    cmd.setViewport(0, 1, &viewport);

    // Scissor: current clip region
    vk::Rect2D scissor = createClipScissor();
    cmd.setScissor(0, 1, &scissor);
  }

void gr_vulkan_delete_buffer(gr_buffer_handle handle)
{
  Assertion(renderer_instance != nullptr, "Vulkan renderer must be initialized before deleteBuffer");
  renderer_instance->deleteBuffer(handle);
}

static void gr_vulkan_bind_uniform_buffer(uniform_block_type type,
  size_t offset,
  size_t size,
  gr_buffer_handle handle)
{
  Assertion(renderer_instance != nullptr, "bind_uniform_buffer called without renderer");
  Assertion(g_currentFrame != nullptr, "bind_uniform_buffer called before setup_frame");

  if (type == uniform_block_type::ModelData) {
    renderer_instance->setModelUniformBinding(*g_currentFrame, handle, offset, size);
  } else if (type == uniform_block_type::NanoVGData) {
    g_currentFrame->nanovgData = { handle, offset, size, true };
  } else if (type == uniform_block_type::Matrices) {
    renderer_instance->setSceneUniformBinding(*g_currentFrame, handle, offset, size);
  } else {
    // Keep running but make it noisy so the offending path gets fixed.
    return;
  }
}

int stub_preload(int /*bitmap_num*/, int /*is_aabitmap*/) { return 0; }
int gr_vulkan_preload(int bitmap_num, int is_aabitmap)
{
  Assertion(renderer_instance != nullptr, "Vulkan renderer must be initialized before preload");
  return renderer_instance->preloadTexture(bitmap_num, is_aabitmap != 0);
}
void stub_resize_buffer(gr_buffer_handle /*handle*/, size_t /*size*/) {}

int stub_save_screen() { return 1; }

int stub_zbuffer_get() { return 0; }

int stub_zbuffer_set(int /*mode*/) { return 0; }

void gr_set_fill_mode_stub(int /*mode*/) {}

void stub_clear() {}

void stub_free_screen(int /*id*/) {}

void stub_get_region(int /*front*/, int /*w*/, int /*h*/, ubyte* /*data*/) {}

void stub_print_screen(const char* /*filename*/) {}

SCP_string stub_blob_screen() { return {}; }

  void gr_vulkan_reset_clip()
  {
  gr_screen.offset_x = gr_screen.offset_x_unscaled = 0;
  gr_screen.offset_y = gr_screen.offset_y_unscaled = 0;

  gr_screen.clip_left = gr_screen.clip_left_unscaled = 0;
  gr_screen.clip_top = gr_screen.clip_top_unscaled = 0;
  gr_screen.clip_right = gr_screen.clip_right_unscaled = gr_screen.max_w - 1;
  gr_screen.clip_bottom = gr_screen.clip_bottom_unscaled = gr_screen.max_h - 1;
  gr_screen.clip_width = gr_screen.clip_width_unscaled = gr_screen.max_w;
  gr_screen.clip_height = gr_screen.clip_height_unscaled = gr_screen.max_h;
  gr_screen.clip_aspect = i2fl(gr_screen.clip_width) / i2fl(gr_screen.clip_height);
  gr_screen.clip_center_x = (gr_screen.clip_left + gr_screen.clip_right) * 0.5f;
  gr_screen.clip_center_y = (gr_screen.clip_top + gr_screen.clip_bottom) * 0.5f;

    if (gr_screen.custom_size) {
      gr_unsize_screen_pos(&gr_screen.max_w_unscaled, &gr_screen.max_h_unscaled);
      gr_unsize_screen_pos(&gr_screen.max_w_unscaled_zoomed, &gr_screen.max_h_unscaled_zoomed);
      gr_unsize_screen_pos(&gr_screen.clip_right_unscaled, &gr_screen.clip_bottom_unscaled);
      gr_unsize_screen_pos(&gr_screen.clip_width_unscaled, &gr_screen.clip_height_unscaled);
    }

    if (g_currentFrame != nullptr) {
      vk::CommandBuffer cmd = g_currentFrame->commandBuffer();
      if (cmd) {
        vk::Rect2D scissor = createClipScissor();
        cmd.setScissor(0, 1, &scissor);
      }
    }
  }

void stub_restore_screen(int /*id*/) {}

void gr_vulkan_update_buffer_data(gr_buffer_handle handle, size_t size, const void* data)
{
  if (renderer_instance) {
    renderer_instance->updateBufferData(handle, size, data);
  }
}

void gr_vulkan_update_buffer_data_offset(gr_buffer_handle handle,
  size_t offset,
  size_t size,
  const void* data)
{
  if (renderer_instance) {
    renderer_instance->updateBufferDataOffset(handle, offset, size, data);
  }
}

void gr_vulkan_resize_buffer(gr_buffer_handle handle, size_t size)
{
  if (renderer_instance) {
    renderer_instance->resizeBuffer(handle, size);
  }
}

void stub_update_transform_buffer(void* /*data*/, size_t /*size*/) {}

  void gr_vulkan_set_clip(int x, int y, int w, int h, int resize_mode)
  {
    applyClipToScreen(x, y, w, h, resize_mode);

    if (g_currentFrame != nullptr) {
      vk::CommandBuffer cmd = g_currentFrame->commandBuffer();
      if (cmd) {
        vk::Rect2D scissor = createClipScissor();
        cmd.setScissor(0, 1, &scissor);
      }
    }
  }

int stub_set_color_buffer(int /*mode*/) { return 0; }

void stub_set_texture_addressing(int /*mode*/) {}

void stub_zbias_stub(int /*bias*/) {}

void stub_zbuffer_clear(int /*mode*/) {}

int stub_stencil_set(int /*mode*/) { return 0; }

void stub_stencil_clear() {}

int stub_alpha_mask_set(int /*mode*/, float /*alpha*/) { return 0; }

void stub_post_process_set_effect(const char* /*name*/, int /*x*/, const vec3d* /*rgb*/) {}

void stub_post_process_set_defaults() {}

void stub_post_process_save_zbuffer() {}

void stub_post_process_begin() {}

void stub_post_process_end() {}

void stub_scene_texture_begin() {}

void stub_scene_texture_end() {}

void stub_copy_effect_texture() {}

void gr_vulkan_deferred_lighting_begin(bool clearNonColorBufs)
{
  // #region agent log
  const bool deferredEnabled = light_deferred_enabled();
  if (!deferredEnabled) {
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
    static std::atomic<uint64_t> eventCounter{0};
    const uint64_t seq = eventCounter.fetch_add(1) + 1;
    std::ofstream logFile(R"(c:\Users\danie\Documents\freespace2\.cursor\debug.log)", std::ios::app);
    if (logFile.is_open()) {
      logFile << R"({"id":"log_def_)" << seq << R"(","timestamp":)" << timestamp 
              << R"(,"location":"VulkanGraphics.cpp:gr_vulkan_deferred_lighting_begin","message":"Deferred disabled","data":{"enabled":false},"sessionId":"debug-session","runId":"run2","hypothesisId":"A"})" << "\n";
      logFile.close();
    }
    return;
  }
  // #endregion agent log
  if (!renderer_instance) {
    return;
  }
  VulkanFrame* frame = renderer_instance->getCurrentRecordingFrame();
  if (!frame) {
    return;
  }
  if (g_deferredBoundaryState != DeferredBoundaryState::Idle) {
    mprintf(("Vulkan deferred API misuse: begin() called while state=%s; resetting to Idle.\n",
      deferredBoundaryStateName(g_deferredBoundaryState)));
    renderer_instance->setPendingRenderTargetSwapchain();
    g_deferredBoundaryState = DeferredBoundaryState::Idle;
  }
  renderer_instance->beginDeferredLighting(frame->commandBuffer(), clearNonColorBufs);
  g_deferredBoundaryState = DeferredBoundaryState::InGeometry;
}

void gr_vulkan_deferred_lighting_msaa()
{
  // MSAA not implemented for deferred yet
}

void gr_vulkan_deferred_lighting_end()
{
  if (!renderer_instance) {
    return;
  }
  VulkanFrame* frame = renderer_instance->getCurrentRecordingFrame();
  if (!frame) {
    return;
  }
  if (g_deferredBoundaryState != DeferredBoundaryState::InGeometry) {
    mprintf(("Vulkan deferred API misuse: end() called while state=%s; ignoring.\n",
      deferredBoundaryStateName(g_deferredBoundaryState)));
    return;
  }
  renderer_instance->endDeferredGeometry(frame->commandBuffer());
  g_deferredBoundaryState = DeferredBoundaryState::AwaitFinish;
}

void gr_vulkan_deferred_lighting_finish()
{
  if (!renderer_instance) {
    return;
  }
  VulkanFrame* frame = renderer_instance->getCurrentRecordingFrame();
  if (!frame) {
    return;
  }
  if (g_deferredBoundaryState == DeferredBoundaryState::InGeometry) {
    mprintf(("Vulkan deferred API misuse: finish() called without end(); auto-ending geometry.\n"));
    renderer_instance->endDeferredGeometry(frame->commandBuffer());
    g_deferredBoundaryState = DeferredBoundaryState::AwaitFinish;
  }
  if (g_deferredBoundaryState != DeferredBoundaryState::AwaitFinish) {
    mprintf(("Vulkan deferred API misuse: finish() called while state=%s; ignoring.\n",
      deferredBoundaryStateName(g_deferredBoundaryState)));
    return;
  }

  // Bind G-buffer textures to global descriptor set
  renderer_instance->bindDeferredGlobalDescriptors();

    // Record deferred lighting draws
    renderer_instance->recordDeferredLighting(*frame);

    // Restore scissor to current clip state for subsequent draws
    {
      vk::CommandBuffer cmd = frame->commandBuffer();
      if (cmd) {
        vk::Rect2D scissor = createClipScissor();
        cmd.setScissor(0, 1, &scissor);
      }
    }

        // Switch back to swapchain rendering mode for subsequent draws
        renderer_instance->setPendingRenderTargetSwapchain();
    g_deferredBoundaryState = DeferredBoundaryState::Idle;
  }

void stub_set_line_width(float width)
{
  static float s_requestedWidth = 1.0f;

  // Sanitize input
  if (!(width > 0.0f)) {
    width = 1.0f;
  }
  s_requestedWidth = width;

  if (!renderer_instance) {
    return;
  }

  // Clamp to device limits (prevents validation errors on devices without wideLines)
  const auto& limits = renderer_instance->vulkanDevice()->properties().limits;
  float minWidth = limits.lineWidthRange[0];
  float maxWidth = limits.lineWidthRange[1];

  if (minWidth > maxWidth) {
    const float tmp = minWidth;
    minWidth = maxWidth;
    maxWidth = tmp;
  }

  float clamped = s_requestedWidth;
  if (clamped < minWidth) {
    clamped = minWidth;
  } else if (clamped > maxWidth) {
    clamped = maxWidth;
  }

  // Snap to reported granularity (if any)
  const float granularity = limits.lineWidthGranularity;
  if (granularity > 0.0f) {
    const float steps = clamped / granularity;
    const int roundedSteps = static_cast<int>(steps + 0.5f);
    clamped = static_cast<float>(roundedSteps) * granularity;

    if (clamped < minWidth) {
      clamped = minWidth;
    } else if (clamped > maxWidth) {
      clamped = maxWidth;
    }
  }

  // Apply to current command buffer if recording
  VulkanFrame* frame = g_currentFrame;
  if (!frame) {
    frame = renderer_instance->getCurrentRecordingFrame();
  }
  if (!frame) {
    return;
  }

  vk::CommandBuffer cmd = frame->commandBuffer();
  if (cmd) {
    cmd.setLineWidth(clamped);
  }
}

void stub_draw_sphere(material* /*material_def*/, float /*rad*/) {}

void stub_clear_states() {}

void stub_update_texture(int /*bitmap_handle*/, int /*bpp*/, const ubyte* /*data*/, int /*width*/, int /*height*/) {}

void stub_get_bitmap_from_texture(void* /*data_out*/, int /*bitmap_num*/) {}

int stub_bm_make_render_target(int /*n*/, int* /*width*/, int* /*height*/, int* /*bpp*/, int* /*mm_lvl*/, int /*flags*/)
{
  return 0;
}

int stub_bm_set_render_target(int /*n*/, int /*face*/) { return 0; }

void stub_bm_create(bitmap_slot* /*slot*/) {}

void stub_bm_free_data(bitmap_slot* /*slot*/, bool /*release*/) {}

void stub_bm_init(bitmap_slot* /*slot*/) {}

void stub_bm_page_in_start() {}

bool stub_bm_data(int /*n*/, bitmap* /*bm*/) { return true; }

int stub_maybe_create_shader(shader_type /*shader_t*/, unsigned int /*flags*/) { return -1; }

void stub_shadow_map_start(matrix4* /*shadow_view_matrix*/, const matrix* /*light_matrix*/, vec3d* /*eye_pos*/) {}

void stub_shadow_map_end() {}

void stub_start_decal_pass() {}
void stub_stop_decal_pass() {}
void stub_render_decals(decal_material* /*material_info*/,
             primitive_type /*prim_type*/,
             vertex_layout* /*layout*/,
             int /*num_elements*/,
             const indexed_vertex_source& /*buffers*/,
             const gr_buffer_handle& /*instance_buffer*/,
             int /*num_instances*/) {}

void stub_render_shield_impact(shield_material* /*material_info*/,
  primitive_type /*prim_type*/,
  vertex_layout* /*layout*/,
  gr_buffer_handle /*buffer_handle*/,
  int /*n_verts*/)
{
}

struct ModelDrawContext {
  VulkanFrame& frame;
  vk::CommandBuffer cmd;
  vk::Pipeline pipeline;
  vk::PipelineLayout pipelineLayout;
  vk::DescriptorSet modelSet;
  uint32_t modelDynamicOffset;
  ModelPushConstants pcs;
  const indexed_vertex_source& vertSource;
  const vertex_buffer& vbuffer;
  size_t texi;
};

static void issueModelDraw(const ModelDrawContext& ctx)
{
  ctx.cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, ctx.pipeline);

  // Set model descriptor set + dynamic UBO offset
  ctx.cmd.bindDescriptorSets(
    vk::PipelineBindPoint::eGraphics,
    ctx.pipelineLayout,
    0,
    1,
    &ctx.modelSet,
    1,
    &ctx.modelDynamicOffset);

  // Push constants (vertex layout + texture indices)
  ctx.cmd.pushConstants(
    ctx.pipelineLayout,
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
    0,
    sizeof(ModelPushConstants),
    &ctx.pcs);

  // Per-batch index data
  const buffer_data& batch = ctx.vbuffer.tex_buf[ctx.texi];

  vk::Buffer indexBuffer = renderer_instance->getBuffer(ctx.vertSource.Ibuffer_handle);
  Assertion(indexBuffer, "Invalid index buffer handle %d", ctx.vertSource.Ibuffer_handle.value());

  // Select index type based on VB_FLAG_LARGE_INDEX
  const bool use32BitIndices = (batch.flags & VB_FLAG_LARGE_INDEX) != 0;
  const vk::IndexType indexType = use32BitIndices ? vk::IndexType::eUint32
                                                  : vk::IndexType::eUint16;

  // Index data is laid out at:
  //   vertSource.Index_offset (heap base) + batch.index_offset (per-batch byte offset)
  const vk::DeviceSize indexOffsetBytes =
    static_cast<vk::DeviceSize>(ctx.vertSource.Index_offset + batch.index_offset);

  ctx.cmd.bindIndexBuffer(indexBuffer, indexOffsetBytes, indexType);

  const uint32_t indexCount = static_cast<uint32_t>(batch.n_verts);

  ctx.cmd.drawIndexed(
    indexCount,
    1,  // instanceCount
    0,  // firstIndex (we already baked the byte offset above)
    0,  // vertexOffset (vertex pulling handles the base)
    0   // firstInstance
  );
  
  // #region agent log
  // Note: We log G-buffer draws from gr_vulkan_render_model where we have access to colorAttachmentCount
  // #endregion agent log
}

void gr_vulkan_render_model(model_material* material_info,
  indexed_vertex_source* vert_source,
  vertex_buffer* bufferp,
  size_t texi)
{
  if (renderer_instance) {
    renderer_instance->incrementModelDraw();
  }

  // Preconditions - frame injected by setup_frame, parameters must be valid
  Assertion(g_currentFrame != nullptr, "render_model called before setup_frame");
  Assertion(renderer_instance != nullptr, "render_model called without renderer");
  Assertion(material_info != nullptr, "render_model called with null material_info");
  Assertion(vert_source != nullptr, "render_model called with null vert_source");
  Assertion(bufferp != nullptr, "render_model called with null bufferp");
  Assertion(texi < bufferp->tex_buf.size(), "render_model called with invalid texi %zu (size=%zu)",
            texi, bufferp->tex_buf.size());

  VulkanFrame& frame = *g_currentFrame;
  vk::CommandBuffer cmd = frame.commandBuffer();
  Assertion(cmd, "render_model called with null command buffer");

  // Start rendering FIRST and get the actual target contract
  const auto& rt = renderer_instance->ensureRenderingStarted(cmd);

  // Get shader modules for model shader
  ShaderModules modules = renderer_instance->getShaderModules(SDR_TYPE_MODEL);
  Assertion(modules.vert != nullptr, "Model vertex shader not loaded");
  Assertion(modules.frag != nullptr, "Model fragment shader not loaded");

  // Build pipeline key from active render target contract
  PipelineKey key{};
  key.type                   = SDR_TYPE_MODEL;
  key.variant_flags          = material_info->get_shader_flags();
  key.color_format           = static_cast<VkFormat>(rt.colorFormat);
  key.depth_format           = static_cast<VkFormat>(rt.depthFormat);
  key.sample_count           = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
  key.color_attachment_count = rt.colorAttachmentCount;
  key.blend_mode             = material_info->get_blend_mode();
  // Model shaders ignore layout_hash (vertex pulling)



  // Get or create pipeline (pass empty layout for vertex pulling)
  vertex_layout emptyLayout;
  vk::Pipeline pipeline = renderer_instance->getPipeline(key, modules, emptyLayout);
  Assertion(pipeline, "Pipeline creation failed for model shader (variant_flags=0x%x)", key.variant_flags);

  vk::PipelineLayout layout = renderer_instance->getModelPipelineLayout();

  // Get buffers
  vk::Buffer vertexBuffer = renderer_instance->getBuffer(vert_source->Vbuffer_handle);
  Assertion(vertexBuffer, "Invalid vertex buffer handle %d", vert_source->Vbuffer_handle.value());

  // Build push constants - vertex layout offsets
  ModelPushConstants pcs{};

  // Base byte offset in the vertex heap for THIS vertex_buffer
  {
    const vk::DeviceSize heapBase   = static_cast<vk::DeviceSize>(vert_source->Vertex_offset);
    const vk::DeviceSize vbOffset   = static_cast<vk::DeviceSize>(bufferp->vertex_offset);
    const vk::DeviceSize byteOffset = heapBase + vbOffset;

    Assertion(byteOffset <= std::numeric_limits<uint32_t>::max(),
              "Model vertex heap offset exceeds uint32 range");
    pcs.vertexOffset = static_cast<uint32_t>(byteOffset);
  }

  pcs.stride            = static_cast<uint32_t>(bufferp->stride);
  pcs.posOffset         = MODEL_OFFSET_ABSENT;
  pcs.normalOffset      = MODEL_OFFSET_ABSENT;
  pcs.texCoordOffset    = MODEL_OFFSET_ABSENT;
  pcs.tangentOffset     = MODEL_OFFSET_ABSENT;
  pcs.boneIndicesOffset = MODEL_OFFSET_ABSENT;
  pcs.boneWeightsOffset = MODEL_OFFSET_ABSENT;

  // Extract offsets from vertex layout
  for (size_t i = 0; i < bufferp->layout.get_num_vertex_components(); ++i) {
    const auto* comp = bufferp->layout.get_vertex_component(i);
    switch (comp->format_type) {
    case vertex_format_data::POSITION3:
      pcs.posOffset = static_cast<uint32_t>(comp->offset);
      break;
    case vertex_format_data::NORMAL:
      pcs.normalOffset = static_cast<uint32_t>(comp->offset);
      break;
    case vertex_format_data::TEX_COORD2:
      pcs.texCoordOffset = static_cast<uint32_t>(comp->offset);
      break;
    case vertex_format_data::TANGENT:
      pcs.tangentOffset = static_cast<uint32_t>(comp->offset);
      break;
    default:
      break;
    }
  }

  // Build push constants - texture indices
  auto toIndex = [](int h) -> uint32_t {
    if (h < 0 || renderer_instance == nullptr) {
      return MODEL_OFFSET_ABSENT;
    }

    auto* textureManager = renderer_instance->textureManager();
    if (!textureManager) {
      return MODEL_OFFSET_ABSENT;
    }

    const int baseFrame = bm_get_base_frame(h, nullptr);
    if (baseFrame < 0) {
      return MODEL_OFFSET_ABSENT;
    }

    return textureManager->getBindlessSlotIndex(baseFrame);
  };

  int baseTex   = material_info->get_texture_map(TM_BASE_TYPE);
  int glowTex   = material_info->get_texture_map(TM_GLOW_TYPE);
  int normalTex = material_info->get_texture_map(TM_NORMAL_TYPE);
  int specTex   = material_info->get_texture_map(TM_SPECULAR_TYPE);

  pcs.baseMapIndex   = toIndex(baseTex);
  pcs.glowMapIndex   = toIndex(glowTex);
  pcs.normalMapIndex = toIndex(normalTex);
  pcs.specMapIndex   = toIndex(specTex);
  pcs.matrixIndex    = 0;  // Unused
  pcs.flags          = material_info->get_shader_flags();

  // Use the per-frame model descriptor set (allocated and synced at frame start)
  Assertion(frame.modelDescriptorSet, "Model descriptor set must be allocated at frame start");
  vk::DescriptorSet modelSet = frame.modelDescriptorSet;

  // Dynamic state: compensate for viewport Y-flip (CCW becomes CW)
  cmd.setFrontFace(vk::FrontFace::eClockwise);

  // Cull mode from material
  cmd.setCullMode(material_info->get_cull_mode() ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone);

  // Depth state from material
  gr_zbuffer_type zMode = material_info->get_depth_mode();
  bool depthWrite = (zMode == ZBUFFER_TYPE_WRITE || zMode == ZBUFFER_TYPE_FULL);
  bool depthTest = (zMode == ZBUFFER_TYPE_READ || zMode == ZBUFFER_TYPE_FULL);
  cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
  cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eLessOrEqual);

  // Enforce contract: ModelData must be bound via gr_bind_uniform_buffer
  Assertion(frame.modelUniformBinding.has_value(), "ModelData UBO binding not set; call gr_bind_uniform_buffer first");
  uint32_t dynOffset = frame.modelUniformBinding->dynamicOffset;

  ModelDrawContext ctx{
    frame,
    cmd,
    pipeline,
    layout,
    modelSet,
    dynOffset,
    pcs,
    *vert_source,
    *bufferp,
    texi,
  };

  // #region agent log
  // Log G-buffer draws (colorAttachmentCount == 5 indicates G-buffer)
  if (rt.colorAttachmentCount == 5) {
    const buffer_data& batch = bufferp->tex_buf[texi];
    const uint32_t indexCount = static_cast<uint32_t>(batch.n_verts);
    logDebugEventDraw("GBUFFER_DRAW", "VulkanGraphics.cpp:gr_vulkan_render_model", "G-buffer draw", indexCount, rt.colorAttachmentCount);
  }
  // #endregion agent log

  issueModelDraw(ctx);
}

void gr_vulkan_render_primitives(material* material_info,
  primitive_type prim_type,
  vertex_layout* layout,
  int offset,
  int n_verts,
  gr_buffer_handle buffer_handle,
  size_t buffer_offset)
{
  if (renderer_instance) {
    renderer_instance->incrementPrimDraw();
  }

  // Preconditions - frame injected by setup_frame, parameters must be valid
  Assertion(g_currentFrame != nullptr, "render_primitives called before setup_frame");
  Assertion(renderer_instance != nullptr, "render_primitives called without renderer");
  Assertion(material_info != nullptr, "render_primitives called with null material_info");
  Assertion(layout != nullptr, "render_primitives called with null vertex layout");
  Assertion(n_verts > 0, "render_primitives called with zero vertices");

  VulkanFrame& frame = *g_currentFrame;
  vk::CommandBuffer cmd = frame.commandBuffer();
  Assertion(cmd, "render_primitives called with null command buffer");

  // Start rendering FIRST and get the actual target contract
  const auto& rt = renderer_instance->ensureRenderingStarted(cmd);

  // Use the shader type requested by the material
  shader_type shaderType = material_info->get_shader_type();

  // Instrumentation: detect shader/layout mismatches that will cause validation warnings
  // DEFAULT_MATERIAL shader expects vertex color at location 1
  if (shaderType == SDR_TYPE_DEFAULT_MATERIAL) {
    bool hasColor = false;
    for (size_t i = 0; i < layout->get_num_vertex_components(); ++i) {
      auto fmt = layout->get_vertex_component(i)->format_type;
      if (fmt == vertex_format_data::COLOR3 ||
          fmt == vertex_format_data::COLOR4 ||
          fmt == vertex_format_data::COLOR4F) {
        hasColor = true;
        break;
      }
    }
    if (!hasColor) {
      // Log everything BEFORE warning dialog (Warning is modal)
      mprintf(("SDR_TYPE_DEFAULT_MATERIAL used without vertex color!\n"));
      mprintf(("  n_verts=%d, prim_type=%d, buffer_handle=%d\n",
              n_verts, static_cast<int>(prim_type), buffer_handle.value()));
      mprintf(("  layout components (%zu):\n", layout->get_num_vertex_components()));
      for (size_t i = 0; i < layout->get_num_vertex_components(); ++i) {
        auto* comp = layout->get_vertex_component(i);
        mprintf(("    [%zu] format=%d stride=%d offset=%d\n",
                i, static_cast<int>(comp->format_type), comp->stride, comp->offset));
      }
      // Dump stack to find caller
      mprintf(("Stack trace:\n"));
      dump_stacktrace();
      // Now show warning dialog
      Warning(LOCATION, "SDR_TYPE_DEFAULT_MATERIAL used without vertex color! Check log for details.");
    }
  }

  // Get shader modules
  ShaderModules shaderModules = renderer_instance->getShaderModules(shaderType);
  Assertion(shaderModules.vert != nullptr, "render_primitives missing vertex shader for shaderType=%d", static_cast<int>(shaderType));
  Assertion(shaderModules.frag != nullptr, "render_primitives missing fragment shader for shaderType=%d", static_cast<int>(shaderType));

  // Build pipeline key from active render target contract
  PipelineKey pipelineKey{};
  pipelineKey.type = shaderType;
  pipelineKey.variant_flags = material_info->get_shader_flags();
  pipelineKey.color_format = static_cast<VkFormat>(rt.colorFormat);
  pipelineKey.depth_format = static_cast<VkFormat>(rt.depthFormat);
  pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
  pipelineKey.color_attachment_count = rt.colorAttachmentCount;
  pipelineKey.blend_mode = material_info->get_blend_mode();
  pipelineKey.layout_hash = layout->hash();

  // Get or create pipeline (passes vertex_layout for vertex input state)
  vk::Pipeline pipeline = renderer_instance->getPipeline(pipelineKey, shaderModules, *layout);
  Assertion(pipeline, "render_primitives pipeline creation failed (shaderType=%d, layout_hash=0x%x)",
    static_cast<int>(shaderType), static_cast<unsigned int>(pipelineKey.layout_hash));

  // Get vertex buffer
  Assertion(buffer_handle.isValid(),
    "render_primitives called with invalid buffer handle (shaderType=%d, material=%p)",
    static_cast<int>(material_info->get_shader_type()), static_cast<const void*>(material_info));

  vk::Buffer vertexBuffer = renderer_instance->getBuffer(buffer_handle);
  Assertion(vertexBuffer, "render_primitives got null buffer for handle %d (shaderType=%d, material=%p)",
    buffer_handle.value(), static_cast<int>(material_info->get_shader_type()),
    static_cast<const void*>(material_info));
  
  // Get matrices from global state
  matrix4 modelViewMatrix = gr_model_view_matrix;
  matrix4 projMatrix = gr_projection_matrix;
  matrix4 modelMatrix = gr_model_matrix_stack.get_transform();

  // 1. Prepare Matrix Data (Common to all types at Binding 0)
  matrixData_default_material_vert matrices{};
  matrices.modelViewMatrix = modelViewMatrix;
  matrices.projMatrix = projMatrix;

  // 2. Prepare Generic Data (Handle Layout Mismatch at Binding 1)
  const void* genericDataPtr = nullptr;
  size_t genericDataSize = 0;

  // Declare instances for both potential layouts
  genericData_interface_frag interfaceData{};
  genericData_default_material_vert defaultData{};

  // Extract common material properties
  vec4 clr = material_info->get_color();
  int textureHandle = material_info->is_textured() ? material_info->get_texture_map(TM_BASE_TYPE) : -1;

  int baseMapIndex = (textureHandle >= 0) ? bm_get_array_index(textureHandle) : 0;
  int alphaTexture = (material_info->get_texture_type() == material::TEX_TYPE_AABITMAP) ? 1 : 0;
  int noTexturing = material_info->is_textured() ? 0 : 1;
  float intensity = material_info->get_color_scale();

  if (shaderType == SDR_TYPE_INTERFACE) {
    // Interface shader: 40-byte layout with color at offset 0
    interfaceData.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};
    interfaceData.baseMapIndex = baseMapIndex;
    interfaceData.alphaTexture = alphaTexture;
    interfaceData.noTexturing = noTexturing;
    interfaceData.srgb = 1;
    interfaceData.intensity = intensity;
    interfaceData.alphaThreshold = 0.f;

    // DEBUG: Log first few interface draws
    static int iface_debug_count = 0;
    if (iface_debug_count < 5) {
      mprintf(("Interface #%d: color=(%.2f,%.2f,%.2f,%.2f) intensity=%.2f tex=%d noTex=%d\n",
               iface_debug_count, clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w,
               intensity, textureHandle, noTexturing));
      ++iface_debug_count;
    }

    genericDataPtr = &interfaceData;
    genericDataSize = sizeof(genericData_interface_frag);
  } else {
    // Default material shader: 124-byte layout with modelMatrix at offset 0
    defaultData.modelMatrix = modelMatrix;
    defaultData.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};

    if (material_info->is_clipped()) {
      const auto& clip = material_info->get_clip_plane();
      defaultData.clipEquation = {clip.normal.xyz.x, clip.normal.xyz.y, clip.normal.xyz.z,
                                  -vm_vec_dot(&clip.normal, &clip.position)};
      defaultData.clipEnabled = 1;
    } else {
      defaultData.clipEquation = {0.f, 0.f, 0.f, 0.f};
      defaultData.clipEnabled = 0;
    }

    defaultData.baseMapIndex = baseMapIndex;
    defaultData.alphaTexture = alphaTexture;
    defaultData.noTexturing = noTexturing;
    defaultData.srgb = 1;
    defaultData.intensity = intensity;
    defaultData.alphaThreshold = 0.f;

    genericDataPtr = &defaultData;
    genericDataSize = sizeof(genericData_default_material_vert);
  }

  // 3. Allocate from uniform ring buffer using dynamic size
  const size_t uboAlignmentRaw = renderer_instance->getMinUniformOffsetAlignment();
  const size_t uboAlignment = uboAlignmentRaw == 0 ? 1 : uboAlignmentRaw;
  const size_t matrixSize = sizeof(matrices);
  const size_t genericOffset = ((matrixSize + uboAlignment - 1) / uboAlignment) * uboAlignment;
  const size_t totalUniformSize = genericOffset + genericDataSize;

  auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, uboAlignment);
  auto* uniformBase = static_cast<char*>(uniformAlloc.mapped);
  std::memcpy(uniformBase, &matrices, sizeof(matrices));
  std::memcpy(uniformBase + genericOffset, genericDataPtr, genericDataSize);

  // Build push descriptor writes
  vk::DescriptorBufferInfo matrixInfo{};
  matrixInfo.buffer = frame.uniformBuffer().buffer();
  matrixInfo.offset = uniformAlloc.offset;
  matrixInfo.range = sizeof(matrices);

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset + genericOffset;
  genericInfo.range = genericDataSize;

  vk::DescriptorImageInfo baseMapInfo{};
  const bool isTextured = textureHandle >= 0;
  if (isTextured) {
    auto samplerKey = VulkanTextureManager::SamplerKey{};
    samplerKey.address = convertTextureAddressing(material_info->get_texture_addressing());

    baseMapInfo = renderer_instance->getTextureDescriptor(
      textureHandle, frame, cmd, samplerKey);
  }

  std::array<vk::WriteDescriptorSet, 3> writes{};
  uint32_t writeCount = 2; // Always have matrix and generic uniform buffers

  writes[0].dstBinding = 0;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &matrixInfo;

  writes[1].dstBinding = 1;
  writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[1].descriptorCount = 1;
  writes[1].pBufferInfo = &genericInfo;

  if (isTextured) {
    writes[2].dstBinding = 2;
    writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[2].descriptorCount = 1;
    writes[2].pImageInfo = &baseMapInfo;
    writeCount = 3;
  }

  // Bind pipeline
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Push descriptors
  cmd.pushDescriptorSetKHR(
    vk::PipelineBindPoint::eGraphics,
    renderer_instance->getPipelineLayout(),
    0, // set 0
    writeCount,
    writes.data());

  // Bind vertex buffer
  vk::DeviceSize vbOffset = buffer_offset;
  cmd.bindVertexBuffers(0, 1, &vertexBuffer, &vbOffset);

  // Set dynamic state
  // Primitive topology
  cmd.setPrimitiveTopology(convertPrimitiveType(prim_type));

  // Cull mode
  cmd.setCullMode(material_info->get_cull_mode() ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);  // CW compensates for negative viewport height Y-flip

  // Depth state
  gr_zbuffer_type zbufferMode = material_info->get_depth_mode();
  bool depthTest = (zbufferMode == gr_zbuffer_type::ZBUFFER_TYPE_READ || zbufferMode == gr_zbuffer_type::ZBUFFER_TYPE_FULL);
  bool depthWrite = (zbufferMode == gr_zbuffer_type::ZBUFFER_TYPE_WRITE || zbufferMode == gr_zbuffer_type::ZBUFFER_TYPE_FULL);
  cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
  cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
  cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);
  if (renderer_instance->supportsExtendedDynamicState3()) {
    const auto& caps = renderer_instance->getExtendedDynamicState3Caps();
    if (caps.colorBlendEnable) {
      // H7 fix: respect material blend mode instead of unconditionally disabling
      vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE;
      cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
    }
    if (caps.colorWriteMask) {
      vk::ColorComponentFlags mask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
      cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(1, &mask));
    }
    if (caps.polygonMode) {
      cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    }
    if (caps.rasterizationSamples) {
      cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    }
  }

  // Viewport and scissor (should be set per-frame, but ensure they're set)
  // Vulkan Y-flip: set y=height and height=-height to match OpenGL coordinate system
    vk::Viewport viewport = createFullScreenViewport();
    cmd.setViewport(0, 1, &viewport);

    vk::Rect2D scissor = createClipScissor();
    cmd.setScissor(0, 1, &scissor);

  // Draw
  cmd.draw(static_cast<uint32_t>(n_verts), 1, static_cast<uint32_t>(offset), 0);
}

void stub_render_primitives(material* material_info,
  primitive_type prim_type,
  vertex_layout* layout,
  int offset,
  int n_verts,
  gr_buffer_handle buffer_handle,
  size_t buffer_offset)
{
  gr_vulkan_render_primitives(material_info, prim_type, layout, offset, n_verts, buffer_handle, buffer_offset);
}

void stub_render_primitives_particle(particle_material* /*material_info*/,
  primitive_type /*prim_type*/,
  vertex_layout* /*layout*/,
  int /*offset*/,
  int /*n_verts*/,
  gr_buffer_handle /*buffer_handle*/)
{
}

void stub_render_primitives_distortion(distortion_material* /*material_info*/,
  primitive_type /*prim_type*/,
  vertex_layout* /*layout*/,
  int /*offset*/,
  int /*n_verts*/,
  gr_buffer_handle /*buffer_handle*/)
{
}
void stub_render_movie(movie_material* /*material_info*/,
  primitive_type /*prim_type*/,
  vertex_layout* /*layout*/,
  int /*n_verts*/,
  gr_buffer_handle /*buffer*/,
  size_t /*buffer_offset*/)
{
}

void stub_render_nanovg(nanovg_material* /*material_info*/,
  primitive_type /*prim_type*/,
  vertex_layout* /*layout*/,
  int /*offset*/,
  int /*n_verts*/,
  gr_buffer_handle /*buffer_handle*/)
{
}

void gr_vulkan_render_nanovg(nanovg_material* material_info,
  primitive_type prim_type,
  vertex_layout* layout,
  int offset,
  int n_verts,
  gr_buffer_handle buffer_handle)
{
  if (renderer_instance) {
    renderer_instance->incrementPrimDraw();
  }

  Assertion(g_currentFrame != nullptr, "render_nanovg called before setup_frame");
  Assertion(renderer_instance != nullptr, "render_nanovg called without renderer");
  Assertion(material_info != nullptr, "render_nanovg called with null material_info");
  Assertion(layout != nullptr, "render_nanovg called with null vertex layout");
  Assertion(n_verts > 0, "render_nanovg called with zero vertices");
  Assertion(buffer_handle.isValid(), "render_nanovg called with invalid vertex buffer handle");

  VulkanFrame& frame = *g_currentFrame;
  Assertion(frame.nanovgData.valid, "render_nanovg called without NanoVGData uniform buffer bound");
  Assertion(frame.nanovgData.handle.isValid(), "render_nanovg called with invalid NanoVGData buffer handle");

  vk::CommandBuffer cmd = frame.commandBuffer();
  Assertion(cmd, "render_nanovg called with null command buffer");

  // NanoVG requires stencil. If we're currently rendering to a swapchain-without-depth target
  // or a non-swapchain target (deferred G-buffer), switch back to the swapchain target with depth/stencil.
  auto rt = renderer_instance->ensureRenderingStarted(cmd);
  const auto swapchainFormat = static_cast<vk::Format>(renderer_instance->getSwapChainImageFormat());
  if (rt.depthFormat == vk::Format::eUndefined || rt.colorAttachmentCount != 1 || rt.colorFormat != swapchainFormat) {
    renderer_instance->setPendingRenderTargetSwapchain();
    rt = renderer_instance->ensureRenderingStarted(cmd);
  }

  Assertion(rt.depthFormat != vk::Format::eUndefined, "render_nanovg requires a depth/stencil attachment");
  Assertion(renderer_instance->renderTargets() != nullptr, "render_nanovg requires render targets");
  Assertion(renderer_instance->renderTargets()->depthHasStencil(), "render_nanovg requires a stencil-capable depth format");

  ShaderModules shaderModules = renderer_instance->getShaderModules(SDR_TYPE_NANOVG);
  Assertion(shaderModules.vert != nullptr, "NanoVG vertex shader not loaded");
  Assertion(shaderModules.frag != nullptr, "NanoVG fragment shader not loaded");

  PipelineKey pipelineKey{};
  pipelineKey.type = SDR_TYPE_NANOVG;
  pipelineKey.variant_flags = 0;
  pipelineKey.color_format = static_cast<VkFormat>(rt.colorFormat);
  pipelineKey.depth_format = static_cast<VkFormat>(rt.depthFormat);
  pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
  pipelineKey.color_attachment_count = rt.colorAttachmentCount;
  pipelineKey.blend_mode = material_info->get_blend_mode();
  pipelineKey.layout_hash = layout->hash();
  pipelineKey.color_write_mask = convertColorWriteMask(material_info->get_color_mask());

  pipelineKey.stencil_test_enable = material_info->is_stencil_enabled();
  const auto& stencilFunc = material_info->get_stencil_func();
  pipelineKey.stencil_compare_op = static_cast<VkCompareOp>(convertComparisionFunction(stencilFunc.compare));
  pipelineKey.stencil_compare_mask = stencilFunc.mask;
  pipelineKey.stencil_reference = static_cast<uint32_t>(stencilFunc.ref);
  pipelineKey.stencil_write_mask = material_info->get_stencil_mask();

  const auto& frontStencilOp = material_info->get_front_stencil_op();
  pipelineKey.front_fail_op = static_cast<VkStencilOp>(convertStencilOperation(frontStencilOp.stencilFailOperation));
  pipelineKey.front_depth_fail_op = static_cast<VkStencilOp>(convertStencilOperation(frontStencilOp.depthFailOperation));
  pipelineKey.front_pass_op = static_cast<VkStencilOp>(convertStencilOperation(frontStencilOp.successOperation));

  const auto& backStencilOp = material_info->get_back_stencil_op();
  pipelineKey.back_fail_op = static_cast<VkStencilOp>(convertStencilOperation(backStencilOp.stencilFailOperation));
  pipelineKey.back_depth_fail_op = static_cast<VkStencilOp>(convertStencilOperation(backStencilOp.depthFailOperation));
  pipelineKey.back_pass_op = static_cast<VkStencilOp>(convertStencilOperation(backStencilOp.successOperation));

  vk::Pipeline pipeline = renderer_instance->getPipeline(pipelineKey, shaderModules, *layout);
  Assertion(pipeline, "Pipeline creation failed for NanoVG shader");

  vk::Buffer vertexBuffer = renderer_instance->getBuffer(buffer_handle);
  Assertion(vertexBuffer, "Failed to resolve Vulkan vertex buffer for NanoVG handle %d", buffer_handle.value());

  vk::Buffer uniformBuffer = renderer_instance->getBuffer(frame.nanovgData.handle);
  Assertion(uniformBuffer, "Failed to resolve Vulkan uniform buffer for NanoVGData handle %d", frame.nanovgData.handle.value());

  vk::DescriptorBufferInfo nanovgInfo{};
  nanovgInfo.buffer = uniformBuffer;
  nanovgInfo.offset = frame.nanovgData.offset;
  nanovgInfo.range = frame.nanovgData.size;

  auto samplerKey = VulkanTextureManager::SamplerKey{};
  samplerKey.address = convertTextureAddressing(material_info->get_texture_addressing());
  vk::DescriptorImageInfo textureInfo =
    renderer_instance->getTextureDescriptor(material_info->get_texture_map(TM_BASE_TYPE), frame, cmd, samplerKey);

  std::array<vk::WriteDescriptorSet, 2> writes{};
  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &nanovgInfo;

  writes[1].dstBinding = 2;
  writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[1].descriptorCount = 1;
  writes[1].pImageInfo = &textureInfo;

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, renderer_instance->getPipelineLayout(), 0, 2, writes.data());

  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &vertexBuffer, &vbOffset);

  cmd.setPrimitiveTopology(convertPrimitiveType(prim_type));
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(material_info->is_stencil_enabled() ? VK_TRUE : VK_FALSE);

  if (renderer_instance->supportsExtendedDynamicState3()) {
    const auto& caps = renderer_instance->getExtendedDynamicState3Caps();
    if (caps.colorBlendEnable) {
      vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE;
      cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
    }
    if (caps.colorWriteMask) {
      auto mask = static_cast<vk::ColorComponentFlags>(pipelineKey.color_write_mask);
      cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(1, &mask));
    }
  }

  vk::Viewport viewport = createFullScreenViewport();
  cmd.setViewport(0, 1, &viewport);
  vk::Rect2D scissor = createClipScissor();
  cmd.setScissor(0, 1, &scissor);

  cmd.draw(static_cast<uint32_t>(n_verts), 1, static_cast<uint32_t>(offset), 0);
}

void gr_vulkan_render_primitives_batched(batched_bitmap_material* material_info,
  primitive_type prim_type,
  vertex_layout* layout,
  int offset,
  int n_verts,
  gr_buffer_handle buffer_handle)
{
  if (renderer_instance) {
    renderer_instance->incrementPrimDraw();
  }

  // Preconditions
  Assertion(g_currentFrame != nullptr, "render_primitives_batched called before setup_frame");
  Assertion(renderer_instance != nullptr, "render_primitives_batched called without renderer");
  Assertion(material_info != nullptr, "render_primitives_batched called with null material_info");
  Assertion(layout != nullptr, "render_primitives_batched called with null vertex layout");
  Assertion(n_verts > 0, "render_primitives_batched called with zero vertices");

  VulkanFrame& frame = *g_currentFrame;
  vk::CommandBuffer cmd = frame.commandBuffer();
  Assertion(cmd, "render_primitives_batched called with null command buffer");

  // Start rendering FIRST and get the actual target contract
  const auto& rt = renderer_instance->ensureRenderingStarted(cmd);

  // Force batched bitmap shader
  shader_type shaderType = SDR_TYPE_BATCHED_BITMAP;

  // Get shader modules
  ShaderModules shaderModules = renderer_instance->getShaderModules(shaderType);
  Assertion(shaderModules.vert != nullptr, "Batched bitmap vertex shader not loaded");
  Assertion(shaderModules.frag != nullptr, "Batched bitmap fragment shader not loaded");

  // Build pipeline key from active render target contract
  PipelineKey pipelineKey{};
  pipelineKey.type = shaderType;
  pipelineKey.variant_flags = material_info->get_shader_flags();
  pipelineKey.color_format = static_cast<VkFormat>(rt.colorFormat);
  pipelineKey.depth_format = static_cast<VkFormat>(rt.depthFormat);
  pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
  pipelineKey.color_attachment_count = rt.colorAttachmentCount;
  pipelineKey.blend_mode = material_info->get_blend_mode();
  pipelineKey.layout_hash = layout->hash();

  // Get or create pipeline
  vk::Pipeline pipeline = renderer_instance->getPipeline(pipelineKey, shaderModules, *layout);
  Assertion(pipeline, "Pipeline creation failed for batched bitmap shader");

  // Get vertex buffer
  Assertion(buffer_handle.isValid(), "render_primitives_batched called with invalid buffer handle");
  vk::Buffer vertexBuffer = renderer_instance->getBuffer(buffer_handle);
  Assertion(vertexBuffer, "Failed to get buffer for handle %d", buffer_handle.value());

  // Get matrices from global state (uses simpler struct than default-material)
  matrixData_batched_bitmap_vert matrices{};
  matrices.modelViewMatrix = gr_model_view_matrix;
  matrices.projMatrix = gr_projection_matrix;

  // Fill generic data from material
  genericData_batched_bitmap_vert generic{};
  vec4 clr = material_info->get_color();
  generic.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};
  generic.intensity = material_info->get_color_scale();

  // Allocate from uniform ring buffer (alignment derived from device limits)
  const size_t uboAlignmentRaw = renderer_instance->getMinUniformOffsetAlignment();
  const size_t uboAlignment = uboAlignmentRaw == 0 ? 1 : uboAlignmentRaw;
  const size_t matrixSize = sizeof(matrices);  // 128 bytes
  const size_t genericOffset = ((matrixSize + uboAlignment - 1) / uboAlignment) * uboAlignment;
  const size_t totalUniformSize = genericOffset + sizeof(generic);

  auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, uboAlignment);
  auto* uniformBase = static_cast<char*>(uniformAlloc.mapped);
  std::memcpy(uniformBase, &matrices, sizeof(matrices));
  std::memcpy(uniformBase + genericOffset, &generic, sizeof(generic));

  // Build descriptor buffer infos
  vk::DescriptorBufferInfo matrixInfo{};
  matrixInfo.buffer = frame.uniformBuffer().buffer();
  matrixInfo.offset = uniformAlloc.offset;
  matrixInfo.range = sizeof(matrices);

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset + genericOffset;
  genericInfo.range = sizeof(generic);

  // Get texture descriptor (batched rendering requires texture)
  int textureHandle = material_info->is_textured() ? material_info->get_texture_map(TM_BASE_TYPE) : -1;
  if (textureHandle < 0) {
    return;  // Early exit - batched rendering requires texture array
  }

  auto samplerKey = VulkanTextureManager::SamplerKey{};
  samplerKey.address = convertTextureAddressing(material_info->get_texture_addressing());

  vk::DescriptorImageInfo baseMapInfo = renderer_instance->getTextureDescriptor(
    textureHandle, frame, cmd, samplerKey);

  // Build push descriptor writes (3 bindings: 0=matrix, 1=generic, 2=texture)
  std::array<vk::WriteDescriptorSet, 3> writes{};

  writes[0].dstBinding = 0;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &matrixInfo;

  writes[1].dstBinding = 1;
  writes[1].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[1].descriptorCount = 1;
  writes[1].pBufferInfo = &genericInfo;

  writes[2].dstBinding = 2;
  writes[2].descriptorType = vk::DescriptorType::eCombinedImageSampler;
  writes[2].descriptorCount = 1;
  writes[2].pImageInfo = &baseMapInfo;

  // Bind pipeline
  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);

  // Push descriptors (no descriptor set allocation needed)
  cmd.pushDescriptorSetKHR(
    vk::PipelineBindPoint::eGraphics,
    renderer_instance->getPipelineLayout(),
    0,  // set 0 (per-draw push descriptors)
    3,  // all three bindings
    writes.data());

  // Bind vertex buffer
  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &vertexBuffer, &vbOffset);

  // Set dynamic state: primitive topology
  cmd.setPrimitiveTopology(convertPrimitiveType(prim_type));

  // Set dynamic state: cull mode
  cmd.setCullMode(material_info->get_cull_mode() ?
    vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);

  // Set dynamic state: depth test and write
  gr_zbuffer_type zbufferMode = material_info->get_depth_mode();
  bool depthTest = (zbufferMode == ZBUFFER_TYPE_READ || zbufferMode == ZBUFFER_TYPE_FULL);
  bool depthWrite = (zbufferMode == ZBUFFER_TYPE_WRITE || zbufferMode == ZBUFFER_TYPE_FULL);
  cmd.setDepthTestEnable(depthTest ? VK_TRUE : VK_FALSE);
  cmd.setDepthWriteEnable(depthWrite ? VK_TRUE : VK_FALSE);
  cmd.setDepthCompareOp(depthTest ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  // Set extended dynamic state 3 (if supported)
  if (renderer_instance->supportsExtendedDynamicState3()) {
    const auto& caps = renderer_instance->getExtendedDynamicState3Caps();
    if (caps.colorBlendEnable) {
      // H7 fix: respect material blend mode instead of unconditionally disabling
      vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE;
      cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
    }
    if (caps.colorWriteMask) {
      vk::ColorComponentFlags mask = vk::ColorComponentFlagBits::eR |
        vk::ColorComponentFlagBits::eG | vk::ColorComponentFlagBits::eB |
        vk::ColorComponentFlagBits::eA;
      cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(1, &mask));
    }
    if (caps.polygonMode) {
      cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    }
    if (caps.rasterizationSamples) {
      cmd.setRasterizationSamplesEXT(vk::SampleCountFlagBits::e1);
    }
  }

  // Set viewport and scissor
  vk::Viewport viewport = createFullScreenViewport();
  cmd.setViewport(0, 1, &viewport);
  vk::Rect2D scissor = createClipScissor();
  cmd.setScissor(0, 1, &scissor);

  // Draw call
  cmd.draw(static_cast<uint32_t>(n_verts), 1, static_cast<uint32_t>(offset), 0);
}

void gr_vulkan_render_rocket_primitives(interface_material* material_info,
  primitive_type prim_type,
  vertex_layout* layout,
  int n_indices,
  gr_buffer_handle vertex_buffer,
  gr_buffer_handle index_buffer)
{
  if (renderer_instance) {
    renderer_instance->incrementPrimDraw();
  }

  // Preconditions
  Assertion(g_currentFrame != nullptr, "render_rocket_primitives called before setup_frame");
  Assertion(renderer_instance != nullptr, "render_rocket_primitives called without renderer");
  Assertion(material_info != nullptr, "render_rocket_primitives called with null material_info");
  Assertion(layout != nullptr, "render_rocket_primitives called with null vertex layout");
  Assertion(n_indices > 0, "render_rocket_primitives called with zero indices");
  Assertion(vertex_buffer.isValid(), "render_rocket_primitives called with invalid vertex buffer handle");
  Assertion(index_buffer.isValid(), "render_rocket_primitives called with invalid index buffer handle");

  GR_DEBUG_SCOPE("Render rocket ui primitives");

  // RocketUI expects 2D projection in gr_projection_matrix
  gr_set_2d_matrix();

  VulkanFrame& frame = *g_currentFrame;
  vk::CommandBuffer cmd = frame.commandBuffer();
  Assertion(cmd, "render_rocket_primitives called with null command buffer");

  // Ensure we're rendering to swapchain (menus/UI are swapchain-targeted).
  auto rt = renderer_instance->ensureRenderingStarted(cmd);
  const auto swapchainFormat = static_cast<vk::Format>(renderer_instance->getSwapChainImageFormat());
  if (rt.colorAttachmentCount != 1 || rt.colorFormat != swapchainFormat) {
    renderer_instance->setPendingRenderTargetSwapchain();
    rt = renderer_instance->ensureRenderingStarted(cmd);
  }

  ShaderModules shaderModules = renderer_instance->getShaderModules(SDR_TYPE_ROCKET_UI);
  Assertion(shaderModules.vert != nullptr, "Rocket UI vertex shader not loaded");
  Assertion(shaderModules.frag != nullptr, "Rocket UI fragment shader not loaded");

  PipelineKey pipelineKey{};
  pipelineKey.type = SDR_TYPE_ROCKET_UI;
  pipelineKey.variant_flags = 0;
  pipelineKey.color_format = static_cast<VkFormat>(rt.colorFormat);
  pipelineKey.depth_format = static_cast<VkFormat>(rt.depthFormat);
  pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
  pipelineKey.color_attachment_count = rt.colorAttachmentCount;
  pipelineKey.blend_mode = material_info->get_blend_mode();
  pipelineKey.layout_hash = layout->hash();
  pipelineKey.color_write_mask = convertColorWriteMask(material_info->get_color_mask());

  vk::Pipeline pipeline = renderer_instance->getPipeline(pipelineKey, shaderModules, *layout);
  Assertion(pipeline, "Pipeline creation failed for Rocket UI shader");

  vk::Buffer vertexBuffer = renderer_instance->getBuffer(vertex_buffer);
  Assertion(vertexBuffer, "Failed to resolve Vulkan vertex buffer for Rocket UI handle %d", vertex_buffer.value());
  vk::Buffer indexBuffer = renderer_instance->getBuffer(index_buffer);
  Assertion(indexBuffer, "Failed to resolve Vulkan index buffer for Rocket UI handle %d", index_buffer.value());

  // Build Rocket UI uniform data (matches rocketui_data std140 layout)
  graphics::generic_data::rocketui_data rocketData{};
  rocketData.projMatrix = gr_projection_matrix;
  rocketData.offset = material_info->get_offset();
  rocketData.textured = material_info->is_textured() ? 1 : 0;

  const int textureHandle = material_info->is_textured() ? material_info->get_texture_map(TM_BASE_TYPE) : -1;
  rocketData.baseMapIndex = (textureHandle >= 0) ? bm_get_array_index(textureHandle) : 0;
  rocketData.horizontalSwipeOffset = material_info->get_horizontal_swipe();

  // Allocate uniform block for binding 1
  const size_t uboAlignmentRaw = renderer_instance->getMinUniformOffsetAlignment();
  const size_t uboAlignment = uboAlignmentRaw == 0 ? 1 : uboAlignmentRaw;
  auto uniformAlloc = frame.uniformBuffer().allocate(sizeof(rocketData), uboAlignment);
  std::memcpy(uniformAlloc.mapped, &rocketData, sizeof(rocketData));

  vk::DescriptorBufferInfo genericInfo{};
  genericInfo.buffer = frame.uniformBuffer().buffer();
  genericInfo.offset = uniformAlloc.offset;
  genericInfo.range = sizeof(rocketData);

  std::array<vk::WriteDescriptorSet, 2> writes{};
  uint32_t writeCount = 1;

  writes[0].dstBinding = 1;
  writes[0].descriptorType = vk::DescriptorType::eUniformBuffer;
  writes[0].descriptorCount = 1;
  writes[0].pBufferInfo = &genericInfo;

  vk::DescriptorImageInfo baseMapInfo{};
  if (textureHandle >= 0) {
    auto samplerKey = VulkanTextureManager::SamplerKey{};
    samplerKey.address = convertTextureAddressing(material_info->get_texture_addressing());
    baseMapInfo = renderer_instance->getTextureDescriptor(textureHandle, frame, cmd, samplerKey);

    writes[1].dstBinding = 2;
    writes[1].descriptorType = vk::DescriptorType::eCombinedImageSampler;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &baseMapInfo;
    writeCount = 2;
  }

  cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
  cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, renderer_instance->getPipelineLayout(), 0, writeCount, writes.data());

  vk::DeviceSize vbOffset = 0;
  cmd.bindVertexBuffers(0, 1, &vertexBuffer, &vbOffset);
  cmd.bindIndexBuffer(indexBuffer, 0, vk::IndexType::eUint32);

  cmd.setPrimitiveTopology(convertPrimitiveType(prim_type));
  cmd.setCullMode(vk::CullModeFlagBits::eNone);
  cmd.setFrontFace(vk::FrontFace::eClockwise);
  cmd.setDepthTestEnable(VK_FALSE);
  cmd.setDepthWriteEnable(VK_FALSE);
  cmd.setDepthCompareOp(vk::CompareOp::eAlways);
  cmd.setStencilTestEnable(VK_FALSE);

  if (renderer_instance->supportsExtendedDynamicState3()) {
    const auto& caps = renderer_instance->getExtendedDynamicState3Caps();
    if (caps.colorBlendEnable) {
      vk::Bool32 blendEnable = (material_info->get_blend_mode() != ALPHA_BLEND_NONE) ? VK_TRUE : VK_FALSE;
      cmd.setColorBlendEnableEXT(0, vk::ArrayProxy<const vk::Bool32>(1, &blendEnable));
    }
    if (caps.colorWriteMask) {
      auto mask = static_cast<vk::ColorComponentFlags>(pipelineKey.color_write_mask);
      cmd.setColorWriteMaskEXT(0, vk::ArrayProxy<const vk::ColorComponentFlags>(1, &mask));
    }
  }

  vk::Viewport viewport = createFullScreenViewport();
  cmd.setViewport(0, 1, &viewport);
  vk::Rect2D scissor = createClipScissor();
  cmd.setScissor(0, 1, &scissor);

  cmd.drawIndexed(static_cast<uint32_t>(n_indices), 1, 0, 0, 0);

  gr_end_2d_matrix();
}

bool gr_vulkan_is_capable(gr_capability capability)
{
  switch (capability) {
  case gr_capability::CAPABILITY_INSTANCED_RENDERING:
    // Report instancing only when the device supports attribute divisors
    return renderer_instance && renderer_instance->supportsVertexAttributeDivisor();
  case gr_capability::CAPABILITY_PERSISTENT_BUFFER_MAPPING:
    // Disabled for now: our buffer upload path expects non-null data on creation,
    // while the persistent-mapped path would pass nullptr initially.
    return false;
  default:
    return false;
  }
}
bool gr_vulkan_get_property(gr_property p, void* dest)
{
  if (p == gr_property::UNIFORM_BUFFER_OFFSET_ALIGNMENT) {
    if (renderer_instance) {
      *reinterpret_cast<int*>(dest) = static_cast<int>(renderer_instance->getMinUniformBufferAlignment());
      return true;
    }
    *reinterpret_cast<int*>(dest) = 256;
    return true;
  }
  return false;
}

void gr_vulkan_push_debug_group(const char* name)
{
  if (!g_currentFrame) {
    return;
  }
  vk::CommandBuffer cmd = g_currentFrame->commandBuffer();
  if (!cmd) {
    return;
  }

  vk::DebugUtilsLabelEXT label{};
  label.pLabelName = name;
  // Default color (white with alpha)
  label.color[0] = 1.0f;
  label.color[1] = 1.0f;
  label.color[2] = 1.0f;
  label.color[3] = 1.0f;

  cmd.beginDebugUtilsLabelEXT(label);
}

void gr_vulkan_pop_debug_group()
{
  if (!g_currentFrame) {
    return;
  }
  vk::CommandBuffer cmd = g_currentFrame->commandBuffer();
  if (!cmd) {
    return;
  }

  cmd.endDebugUtilsLabelEXT();
}

int stub_create_query_object() { return -1; }

void stub_query_value(int /*obj*/, QueryType /*type*/) {}

bool stub_query_value_available(int /*obj*/) { return false; }

std::uint64_t stub_get_query_value(int /*obj*/) { return 0; }

void stub_delete_query_object(int /*obj*/) {}

SCP_vector<const char*> stub_openxr_get_extensions() { return {}; }

bool stub_openxr_test_capabilities() { return false; }

bool stub_openxr_create_session() { return false; }

int64_t stub_openxr_get_swapchain_format(const SCP_vector<int64_t>& /*allowed*/) { return 0; }

bool stub_openxr_acquire_swapchain_buffers() { return false; }

bool stub_openxr_flip() { return false; }

} // namespace (anonymous)

void init_function_pointers()
{
  // Start with stubs as base, then override with Vulkan implementations
  gr_stub_init_function_pointers();

  // Core frame management
  gr_screen.gf_flip = []() {
    g_currentFrame = nullptr;
    if (renderer_instance) {
      renderer_instance->flip();
    }
  };
  gr_screen.gf_setup_frame = gr_vulkan_setup_frame;
  gr_screen.gf_clear = []() {
    if (renderer_instance) {
      renderer_instance->requestClear();
    }
  };
  gr_screen.gf_set_clear_color = [](int r, int g, int b) {
    if (renderer_instance) {
      renderer_instance->setClearColor(r, g, b);
    }
  };

  // Clipping
  gr_screen.gf_set_clip = gr_vulkan_set_clip;
  gr_screen.gf_reset_clip = gr_vulkan_reset_clip;

  // Depth/cull state
  gr_screen.gf_set_cull = [](int cull) -> int {
    if (renderer_instance) {
      return renderer_instance->setCullMode(cull);
    }
    return 0;
  };
  gr_screen.gf_zbuffer_set = [](int mode) -> int {
    if (renderer_instance) {
      return renderer_instance->setZbufferMode(mode);
    }
    return mode;
  };
  gr_screen.gf_zbuffer_get = []() -> int {
    if (renderer_instance) {
      return renderer_instance->getZbufferMode();
    }
    return 0;
  };
  gr_screen.gf_zbuffer_clear = [](int mode) {
    if (renderer_instance) {
      renderer_instance->zbufferClear(mode);
    }
  };

  // Texture preloading
  gr_screen.gf_preload = [](int bitmap_num, int is_aabitmap) -> int {
    if (renderer_instance) {
      return renderer_instance->preloadTexture(bitmap_num, is_aabitmap != 0);
    }
    return 0;
  };

  // Buffer management
  gr_screen.gf_create_buffer = gr_vulkan_create_buffer;
  gr_screen.gf_delete_buffer = gr_vulkan_delete_buffer;
  gr_screen.gf_update_buffer_data = gr_vulkan_update_buffer_data;
  gr_screen.gf_update_buffer_data_offset = gr_vulkan_update_buffer_data_offset;
  gr_screen.gf_resize_buffer = gr_vulkan_resize_buffer;
  gr_screen.gf_map_buffer = [](gr_buffer_handle handle) -> void* {
    if (renderer_instance) {
      return renderer_instance->mapBuffer(handle);
    }
    return nullptr;
  };
  gr_screen.gf_flush_mapped_buffer = [](gr_buffer_handle handle, size_t offset, size_t size) {
    if (renderer_instance) {
      renderer_instance->flushMappedBuffer(handle, offset, size);
    }
  };
  gr_screen.gf_bind_uniform_buffer = gr_vulkan_bind_uniform_buffer;
  gr_screen.gf_register_model_vertex_heap = [](gr_buffer_handle handle) {
    Assertion(renderer_instance != nullptr,
      "register_model_vertex_heap called before renderer initialization");
    renderer_instance->setModelVertexHeapHandle(handle);
  };

  // Rendering
  gr_screen.gf_render_model = gr_vulkan_render_model;
  gr_screen.gf_render_primitives = gr_vulkan_render_primitives;
  gr_screen.gf_render_primitives_batched = gr_vulkan_render_primitives_batched;
  gr_screen.gf_render_nanovg = gr_vulkan_render_nanovg;
  gr_screen.gf_render_rocket_primitives = gr_vulkan_render_rocket_primitives;

  // Deferred lighting
  gr_screen.gf_deferred_lighting_begin = gr_vulkan_deferred_lighting_begin;
  gr_screen.gf_deferred_lighting_msaa = gr_vulkan_deferred_lighting_msaa;
  gr_screen.gf_deferred_lighting_end = gr_vulkan_deferred_lighting_end;
  gr_screen.gf_deferred_lighting_finish = gr_vulkan_deferred_lighting_finish;

  // Line width
  gr_screen.gf_set_line_width = stub_set_line_width;

  // Debug groups
  gr_screen.gf_push_debug_group = gr_vulkan_push_debug_group;
  gr_screen.gf_pop_debug_group = gr_vulkan_pop_debug_group;

  // Capabilities
  gr_screen.gf_is_capable = gr_vulkan_is_capable;
  gr_screen.gf_get_property = gr_vulkan_get_property;

  Bm_paging = 0;
}

void initialize_function_pointers() {
  // Set minimal stubs for functions that might be called before initialize()
  // Full initialization happens in initialize() after renderer is created
  gr_stub_init_function_pointers();
}

bool initialize(std::unique_ptr<os::GraphicsOperations>&& graphicsOps)
{
  renderer_instance = std::make_unique<VulkanRenderer>(std::move(graphicsOps));
  if (!renderer_instance->initialize()) {
    return false;
  }

  // Initialize all function pointers now that renderer is available
  init_function_pointers();
  return true;
}

VulkanRenderer* getRendererInstance()
{
  return renderer_instance.get();
}

void cleanup()
{
  renderer_instance->shutdown();
  renderer_instance = nullptr;
}

} // namespace graphics::vulkan
