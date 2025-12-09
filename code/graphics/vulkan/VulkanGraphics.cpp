
#include "VulkanGraphics.h"

#include "VulkanModelTypes.h"
#include "VulkanRenderer.h"
#include "VulkanPipelineManager.h"
#include "VulkanVertexTypes.h"
#include "VulkanDebug.h"

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

extern transform_stack gr_model_matrix_stack;
extern matrix4 gr_view_matrix;
extern matrix4 gr_model_view_matrix;
extern matrix4 gr_projection_matrix;

#define BMPMAN_INTERNAL
#include "bmpman/bm_internal.h"

namespace graphics::vulkan {

namespace {
std::unique_ptr<VulkanRenderer> renderer_instance;
VulkanFrame* g_currentFrame = nullptr;  // Injected by setup_frame, used by render functions

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
	vk::Rect2D scissor{};
	scissor.offset = vk::Offset2D{
		static_cast<int32_t>(gr_screen.clip_left),
		static_cast<int32_t>(gr_screen.clip_top)};
	scissor.extent = vk::Extent2D{
		static_cast<uint32_t>(gr_screen.clip_width),
		static_cast<uint32_t>(gr_screen.clip_height)};
	return scissor;
}

vk::Rect2D createFullScreenScissor()
{
	vk::Rect2D scissor{};
	scissor.offset = vk::Offset2D{0, 0};
	scissor.extent = vk::Extent2D{
		static_cast<uint32_t>(gr_screen.max_w),
		static_cast<uint32_t>(gr_screen.max_h)};
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

	// Reset per-draw dynamic offset; descriptor only rewritten when buffer handle changes
	g_currentFrame->modelUniformState.dynamicOffset = UINT32_MAX;

	vk::CommandBuffer cmd = g_currentFrame->commandBuffer();
	Assertion(cmd, "Frame has no valid command buffer");

	// Ensure dynamic rendering has begun so subsequent state and draws are valid
	renderer_instance->ensureRenderingStarted(cmd);

	// Viewport: full-screen with Vulkan Y-flip (y = height, height = -height)
	// Note: ensureRenderingStarted() already sets viewport, but we override to ensure
	// it matches gr_screen dimensions (which should match swapchain extent)
	vk::Viewport viewport = createFullScreenViewport();
	cmd.setViewport(0, 1, &viewport);

	// Scissor: respect current clip region from gr_screen
	// This ensures that any clip set before setup_frame is applied
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

	if (type != uniform_block_type::ModelData) {
		// Keep running but make it noisy so the offending path gets fixed.
		vkprintf("VULKAN ERROR: gr_bind_uniform_buffer called with unsupported block type %d (offset=%zu, size=%zu, handle=%d). "
		         "Only ModelData is implemented in the Vulkan backend; this call must be fixed.\n",
		         static_cast<int>(type),
		         offset,
		         size,
		         handle.value());
		return;
	}

	renderer_instance->setModelUniformBinding(*g_currentFrame, handle, offset, size);
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

void stub_set_clear_color(int /*r*/, int /*g*/, int /*b*/) {}

void gr_vulkan_set_clip(int x, int y, int w, int h, int resize_mode)
{
	// Store unscaled values
	gr_screen.offset_x_unscaled = x;
	gr_screen.offset_y_unscaled = y;
	gr_screen.clip_width_unscaled = w;
	gr_screen.clip_height_unscaled = h;

	// Apply resize scaling if not GR_RESIZE_NONE
	if (resize_mode != GR_RESIZE_NONE) {
		gr_resize_screen_pos(&x, &y, &w, &h, resize_mode);
	}

	// Clamp to screen bounds
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	if (x + w > gr_screen.max_w) {
		w = gr_screen.max_w - x;
	}
	if (y + h > gr_screen.max_h) {
		h = gr_screen.max_h - y;
	}

	// Ensure valid dimensions
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}

	gr_screen.offset_x = x;
	gr_screen.offset_y = y;
	gr_screen.clip_left = x;
	gr_screen.clip_top = y;
	gr_screen.clip_right = x + w - 1;
	gr_screen.clip_bottom = y + h - 1;
	gr_screen.clip_width = w;
	gr_screen.clip_height = h;
	gr_screen.clip_aspect = i2fl(w) / i2fl(h);
	gr_screen.clip_center_x = (gr_screen.clip_left + gr_screen.clip_right) * 0.5f;
	gr_screen.clip_center_y = (gr_screen.clip_top + gr_screen.clip_bottom) * 0.5f;

	// Set unscaled clip bounds
	gr_screen.clip_left_unscaled = gr_screen.offset_x_unscaled;
	gr_screen.clip_top_unscaled = gr_screen.offset_y_unscaled;
	gr_screen.clip_right_unscaled = gr_screen.offset_x_unscaled + gr_screen.clip_width_unscaled - 1;
	gr_screen.clip_bottom_unscaled = gr_screen.offset_y_unscaled + gr_screen.clip_height_unscaled - 1;
}

int stub_set_cull(int /*cull*/) { return 0; }

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
	if (!renderer_instance) {
		return;
	}
	renderer_instance->beginDeferredLighting(clearNonColorBufs);
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
	renderer_instance->endDeferredGeometry(frame->commandBuffer());
}

void gr_vulkan_deferred_lighting_finish()
{
	if (!renderer_instance) {
		return;
	}
	renderer_instance->bindDeferredGlobalDescriptors();
	renderer_instance->setPendingRenderTargetSwapchain();
}

void stub_set_line_width(float /*width*/) {}

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

	ctx.cmd.bindDescriptorSets(
		vk::PipelineBindPoint::eGraphics,
		ctx.pipelineLayout,
		0,
		1,
		&ctx.modelSet,
		1,
		&ctx.modelDynamicOffset);

	ctx.cmd.pushConstants(
		ctx.pipelineLayout,
		vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
		0,
		sizeof(ModelPushConstants),
		&ctx.pcs);

	vk::Buffer indexBuffer = renderer_instance->getBuffer(ctx.vertSource.Ibuffer_handle);
	Assertion(indexBuffer, "Invalid index buffer handle %d", ctx.vertSource.Ibuffer_handle.value());

	const vk::IndexType indexType = vk::IndexType::eUint16;
	ctx.cmd.bindIndexBuffer(indexBuffer, ctx.vertSource.Index_offset, indexType);

	const buffer_data* datap = &ctx.vbuffer.tex_buf[ctx.texi];
	const uint32_t indexCount = static_cast<uint32_t>(datap->n_verts);

	ctx.cmd.drawIndexed(
		indexCount,
		1,  // instanceCount
		0,  // firstIndex
		0,  // vertexOffset
		0   // firstInstance
	);
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
	buffer_data* datap = &bufferp->tex_buf[texi];

	// Get shader modules for model shader
	ShaderModules modules = renderer_instance->getShaderModules(SDR_TYPE_MODEL);
	Assertion(modules.vert != nullptr, "Model vertex shader not loaded");
	Assertion(modules.frag != nullptr, "Model fragment shader not loaded");

	// Build pipeline key
	PipelineKey key{};
	key.type = SDR_TYPE_MODEL;
	key.variant_flags = material_info->get_shader_flags();
	key.color_format = renderer_instance->getCurrentColorFormat();
	key.depth_format = renderer_instance->getDepthFormat();
	key.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
	key.color_attachment_count = renderer_instance->getCurrentColorAttachmentCount();
	key.blend_mode = material_info->get_blend_mode();
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
	pcs.vertexOffset = static_cast<uint32_t>(vert_source->Vertex_offset);
	pcs.stride = static_cast<uint32_t>(bufferp->stride);
	pcs.posOffset = MODEL_OFFSET_ABSENT;
	pcs.normalOffset = MODEL_OFFSET_ABSENT;
	pcs.texCoordOffset = MODEL_OFFSET_ABSENT;
	pcs.tangentOffset = MODEL_OFFSET_ABSENT;
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
		return (h >= 0) ? static_cast<uint32_t>(bm_get_array_index(h)) : MODEL_OFFSET_ABSENT;
	};

	int baseTex = material_info->get_texture_map(TM_BASE_TYPE);
	int glowTex = material_info->get_texture_map(TM_GLOW_TYPE);
	int normalTex = material_info->get_texture_map(TM_NORMAL_TYPE);
	int specTex = material_info->get_texture_map(TM_SPECULAR_TYPE);

	pcs.baseMapIndex = toIndex(baseTex);
	pcs.glowMapIndex = toIndex(glowTex);
	pcs.normalMapIndex = toIndex(normalTex);
	pcs.specMapIndex = toIndex(specTex);
	pcs.matrixIndex = 0;  // Unused
	pcs.flags = material_info->get_shader_flags();

	// Descriptor set
	vk::DescriptorSet modelSet = frame.modelDescriptorSet;
	Assertion(modelSet, "Model descriptor set not initialized for frame");

	std::vector<std::pair<uint32_t, int>> texturesToBind;
	texturesToBind.reserve(4);
	for (const auto& entry : {std::pair<uint32_t, int>{pcs.baseMapIndex, baseTex},
							  std::pair<uint32_t, int>{pcs.glowMapIndex, glowTex},
							  std::pair<uint32_t, int>{pcs.normalMapIndex, normalTex},
							  std::pair<uint32_t, int>{pcs.specMapIndex, specTex}}) {
		if (entry.first != MODEL_OFFSET_ABSENT) {
			texturesToBind.emplace_back(entry);
		}
	}

	renderer_instance->updateModelDescriptors(
		modelSet, vertexBuffer,
		texturesToBind, frame, cmd);

	// Ensure rendering has started
	renderer_instance->ensureRenderingStarted(cmd);

	// Enforce contract: ModelData must be bound via gr_bind_uniform_buffer
	uint32_t dynOffset = frame.modelUniformState.dynamicOffset;
	Assertion(dynOffset != UINT32_MAX, "ModelData UBO dynamic offset not set; call gr_bind_uniform_buffer first");
	Assertion(frame.modelUniformState.bufferHandle.isValid(),
		"ModelData UBO buffer handle not set; call gr_bind_uniform_buffer first");

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

	issueModelDraw(ctx);

#if defined(_DEBUG) || defined(DEBUG)
	// Catch missing gr_bind_uniform_buffer on subsequent draws
	frame.modelUniformState.dynamicOffset = UINT32_MAX;
#endif
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

	// Use the shader type requested by the material
	shader_type shaderType = material_info->get_shader_type();

	// Get shader modules
	ShaderModules shaderModules = renderer_instance->getShaderModules(shaderType);
	Assertion(shaderModules.vert != nullptr, "render_primitives missing vertex shader for shaderType=%d", static_cast<int>(shaderType));
	Assertion(shaderModules.frag != nullptr, "render_primitives missing fragment shader for shaderType=%d", static_cast<int>(shaderType));

	// Build pipeline key using vertex_layout hash
	PipelineKey pipelineKey{};
	pipelineKey.type = shaderType;
	pipelineKey.variant_flags = material_info->get_shader_flags();
	pipelineKey.color_format = renderer_instance->getCurrentColorFormat();
	pipelineKey.depth_format = renderer_instance->getDepthFormat();
	pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
	pipelineKey.color_attachment_count = renderer_instance->getCurrentColorAttachmentCount();
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

	// Fill uniform structs
	matrixData_default_material_vert matrices{};
	matrices.modelViewMatrix = modelViewMatrix;
	matrices.projMatrix = projMatrix;

	genericData_default_material_vert generic{};
	generic.modelMatrix = modelMatrix;
	vec4 clr = material_info->get_color();
	generic.color = {clr.xyzw.x, clr.xyzw.y, clr.xyzw.z, clr.xyzw.w};

	if (material_info->is_clipped()) {
		const auto& clip = material_info->get_clip_plane();
		generic.clipEquation = {clip.normal.xyz.x, clip.normal.xyz.y, clip.normal.xyz.z,
		                        -vm_vec_dot(&clip.normal, &clip.position)};
		generic.clipEnabled = 1;
	} else {
		generic.clipEquation = {0.f, 0.f, 0.f, 0.f};
		generic.clipEnabled = 0;
	}

	int textureHandle = material_info->is_textured() ? material_info->get_texture_map(TM_BASE_TYPE) : -1;
	generic.baseMapIndex = (textureHandle >= 0) ? bm_get_array_index(textureHandle) : 0;
	generic.alphaTexture = (material_info->get_texture_type() == material::TEX_TYPE_AABITMAP) ? 1 : 0;
	generic.noTexturing = material_info->is_textured() ? 0 : 1;
	generic.srgb = 1;
	generic.intensity = material_info->get_color_scale();
	generic.alphaThreshold = 0.f;

	// Allocate from uniform ring buffer (single allocation to avoid extra padding)
	const size_t matrixSize = sizeof(matrices);
	const size_t genericOffset = (matrixSize + 255u) & ~static_cast<size_t>(255);
	const size_t totalUniformSize = genericOffset + sizeof(generic);

	auto uniformAlloc = frame.uniformBuffer().allocate(totalUniformSize, 256);
	auto* uniformBase = static_cast<char*>(uniformAlloc.mapped);
	std::memcpy(uniformBase, &matrices, sizeof(matrices));
	std::memcpy(uniformBase + genericOffset, &generic, sizeof(generic));

	// Build push descriptor writes
	vk::DescriptorBufferInfo matrixInfo{};
	matrixInfo.buffer = frame.uniformBuffer().buffer();
	matrixInfo.offset = uniformAlloc.offset;
	matrixInfo.range = sizeof(matrices);

	vk::DescriptorBufferInfo genericInfo{};
	genericInfo.buffer = frame.uniformBuffer().buffer();
	genericInfo.offset = uniformAlloc.offset + genericOffset;
	genericInfo.range = sizeof(generic);

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

	// Ensure rendering has started before binding pipeline/descriptors
	renderer_instance->ensureRenderingStarted(cmd);

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
			vk::Bool32 blendEnable = VK_FALSE;
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

	vk::Rect2D scissor = createFullScreenScissor();
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

void stub_render_primitives_batched(batched_bitmap_material* /*material_info*/,
	primitive_type /*prim_type*/,
	vertex_layout* /*layout*/,
	int /*offset*/,
	int /*n_verts*/,
	gr_buffer_handle /*buffer_handle*/)
{
}

void stub_render_rocket_primitives(interface_material* /*material_info*/,
	primitive_type /*prim_type*/,
	vertex_layout* /*layout*/,
	int /*n_indices*/,
	gr_buffer_handle /*vertex_buffer*/,
	gr_buffer_handle /*index_buffer*/)
{
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

void stub_push_debug_group(const char*) {}

void stub_pop_debug_group() {}

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

	// Deferred lighting
	gr_screen.gf_deferred_lighting_begin = gr_vulkan_deferred_lighting_begin;
	gr_screen.gf_deferred_lighting_msaa = gr_vulkan_deferred_lighting_msaa;
	gr_screen.gf_deferred_lighting_end = gr_vulkan_deferred_lighting_end;
	gr_screen.gf_deferred_lighting_finish = gr_vulkan_deferred_lighting_finish;

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
