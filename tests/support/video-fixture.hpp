#pragma once

#include "pipewire-server.hpp"

#include <atomic>
#include <map>
#include <string>
#include <vector>

// Solid-color video and explicit links; the server must outlive its producers.
class VideoProducer
{
      public:
	explicit VideoProducer(PipeWireServer &server, uint32_t bgra = 0xffe08020u) : server(server), bgra(bgra) {}
	~VideoProducer();
	bool start();
	bool setActive(bool active);
	void setColor(uint32_t color) { bgra = color; }
	void stop();
	uint32_t nodeId() const { return producerNode.load(); }
	uint64_t serial() const { return producerSerial.load(); }

      private:
	PipeWireServer &server;
	std::atomic<uint32_t> bgra;
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
	static void global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
			   const spa_dict *props);
	static void paramChanged(void *data, uint32_t id, const spa_pod *param);
	static void process(void *data);
};
