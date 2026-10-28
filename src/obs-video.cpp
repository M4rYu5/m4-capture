#include "obs-video.hpp"
#include "obs-dmabuf.hpp"

#include <obs-module.h>
#include <utility>

void draw_video_texture(gs_texture_t *texture, const VideoLayout &layout)
{
	auto width = gs_texture_get_width(texture);
	auto height = gs_texture_get_height(texture);
	if (!layout.rotation && !layout.flip && !layout.x && !layout.y && layout.width == width &&
	    layout.height == height) {
		gs_draw_sprite(texture, 0, width, height);
		return;
	}
	// Reuse OBS's immediate vertex buffer for the four-vertex quad.
	gs_render_start(false);
	for (auto corner : {std::pair{0u, 0u}, std::pair{layout.width, 0u}, std::pair{0u, layout.height},
			    std::pair{layout.width, layout.height}}) {
		auto [x, y] = corner;
		uint32_t cx = layout.flip ? layout.width - x : x;
		uint32_t dx = cx, dy = y;
		switch (layout.rotation) {
		case 1:
			dx = y;
			dy = layout.width - cx;
			break;
		case 2:
			dx = layout.width - cx;
			dy = layout.height - y;
			break;
		case 3:
			dx = layout.height - y;
			dy = cx;
			break;
		}
		gs_vertex2f(dx, dy);
		gs_texcoord(float(layout.x + x) / width, float(layout.y + y) / height, 0);
	}
	gs_render_stop(GS_TRISTRIP);
}

ObsVideoRenderer::~ObsVideoRenderer()
{
	obs_enter_graphics();
	gs_texture_destroy(texture);
	obs_leave_graphics();
}

void ObsVideoRenderer::render(const VideoFrame &frame, bool updated)
{
	auto image = obs_frame_texture(frame);
	const auto &layout = frame.layout;
	bool opaque = false;
	if (!image) {
		bool rgb = frame.format == SPA_VIDEO_FORMAT_RGBA || frame.format == SPA_VIDEO_FORMAT_RGBx;
		auto format = rgb ? GS_RGBA : GS_BGRA;
		opaque = frame.format == SPA_VIDEO_FORMAT_BGRx || frame.format == SPA_VIDEO_FORMAT_RGBx;
		if (!texture || gs_texture_get_width(texture) != layout.width ||
		    gs_texture_get_height(texture) != layout.height || gs_texture_get_color_format(texture) != format) {
			gs_texture_destroy(texture);
			texture = gs_texture_create(layout.width, layout.height, format, 1, nullptr, GS_DYNAMIC);
			updated = true;
		}
		if (!texture)
			return;
		if (updated)
			gs_texture_set_image(texture, frame.pixels.data(), layout.width * 4, false);
		image = texture;
	}

	auto effect = obs_get_base_effect(opaque ? OBS_EFFECT_OPAQUE : OBS_EFFECT_DEFAULT);
	bool previous = gs_framebuffer_srgb_enabled();
	// KWin pixels are premultiplied; do not apply alpha to their colors again.
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), image);
	while (gs_effect_loop(effect, "Draw"))
		draw_video_texture(image, layout);
	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
}
