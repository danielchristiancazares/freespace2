#include "VideoPresenter.h"

#include "graphics/matrix.h"

using namespace cutscene;
using namespace cutscene::player;

namespace {
struct movie_vertex {
	vec2d pos;
	vec2d uv;
};
}

namespace cutscene {
namespace player {
VideoPresenter::VideoPresenter(const MovieProperties& props) : _properties(props)
{
	GR_DEBUG_SCOPE("Init video");

	_planeTextureHandles.fill(-1);

	auto w = static_cast<int>(props.size.width);
	auto h = static_cast<int>(props.size.height);

	if (props.pixelFormat == FramePixelFormat::YUV420) {
		_movieTextureHandle = gr_movie_texture_create(static_cast<uint32_t>(w),
			static_cast<uint32_t>(h),
			props.colorSpace,
			props.colorRange);
		if (gr_is_valid(_movieTextureHandle)) {
			_useNativeYCbCr = true;
			return;
		}
		if (gr_screen.mode == GR_VULKAN) {
			return;
		}
		_useLegacyTextures = true;
		_planeTextureBuffers[0].reset(new uint8_t[w * h]);
		_planeTextureHandles[0] = bm_create(8, w, h, _planeTextureBuffers[0].get(), BMP_AABITMAP);

		_planeTextureBuffers[1].reset(new uint8_t[w / 2 * h / 2]);
		_planeTextureHandles[1] = bm_create(8, w / 2, h / 2, _planeTextureBuffers[1].get(), BMP_AABITMAP);

		_planeTextureBuffers[2].reset(new uint8_t[w / 2 * h / 2]);
		_planeTextureHandles[2] = bm_create(8, w / 2, h / 2, _planeTextureBuffers[2].get(), BMP_AABITMAP);

		material_set_movie(&_movie_material, _planeTextureHandles[0], _planeTextureHandles[1], _planeTextureHandles[2]);
		return;
	}

	_useLegacyTextures = true;
	switch (props.pixelFormat) {
	case FramePixelFormat::BGR:
		_planeTextureBuffers[0].reset(new uint8_t[w * h * 3]);
		_planeTextureHandles[0] = bm_create(24, w, h, _planeTextureBuffers[0].get(), 0);

		material_set_unlit(&_rgb_material, _planeTextureHandles[0], 1.0f, false, false);
		break;
	case FramePixelFormat::BGRA:
		_planeTextureBuffers[0].reset(new uint8_t[w * h * 4]);
		_planeTextureHandles[0] = bm_create(32, w, h, _planeTextureBuffers[0].get(), 0);

		material_set_unlit(&_rgb_material, _planeTextureHandles[0], 1.0f, true, false);
		break;
	default:
		UNREACHABLE("Unhandled enum value!");
		break;
	}
}

VideoPresenter::~VideoPresenter() {
	GR_DEBUG_SCOPE("Deinit video");

	if (_useNativeYCbCr && gr_is_valid(_movieTextureHandle)) {
		gr_movie_texture_release(_movieTextureHandle);
		_movieTextureHandle = MovieTextureHandle::Invalid;
	}

	for (auto& handle : _planeTextureHandles) {
		if (handle > 0) {
			bm_release(handle);
			handle = -1;
		}
	}
}

void VideoPresenter::uploadVideoFrame(const VideoFramePtr& frame) {
	GR_DEBUG_SCOPE("Update video frame");

	if (_useNativeYCbCr) {
		if (!gr_is_valid(_movieTextureHandle) || !frame) {
			return;
		}

		if (frame->getPlaneNumber() < 3) {
			return;
		}

		const auto ySize = frame->getPlaneSize(0);
		const auto uSize = frame->getPlaneSize(1);
		const auto vSize = frame->getPlaneSize(2);

		const auto* yData = static_cast<const ubyte*>(frame->getPlaneData(0));
		const auto* uData = static_cast<const ubyte*>(frame->getPlaneData(1));
		const auto* vData = static_cast<const ubyte*>(frame->getPlaneData(2));
		if (!yData || !uData || !vData) {
			return;
		}

		gr_movie_texture_upload(_movieTextureHandle,
			yData, static_cast<int>(ySize.stride),
			uData, static_cast<int>(uSize.stride),
			vData, static_cast<int>(vSize.stride));
		return;
	}

	if (!_useLegacyTextures) {
		return;
	}

	for (size_t i = 0; i < frame->getPlaneNumber(); ++i) {
		auto size = frame->getPlaneSize(i);
		auto data = frame->getPlaneData(i);

		memcpy(_planeTextureBuffers[i].get(), data, size.stride * size.height);

		int bpp = 0;
		switch (_properties.pixelFormat) {
		case FramePixelFormat::YUV420:
			bpp = 8;
			break;
		case FramePixelFormat::BGR:
			bpp = 24;
			break;
		case FramePixelFormat::BGRA:
			bpp = 32;
			break;
		default:
			UNREACHABLE("Unhandled enum value!");
			break;
		}

		gr_update_texture(_planeTextureHandles[i], bpp, _planeTextureBuffers[i].get(), static_cast<int>(size.width),
						  static_cast<int>(size.height));
	}
}

void VideoPresenter::displayFrame(float x1, float y1, float x2, float y2, float alpha) {
	GR_DEBUG_SCOPE("Draw video frame");

	if (_useNativeYCbCr) {
		if (!gr_is_valid(_movieTextureHandle)) {
			return;
		}
		gr_movie_texture_draw(_movieTextureHandle, x1, y1, x2, y2, alpha);
		return;
	}

	if (!_useLegacyTextures) {
		return;
	}

	movie_vertex vertex_data[4];

	vertex_data[0].pos.x = x1;
	vertex_data[0].pos.y = y1;
	vertex_data[0].uv.x = 0.0f;
	vertex_data[0].uv.y = 0.0f;

	vertex_data[1].pos.x = x1;
	vertex_data[1].pos.y = y2;
	vertex_data[1].uv.x = 0.0f;
	vertex_data[1].uv.y = 1.0f;

	vertex_data[2].pos.x = x2;
	vertex_data[2].pos.y = y1;
	vertex_data[2].uv.x = 1.0f;
	vertex_data[2].uv.y = 0.0f;

	vertex_data[3].pos.x = x2;
	vertex_data[3].pos.y = y2;
	vertex_data[3].uv.x = 1.0f;
	vertex_data[3].uv.y = 1.0f;

	// Use the immediate buffer for storing our data since that is exactly what we need
	auto offset = gr_add_to_immediate_buffer(sizeof(vertex_data), vertex_data);

	vertex_layout layout;
	layout.add_vertex_component(vertex_format_data::POSITION2, sizeof(vertex_data[0]), offsetof(movie_vertex, pos));
	layout.add_vertex_component(vertex_format_data::TEX_COORD2, sizeof(vertex_data[0]), offsetof(movie_vertex, uv));

	switch (_properties.pixelFormat) {
	case FramePixelFormat::YUV420:
		material_set_movie(&_movie_material,
				_planeTextureHandles[0],
				_planeTextureHandles[1],
				_planeTextureHandles[2],
				alpha);
		gr_render_movie(&_movie_material, PRIM_TYPE_TRISTRIP, &layout, 4, gr_immediate_buffer_handle, offset);
		break;
	case FramePixelFormat::BGR:
		material_set_unlit(&_rgb_material, _planeTextureHandles[0], alpha, false, false);
		gr_render_primitives(&_rgb_material, PRIM_TYPE_TRISTRIP, &layout, 0, 4, gr_immediate_buffer_handle, offset);
		break;
	case FramePixelFormat::BGRA:
		material_set_unlit(&_rgb_material, _planeTextureHandles[0], alpha, true, false);
		gr_render_primitives(&_rgb_material, PRIM_TYPE_TRISTRIP, &layout, 0, 4, gr_immediate_buffer_handle, offset);
		break;
	default:
		UNREACHABLE("Unhandled enum value!");
		break;
	}
}

}
}
