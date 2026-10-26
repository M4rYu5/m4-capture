#pragma once

#include "capture.hpp"
#include "video-frame.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>

#include <functional>

class PipeWireCapture
{
      public:
	PipeWireCapture(std::shared_ptr<CaptureState> state, std::function<void(QString)> failed,
			std::function<void(CaptureStatus)> changed);
	~PipeWireCapture();
	bool start(int fd, uint32_t node, const QString &serial);

      private:
	std::shared_ptr<CaptureState> state;
	std::function<void(QString)> failed;
	std::function<void(CaptureStatus)> changed;
	pw_thread_loop *loop = nullptr;
	pw_context *context = nullptr;
	pw_core *core = nullptr;
	pw_registry *registry = nullptr;
	pw_stream *stream = nullptr;
	spa_hook coreListener{};
	spa_hook registryListener{};
	spa_hook streamListener{};
	spa_video_info_raw format{};
	VideoFrame scratch;
	uint32_t nodeId = 0;
	bool started = false;
	bool stopping = false;
	bool failedOnce = false;

	void fail(const QString &message);
	void process();
	void formatChanged(const spa_pod *param);
	static void onState(void *data, pw_stream_state old, pw_stream_state next, const char *error);
	static void onParam(void *data, uint32_t id, const spa_pod *param);
	static void onProcess(void *data);
	static void onCoreError(void *data, uint32_t id, int seq, int res, const char *message);
	static void onGlobalRemove(void *data, uint32_t id);
};
