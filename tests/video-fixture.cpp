#include "video-fixture.hpp"

#include <QFile>
#include <QTest>

#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

QString TestVideo::socketPath() const { return directory.path() + "/pipewire-wc-test"; }

TestVideo::~TestVideo()
{
	stopProducer();
	if (loop)
		pw_thread_loop_stop(loop);
	for (auto link : links)
		pw_proxy_destroy(link);
	if (registry)
		pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
	if (core)
		pw_core_disconnect(core);
	if (context)
		pw_context_destroy(context);
	if (loop)
		pw_thread_loop_destroy(loop);
	if (daemon.state() != QProcess::NotRunning) {
		daemon.terminate();
		daemon.waitForFinished(3000);
	}
}

bool TestVideo::start()
{
	QFile config(directory.path() + "/test.conf");
	if (!config.open(QIODevice::WriteOnly))
		return false;
	config.write(R"(
context.properties = { core.daemon = true core.name = pipewire-wc-test support.dbus = false }
context.spa-libs = { support.* = support/libspa-support }
context.modules = [
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-spa-node-factory }
  { name = libpipewire-module-client-node }
  { name = libpipewire-module-link-factory }
  { name = libpipewire-module-access args = { access.force = unrestricted } }
]
context.objects = [
  { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver priority.driver = 20000 } }
]
)");
	config.close();
	auto environment = QProcessEnvironment::systemEnvironment();
	environment.insert("PIPEWIRE_RUNTIME_DIR", directory.path());
	// Disable user PipeWire config snippets for this isolated test daemon.
	environment.insert("PIPEWIRE_CONFIG_DIR", directory.path());
	daemon.setProcessEnvironment(environment);
	daemon.start("pipewire", {"-c", config.fileName()});
	if (!daemon.waitForStarted(3000))
		return false;
	for (int attempt = 0; attempt < 100 && !QFile::exists(socketPath()); ++attempt)
		QTest::qWait(10);
	if (!QFile::exists(socketPath())) {
		qWarning() << daemon.readAllStandardError();
		return false;
	}
	pw_init(nullptr, nullptr);
	loop = pw_thread_loop_new("wc-test-video", nullptr);
	if (!loop)
		return false;
	context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
	if (!context)
		return false;
	int fd = openRemote();
	if (fd < 0)
		return false;
	core = pw_context_connect_fd(context, fd, nullptr, 0);
	if (!core)
		return false;
	registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
	static const pw_registry_events events = [] {
		pw_registry_events e{};
		e.version = PW_VERSION_REGISTRY_EVENTS;
		e.global = global;
		return e;
	}();
	pw_registry_add_listener(registry, &registryListener, &events, this);
	return pw_thread_loop_start(loop) == 0;
}

int TestVideo::openRemote() const
{
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	auto path = socketPath().toUtf8();
	if (path.size() >= int(sizeof(address.sun_path))) {
		close(fd);
		return -1;
	}
	std::memcpy(address.sun_path, path.constData(), path.size() + 1);
	if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool TestVideo::startProducer()
{
	pw_thread_loop_lock(loop);
	producer = pw_stream_new(core, "wc-test-video",
				 pw_properties_new(PW_KEY_NODE_NAME, "wc-test-video", PW_KEY_MEDIA_TYPE, "Video",
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
	pw_thread_loop_unlock(loop);
	for (int attempt = 0; attempt < 100 && producerNode == PW_ID_ANY; ++attempt)
		QTest::qWait(10);
	return result == 0 && producerNode != PW_ID_ANY;
}

void TestVideo::stopProducer()
{
	if (!producer)
		return;
	pw_thread_loop_lock(loop);
	pw_stream_destroy(producer);
	producer = nullptr;
	producerNode = PW_ID_ANY;
	pw_thread_loop_unlock(loop);
}

void TestVideo::global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
{
	auto self = static_cast<TestVideo *>(data);
	if (!props)
		return;
	if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
		auto name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
		self->nodes[id] = name ? name : "";
		if (self->nodes[id] == "wc-test-video") {
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

void TestVideo::tryLink()
{
	// No session manager runs in this daemon: explicitly link producer to consumer.
	uint32_t output = PW_ID_ANY, input = PW_ID_ANY, inputNode = PW_ID_ANY;
	for (auto &[id, port] : ports) {
		if (port.second && port.first == producerNode)
			output = id;
		if (!port.second && nodes[port.first] == "WaylandConnect") {
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
	auto link = static_cast<pw_proxy *>(
	    pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &properties->dict, 0));
	if (link)
		links.push_back(link);
	pw_properties_free(properties);
}

void TestVideo::paramChanged(void *data, uint32_t id, const spa_pod *param)
{
	if (id != SPA_PARAM_Format || !param)
		return;
	auto self = static_cast<TestVideo *>(data);
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

void TestVideo::process(void *data)
{
	auto self = static_cast<TestVideo *>(data);
	auto buffer = pw_stream_dequeue_buffer(self->producer);
	if (!buffer)
		return;
	auto &plane = buffer->buffer->datas[0];
	if (plane.data && plane.maxsize >= 160 * 90 * 4) {
		auto pixels = static_cast<uint32_t *>(plane.data);
		std::fill(pixels, pixels + 160 * 90, 0xffe08020u);
		plane.chunk->offset = 0;
		plane.chunk->stride = 160 * 4;
		plane.chunk->size = 160 * 90 * 4;
	}
	pw_stream_queue_buffer(self->producer, buffer);
}
