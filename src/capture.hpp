#pragma once

#include "window-rule.hpp"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <obs.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class KWinCapture;
class KWinConnection;
class KWinStream;
class PipeWireCapture;

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
	std::mutex mutex;
	CaptureConfig config;
	std::vector<uint8_t> pixels;
	uint32_t width = 1280;
	uint32_t height = 720;
	bool hasFrame = false;
	bool frameDirty = false;
	CaptureStatus status = CaptureStatus::Idle;
	std::string detail;
	QString matchedWindow;
	std::atomic<bool> alive{true};
	OBSWeakSource source;
	// Only touched on the Qt UI thread.
	QPointer<KWinCapture> controller;
	void clear(CaptureStatus next, const std::string &message = {});
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
	KWinConnection *backend;
	CaptureConfig config;
	std::unique_ptr<KWinStream> stream;
	std::unique_ptr<PipeWireCapture> pipewire;
	QTimer retry;
	QTimer deadline;
	QString current;
	uint64_t generation = 0;
	bool configured = false;
	bool armed = false;
	bool wasFocused = false;
	void reconcile();
	void startStream();
	void stopStream();
	void failed(const QString &error);
	void status(CaptureStatus next, const QString &detail = {});
	void refreshProperties();
	void persist();
	void focused(const WindowInfo &window);
};
