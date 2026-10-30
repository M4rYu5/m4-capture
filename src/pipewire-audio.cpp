#include "pipewire-audio.hpp"

#include <QFile>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/node/io.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <semaphore.h>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

namespace
{
constexpr std::array<const char *, 9> channels = {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR", "MONO"};
QString property(const spa_dict *props, const char *key)
{
	const auto value = props ? spa_dict_lookup(props, key) : nullptr;
	return QString::fromUtf8(value ? value : "");
}

QString applicationName(QString name)
{
	if (name.endsWith(".desktop", Qt::CaseInsensitive))
		name.chop(8);
	return name.toCaseFolded();
}

bool belongsToProcess(uint32_t pid, uint32_t target)
{
	// Browsers and games may play audio from a child of the window process.
	for (int depth = 0; pid > 1 && target > 1 && depth < 64; ++depth) {
		if (pid == target)
			return true;
		QFile status(QStringLiteral("/proc/%1/status").arg(pid));
		if (!status.open(QIODevice::ReadOnly))
			break;
		const auto contents = status.readAll();
		const auto start = contents.indexOf("\nPPid:");
		if (start < 0)
			break;
		const auto end = contents.indexOf('\n', start + 1);
		const auto parent = contents.mid(start + 6, end - start - 6).trimmed().toUInt();
		if (parent == pid)
			break;
		pid = parent;
	}
	return false;
}
} // namespace

struct PipeWireAudioCapture::Impl {
	QString appId;
	uint32_t pid;
	std::function<void(const AudioFrame &)> output;
	std::function<void(QString)> failed;
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_registry *registry = nullptr;
	pw_filter *filter = nullptr;
	spa_hook registryListener{}, coreListener{}, filterListener{};
	bool started = false;
	bool failedOnce = false;
	std::atomic<bool> stopping{false};
	std::array<void *, channels.size()> inputs{};
	// One producer (PipeWire RT) and one consumer (OBS delivery). A full queue
	// drops new samples rather than blocking the graph or growing its latency.
	struct AudioBlock {
		std::array<std::array<float, 1024>, 2> samples;
		uint32_t frames, rate;
		uint64_t timestamp;
	};
	std::array<AudioBlock, 8> outputQueue{};
	std::atomic<uint32_t> written{0}, read{0};
	static_assert(std::atomic<uint32_t>::is_always_lock_free);
	sem_t outputReady;
	bool outputReadyInitialized = false;
	std::thread outputThread;

	struct Identity {
		QString mediaClass, processId, appId, binary, name;
		bool operator==(const Identity &other) const
		{
			return std::tie(mediaClass, processId, appId, binary, name) ==
			       std::tie(other.mediaClass, other.processId, other.appId, other.binary, other.name);
		}
	};
	struct Object {
		Impl *owner;
		uint32_t id;
		pw_proxy *proxy = nullptr;
		spa_hook listener{};
		pw_properties *props = nullptr;
		// Includes client properties when the node does not supply its own.
		Identity identity;
		bool matched = false;
		~Object()
		{
			if (proxy) {
				spa_hook_remove(&listener);
				pw_proxy_destroy(proxy);
			}
			if (props)
				pw_properties_free(props);
		}
	};
	struct Port {
		uint32_t node;
		size_t channel;
		bool output;
	};
	std::map<uint32_t, std::unique_ptr<Object>> nodes, clients;
	std::map<uint32_t, Port> ports;
	std::map<std::pair<uint32_t, uint32_t>, pw_proxy *> links;

	~Impl()
	{
		stopping = true;
		if (started)
			pw_thread_loop_stop(loop);
		for (auto &[key, link] : links)
			pw_proxy_destroy(link);
		if (filter)
			pw_filter_destroy(filter);
		if (outputThread.joinable()) {
			sem_post(&outputReady);
			outputThread.join();
		}
		if (outputReadyInitialized)
			sem_destroy(&outputReady);
		nodes.clear();
		clients.clear();
		if (registry)
			pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
		if (core)
			pw_core_disconnect(core);
		if (context)
			pw_context_destroy(context);
		if (loop)
			pw_thread_loop_destroy(loop);
	}

	void fail(const QString &error)
	{
		if (!stopping && !failedOnce) {
			failedOnce = true;
			failed(error);
		}
	}

	void deliverAudio()
	{
		for (;;) {
			if (sem_wait(&outputReady) < 0) {
				if (errno == EINTR)
					continue;
				return;
			}
			if (stopping)
				return;
			const auto index = read.load(std::memory_order_relaxed);
			if (index == written.load(std::memory_order_acquire))
				continue;
			const auto &block = outputQueue[index % outputQueue.size()];
			output({{block.samples[0].data(), block.samples[1].data()},
				block.frames,
				block.rate,
				block.timestamp});
			read.store(index + 1, std::memory_order_release);
		}
	}

	bool matches(const Identity &identity) const
	{
		if (identity.mediaClass != "Stream/Output/Audio")
			return false;
		const auto id = applicationName(identity.appId);
		if (!appId.isEmpty() && id == appId)
			return true;
		if (belongsToProcess(identity.processId.toUInt(), pid))
			return true;
		if (!id.isEmpty())
			return false;
		// Some PulseAudio clients publish only an executable/application name.
		const auto shortName = appId.section('.', -1);
		const auto binary = applicationName(identity.binary);
		const auto name = applicationName(identity.name);
		return !appId.isEmpty() &&
		       (binary == appId || binary == shortName || name == appId || name == shortName);
	}

	void refreshNode(Object &node)
	{
		const auto nodeProps = &node.props->dict;
		const auto client = clients.find(property(nodeProps, PW_KEY_CLIENT_ID).toUInt());
		const auto clientProps = client != clients.end() ? &client->second->props->dict : nullptr;
		auto value = [&](const char *key) {
			auto result = property(nodeProps, key);
			return result.isEmpty() ? property(clientProps, key) : result;
		};
		Identity identity{property(nodeProps, PW_KEY_MEDIA_CLASS), value(PW_KEY_APP_PROCESS_ID),
				  value(PW_KEY_APP_ID), value(PW_KEY_APP_PROCESS_BINARY), value(PW_KEY_APP_NAME)};
		if (identity == node.identity)
			return;
		node.identity = std::move(identity);
		const bool matched = matches(node.identity);
		if (matched != node.matched) {
			node.matched = matched;
			reconcile();
		}
	}

	void refreshClient(uint32_t id)
	{
		for (const auto &[nodeId, node] : nodes)
			if (property(&node->props->dict, PW_KEY_CLIENT_ID).toUInt() == id)
				refreshNode(*node);
	}

	bool matchedNode(uint32_t id) const
	{
		const auto node = nodes.find(id);
		return node != nodes.end() && node->second->matched;
	}

	void reconcile()
	{
		if (stopping || failedOnce || !filter)
			return;
		const auto ownNode = pw_filter_get_node_id(filter);
		if (ownNode == PW_ID_ANY)
			return;
		std::set<std::pair<uint32_t, uint32_t>> wanted;
		std::array<uint32_t, channels.size()> inputPorts;
		inputPorts.fill(PW_ID_ANY);
		for (const auto &[id, port] : ports)
			if (!port.output && port.node == ownNode)
				inputPorts[port.channel] = id;
		for (const auto &[outputId, out] : ports) {
			if (!out.output || !matchedNode(out.node))
				continue;
			if (inputPorts[out.channel] != PW_ID_ANY)
				wanted.emplace(outputId, inputPorts[out.channel]);
		}
		for (auto it = links.begin(); it != links.end();) {
			if (!wanted.count(it->first)) {
				pw_proxy_destroy(it->second);
				it = links.erase(it);
			} else {
				++it;
			}
		}
		for (const auto &key : wanted) {
			if (links.count(key))
				continue;
			auto props = pw_properties_new(nullptr, nullptr);
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", ports.at(key.first).node);
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", key.first);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", ownNode);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", key.second);
			auto link = static_cast<pw_proxy *>(pw_core_create_object(
			    core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
			pw_properties_free(props);
			if (link)
				links.emplace(key, link);
			else
				fail(QStringLiteral("Could not link application audio."));
		}
	}

	static void global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props)
	{
		auto self = static_cast<Impl *>(data);
		if (!props || self->stopping)
			return;
		if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0 ||
		    std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
			bool isNode = std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0;
			auto object = std::make_unique<Object>();
			object->owner = self;
			object->id = id;
			object->props = pw_properties_new_dict(props);
			object->proxy = static_cast<pw_proxy *>(pw_registry_bind(
			    self->registry, id, type, isNode ? PW_VERSION_NODE : PW_VERSION_CLIENT, 0));
			if (!object->proxy)
				return;
			static const pw_node_events nodeEvents = [] {
				pw_node_events e{};
				e.version = PW_VERSION_NODE_EVENTS;
				e.info = [](void *data, const pw_node_info *info) {
					auto object = static_cast<Object *>(data);
					if (info->props && pw_properties_update(object->props, info->props) > 0)
						object->owner->refreshNode(*object);
				};
				return e;
			}();
			static const pw_client_events clientEvents = [] {
				pw_client_events e{};
				e.version = PW_VERSION_CLIENT_EVENTS;
				e.info = [](void *data, const pw_client_info *info) {
					auto object = static_cast<Object *>(data);
					if (info->props && pw_properties_update(object->props, info->props) > 0)
						object->owner->refreshClient(object->id);
				};
				return e;
			}();
			auto entry = object.get();
			(isNode ? self->nodes : self->clients).emplace(id, std::move(object));
			if (isNode) {
				pw_node_add_listener(reinterpret_cast<pw_node *>(entry->proxy), &entry->listener,
						     &nodeEvents, entry);
				self->refreshNode(*entry);
			} else {
				pw_client_add_listener(reinterpret_cast<pw_client *>(entry->proxy), &entry->listener,
						       &clientEvents, entry);
				self->refreshClient(id);
			}
		} else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
			auto channel = property(props, PW_KEY_AUDIO_CHANNEL);
			if (channel == "AUX0")
				channel = "FL";
			if (channel == "AUX1")
				channel = "FR";
			const auto found = std::find(channels.begin(), channels.end(), channel);
			if (found == channels.end())
				return;
			const auto node = property(props, PW_KEY_NODE_ID).toUInt();
			self->ports[id] = {node, size_t(found - channels.begin()),
					   property(props, PW_KEY_PORT_DIRECTION) == "out"};
			if (self->matchedNode(node) || node == pw_filter_get_node_id(self->filter))
				self->reconcile();
		}
	}

	static void globalRemove(void *data, uint32_t id)
	{
		auto self = static_cast<Impl *>(data);
		bool changed = self->matchedNode(id);
		if (const auto port = self->ports.find(id); port != self->ports.end()) {
			changed |= self->matchedNode(port->second.node) ||
				   port->second.node == pw_filter_get_node_id(self->filter);
			self->ports.erase(port);
		}
		self->nodes.erase(id);
		if (self->clients.erase(id))
			self->refreshClient(id);
		if (changed)
			self->reconcile();
	}

	static void process(void *data, spa_io_position *position)
	{
		auto self = static_cast<Impl *>(data);
		if (self->stopping || !position || !position->clock.rate.num)
			return;
		const auto frames = uint32_t(position->clock.duration);
		const auto rate = position->clock.rate.denom / position->clock.rate.num;
		if (!frames || !rate)
			return;
		std::array<const float *, channels.size()> samples{};
		for (size_t c = 0; c < samples.size(); ++c)
			samples[c] = static_cast<const float *>(pw_filter_get_dsp_buffer(self->inputs[c], frames));
		// PipeWire mixes multiple links on each DSP input. Downmix surround to stereo.
		constexpr float weights[2][9] = {{1, 0, .7071f, .5f, .7071f, 0, .7071f, 0, 1},
						 {0, 1, .7071f, .5f, 0, .7071f, 0, .7071f, 1}};
		for (uint32_t offset = 0; offset < frames;) {
			const auto index = self->written.load(std::memory_order_relaxed);
			auto &block = self->outputQueue[index % self->outputQueue.size()];
			auto count = std::min<uint32_t>(frames - offset, block.samples[0].size());
			if (index - self->read.load(std::memory_order_acquire) == self->outputQueue.size()) {
				offset += count;
				continue;
			}
			for (size_t side = 0; side < 2; ++side) {
				auto &mix = block.samples[side];
				std::fill_n(mix.data(), count, 0.f);
				for (size_t c = 0; c < samples.size(); ++c)
					if (samples[c] && weights[side][c])
						for (uint32_t i = 0; i < count; ++i)
							mix[i] += samples[c][offset + i] * weights[side][c];
			}
			block.frames = count;
			block.rate = rate;
			block.timestamp = position->clock.nsec + uint64_t(offset) * SPA_NSEC_PER_SEC / rate;
			self->written.store(index + 1, std::memory_order_release);
			sem_post(&self->outputReady);
			offset += count;
		}
	}

	bool start()
	{
		static std::once_flag initialized;
		std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
		if (sem_init(&outputReady, 0, 0) < 0)
			return false;
		outputReadyInitialized = true;
		try {
			outputThread = std::thread([this] { deliverAudio(); });
		} catch (const std::system_error &) {
			return false;
		}
		loop = pw_thread_loop_new("m4capture-audio", nullptr);
		if (!loop)
			return false;
		context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
		if (!context || !(core = pw_context_connect(context, nullptr, 0)))
			return false;
		static const pw_core_events coreEvents = [] {
			pw_core_events e{};
			e.version = PW_VERSION_CORE_EVENTS;
			e.error = [](void *data, uint32_t, int, int, const char *error) {
				static_cast<Impl *>(data)->fail(QStringLiteral("PipeWire audio: %1").arg(error));
			};
			return e;
		}();
		pw_core_add_listener(core, &coreListener, &coreEvents, this);
		registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
		if (!registry)
			return false;
		static const pw_registry_events registryEvents = [] {
			pw_registry_events e{};
			e.version = PW_VERSION_REGISTRY_EVENTS;
			e.global = global;
			e.global_remove = globalRemove;
			return e;
		}();
		pw_registry_add_listener(registry, &registryListener, &registryEvents, this);
		filter = pw_filter_new(core, "M4Capture Audio",
				       pw_properties_new(PW_KEY_NODE_NAME, "M4Capture Audio", PW_KEY_MEDIA_TYPE,
							 "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_CLASS,
							 "Stream/Input/Audio", PW_KEY_NODE_AUTOCONNECT, "false",
							 PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true",
							 nullptr));
		if (!filter)
			return false;
		static const pw_filter_events filterEvents = [] {
			pw_filter_events e{};
			e.version = PW_VERSION_FILTER_EVENTS;
			e.process = process;
			e.state_changed = [](void *data, pw_filter_state, pw_filter_state next, const char *error) {
				auto self = static_cast<Impl *>(data);
				if (next == PW_FILTER_STATE_ERROR)
					self->fail(
					    QStringLiteral("PipeWire audio: %1").arg(error ? error : "stream error"));
				else
					self->reconcile();
			};
			return e;
		}();
		pw_filter_add_listener(filter, &filterListener, &filterEvents, this);
		for (size_t c = 0; c < inputs.size(); ++c) {
			inputs[c] = pw_filter_add_port(filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
						       pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
									 PW_KEY_PORT_NAME, channels[c],
									 PW_KEY_AUDIO_CHANNEL, channels[c], nullptr),
						       nullptr, 0);
			if (!inputs[c])
				return false;
		}
		if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0 ||
		    pw_thread_loop_start(loop) < 0)
			return false;
		started = true;
		return true;
	}
};

PipeWireAudioCapture::PipeWireAudioCapture(QString appId, uint32_t pid, std::function<void(const AudioFrame &)> output,
					   std::function<void(QString)> failed)
    : impl(std::make_unique<Impl>())
{
	impl->appId = applicationName(std::move(appId));
	impl->pid = pid;
	impl->output = std::move(output);
	impl->failed = std::move(failed);
}

PipeWireAudioCapture::~PipeWireAudioCapture() = default;
bool PipeWireAudioCapture::start() { return impl->start(); }
