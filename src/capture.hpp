#pragma once

#include "frame-buffer.hpp"
#include "pipewire-audio.hpp"
#include "window-rule.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class M4Capture;
class KWinConnection;
class KWinStream;
class PipeWireCapture;
class DmaBufCapture;
enum class PipeWireStatus;

// Idle: unconfigured; Waiting: no unique target; Connecting:
// awaiting frames; Capturing: frames available; Paused: live stream without frames;
// Failed: unavailable backend or failed attempt (which may have a retry pending).
enum class CaptureStatus { Idle, Waiting, Connecting, Capturing, Paused, Failed };

struct CaptureConfig {
	WindowRule rule;
	QString preferredWindow;
	bool cursor = true;
	bool raiseOnFocus = false;
	bool audio = false;
};

struct CaptureDiagnostics {
	CaptureStatus status = CaptureStatus::Idle;
	QString detail;
};

class CaptureState;
struct CaptureCallbacks {
	// Invoked on the controller's Qt thread except audio. Empty callbacks are supported.
	std::function<void(const CaptureState &)> save;
	std::function<void()> focused;
	std::function<void(const QString &)> error;
	std::function<void(const CaptureState &)> changed;
	// Audio is delivered on a dedicated worker thread, outside PipeWire processing.
	std::function<void(const AudioFrame &)> audio;
	std::shared_ptr<DmaBufCapture> gpu;
};

// Configuration and diagnostic accessors are safe to call from any thread.
class CaptureState
{
      public:
	explicit CaptureState(uint32_t width = 1280, uint32_t height = 720, CaptureCallbacks callbacks = {});
	void configure(const CaptureConfig &config);
	CaptureConfig configuration() const;
	CaptureDiagnostics diagnostics() const;
	void setShowing(bool showing);
	bool isShowing() const;
	bool isAlive() const { return alive.load(); }
	const std::shared_ptr<FrameBuffer> frames;

      private:
	friend class M4Capture;
	friend void queue_capture_update(const std::shared_ptr<CaptureState> &);
	friend void release_capture(const std::shared_ptr<CaptureState> &);
	mutable std::mutex mutex;
	CaptureConfig config;
	CaptureDiagnostics diagnostic;
	bool showing = true;
	std::atomic<bool> alive{true};
	std::atomic<bool> audioEnabled{false};
	const CaptureCallbacks callbacks;
	// Only accessed by the controller and runtime on the Qt UI thread.
	QPointer<M4Capture> controller;
	void setStatus(CaptureStatus status, const QString &detail);
};

void initialize_capture();
void shutdown_capture();
void queue_capture_update(const std::shared_ptr<CaptureState> &state);
void release_capture(const std::shared_ptr<CaptureState> &state);
QList<WindowInfo> capture_windows();
const char *capture_status_name(CaptureStatus status);

// Runs on the Qt UI thread; PipeWire callbacks are queued onto this thread.
class M4Capture : public QObject
{
	Q_OBJECT
      public:
	M4Capture(std::shared_ptr<CaptureState> state, KWinConnection *backend, QObject *parent);
	~M4Capture() override;
	void update();

      private:
	std::shared_ptr<CaptureState> state;
	QPointer<KWinConnection> backend;
	// Last applied configuration, compared with the latest shared settings on each update.
	CaptureConfig config;
	std::unique_ptr<KWinStream> stream;
	std::unique_ptr<PipeWireCapture> pipewire;
	std::unique_ptr<PipeWireAudioCapture> audio;
	QTimer audioRetry;
	QString audioAppId;
	uint32_t audioPid = 0;
	uint64_t audioGeneration = 0;
	QTimer retry;
	QTimer deadline;
	// The selected window survives stream restarts (for example, cursor changes).
	QString currentWindowUuid;
	// Incremented before teardown so callbacks from older attempts are ignored.
	uint64_t generation = 0;
	bool wasFocused = false;
	void applyConfiguration();
	void reconcile();
	void startStream();
	void streamReady(uint64_t attempt, uint32_t node, const QString &serial);
	void streamFailed(uint64_t attempt, const QString &error);
	void streamStatusChanged(uint64_t attempt, PipeWireStatus next);
	void stopStream();
	void updateAudio(const WindowInfo &window);
	void stopAudio();
	void audioFailed(uint64_t attempt, const QString &error);
	void failed(const QString &error);
	void status(CaptureStatus next, const QString &detail = {});
	void persist();
	void focused(const WindowInfo &window);
};
