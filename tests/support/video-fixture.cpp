#include "video-fixture.hpp"

#include <QTest>
#include <algorithm>
#include <cstring>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

VideoProducer::~VideoProducer()
{
	stop();
	if (!registry)
		return;
	pw_thread_loop_lock(server.threadLoop());
	for (auto link : links)
		pw_proxy_destroy(link);
	pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
	pw_thread_loop_unlock(server.threadLoop());
}

bool VideoProducer::setActive(bool active)
{
	pw_thread_loop_lock(server.threadLoop());
	int result = pw_stream_set_active(producer, active);
	pw_thread_loop_unlock(server.threadLoop());
	return result == 0;
}

bool VideoProducer::start()
{
	pw_thread_loop_lock(server.threadLoop());
	if (!registry) {
		registry = pw_core_get_registry(server.connection(), PW_VERSION_REGISTRY, 0);
		static const pw_registry_events registryEvents = [] {
			pw_registry_events e{};
			e.version = PW_VERSION_REGISTRY_EVENTS;
			e.global = global;
			return e;
		}();
		pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
	}
	producer = pw_stream_new(server.connection(), "m4capture-test-video",
				 pw_properties_new(PW_KEY_NODE_NAME, "m4capture-test-video", PW_KEY_MEDIA_TYPE, "Video",
						   PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS, "Video/Source",
						   PW_KEY_NODE_WANT_DRIVER, "true", nullptr));
	static const pw_stream_events events = [] {
		pw_stream_events e{};
		e.version = PW_VERSION_STREAM_EVENTS;
		e.param_changed = paramChanged;
		e.process = process;
		return e;
	}();
	pw_stream_add_listener(producer, &streamListener, &events, this);
	uint8_t bytes[1024];
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, bytes, sizeof(bytes));
	spa_video_info_raw format{};
	format.format = SPA_VIDEO_FORMAT_BGRA;
	format.size = {160, 90};
	format.framerate = {25, 1};
	const spa_pod *params[] = {spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
	int result = pw_stream_connect(producer, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
	pw_thread_loop_unlock(server.threadLoop());
	for (int attempt = 0; attempt < 100 && producerNode == PW_ID_ANY; ++attempt)
		QTest::qWait(10);
	return result == 0 && producerNode != PW_ID_ANY;
}

void VideoProducer::stop()
{
	if (!producer)
		return;
	pw_thread_loop_lock(server.threadLoop());
	pw_stream_destroy(producer);
	producer = nullptr;
	producerNode = PW_ID_ANY;
	pw_thread_loop_unlock(server.threadLoop());
}

void VideoProducer::global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
{
	auto self = static_cast<VideoProducer *>(data);
	if (!props)
		return;
	if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		auto name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		self->nodes[id] = name ? name : "";
		if (self->nodes[id] == "m4capture-test-video") {
			auto serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
			self->producerSerial = serial ? std::stoull(serial) : 0;
			self->producerNode = id;
		}
	} else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
		auto node = spa_dict_lookup(props, PW_KEY_NODE_ID);
		auto direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
		if (node && direction) {
			self->ports[id] = {uint32_t(std::stoul(node)), std::strcmp(direction, "out") == 0};
		}
	}
	self->tryLink();
}

void VideoProducer::tryLink()
{
	// No session manager runs in this daemon: explicitly link producer to consumer.
	uint32_t output = PW_ID_ANY, input = PW_ID_ANY, inputNode = PW_ID_ANY;
	for (auto &[id, port] : ports) {
		if (port.second && port.first == producerNode)
			output = id;
		if (!port.second && nodes[port.first] == "M4Capture") {
			input = id;
			inputNode = port.first;
		}
	}
	if (input == PW_ID_ANY || output == PW_ID_ANY)
		return;
	ports.erase(input); // Link each consumer port only once.
	auto properties = pw_properties_new(nullptr, nullptr);
	pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", producerNode.load());
	pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u", output);
	pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", inputNode);
	pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u", input);
	auto link = static_cast<pw_proxy *>(pw_core_create_object(
	    server.connection(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &properties->dict, 0));
	if (link)
		links.push_back(link);
	pw_properties_free(properties);
}

void VideoProducer::paramChanged(void *data, uint32_t id, const spa_pod *param)
{
	if (id != SPA_PARAM_Format || !param)
		return;
	auto self = static_cast<VideoProducer *>(data);
	uint8_t bytes[1024];
	spa_pod_builder builder;
	spa_pod_builder_init(&builder, bytes, sizeof(bytes));
	const spa_pod *params[] = {static_cast<spa_pod *>(spa_pod_builder_add_object(
	    &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
	    SPA_POD_CHOICE_RANGE_Int(8, 2, 16), SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
	    SPA_POD_Int(160 * 90 * 4), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(160 * 4), SPA_PARAM_BUFFERS_dataType,
	    SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))))};
	pw_stream_update_params(self->producer, params, 1);
}

void VideoProducer::process(void *data)
{
	auto self = static_cast<VideoProducer *>(data);
	auto buffer = pw_stream_dequeue_buffer(self->producer);
	if (!buffer)
		return;
	auto &plane = buffer->buffer->datas[0];
	if (plane.data && plane.maxsize >= 160 * 90 * 4) {
		auto pixels = static_cast<uint32_t *>(plane.data);
		std::fill(pixels, pixels + 160 * 90, self->bgra.load());
		plane.chunk->offset = 0;
		plane.chunk->stride = 160 * 4;
		plane.chunk->size = 160 * 90 * 4;
	}
	pw_stream_queue_buffer(self->producer, buffer);
}
