#pragma once

#include "dmabuf-capture.hpp"
#include "frame-buffer.hpp"

#include <QString>

#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <functional>
#include <memory>

enum class PipeWireStatus { Connecting, Paused, Streaming };

class PipeWireCapture
{
      public:
	// Callbacks may run on the PipeWire thread; the caller must marshal UI work.
	PipeWireCapture(std::shared_ptr<FrameBuffer> frames, std::function<void(QString)> failed,
			std::function<void(PipeWireStatus)> changed, std::shared_ptr<DmaBufCapture> gpu = {});
	~PipeWireCapture();
	PipeWireCapture(const PipeWireCapture &) = delete;
	PipeWireCapture &operator=(const PipeWireCapture &) = delete;
	bool start(int fd, uint32_t node, const QString &serial);
	void disableGpu();

      private:
	std::shared_ptr<FrameBuffer> frames;
	std::function<void(QString)> failed;
	std::function<void(PipeWireStatus)> changed;
	std::shared_ptr<DmaBufCapture> gpu;
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
	bool cpuOnly = false;
	bool dmaBuf = false;

	std::vector<const spa_pod *> buildFormats(spa_pod_builder &builder);
	void fallbackToCpu();

	void fail(const QString &message);
	void process();
	void formatChanged(const spa_pod *param);
	static void onState(void *data, pw_stream_state old, pw_stream_state next, const char *error);
	static void onParam(void *data, uint32_t id, const spa_pod *param);
	static void onProcess(void *data);
	static void onCoreError(void *data, uint32_t id, int seq, int res, const char *message);
	static void onGlobalRemove(void *data, uint32_t id);
};
