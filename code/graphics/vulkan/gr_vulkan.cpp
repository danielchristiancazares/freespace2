
#include "gr_vulkan.h"

#include "VulkanRenderer.h"
#include "VulkanPipelineManager.h"
#include "VulkanVertexTypes.h"

#include "backends/imgui_impl_sdl.h"
#include "backends/imgui_impl_vulkan.h"
#include "graphics/2d.h"
#include "graphics/material.h"
#include "graphics/matrix.h"
#include "graphics/util/uniform_structs.h"
#include "mod_table/mod_table.h"

extern transform_stack gr_model_matrix_stack;
extern matrix4 gr_view_matrix;
extern matrix4 gr_model_view_matrix;
extern matrix4 gr_projection_matrix;

#define BMPMAN_INTERNAL
#include "bmpman/bm_internal.h"

namespace graphics {
namespace vulkan {

namespace {
std::unique_ptr<VulkanRenderer> renderer_instance;

// Stub implementations for unimplemented functions
gr_buffer_handle gr_vulkan_create_buffer(BufferType type, BufferUsageHint usage)
{
	if (!renderer_instance) {
		return gr_buffer_handle::invalid();
	}
	return renderer_instance->createBuffer(type, usage);
}

void stub_setup_frame() {}

void gr_vulkan_delete_buffer(gr_buffer_handle handle)
{
	if (renderer_instance) {
		renderer_instance->deleteBuffer(handle);
	}
}

int stub_preload(int /*bitmap_num*/, int /*is_aabitmap*/) { return 0; }

int stub_save_screen() { return 1; }

int stub_zbuffer_get() { return 0; }

int stub_zbuffer_set(int /*mode*/) { return 0; }

void gr_set_fill_mode_stub(int /*mode*/) {}

void stub_clear() {}

void stub_free_screen(int /*id*/) {}

void stub_get_region(int /*front*/, int /*w*/, int /*h*/, ubyte* /*data*/) {}

void stub_print_screen(const char* /*filename*/) {}

SCP_string stub_blob_screen() { return ""; }

void stub_reset_clip() {}

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

void stub_update_transform_buffer(void* /*data*/, size_t /*size*/) {}

void stub_set_clear_color(int /*r*/, int /*g*/, int /*b*/) {}

void stub_set_clip(int /*x*/, int /*y*/, int /*w*/, int /*h*/, int /*resize_mode*/) {}

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

void stub_deferred_lighting_begin(bool /*clearNonColorBufs*/) {}

void stub_deferred_lighting_msaa() {}

void stub_deferred_lighting_end() {}

void stub_deferred_lighting_finish() {}

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

void stub_render_model(model_material* /*material_info*/,
	indexed_vertex_source* /*vert_source*/,
	vertex_buffer* /*bufferp*/,
	size_t /*texi*/)
{
}

void gr_vulkan_render_primitives(material* material_info,
	primitive_type prim_type,
	vertex_layout* layout,
	int offset,
	int n_verts,
	gr_buffer_handle buffer_handle,
	size_t buffer_offset)
{
	if (!renderer_instance || !material_info || !layout || n_verts == 0) {
		return;
	}

	auto* frame = renderer_instance->getCurrentRecordingFrame();
	if (!frame) {
		if (renderer_instance->warnOnceIfNotRecording()) {
			mprintf(("Vulkan: draw call skipped because no frame is currently recording (call flip() first).\n"));
		}
		return;
	}

	vk::CommandBuffer cmd = frame->commandBuffer();
	if (!cmd) {
		return;
	}

	// For base material, use DEFAULT_MATERIAL shader
	// Specialized functions (particle, model, etc.) will use appropriate types
	shader_type shaderType = SDR_TYPE_DEFAULT_MATERIAL;

	// Get shader modules
	ShaderModules shaderModules = renderer_instance->getShaderModules(shaderType);
	if (!shaderModules.vert || !shaderModules.frag) {
		return; // Shader not loaded
	}

	// Build pipeline key using vertex_layout hash
	PipelineKey pipelineKey{};
	pipelineKey.type = shaderType;
	pipelineKey.variant_flags = material_info->get_shader_flags();
	pipelineKey.color_format = renderer_instance->getSwapChainImageFormat();
	pipelineKey.depth_format = renderer_instance->getDepthFormat();
	pipelineKey.sample_count = static_cast<VkSampleCountFlagBits>(renderer_instance->getSampleCount());
	pipelineKey.color_attachment_count = renderer_instance->getColorAttachmentCount();
	pipelineKey.blend_mode = material_info->get_blend_mode();
	pipelineKey.layout_hash = layout->hash();

	// Get or create pipeline (passes vertex_layout for vertex input state)
	vk::Pipeline pipeline = renderer_instance->getPipeline(pipelineKey, shaderModules, *layout);
	if (!pipeline) {
		return;
	}

	// Get vertex buffer
	if (!buffer_handle.isValid()) {
		return;
	}

	vk::Buffer vertexBuffer = renderer_instance->getBuffer(buffer_handle);
	if (!vertexBuffer) {
		return;
	}

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

	generic.baseMapIndex = material_info->is_textured() ? material_info->get_texture_map(TM_BASE_TYPE) : 0;
	generic.alphaTexture = (material_info->get_texture_type() == material::TEX_TYPE_AABITMAP) ? 1 : 0;
	generic.noTexturing = material_info->is_textured() ? 0 : 1;
	generic.srgb = 1;
	generic.intensity = material_info->get_color_scale();
	generic.alphaThreshold = 0.f;

	// Allocate from uniform ring buffer
	auto matrixAlloc = frame->uniformBuffer().allocate(sizeof(matrices), 256);
	std::memcpy(matrixAlloc.mapped, &matrices, sizeof(matrices));
	auto genericAlloc = frame->uniformBuffer().allocate(sizeof(generic), 256);
	std::memcpy(genericAlloc.mapped, &generic, sizeof(generic));

	// Build push descriptor writes
	vk::DescriptorBufferInfo matrixInfo{};
	matrixInfo.buffer = frame->uniformBuffer().buffer();
	matrixInfo.offset = matrixAlloc.offset;
	matrixInfo.range = sizeof(matrices);

	vk::DescriptorBufferInfo genericInfo{};
	genericInfo.buffer = frame->uniformBuffer().buffer();
	genericInfo.offset = genericAlloc.offset;
	genericInfo.range = sizeof(generic);

	vk::DescriptorImageInfo baseMapInfo{};
	baseMapInfo.sampler = renderer_instance->getDummySampler();
	baseMapInfo.imageView = renderer_instance->getDummyImageView();
	baseMapInfo.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;

	// TODO: Look up actual texture from material_info->get_texture_map(TM_BASE_TYPE)

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

	// Push descriptors
	cmd.pushDescriptorSetKHR(
		vk::PipelineBindPoint::eGraphics,
		renderer_instance->getPipelineLayout(),
		0, // set 0
		static_cast<uint32_t>(writes.size()),
		writes.data());

	// Bind vertex buffer
	vk::DeviceSize vbOffset = buffer_offset;
	cmd.bindVertexBuffers(0, 1, &vertexBuffer, &vbOffset);

	// Set dynamic state
	// Primitive topology
	vk::PrimitiveTopology topology;
	switch (prim_type) {
	case PRIM_TYPE_POINTS:
		topology = vk::PrimitiveTopology::ePointList;
		break;
	case PRIM_TYPE_LINES:
		topology = vk::PrimitiveTopology::eLineList;
		break;
	case PRIM_TYPE_LINESTRIP:
		topology = vk::PrimitiveTopology::eLineStrip;
		break;
	case PRIM_TYPE_TRIS:
		topology = vk::PrimitiveTopology::eTriangleList;
		break;
	case PRIM_TYPE_TRISTRIP:
		topology = vk::PrimitiveTopology::eTriangleStrip;
		break;
	case PRIM_TYPE_TRIFAN:
		topology = vk::PrimitiveTopology::eTriangleFan;
		break;
	default:
		topology = vk::PrimitiveTopology::eTriangleList;
		break;
	}
	cmd.setPrimitiveTopology(topology);

	// Cull mode
	cmd.setCullMode(material_info->get_cull_mode() ? vk::CullModeFlagBits::eBack : vk::CullModeFlagBits::eNone);
	cmd.setFrontFace(vk::FrontFace::eCounterClockwise);

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
	vk::Viewport viewport{};
	viewport.x = 0.f;
	viewport.y = 0.f;
	viewport.width = static_cast<float>(gr_screen.max_w);
	viewport.height = static_cast<float>(gr_screen.max_h);
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	cmd.setViewport(0, 1, &viewport);

	vk::Rect2D scissor{};
	scissor.offset = vk::Offset2D{0, 0};
	scissor.extent = vk::Extent2D{static_cast<uint32_t>(gr_screen.max_w), static_cast<uint32_t>(gr_screen.max_h)};
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

bool stub_is_capable(gr_capability /*capability*/) { return false; }
bool stub_get_property(gr_property p, void* dest)
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
};

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

void init_stub_pointers()
{
	gr_screen.gf_setup_frame = stub_setup_frame;
	gr_screen.gf_set_clip = stub_set_clip;
	gr_screen.gf_reset_clip = stub_reset_clip;

	gr_screen.gf_clear = stub_clear;

	gr_screen.gf_print_screen = stub_print_screen;
	gr_screen.gf_blob_screen = stub_blob_screen;

	gr_screen.gf_zbuffer_get = stub_zbuffer_get;
	gr_screen.gf_zbuffer_set = stub_zbuffer_set;
	gr_screen.gf_zbuffer_clear = stub_zbuffer_clear;

	gr_screen.gf_stencil_set = stub_stencil_set;
	gr_screen.gf_stencil_clear = stub_stencil_clear;

	gr_screen.gf_alpha_mask_set = stub_alpha_mask_set;

	gr_screen.gf_save_screen = stub_save_screen;
	gr_screen.gf_restore_screen = stub_restore_screen;
	gr_screen.gf_free_screen = stub_free_screen;

	gr_screen.gf_get_region = stub_get_region;

	gr_screen.gf_bm_free_data = stub_bm_free_data;
	gr_screen.gf_bm_create = stub_bm_create;
	gr_screen.gf_bm_init = stub_bm_init;
	gr_screen.gf_bm_page_in_start = stub_bm_page_in_start;
	gr_screen.gf_bm_data = stub_bm_data;
	gr_screen.gf_bm_make_render_target = stub_bm_make_render_target;
	gr_screen.gf_bm_set_render_target = stub_bm_set_render_target;

	gr_screen.gf_set_cull = stub_set_cull;
	gr_screen.gf_set_color_buffer = stub_set_color_buffer;

	gr_screen.gf_set_clear_color = stub_set_clear_color;

	gr_screen.gf_preload = stub_preload;

	gr_screen.gf_set_texture_addressing = stub_set_texture_addressing;
	gr_screen.gf_zbias = stub_zbias_stub;
	gr_screen.gf_set_fill_mode = gr_set_fill_mode_stub;

	gr_screen.gf_create_buffer = gr_vulkan_create_buffer;
	gr_screen.gf_delete_buffer = gr_vulkan_delete_buffer;

	gr_screen.gf_update_transform_buffer = stub_update_transform_buffer;
	gr_screen.gf_update_buffer_data = gr_vulkan_update_buffer_data;
	gr_screen.gf_update_buffer_data_offset = gr_vulkan_update_buffer_data_offset;
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

	gr_screen.gf_post_process_set_effect = stub_post_process_set_effect;
	gr_screen.gf_post_process_set_defaults = stub_post_process_set_defaults;

	gr_screen.gf_post_process_begin = stub_post_process_begin;
	gr_screen.gf_post_process_end = stub_post_process_end;
	gr_screen.gf_post_process_save_zbuffer = stub_post_process_save_zbuffer;
	gr_screen.gf_post_process_restore_zbuffer = []() {};

	gr_screen.gf_scene_texture_begin = stub_scene_texture_begin;
	gr_screen.gf_scene_texture_end = stub_scene_texture_end;
	gr_screen.gf_copy_effect_texture = stub_copy_effect_texture;

	gr_screen.gf_deferred_lighting_begin = stub_deferred_lighting_begin;
	gr_screen.gf_deferred_lighting_msaa = stub_deferred_lighting_msaa;
	gr_screen.gf_deferred_lighting_end = stub_deferred_lighting_end;
	gr_screen.gf_deferred_lighting_finish = stub_deferred_lighting_finish;

	gr_screen.gf_set_line_width = stub_set_line_width;

	gr_screen.gf_sphere = stub_draw_sphere;

	gr_screen.gf_shadow_map_start = stub_shadow_map_start;
	gr_screen.gf_shadow_map_end = stub_shadow_map_end;

	gr_screen.gf_start_decal_pass = stub_start_decal_pass;
	gr_screen.gf_stop_decal_pass = stub_stop_decal_pass;
	gr_screen.gf_render_decals = stub_render_decals;

	gr_screen.gf_render_shield_impact = stub_render_shield_impact;

	gr_screen.gf_maybe_create_shader = stub_maybe_create_shader;

	gr_screen.gf_clear_states = stub_clear_states;

	gr_screen.gf_update_texture = stub_update_texture;
	gr_screen.gf_get_bitmap_from_texture = stub_get_bitmap_from_texture;

	gr_screen.gf_render_model = stub_render_model;
	gr_screen.gf_render_primitives = stub_render_primitives;
	gr_screen.gf_render_primitives_particle = stub_render_primitives_particle;
	gr_screen.gf_render_primitives_distortion = stub_render_primitives_distortion;
	gr_screen.gf_render_movie = stub_render_movie;
	gr_screen.gf_render_nanovg = stub_render_nanovg;
	gr_screen.gf_render_primitives_batched = stub_render_primitives_batched;
	gr_screen.gf_render_rocket_primitives = stub_render_rocket_primitives;

	gr_screen.gf_is_capable = stub_is_capable;
	gr_screen.gf_get_property = stub_get_property;

	gr_screen.gf_push_debug_group = stub_push_debug_group;
	gr_screen.gf_pop_debug_group = stub_pop_debug_group;

	gr_screen.gf_create_query_object = stub_create_query_object;
	gr_screen.gf_query_value = stub_query_value;
	gr_screen.gf_query_value_available = stub_query_value_available;
	gr_screen.gf_get_query_value = stub_get_query_value;
	gr_screen.gf_delete_query_object = stub_delete_query_object;

	gr_screen.gf_create_viewport = [](const os::ViewPortProperties&) { return std::unique_ptr<os::Viewport>(); };
	gr_screen.gf_use_viewport = [](os::Viewport*) {};

	gr_screen.gf_bind_uniform_buffer = [](uniform_block_type, size_t, size_t, gr_buffer_handle) {};

	gr_screen.gf_sync_fence = []() -> gr_sync { return nullptr; };
	gr_screen.gf_sync_wait = [](gr_sync /*sync*/, uint64_t /*timeoutns*/) { return true; };
	gr_screen.gf_sync_delete = [](gr_sync /*sync*/) {};

	gr_screen.gf_set_viewport = [](int /*x*/, int /*y*/, int /*width*/, int /*height*/) {};

	gr_screen.gf_openxr_get_extensions = stub_openxr_get_extensions;
	gr_screen.gf_openxr_test_capabilities = stub_openxr_test_capabilities;
	gr_screen.gf_openxr_create_session = stub_openxr_create_session;
	gr_screen.gf_openxr_get_swapchain_format = stub_openxr_get_swapchain_format;
	gr_screen.gf_openxr_acquire_swapchain_buffers = stub_openxr_acquire_swapchain_buffers;
	gr_screen.gf_openxr_flip = stub_openxr_flip;
}

void init_function_pointers()
{
	// First set all stubs as defaults
	init_stub_pointers();

	// Then override with implemented functions that use the renderer instance
	gr_screen.gf_flip = []() {
		if (renderer_instance) {
			renderer_instance->flip();
		}
	};

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
}
}

void initialize_function_pointers() {
	// Set minimal stubs for functions that might be called before initialize()
	// Full initialization happens in initialize() after renderer is created
	init_stub_pointers();
}

bool initialize(std::unique_ptr<os::GraphicsOperations>&& graphicsOps)
{
	renderer_instance.reset(new VulkanRenderer(std::move(graphicsOps)));
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

} // namespace vulkan
} // namespace graphics
