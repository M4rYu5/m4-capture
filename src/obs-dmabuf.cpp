#include "obs-dmabuf.hpp"

#include <GL/gl.h>
#include <QtGlobal>
#include <drm_fourcc.h>
#include <obs-module.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>

#include <algorithm>
#include <array>

namespace
{
struct GraphicsLock {
	GraphicsLock() { obs_enter_graphics(); }
	~GraphicsLock() { obs_leave_graphics(); }
};

struct ObsGpuFrame : GpuFrame {
	gs_texrender_t *target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	~ObsGpuFrame() override
	{
		GraphicsLock lock;
		gs_texrender_destroy(target);
	}
};

uint32_t drmFormat(uint32_t format)
{
	switch (format) {
	case SPA_VIDEO_FORMAT_BGRA:
		return DRM_FORMAT_ARGB8888;
	case SPA_VIDEO_FORMAT_BGRx:
		return DRM_FORMAT_XRGB8888;
	case SPA_VIDEO_FORMAT_RGBA:
		return DRM_FORMAT_ABGR8888;
	case SPA_VIDEO_FORMAT_RGBx:
		return DRM_FORMAT_XBGR8888;
	default:
		return 0;
	}
}

// All buffer access happens before this scope ends. Fence the GPU read before
// returning the buffer, including failures, dropped frames and paused streams.
class BufferRelease
{
      public:
	spa_meta_sync_timeline *timeline = nullptr;
	int acquireFd = -1, releaseFd = -1;
	bool used = false;
	explicit BufferRelease(const spa_buffer &buffer)
	{
#if PW_CHECK_VERSION(1, 2, 0)
		timeline = static_cast<spa_meta_sync_timeline *>(
		    spa_buffer_find_meta_data(&buffer, SPA_META_SyncTimeline, sizeof(spa_meta_sync_timeline)));
		if (timeline && buffer.n_datas >= 3 && buffer.datas[buffer.n_datas - 2].type == SPA_DATA_SyncObj &&
		    buffer.datas[buffer.n_datas - 1].type == SPA_DATA_SyncObj) {
			acquireFd = buffer.datas[buffer.n_datas - 2].fd;
			releaseFd = buffer.datas[buffer.n_datas - 1].fd;
		} else {
			timeline = nullptr;
		}
#else
		(void)buffer;
#endif
	}
	bool acquire()
	{
		if (!timeline)
			return true;
		auto sync = gs_sync_create_from_syncobj_timeline_point(acquireFd, timeline->acquire_point);
		if (!sync)
			return false;
		bool ready = gs_sync_wait(sync);
		gs_sync_destroy(sync);
		return ready;
	}
	~BufferRelease()
	{
		if (used && timeline) {
			auto sync = gs_sync_create();
			bool exported =
			    sync && gs_sync_export_syncobj_timeline_point(sync, releaseFd, timeline->release_point);
			if (sync)
				gs_sync_destroy(sync);
			if (exported) {
#ifdef SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE
				timeline->flags &= ~SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE;
#endif
				return;
			}
		}
		// Older producers can use implicit synchronization. Wait for our read
		// before returning their buffer; never let them overwrite a queued draw.
		if (used)
			glFinish();
		if (timeline) {
			gs_sync_signal_syncobj_timeline_point(releaseFd, timeline->release_point);
#ifdef SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE
			timeline->flags &= ~SPA_META_SYNC_TIMELINE_UNSCHEDULED_RELEASE;
#endif
		}
	}
};

class ObsDmaBufCapture : public DmaBufCapture
{
      public:
	FrameResult copy(const spa_buffer &buffer, const spa_video_info_raw &format, VideoFrame &output) override
	{
		GraphicsLock lock;
		BufferRelease release(buffer);
		if (!supported_video_format(format))
			return FrameResult::UnsupportedFormat;
		uint32_t planes = 0;
		std::array<int, 4> fds;
		std::array<uint32_t, 4> offsets, strides;
		std::array<uint64_t, 4> modifiers;
		while (planes < buffer.n_datas && buffer.datas[planes].type == SPA_DATA_DmaBuf) {
			const auto &plane = buffer.datas[planes];
			if (planes >= fds.size() || plane.fd < 0 || !plane.chunk || plane.chunk->stride <= 0)
				return FrameResult::InvalidLayout;
			if (plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
				return FrameResult::Empty;
			fds[planes] = plane.fd;
			offsets[planes] = plane.chunk->offset;
			strides[planes] = plane.chunk->stride;
			modifiers[planes] = format.modifier;
			++planes;
		}
		if (!planes)
			return FrameResult::InvalidLayout;
		if (!release.acquire())
			return FrameResult::UnmappedBuffer;
		auto texture = gs_texture_create_from_dmabuf(
		    format.size.width, format.size.height, drmFormat(format.format), GS_BGRA, planes, fds.data(),
		    strides.data(), offsets.data(),
		    format.modifier == DRM_FORMAT_MOD_INVALID ? nullptr : modifiers.data());
		if (!texture)
			return FrameResult::UnmappedBuffer;
		auto layout = video_layout(buffer, format);
		auto frame = std::dynamic_pointer_cast<ObsGpuFrame>(output.gpu);
		if (!frame)
			frame = std::make_shared<ObsGpuFrame>();
		gs_texrender_reset(frame->target);
		if (!gs_texrender_begin(frame->target, layout.outputWidth(), layout.outputHeight())) {
			gs_texture_destroy(texture);
			return FrameResult::UnmappedBuffer;
		}
		bool srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_blend_state_push();
		gs_enable_blending(false);
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();
		gs_ortho(0, layout.outputWidth(), 0, layout.outputHeight(), -100, 100);
		bool opaque = format.format == SPA_VIDEO_FORMAT_BGRx || format.format == SPA_VIDEO_FORMAT_RGBx;
		auto effect = obs_get_base_effect(opaque ? OBS_EFFECT_OPAQUE : OBS_EFFECT_DEFAULT);
		// Copy encoded color values, preserving premultiplied alpha. The source
		// render performs the same sRGB conversion as the CPU upload path.
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
		while (gs_effect_loop(effect, "Draw")) {
			// Reuse OBS's immediate vertex buffer for the four-vertex quad.
			gs_render_start(false);
			for (auto corner : {std::pair{0u, 0u}, std::pair{layout.width, 0u},
					    std::pair{0u, layout.height}, std::pair{layout.width, layout.height}}) {
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
				gs_texcoord(float(layout.x + x) / format.size.width,
					    float(layout.y + y) / format.size.height, 0);
			}
			gs_render_stop(GS_TRISTRIP);
		}
		release.used = true;
		gs_matrix_pop();
		gs_projection_pop();
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(srgb);
		gs_texrender_end(frame->target);
		gs_texture_destroy(texture);
		output.width = layout.outputWidth();
		output.height = layout.outputHeight();
		output.pixels.clear();
		output.gpu = std::move(frame);
		return FrameResult::Ready;
	}
	void discard(const spa_buffer &buffer) override
	{
		if (!buffer.n_datas || buffer.datas[0].type != SPA_DATA_DmaBuf)
			return;
		GraphicsLock lock;
		BufferRelease release(buffer);
	}
};
} // namespace

std::shared_ptr<DmaBufCapture> create_obs_dmabuf_capture()
{
	if (qEnvironmentVariableIsSet("KWINCAPTURE_FORCE_CPU"))
		return {};
	GraphicsLock lock;
	if (gs_get_device_type() != GS_DEVICE_OPENGL)
		return {};
	gs_dmabuf_flags flags{};
	uint32_t *formats = nullptr;
	size_t count = 0;
	if (!gs_query_dmabuf_capabilities(&flags, &formats, &count)) {
		bfree(formats);
		return {};
	}
	auto capture = std::make_shared<ObsDmaBufCapture>();
	for (auto format :
	     {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx}) {
		auto drm = drmFormat(format);
		if (count && std::find(formats, formats + count, drm) == formats + count)
			continue;
		uint64_t *modifiers = nullptr;
		size_t modifierCount = 0;
		gs_query_dmabuf_modifiers_for_format(drm, &modifiers, &modifierCount);
		DmaBufFormat entry{uint32_t(format), {}};
		if (modifierCount)
			entry.modifiers.assign(modifiers, modifiers + modifierCount);
		if (flags & GS_DMABUF_FLAG_IMPLICIT_MODIFIERS_SUPPORTED)
			entry.modifiers.push_back(DRM_FORMAT_MOD_INVALID);
		bfree(modifiers);
		if (!entry.modifiers.empty())
			capture->formats.push_back(std::move(entry));
	}
	bfree(formats);
#if PW_CHECK_VERSION(1, 2, 0)
	capture->explicitSync = gs_query_sync_capabilities();
#endif
	return capture->formats.empty() ? nullptr : capture;
}

gs_texture_t *obs_frame_texture(const VideoFrame &frame)
{
	auto gpu = std::static_pointer_cast<ObsGpuFrame>(frame.gpu);
	return gpu ? gs_texrender_get_texture(gpu->target) : nullptr;
}
