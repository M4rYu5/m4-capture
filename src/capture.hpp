#pragma once

#include <obs.hpp>

#include <QDBusMessage>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class PortalCapture;
class PipeWireCapture;

enum class CaptureStatus { Idle, Choosing, Connecting, Capturing, Failed };
enum class CaptureAction { Choose, Connect };

struct CaptureState {
	std::mutex mutex;
	std::vector<uint8_t> pixels;
	uint32_t width = 1280;
	uint32_t height = 720;
	bool hasFrame = false;
	bool frameDirty = false;
	bool selectionAttempted = false;
	std::string restoreToken;
	std::string selectionId;
	CaptureStatus status = CaptureStatus::Idle;
	std::string detail;
	std::atomic<bool> alive{true};
	OBSWeakSource source;
	// Accessed only on the Qt UI thread; media and settings above use the mutex.
	QPointer<PortalCapture> controller;

	void clear(CaptureStatus next, const std::string &message = {});
};

void initialize_capture();
void shutdown_capture();
void queue_capture_action(const std::shared_ptr<CaptureState> &state, CaptureAction action);
void release_capture(const std::shared_ptr<CaptureState> &state);
const char *capture_status_name(CaptureStatus status);

class PortalCapture : public QObject
{
	Q_OBJECT

      public:
	explicit PortalCapture(std::shared_ptr<CaptureState> state, QObject *parent);
	~PortalCapture() override;
	void start(bool choose);

      private slots:
	void response(uint code, const QVariantMap &results, const QDBusMessage &message);
	void sessionClosed(const QVariantMap &, const QDBusMessage &message);

      private:
	enum class Step { None, Create, Select, Start, Remote };
	std::shared_ptr<CaptureState> state;
	std::unique_ptr<PipeWireCapture> pipewire;
	QString sessionPath;
	QString requestPath;
	QTimer requestTimeout;
	QTimer frameTimeout;
	Step step = Step::None;
	uint64_t generation = 0;
	bool choosing = false;
	uint sourceTypes = 0;
	uint cursorMode = 1;
	uint nodeId = 0;
	QString nodeSerial;

	void request(const QString &method, const QList<QVariant> &arguments, QVariantMap options, Step next);
	void selectSources();
	void openRemote();
	void stop();
	void fail(const QString &message);
	void persist();
	void refreshProperties();
};
