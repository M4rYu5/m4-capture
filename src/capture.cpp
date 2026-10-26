#include "capture.hpp"
#include "kwin-connection.hpp"
#include "pipewire-capture.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCoreApplication>
#include <algorithm>

namespace
{
QObject *captureContext = nullptr;
KWinConnection *connection = nullptr;
std::mutex windowsMutex;
QList<WindowInfo> windowsSnapshot;

bool requiresTargetSelection(const CaptureConfig &a, const CaptureConfig &b)
{
	return a.rule.appId != b.rule.appId || a.rule.title != b.rule.title || a.rule.mode != b.rule.mode ||
	       a.rule.caseSensitive != b.rule.caseSensitive || a.preferredWindow != b.preferredWindow;
}

void ensureConnection()
{
	if (connection)
		return;
	connection = new KWinConnection(captureContext);
	QObject::connect(connection, &KWinConnection::changed, captureContext, [] {
		std::lock_guard<std::mutex> lock(windowsMutex);
		windowsSnapshot = connection->windows();
	});
}
} // namespace

CaptureConfig read_capture_config(obs_data_t *settings)
{
	CaptureConfig config;
	config.rule.appId = QString::fromUtf8(obs_data_get_string(settings, "app_id"));
	config.rule.title = QString::fromUtf8(obs_data_get_string(settings, "title_filter"));
	config.rule.mode =
	    static_cast<TitleMatch>(std::clamp<long long>(obs_data_get_int(settings, "title_match"), 0, 3));
	config.rule.caseSensitive = obs_data_get_bool(settings, "case_sensitive");
	config.rule.disconnectOnTitleMismatch = obs_data_get_bool(settings, "disconnect_on_title_mismatch");
	config.preferredWindow = QString::fromUtf8(obs_data_get_string(settings, "selected_window"));
	config.cursor = obs_data_get_bool(settings, "capture_cursor");
	config.autoStart = obs_data_get_bool(settings, "auto_start");
	config.autoReconnect = obs_data_get_bool(settings, "auto_reconnect");
	config.raiseOnFocus = obs_data_get_bool(settings, "raise_on_focus");
	return config;
}

void write_capture_config(obs_data_t *settings, const CaptureConfig &config)
{
	obs_data_set_string(settings, "app_id", config.rule.appId.toUtf8().constData());
	obs_data_set_string(settings, "title_filter", config.rule.title.toUtf8().constData());
	obs_data_set_int(settings, "title_match", int(config.rule.mode));
	obs_data_set_bool(settings, "case_sensitive", config.rule.caseSensitive);
	obs_data_set_bool(settings, "disconnect_on_title_mismatch", config.rule.disconnectOnTitleMismatch);
	obs_data_set_string(settings, "selected_window", config.preferredWindow.toUtf8().constData());
	obs_data_set_bool(settings, "capture_cursor", config.cursor);
	obs_data_set_bool(settings, "auto_start", config.autoStart);
	obs_data_set_bool(settings, "auto_reconnect", config.autoReconnect);
	obs_data_set_bool(settings, "raise_on_focus", config.raiseOnFocus);
}

const char *capture_status_name(CaptureStatus status)
{
	switch (status) {
	case CaptureStatus::Waiting:
		return "waiting";
	case CaptureStatus::Connecting:
		return "connecting";
	case CaptureStatus::Capturing:
		return "capturing";
	case CaptureStatus::Failed:
		return "failed";
	default:
		return "idle";
	}
}

void initialize_capture()
{
	if (!QCoreApplication::instance())
		return;
	captureContext = new QObject;
	captureContext->moveToThread(QCoreApplication::instance()->thread());
	QMetaObject::invokeMethod(captureContext, [] { ensureConnection(); }, Qt::QueuedConnection);
}

void shutdown_capture()
{
	delete captureContext;
	captureContext = nullptr;
	connection = nullptr;
	std::lock_guard<std::mutex> lock(windowsMutex);
	windowsSnapshot.clear();
}

QList<WindowInfo> capture_windows()
{
	std::lock_guard<std::mutex> lock(windowsMutex);
	return windowsSnapshot;
}

void queue_capture_action(const std::shared_ptr<CaptureState> &state, CaptureAction action)
{
	if (!captureContext || !state->alive)
		return;
	QMetaObject::invokeMethod(
	    captureContext,
	    [state, action] {
		    if (!state->alive)
			    return;
		    ensureConnection();
		    if (!state->controller)
			    state->controller = new KWinCapture(state, connection, captureContext);
		    state->controller->action(action);
	    },
	    Qt::QueuedConnection);
}

void release_capture(const std::shared_ptr<CaptureState> &state)
{
	state->alive = false;
	if (captureContext)
		QMetaObject::invokeMethod(
		    captureContext, [state] { delete state->controller.data(); }, Qt::QueuedConnection);
}

KWinCapture::KWinCapture(std::shared_ptr<CaptureState> state, KWinConnection *backend, QObject *parent)
    : QObject(parent), state(std::move(state)), backend(backend)
{
	retry.setSingleShot(true);
	retry.setInterval(2000);
	deadline.setSingleShot(true);
	deadline.setInterval(15000);
	connect(&retry, &QTimer::timeout, this, [this] { reconcile(); });
	connect(&deadline, &QTimer::timeout, this,
		[this] { failed(QStringLiteral("No video frames were received.")); });
	connect(backend, &KWinConnection::changed, this, [this] { reconcile(); });
}

KWinCapture::~KWinCapture() { stopStream(); }

void KWinCapture::action(CaptureAction action)
{
	CaptureConfig next;
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (action == CaptureAction::Connect && state->status == CaptureStatus::Connecting)
			return;
		next = state->config;
	}
	bool selectTarget = !configured || requiresTargetSelection(next, config);
	bool restart = !configured || next.cursor != config.cursor;
	if (selectTarget) {
		bool ambiguous;
		auto windows = backend ? backend->windows() : QList<WindowInfo>{};
		auto match = select_window(windows, next.rule, {}, next.preferredWindow, &ambiguous);
		restart |= match != currentWindowUuid || !next.rule.error().isEmpty();
	}
	if (!configured) {
		connectionEnabled = next.autoStart;
		configured = true;
	}
	if (!config.raiseOnFocus && next.raiseOnFocus)
		wasFocused = false;
	config = next;
	if (action == CaptureAction::Stop) {
		connectionEnabled = false;
		retry.stop();
		stopStream();
		status(CaptureStatus::Idle);
		persist();
		return;
	}
	if (action == CaptureAction::Connect) {
		connectionEnabled = true;
		restart = true;
	}
	if (selectTarget || restart)
		retry.stop();
	if (restart) {
		stopStream();
		if (selectTarget || action == CaptureAction::Connect)
			currentWindowUuid.clear();
		wasFocused = false;
		if (!connectionEnabled)
			status(CaptureStatus::Idle);
	}
	reconcile();
}

void KWinCapture::stopStream()
{
	++generation;
	deadline.stop();
	pipewire.reset();
	stream.reset();
	if (backend && backend->available())
		backend->flush();
	state->frames->clear();
}

void KWinCapture::status(CaptureStatus next, const QString &detail)
{
	std::lock_guard<std::mutex> lock(state->mutex);
	state->status = next;
	state->detail = detail.toStdString();
}

void KWinCapture::focused(const WindowInfo &window)
{
	if (window.active && !wasFocused && config.raiseOnFocus) {
		auto source = OBSGetStrongRef(state->source);
		if (source && obs_frontend_get_main_window()) {
			OBSSourceAutoRelease sceneSource = obs_frontend_get_current_scene();
			auto scene = obs_scene_from_source(sceneSource);
			if (scene && raise_source_items(scene, source))
				obs_frontend_save();
		}
	}
	wasFocused = window.active;
}

void KWinCapture::reconcile()
{
	if (!state->alive) {
		// Source deletion is queued; stop producing frames while awaiting controller deletion.
		stopStream();
		return;
	}
	if (!connectionEnabled)
		return;
	if (config.rule.appId.isEmpty()) {
		status(CaptureStatus::Idle, QStringLiteral("Choose a window or enter its application ID."));
		return;
	}
	if (!backend || !backend->available()) {
		auto error = backend ? backend->error() : QStringLiteral("The KWin connection was destroyed.");
		if (stream)
			failed(error);
		else
			status(CaptureStatus::Failed, error);
		return;
	}
	if (!config.rule.error().isEmpty()) {
		status(CaptureStatus::Failed, config.rule.error());
		return;
	}
	auto windows = backend->windows();
	bool ambiguous;
	auto match = select_window(windows, config.rule, currentWindowUuid, config.preferredWindow, &ambiguous);
	if (stream && match != currentWindowUuid) {
		stopStream();
		if (!config.autoReconnect) {
			connectionEnabled = false;
			status(CaptureStatus::Idle);
			persist();
			return;
		}
	}
	if (match.isEmpty()) {
		currentWindowUuid.clear();
		wasFocused = false;
		status(CaptureStatus::Waiting,
		       ambiguous ? QStringLiteral("Several windows match. Narrow the title filter or choose a window.")
				 : QStringLiteral("Waiting for a matching window."));
		return;
	}
	if (currentWindowUuid != match) {
		currentWindowUuid = match;
		wasFocused = false;
	}
	for (const auto &window : windows)
		if (window.uuid == currentWindowUuid)
			focused(window);
	if (!stream && !retry.isActive())
		startStream();
}

void KWinCapture::startStream()
{
	status(CaptureStatus::Connecting);
	stream.reset(backend->capture(currentWindowUuid, config.cursor, this));
	if (!stream) {
		failed(backend->error());
		return;
	}
	const auto attempt = generation;
	connect(
	    stream.get(), &KWinStream::closed, this,
	    [this, attempt](const QString &error) { streamFailed(attempt, error); }, Qt::QueuedConnection);
	connect(
	    stream.get(), &KWinStream::ready, this,
	    [this, attempt](uint32_t node, const QString &serial) { streamReady(attempt, node, serial); },
	    Qt::QueuedConnection);
	backend->flush();
	if (stream)
		deadline.start();
}

void KWinCapture::streamReady(uint64_t attempt, uint32_t node, const QString &serial)
{
	if (generation != attempt || !state->alive)
		return;
	pipewire = std::make_unique<PipeWireCapture>(
	    state->frames,
	    [this, attempt](QString error) {
		    QMetaObject::invokeMethod(
			this, [this, attempt, error] { streamFailed(attempt, error); }, Qt::QueuedConnection);
	    },
	    [this, attempt](PipeWireStatus next) {
		    QMetaObject::invokeMethod(
			this, [this, attempt, next] { streamStatusChanged(attempt, next); }, Qt::QueuedConnection);
	    });
	if (!pipewire->start(-1, node, serial)) {
		failed(QStringLiteral("Could not connect to the KWin PipeWire stream."));
		return;
	}
	deadline.start();
}

void KWinCapture::streamFailed(uint64_t attempt, const QString &error)
{
	if (generation == attempt && state->alive)
		failed(error);
}

void KWinCapture::streamStatusChanged(uint64_t attempt, PipeWireStatus next)
{
	if (generation != attempt || !state->alive)
		return;
	switch (next) {
	case PipeWireStatus::Connecting:
		status(CaptureStatus::Connecting);
		break;
	case PipeWireStatus::Paused:
		status(CaptureStatus::Idle);
		break;
	case PipeWireStatus::Streaming:
		status(CaptureStatus::Capturing);
		deadline.stop();
		persist();
		break;
	}
}

void KWinCapture::failed(const QString &error)
{
	stopStream();
	status(CaptureStatus::Failed, error);
	blog(LOG_WARNING, "[KCow] %s", error.toUtf8().constData());
	persist();
	if (config.autoReconnect && connectionEnabled)
		retry.start();
	else
		connectionEnabled = false;
}

void KWinCapture::persist()
{
	auto source = OBSGetStrongRef(state->source);
	if (!source || obs_source_removed(source))
		return;
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		write_capture_config(settings, state->config);
	}
	auto size = state->frames->size();
	obs_data_set_int(settings, "width", size.width);
	obs_data_set_int(settings, "height", size.height);
	if (obs_frontend_get_main_window())
		obs_frontend_save();
}
