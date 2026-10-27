#pragma once

#include "frame-buffer.hpp"
#include "window-rule.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class KWinCapture;
class KWinConnection;
class KWinStream;
class PipeWireCapture;
enum class PipeWireStatus;

// Idle: stopped or unconfigured; Waiting: armed with no unique target; Connecting:
// awaiting frames; Capturing: frames available; Paused: live stream without frames;
// Failed: unavailable backend or failed attempt (which may have a retry pending).
enum class CaptureStatus { Idle, Waiting, Connecting, Capturing, Paused, Failed };
enum class CaptureAction { Configure, Connect, Stop };

struct CaptureConfig {
	WindowRule rule;
	QString preferredWindow;
	bool cursor = true;
	bool autoStart = true;
	bool autoReconnect = true;
	bool raiseOnFocus = false;
};

struct CaptureDiagnostics {
	CaptureStatus status = CaptureStatus::Idle;
	QString detail;
};

class CaptureState;
struct CaptureCallbacks {
	// Invoked on the controller's Qt thread. Empty callbacks are supported.
	std::function<void(const CaptureState &)> save;
	std::function<void()> focused;
	std::function<void(const QString &)> error;
};

// Configuration and diagnostic accessors are safe to call from any thread.
class CaptureState
{
      public:
	explicit CaptureState(uint32_t width = 1280, uint32_t height = 720, CaptureCallbacks callbacks = {});
	void configure(const CaptureConfig &config);
	CaptureConfig configuration() const;
	CaptureDiagnostics diagnostics() const;
	bool isAlive() const { return alive.load(); }
	const std::shared_ptr<FrameBuffer> frames;

      private:
	friend class KWinCapture;
	friend void queue_capture_action(const std::shared_ptr<CaptureState> &, CaptureAction);
	friend void release_capture(const std::shared_ptr<CaptureState> &);
	mutable std::mutex mutex;
	CaptureConfig config;
	CaptureDiagnostics diagnostic;
	std::atomic<bool> alive{true};
	const CaptureCallbacks callbacks;
	// Only accessed by the controller and runtime on the Qt UI thread.
	QPointer<KWinCapture> controller;
	void setStatus(CaptureStatus status, const QString &detail);
};

void initialize_capture();
void shutdown_capture();
void queue_capture_action(const std::shared_ptr<CaptureState> &state, CaptureAction action);
void release_capture(const std::shared_ptr<CaptureState> &state);
QList<WindowInfo> capture_windows();
const char *capture_status_name(CaptureStatus status);

// Runs on the Qt UI thread; PipeWire callbacks are queued onto this thread.
class KWinCapture : public QObject
{
	Q_OBJECT
      public:
	KWinCapture(std::shared_ptr<CaptureState> state, KWinConnection *backend, QObject *parent);
	~KWinCapture() override;
	void action(CaptureAction action);

      private:
	std::shared_ptr<CaptureState> state;
	QPointer<KWinConnection> backend;
	// Last applied configuration, compared with the latest shared settings on each action.
	CaptureConfig config;
	std::unique_ptr<KWinStream> stream;
	std::unique_ptr<PipeWireCapture> pipewire;
	QTimer retry;
	QTimer deadline;
	// The selected window survives stream restarts (for example, cursor changes).
	QString currentWindowUuid;
	// Incremented before teardown so callbacks from older attempts are ignored.
	uint64_t generation = 0;
	// Startup settings are applied once; explicit start/target edits can rearm capture.
	bool configured = false;
	// User intent is separate from stream status: waiting/failed/paused may stay armed.
	bool connectionEnabled = false;
	// Preserve an API Stop or disabled startup through unrelated settings edits.
	bool manuallyStopped = false;
	// Distinguish retrying a lost stream from waiting for the first target.
	bool reconnectPending = false;
	bool wasFocused = false;
	void applyConfiguration();
	void stopWaiting();
	void reconcile();
	void startStream();
	void streamReady(uint64_t attempt, uint32_t node, const QString &serial);
	void streamFailed(uint64_t attempt, const QString &error);
	void streamStatusChanged(uint64_t attempt, PipeWireStatus next);
	void stopStream();
	void failed(const QString &error);
	void status(CaptureStatus next, const QString &detail = {});
	void persist();
	void focused(const WindowInfo &window);
};
