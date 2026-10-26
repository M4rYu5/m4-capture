#pragma once

#include "frame-buffer.hpp"
#include "window-rule.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <obs.hpp>

#include <atomic>
#include <memory>
#include <mutex>

class KWinCapture;
class KWinConnection;
class KWinStream;
class PipeWireCapture;
enum class PipeWireStatus;

enum class CaptureStatus { Idle, Waiting, Connecting, Capturing, Failed };
enum class CaptureAction { Configure, Connect, Stop };

struct CaptureConfig {
	WindowRule rule;
	QString preferredWindow;
	bool cursor = true;
	bool autoStart = true;
	bool autoReconnect = true;
	bool raiseOnFocus = false;
};

struct CaptureState {
	// Protects settings and diagnostics; frames have their own synchronization.
	std::mutex mutex;
	CaptureConfig config;
	std::shared_ptr<FrameBuffer> frames = std::make_shared<FrameBuffer>();
	CaptureStatus status = CaptureStatus::Idle;
	std::string detail;
	std::atomic<bool> alive{true};
	OBSWeakSource source;
	// Only touched on the Qt UI thread.
	QPointer<KWinCapture> controller;
};

void initialize_capture();
void shutdown_capture();
void queue_capture_action(const std::shared_ptr<CaptureState> &state, CaptureAction action);
void release_capture(const std::shared_ptr<CaptureState> &state);
QList<WindowInfo> capture_windows();
const char *capture_status_name(CaptureStatus status);
bool raise_source_items(obs_scene_t *scene, obs_source_t *source);
CaptureConfig read_capture_config(obs_data_t *settings);
void write_capture_config(obs_data_t *settings, const CaptureConfig &config);

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
	CaptureConfig config;
	std::unique_ptr<KWinStream> stream;
	std::unique_ptr<PipeWireCapture> pipewire;
	QTimer retry;
	QTimer deadline;
	// The selected window survives stream restarts (for example, cursor changes).
	QString currentWindowUuid;
	// Incremented before teardown so callbacks from older attempts are ignored.
	uint64_t generation = 0;
	// Startup settings arm capture only once; subsequent edits preserve manual stops.
	bool configured = false;
	bool connectionEnabled = false;
	bool wasFocused = false;
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
