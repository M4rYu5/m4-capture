#pragma once

#include <initializer_list>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <utility>
#include <vector>

// Constant DSP samples on an isolated daemon, with application identity metadata.
class AudioProducer
{
      public:
	~AudioProducer();
	bool start(int fd, const char *appId,
		   std::initializer_list<std::pair<const char *, float>> samples = {{"FL", .125f}, {"FR", -.25f}},
		   uint32_t pid = 1);
	bool setIdentity(const char *appId, uint32_t pid, bool onClient = false);

      private:
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_filter *filter = nullptr;
	spa_hook listener{};
	bool started = false;
	std::vector<void *> ports;
};
