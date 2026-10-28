#include "pipewire-capture.hpp"

#include <array>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <unistd.h>

PipeWireCapture::PipeWireCapture(std::shared_ptr<FrameBuffer> frames, std::function<void(QString)> failed,
				 std::function<void(PipeWireStatus)> changed, std::shared_ptr<DmaBufCapture> gpu)
    : frames(std::move(frames)), failed(std::move(failed)), changed(std::move(changed)), gpu(std::move(gpu))
{
	cpuOnly = this->gpu && !this->gpu->enabled;
}

void PipeWireCapture::disableGpu()
{
	if (gpu)
		gpu->enabled = false;
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
	loop = pw_thread_loop_new("kwincapture", nullptr);
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
	auto properties = pw_properties_new(PW_KEY_NODE_NAME, "KWinCapture", PW_KEY_MEDIA_TYPE, "Video",
					    PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
	if (!serial.isEmpty())
		pw_properties_set(properties, PW_KEY_TARGET_OBJECT, serial.toUtf8().constData());
	stream = pw_stream_new(core, "KWinCapture", properties);
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
	std::array<uint8_t, 65536> storage;
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, storage.data(), storage.size());
	auto params = buildFormats(builder);
	auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
						  PW_STREAM_FLAG_DONT_RECONNECT);
	if (pw_stream_connect(stream, PW_DIRECTION_INPUT, serial.isEmpty() ? nodeId : PW_ID_ANY, flags, params.data(),
			      params.size()) < 0) {
		disableGpu();
		return false;
	}
	if (pw_thread_loop_start(loop) < 0)
		return false;
	started = true;
	return true;
}

std::vector<const spa_pod *> PipeWireCapture::buildFormats(spa_pod_builder &builder)
{
	std::vector<const spa_pod *> params;
	spa_rectangle preferred{1920, 1080}, minimum{1, 1}, maximum{16384, 16384};
	spa_fraction preferredRate{30, 1}, minimumRate{0, 1}, maximumRate{240, 1};
	if (gpu && !cpuOnly) {
		for (const auto &entry : gpu->formats) {
			if (entry.modifiers.empty())
				continue;
			spa_pod_frame object, choice;
			spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
			spa_pod_builder_add(&builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
					    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
					    SPA_FORMAT_VIDEO_format, SPA_POD_Id(entry.format), 0);
			spa_pod_builder_prop(&builder, SPA_FORMAT_VIDEO_modifier,
					     SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
			spa_pod_builder_push_choice(&builder, &choice, SPA_CHOICE_Enum, 0);
			spa_pod_builder_long(&builder, entry.modifiers.front());
			for (auto modifier : entry.modifiers)
				spa_pod_builder_long(&builder, modifier);
			spa_pod_builder_pop(&builder, &choice);
			spa_pod_builder_add(
			    &builder, SPA_FORMAT_VIDEO_size,
			    SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum), SPA_FORMAT_VIDEO_framerate,
			    SPA_POD_CHOICE_RANGE_Fraction(&preferredRate, &minimumRate, &maximumRate), 0);
			params.push_back(static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &object)));
		}
	}
	params.push_back(static_cast<spa_pod *>(spa_pod_builder_add_object(
	    &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
	    SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
	    SPA_FORMAT_VIDEO_format,
	    SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
				   SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx),
	    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
	    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&preferredRate, &minimumRate, &maximumRate))));
	return params;
}

void PipeWireCapture::fallbackToCpu()
{
	disableGpu();
	cpuOnly = true;
	frames->clear();
	std::array<uint8_t, 65536> storage;
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, storage.data(), storage.size());
	auto params = buildFormats(builder);
	changed(PipeWireStatus::Connecting);
	if (pw_stream_update_params(stream, params.data(), params.size()) < 0)
		fail(QStringLiteral("Could not switch to CPU capture."));
}

void PipeWireCapture::fail(const QString &message)
{
	if (stopping || failedOnce)
		return;
	failedOnce = true;
	frames->clear();
	failed(message);
}

void PipeWireCapture::onState(void *data, pw_stream_state old, pw_stream_state next, const char *error)
{
	auto self = static_cast<PipeWireCapture *>(data);
	if (next == PW_STREAM_STATE_ERROR && !self->failedOnce)
		self->disableGpu();
	if (next == PW_STREAM_STATE_ERROR || next == PW_STREAM_STATE_UNCONNECTED)
		self->fail(QStringLiteral("PipeWire stream disconnected: %1").arg(error ? error : "closed"));
	else if (next == PW_STREAM_STATE_PAUSED) {
		self->frames->clear();
		self->changed(old == PW_STREAM_STATE_STREAMING ? PipeWireStatus::Paused : PipeWireStatus::Connecting);
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
	frames->clear();
	changed(PipeWireStatus::Connecting);
	if (!param)
		return;
	if (spa_format_video_raw_parse(param, &format) < 0 || !supported_video_format(format)) {
		fail(QStringLiteral("The compositor supplied an unsupported video format."));
		return;
	}
	uint8_t storage[1024];
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, storage, sizeof(storage));
	dmaBuf = gpu && !cpuOnly && spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
	std::vector<const spa_pod *> params;
#if PW_CHECK_VERSION(1, 2, 0)
	if (dmaBuf && gpu->explicitSync) {
		spa_pod_frame object;
		spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
		spa_pod_builder_add(&builder, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_DmaBuf), 0);
		spa_pod_builder_prop(&builder, SPA_PARAM_BUFFERS_metaType, SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_int(&builder, 1 << SPA_META_SyncTimeline);
		params.push_back(static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &object)));
		params.push_back(static_cast<spa_pod *>(
		    spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
					       SPA_POD_Id(SPA_META_SyncTimeline), SPA_PARAM_META_size,
					       SPA_POD_Int(sizeof(spa_meta_sync_timeline)))));
	}
#endif
	params.push_back(static_cast<spa_pod *>(spa_pod_builder_add_object(
	    &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
	    SPA_POD_CHOICE_FLAGS_Int(dmaBuf ? (1 << SPA_DATA_DmaBuf)
					    : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))))));
	params.push_back(static_cast<spa_pod *>(spa_pod_builder_add_object(
	    &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
	    SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_region)))));
	params.push_back(static_cast<spa_pod *>(spa_pod_builder_add_object(
	    &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
	    SPA_POD_Id(SPA_META_VideoTransform), SPA_PARAM_META_size, SPA_POD_Int(sizeof(spa_meta_videotransform)))));
	if (pw_stream_update_params(stream, params.data(), params.size()) < 0)
		fail(QStringLiteral("Could not negotiate PipeWire video buffers."));
}

void PipeWireCapture::onProcess(void *data) { static_cast<PipeWireCapture *>(data)->process(); }

void PipeWireCapture::process()
{
	bool notify = false;
	pw_buffer *latest = nullptr;
	while (auto next = pw_stream_dequeue_buffer(stream)) {
		if (latest) {
			if (gpu)
				gpu->discard(*latest->buffer);
			pw_stream_queue_buffer(stream, latest);
		}
		latest = next;
	}
	if (!latest)
		return;
	bool fallback = false;
	if (frames->isEnabled() && !failedOnce && format.size.width && format.size.height) {
		bool isDmaBuf = latest->buffer->n_datas && latest->buffer->datas[0].type == SPA_DATA_DmaBuf;
		FrameResult result;
		if (isDmaBuf && gpu) {
			if (cpuOnly) {
				gpu->discard(*latest->buffer);
				result = FrameResult::Empty;
			} else {
				result = gpu->copy(*latest->buffer, format, scratch);
			}
		} else {
			result = convert_video_frame(*latest->buffer, format, scratch);
		}
		if (isDmaBuf && result != FrameResult::Ready && result != FrameResult::Empty) {
			fallback = true;
		} else
			switch (result) {
			case FrameResult::Ready:
				notify = frames->publish(scratch);
				break;
			case FrameResult::Empty:
				// Metadata-only or corrupted buffers do not replace a valid frame.
				break;
			case FrameResult::UnsupportedFormat:
				fail(QStringLiteral("The compositor supplied an unsupported video format."));
				break;
			case FrameResult::UnmappedBuffer:
				fail(QStringLiteral("The compositor did not provide mapped video buffers."));
				break;
			case FrameResult::InvalidLayout:
				fail(QStringLiteral("PipeWire supplied an invalid video buffer layout."));
				break;
			}
	} else if (gpu) {
		gpu->discard(*latest->buffer);
	}
	pw_stream_queue_buffer(stream, latest);
	if (fallback) {
		fallbackToCpu();
		return;
	}
	if (notify)
		changed(PipeWireStatus::Streaming);
}
