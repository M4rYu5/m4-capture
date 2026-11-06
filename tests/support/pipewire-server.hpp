#pragma once

#include <QProcess>
#include <QTemporaryDir>
#include <atomic>
#include <pipewire/pipewire.h>

// Private daemon and client connection, independent of media producers and KWin.
class PipeWireServer
{
      public:
	~PipeWireServer();
	bool start();
	int openRemote() const;
	QString socketPath() const;
	uint32_t audioCapturesCreated() const { return audioCaptures.load(); }
	pw_thread_loop *threadLoop() const { return loop; }
	pw_core *connection() const { return core; }

      private:
	QTemporaryDir directory;
	QProcess daemon;
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_registry *registry = nullptr;
	spa_hook listener{};
	std::atomic<uint32_t> audioCaptures{0};
	static void global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
			   const spa_dict *props);
};
