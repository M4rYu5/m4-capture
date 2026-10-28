#pragma once

#include "video-frame.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <array>
#include <drm_fourcc.h>
#include <graphics/graphics.h>
#include <obs.h>
#include <spa/buffer/meta.h>
#include <unistd.h>

// Export a real texture on the current OBS GPU. No compositor/window contents
// are involved, and fd ownership lasts until after the import tests finish.
struct TestDmaBuf {
	gs_texture_t *texture = nullptr;
	std::array<spa_data, 4> planes{};
	std::array<spa_chunk, 4> chunks{};
	std::array<int, 4> fds{-1, -1, -1, -1};
	spa_video_info_raw format{};
	spa_meta_region crop{};
	spa_meta_videotransform transform{};
	spa_meta metadata[2]{{SPA_META_VideoCrop, sizeof(crop), &crop},
			     {SPA_META_VideoTransform, sizeof(transform), &transform}};
	spa_buffer buffer{};
	std::vector<uint8_t> pixels;

	TestDmaBuf()
	{
		obs_enter_graphics();
		format.size = {16, 12};
		for (unsigned y = 0; y < format.size.height; ++y)
			for (unsigned x = 0; x < format.size.width; ++x) {
				uint8_t alpha = (x % 3) * 127;
				pixels.insert(pixels.end(), {uint8_t(x * alpha / 16), uint8_t(y * alpha / 12),
							     uint8_t((x + y) * alpha / 28), alpha});
			}
		const auto *data = pixels.data();
		texture = gs_texture_create(16, 12, GS_BGRA, 1, &data, 0);
		glFinish();
		auto query = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(
		    eglGetProcAddress("eglExportDMABUFImageQueryMESA"));
		auto exportImage =
		    reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));
		if (texture && query && exportImage) {
			const EGLAttrib attributes[] = {EGL_GL_TEXTURE_LEVEL, 0, EGL_IMAGE_PRESERVED, EGL_TRUE,
							EGL_NONE};
			auto display = eglGetCurrentDisplay();
			auto image = eglCreateImage(display, eglGetCurrentContext(), EGL_GL_TEXTURE_2D,
						    reinterpret_cast<EGLClientBuffer>(
							uintptr_t(*static_cast<GLuint *>(gs_texture_get_obj(texture)))),
						    attributes);
			int drm = 0, count = 0;
			EGLuint64KHR modifier = 0;
			std::array<EGLint, 4> strides{}, offsets{};
			if (image != EGL_NO_IMAGE && query(display, image, &drm, &count, &modifier) && count > 0 &&
			    count <= 4 && exportImage(display, image, fds.data(), strides.data(), offsets.data())) {
				format.modifier = modifier;
				switch (drm) {
				case DRM_FORMAT_ARGB8888:
					format.format = SPA_VIDEO_FORMAT_BGRA;
					break;
				case DRM_FORMAT_ABGR8888:
					format.format = SPA_VIDEO_FORMAT_RGBA;
					break;
				default:
					break;
				}
				for (int i = 0; i < count; ++i) {
					chunks[i].stride = strides[i];
					chunks[i].offset = offsets[i];
					planes[i].type = SPA_DATA_DmaBuf;
					planes[i].fd = fds[i];
					planes[i].chunk = &chunks[i];
				}
				buffer.n_datas = count;
				buffer.datas = planes.data();
				buffer.n_metas = 2;
				buffer.metas = metadata;
			}
			if (image != EGL_NO_IMAGE)
				eglDestroyImage(display, image);
		}
		glFinish();
		obs_leave_graphics();
	}
	~TestDmaBuf()
	{
		for (auto fd : fds)
			if (fd >= 0)
				close(fd);
		obs_enter_graphics();
		gs_texture_destroy(texture);
		obs_leave_graphics();
	}
};
