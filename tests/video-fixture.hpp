#pragma once

#include <QProcess>
#include <QTemporaryDir>

#include <pipewire/pipewire.h>

#include <atomic>
#include <map>
#include <string>
#include <vector>

// An isolated PipeWire daemon and solid-color video source; never uses the desktop.
class TestVideo
{
      public:
	~TestVideo();
	bool start();
	bool startProducer();
	void stopProducer();
	int openRemote() const;
	uint32_t nodeId() const { return producerNode.load(); }
	uint64_t serial() const { return producerSerial.load(); }
	QString socketPath() const;

      private:
	QTemporaryDir directory;
	QProcess daemon;
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_registry *registry = nullptr;
	pw_stream *producer = nullptr;
	spa_hook registryListener{};
	spa_hook streamListener{};
	std::atomic<uint32_t> producerNode{PW_ID_ANY};
	std::atomic<uint64_t> producerSerial{0};
	std::map<uint32_t, std::string> nodes;
	std::map<uint32_t, std::pair<uint32_t, bool>> ports;
	std::vector<pw_proxy *> links;
	void tryLink();
	static void global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props);
	static void paramChanged(void *data, uint32_t id, const spa_pod *param);
	static void process(void *data);
};
