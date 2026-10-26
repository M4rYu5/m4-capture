#include "pipewire-capture.hpp"

#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <cstring>
#include <unistd.h>

PipeWireCapture::PipeWireCapture(std::shared_ptr<CaptureState> state, std::function<void(QString)> failed,
				 std::function<void()> changed)
    : state(std::move(state)), failed(std::move(failed)), changed(std::move(changed))
{
}

PipeWireCapture::~PipeWireCapture()
{
	// Stop/join before destroying anything callbacks can access.
	if (started)
		pw_thread_loop_stop(loop);
	stopping = true;
	if (stream)
		pw_stream_destroy(stream);
	if (registry)
		pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
	if (core)
		pw_core_disconnect(core);
	if (context)
		pw_context_destroy(context);
	if (loop)
		pw_thread_loop_destroy(loop);
}

bool PipeWireCapture::start(int fd, uint32_t node, const QString &serial)
{
	static std::once_flag initialized;
	std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
	nodeId = node;
	loop = pw_thread_loop_new("waylandconnect", nullptr);
	if (!loop) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
	if (!context) {
		if (fd >= 0)
			close(fd);
		return false;
	}
	// connect_fd takes ownership of fd, including on failure.
	core = fd >= 0 ? pw_context_connect_fd(context, fd, nullptr, 0) : pw_context_connect(context, nullptr, 0);
	if (!core)
		return false;
	static const pw_core_events coreEvents = [] {
		pw_core_events events{};
		events.version = PW_VERSION_CORE_EVENTS;
		events.error = onCoreError;
		return events;
	}();
	pw_core_add_listener(core, &coreListener, &coreEvents, this);
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	if (!registry)
		return false;
	static const pw_registry_events registryEvents = [] {
		pw_registry_events events{};
		events.version = PW_VERSION_REGISTRY_EVENTS;
		events.global_remove = onGlobalRemove;
		return events;
	}();
	pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
	auto properties = pw_properties_new(PW_KEY_NODE_NAME, "WaylandConnect", PW_KEY_MEDIA_TYPE, "Video",
					    PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
	if (!serial.isEmpty())
		pw_properties_set(properties, PW_KEY_TARGET_OBJECT, serial.toUtf8().constData());
	stream = pw_stream_new(core, "WaylandConnect", properties);
	if (!stream)
		return false;
	static const pw_stream_events streamEvents = [] {
		pw_stream_events events{};
		events.version = PW_VERSION_STREAM_EVENTS;
		events.state_changed = onState;
		events.param_changed = onParam;
		events.process = onProcess;
		return events;
	}();
	pw_stream_add_listener(stream, &streamListener, &streamEvents, this);
	uint8_t storage[1024];
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, storage, sizeof(storage));
	spa_rectangle preferred{1920, 1080}, minimum{1, 1}, maximum{16384, 16384};
	spa_fraction preferredRate{30, 1}, minimumRate{0, 1}, maximumRate{240, 1};
	const spa_pod *params[] = {static_cast<spa_pod *>(spa_pod_builder_add_object(
	    &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
	    SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	    SPA_FORMAT_VIDEO_format,
	    SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
				   SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx),
	    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
	    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&preferredRate, &minimumRate, &maximumRate)))};
	auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
						  PW_STREAM_FLAG_DONT_RECONNECT);
	if (pw_stream_connect(stream, PW_DIRECTION_INPUT, serial.isEmpty() ? nodeId : PW_ID_ANY, flags, params, 1) < 0)
		return false;
	if (pw_thread_loop_start(loop) < 0)
		return false;
	started = true;
	return true;
}

void PipeWireCapture::fail(const QString &message)
{
	if (stopping || failedOnce)
		return;
	failedOnce = true;
	state->clear(CaptureStatus::Failed, message.toStdString());
	failed(message);
}

void PipeWireCapture::onState(void *data, pw_stream_state old, pw_stream_state next, const char *error)
{
	auto self = static_cast<PipeWireCapture *>(data);
	if (next == PW_STREAM_STATE_ERROR || next == PW_STREAM_STATE_UNCONNECTED)
		self->fail(QStringLiteral("PipeWire stream disconnected: %1").arg(error ? error : "closed"));
	else if (next == PW_STREAM_STATE_PAUSED) {
		self->state->clear(old == PW_STREAM_STATE_STREAMING ? CaptureStatus::Idle : CaptureStatus::Connecting);
		self->changed();
	}
}

void PipeWireCapture::onCoreError(void *data, uint32_t id, int, int, const char *message)
{
	if (id == PW_ID_CORE)
		static_cast<PipeWireCapture *>(data)->fail(
		    QStringLiteral("PipeWire connection failed: %1").arg(message));
}

void PipeWireCapture::onGlobalRemove(void *data, uint32_t id)
{
	auto self = static_cast<PipeWireCapture *>(data);
	if (id == self->nodeId)
		self->fail(QStringLiteral("The captured window or screen is no longer available."));
}

void PipeWireCapture::onParam(void *data, uint32_t id, const spa_pod *param)
{
	if (id == SPA_PARAM_Format)
		static_cast<PipeWireCapture *>(data)->formatChanged(param);
}

void PipeWireCapture::formatChanged(const spa_pod *param)
{
	format = {};
	state->clear(CaptureStatus::Connecting);
	changed();
	if (!param)
		return;
	if (spa_format_video_raw_parse(param, &format) < 0 || !format.size.width || !format.size.height ||
	    format.size.width > 16384 || format.size.height > 16384 ||
	    uint64_t(format.size.width) * format.size.height > 64 * 1024 * 1024 ||
	    (format.format != SPA_VIDEO_FORMAT_BGRA && format.format != SPA_VIDEO_FORMAT_BGRx &&
	     format.format != SPA_VIDEO_FORMAT_RGBA && format.format != SPA_VIDEO_FORMAT_RGBx)) {
		fail(QStringLiteral("The compositor supplied an unsupported video format."));
		return;
	}
	uint8_t storage[1024];
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, storage, sizeof(storage));
	// Request mapped shared-memory buffers, never unmapped GPU DMA buffers.
	const spa_pod *params[] = {
	    static_cast<spa_pod *>(spa_pod_builder_add_object(
		&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
		SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)))),
	    static_cast<spa_pod *>(spa_pod_builder_add_object(
		&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
		SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region)))),
	    static_cast<spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
							      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
							      SPA_PARAM_META_size,
							      SPA_POD_Int(sizeof(spa_meta_videotransform)))),
	};
	if (pw_stream_update_params(stream, params, 3) < 0)
		fail(QStringLiteral("Could not negotiate PipeWire video buffers."));
}

void PipeWireCapture::onProcess(void *data) { static_cast<PipeWireCapture *>(data)->process(); }

void PipeWireCapture::process()
{
	bool notify = false;
	pw_buffer *latest = nullptr;
	while (auto next = pw_stream_dequeue_buffer(stream)) {
		if (latest)
			pw_stream_queue_buffer(stream, latest);
		latest = next;
	}
	if (!latest)
		return;
	auto buffer = latest->buffer;
	if (!failedOnce && state->alive && buffer->n_datas && format.size.width && format.size.height) {
		auto &plane = buffer->datas[0];
		auto chunk = plane.chunk;
		if (!plane.data || !chunk || plane.type == SPA_DATA_DmaBuf) {
			fail(QStringLiteral("The compositor did not provide mapped video buffers."));
		} else if (!chunk->size || (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
			// Metadata-only buffers do not replace a valid frame. Actual disconnects
			// are handled by the stream, node, core, and portal lifecycle events.
		} else {
			int64_t stride = chunk->stride ? chunk->stride : int64_t(format.size.width) * 4;
			int64_t first = chunk->offset;
			int64_t last = first + int64_t(format.size.height - 1) * stride;
			uint64_t end = std::max(first, last) + uint64_t(format.size.width) * 4;
			if (std::min(first, last) < 0 || end > plane.maxsize ||
			    uint64_t(std::abs(stride)) < uint64_t(format.size.width) * 4 ||
			    end - std::min(first, last) > chunk->size) {
				fail(QStringLiteral("PipeWire supplied an invalid video buffer layout."));
			} else {
				uint32_t x = 0, y = 0, width = format.size.width, height = format.size.height;
				auto crop = static_cast<spa_meta_region *>(
				    spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
				if (crop && spa_meta_region_is_valid(crop) && crop->region.position.x >= 0 &&
				    crop->region.position.y >= 0 &&
				    uint64_t(crop->region.position.x) + crop->region.size.width <= width &&
				    uint64_t(crop->region.position.y) + crop->region.size.height <= height) {
					x = crop->region.position.x;
					y = crop->region.position.y;
					width = crop->region.size.width;
					height = crop->region.size.height;
				}
				auto transform = static_cast<spa_meta_videotransform *>(spa_buffer_find_meta_data(
				    buffer, SPA_META_VideoTransform, sizeof(spa_meta_videotransform)));
				uint32_t rotation = transform ? (transform->transform & 3) : 0;
				bool flip = transform && (transform->transform & 4);
				uint32_t outputWidth = (rotation & 1) ? height : width;
				uint32_t outputHeight = (rotation & 1) ? width : height;
				scratch.resize(size_t(outputWidth) * outputHeight * 4);
				bool rgb =
				    format.format == SPA_VIDEO_FORMAT_RGBA || format.format == SPA_VIDEO_FORMAT_RGBx;
				bool opaque =
				    format.format == SPA_VIDEO_FORMAT_BGRx || format.format == SPA_VIDEO_FORMAT_RGBx;
				auto bytes = static_cast<const uint8_t *>(plane.data);
				for (uint32_t row = 0; row < height; ++row) {
					auto input = bytes + first + int64_t(y + row) * stride + x * 4;
					if (!rotation && !flip && !rgb && !opaque) {
						std::memcpy(scratch.data() + size_t(row) * width * 4, input,
							    size_t(width) * 4);
						continue;
					}
					for (uint32_t column = 0; column < width; ++column) {
						uint32_t cx = flip ? width - 1 - column : column;
						uint32_t dx = cx, dy = row;
						if (rotation == 1) {
							dx = row;
							dy = width - 1 - cx;
						} else if (rotation == 2) {
							dx = width - 1 - cx;
							dy = height - 1 - row;
						} else if (rotation == 3) {
							dx = height - 1 - row;
							dy = cx;
						}
						auto pixel = input + column * 4;
						auto output = scratch.data() + (size_t(dy) * outputWidth + dx) * 4;
						output[0] = pixel[rgb ? 2 : 0];
						output[1] = pixel[1];
						output[2] = pixel[rgb ? 0 : 2];
						output[3] = opaque ? 255 : pixel[3];
					}
				}
				std::lock_guard<std::mutex> lock(state->mutex);
				state->pixels.swap(scratch);
				state->width = outputWidth;
				state->height = outputHeight;
				state->hasFrame = true;
				state->frameDirty = true;
				notify = state->status != CaptureStatus::Capturing;
				state->status = CaptureStatus::Capturing;
				state->detail.clear();
			}
		}
	}
	pw_stream_queue_buffer(stream, latest);
	if (notify)
		changed();
}
