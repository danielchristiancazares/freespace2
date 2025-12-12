#pragma once

#include "graphics/2d.h"

#include <cstdint>

namespace graphics::vulkan {

struct ClipScissorRect {
	int32_t x = 0;
	int32_t y = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};

inline ClipScissorRect getClipScissorFromScreen(const ::screen& screen)
{
	ClipScissorRect rect{};
	rect.x = static_cast<int32_t>(screen.offset_x);
	rect.y = static_cast<int32_t>(screen.offset_y);
	rect.width = static_cast<uint32_t>(screen.clip_width);
	rect.height = static_cast<uint32_t>(screen.clip_height);
	return rect;
}

// Updates global gr_screen clip state using the engine's clip semantics (offset + width/height, with clip_{left,top}
// remaining 0). This intentionally mirrors the state updates performed by the OpenGL backend, without any GL calls.
//
// Note: This operates on global gr_screen since the resize helpers (gr_resize_screen_pos / gr_unsize_screen_pos)
// reference global state.
inline void applyClipToScreen(int x, int y, int w, int h, int resize_mode)
{
	// Sanity clamp input
	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}

	const bool to_resize = (resize_mode != GR_RESIZE_NONE && resize_mode != GR_RESIZE_REPLACE
	                        && (gr_screen.custom_size || (gr_screen.rendering_to_texture != -1)));

	int max_w = to_resize ? gr_screen.max_w_unscaled : gr_screen.max_w;
	int max_h = to_resize ? gr_screen.max_h_unscaled : gr_screen.max_h;

	if ((gr_screen.rendering_to_texture != -1) && to_resize) {
		gr_unsize_screen_pos(&max_w, &max_h);
	}

	if (resize_mode != GR_RESIZE_REPLACE) {
		if (x >= max_w) {
			x = max_w - 1;
		}
		if (y >= max_h) {
			y = max_h - 1;
		}

		if (x + w > max_w) {
			w = max_w - x;
		}
		if (y + h > max_h) {
			h = max_h - y;
		}

		if (w > max_w) {
			w = max_w;
		}
		if (h > max_h) {
			h = max_h;
		}
	}

	gr_screen.offset_x_unscaled = x;
	gr_screen.offset_y_unscaled = y;
	gr_screen.clip_left_unscaled = 0;
	gr_screen.clip_right_unscaled = w - 1;
	gr_screen.clip_top_unscaled = 0;
	gr_screen.clip_bottom_unscaled = h - 1;
	gr_screen.clip_width_unscaled = w;
	gr_screen.clip_height_unscaled = h;

	if (to_resize) {
		gr_resize_screen_pos(&x, &y, &w, &h, resize_mode);
	} else {
		gr_unsize_screen_pos(&gr_screen.offset_x_unscaled, &gr_screen.offset_y_unscaled);
		gr_unsize_screen_pos(&gr_screen.clip_right_unscaled, &gr_screen.clip_bottom_unscaled);
		gr_unsize_screen_pos(&gr_screen.clip_width_unscaled, &gr_screen.clip_height_unscaled);
	}

	gr_screen.offset_x = x;
	gr_screen.offset_y = y;
	gr_screen.clip_left = 0;
	gr_screen.clip_right = w - 1;
	gr_screen.clip_top = 0;
	gr_screen.clip_bottom = h - 1;
	gr_screen.clip_width = w;
	gr_screen.clip_height = h;

	gr_screen.clip_aspect = i2fl(w) / i2fl(h);
	gr_screen.clip_center_x = (gr_screen.clip_left + gr_screen.clip_right) * 0.5f;
	gr_screen.clip_center_y = (gr_screen.clip_top + gr_screen.clip_bottom) * 0.5f;
}

} // namespace graphics::vulkan

