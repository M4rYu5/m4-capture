#include "capture.hpp"
#include "pipewire-capture.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusUnixFileDescriptor>
#include <QUuid>

#include <fcntl.h>

namespace
{
constexpr auto desktopService = "org.freedesktop.portal.Desktop";
constexpr auto desktopPath = "/org/freedesktop/portal/desktop";
constexpr auto screencastInterface = "org.freedesktop.portal.ScreenCast";
constexpr auto requestInterface = "org.freedesktop.portal.Request";
constexpr auto sessionInterface = "org.freedesktop.portal.Session";
QObject *captureContext = nullptr;

QString newToken() { return "wc_" + QUuid::createUuid().toString(QUuid::Id128); }

QString senderPath()
{
	QString sender = QDBusConnection::sessionBus().baseService();
	sender.remove(':');
	sender.replace('.', '_');
	return sender;
}

void closePath(const QString &path, const char *interface)
{
	if (!path.isEmpty())
		QDBusConnection::sessionBus().asyncCall(
		    QDBusMessage::createMethodCall(desktopService, path, interface, "Close"));
}
} // namespace

struct PortalStream {
	uint node;
	QVariantMap properties;
};
using PortalStreams = QList<PortalStream>;
Q_DECLARE_METATYPE(PortalStream)
Q_DECLARE_METATYPE(PortalStreams)

static QDBusArgument &operator<<(QDBusArgument &argument, const PortalStream &stream)
{
	argument.beginStructure();
	argument << stream.node << stream.properties;
	argument.endStructure();
	return argument;
}

static const QDBusArgument &operator>>(const QDBusArgument &argument, PortalStream &stream)
{
	argument.beginStructure();
	argument >> stream.node >> stream.properties;
	argument.endStructure();
	return argument;
}

void CaptureState::clear(CaptureStatus next, const std::string &message)
{
	std::lock_guard<std::mutex> lock(mutex);
	hasFrame = false;
	frameDirty = false;
	pixels.clear();
	status = next;
	detail = message;
	// The selection/token and last dimensions deliberately survive every failure.
}

const char *capture_status_name(CaptureStatus status)
{
	switch (status) {
	case CaptureStatus::Choosing:
		return "choosing";
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
}

void shutdown_capture()
{
	// OBS unloads modules on the UI thread after sources have been destroyed.
	delete captureContext;
	captureContext = nullptr;
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
		    if (!state->controller)
			    state->controller = new PortalCapture(state, captureContext);
		    state->controller->start(action == CaptureAction::Choose);
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

PortalCapture::PortalCapture(std::shared_ptr<CaptureState> state, QObject *parent)
    : QObject(parent), state(std::move(state))
{
	qDBusRegisterMetaType<PortalStream>();
	qDBusRegisterMetaType<PortalStreams>();
	requestTimeout.setSingleShot(true);
	requestTimeout.setInterval(120000);
	connect(&requestTimeout, &QTimer::timeout, this,
		[this] { fail(QStringLiteral("The capture request timed out.")); });
	frameTimeout.setSingleShot(true);
	frameTimeout.setInterval(15000);
	connect(&frameTimeout, &QTimer::timeout, this,
		[this] { fail(QStringLiteral("No video frames were received.")); });
	auto watcher = new QDBusServiceWatcher(desktopService, QDBusConnection::sessionBus(),
					       QDBusServiceWatcher::WatchForOwnerChange, this);
	connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
		[this](const QString &, const QString &oldOwner, const QString &) {
			if (!oldOwner.isEmpty() && step != Step::None)
				fail(QStringLiteral("The desktop portal disconnected."));
		});
}

PortalCapture::~PortalCapture() { stop(); }

void PortalCapture::stop()
{
	++generation;
	requestTimeout.stop();
	frameTimeout.stop();
	auto bus = QDBusConnection::sessionBus();
	if (!requestPath.isEmpty())
		bus.disconnect(desktopService, requestPath, requestInterface, "Response", this,
			       SLOT(response(uint, QVariantMap, QDBusMessage)));
	if (!sessionPath.isEmpty())
		bus.disconnect(desktopService, sessionPath, sessionInterface, "Closed", this,
			       SLOT(sessionClosed(QVariantMap, QDBusMessage)));
	closePath(requestPath, requestInterface);
	closePath(sessionPath, sessionInterface);
	requestPath.clear();
	sessionPath.clear();
	step = Step::None;
	pipewire.reset();
	state->clear(CaptureStatus::Idle);
}

void PortalCapture::fail(const QString &message)
{
	stop();
	state->clear(CaptureStatus::Failed, message.toStdString());
	blog(LOG_WARNING, "[WaylandConnect] %s", message.toUtf8().constData());
	persist();
	refreshProperties();
}

void PortalCapture::start(bool choose)
{
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->status == CaptureStatus::Choosing || state->status == CaptureStatus::Connecting)
			return;
		choosing = choose || state->restoreToken.empty();
		state->selectionAttempted = true;
	}
	stop();
	state->clear(choosing ? CaptureStatus::Choosing : CaptureStatus::Connecting);
	refreshProperties();
	const auto attempt = generation;
	// Inspect asynchronously so a slow/unavailable portal cannot block the OBS UI.
	auto call =
	    QDBusMessage::createMethodCall(desktopService, desktopPath, "org.freedesktop.DBus.Properties", "GetAll");
	call << screencastInterface;
	auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, attempt](QDBusPendingCallWatcher *watcher) {
		QDBusPendingReply<QVariantMap> reply = *watcher;
		watcher->deleteLater();
		if (attempt != generation || !state->alive)
			return;
		if (reply.isError()) {
			fail(QStringLiteral("Could not contact the capture portal: %1").arg(reply.error().message()));
			return;
		}
		auto properties = reply.value();
		if (properties.value("version").toUInt() < 4) {
			fail(QStringLiteral("ScreenCast portal version 4 or newer is needed to save selections."));
			return;
		}
		sourceTypes = properties.value("AvailableSourceTypes").toUInt() & 3;
		auto cursors = properties.value("AvailableCursorModes").toUInt();
		cursorMode = (cursors & 2) ? 2 : 1;
		if (!sourceTypes || !(cursors & cursorMode)) {
			fail(QStringLiteral("The desktop portal does not support this capture mode."));
			return;
		}
		auto sessionToken = newToken();
		// Knowing the path in advance also lets destruction cancel a pending CreateSession.
		sessionPath =
		    QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2").arg(senderPath(), sessionToken);
		request("CreateSession", {}, {{"session_handle_token", sessionToken}}, Step::Create);
	});
}

void PortalCapture::request(const QString &method, const QList<QVariant> &arguments, QVariantMap options, Step next)
{
	auto bus = QDBusConnection::sessionBus();
	QString token = newToken();
	options.insert("handle_token", token);
	requestPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(senderPath(), token);
	step = next;
	if (!bus.connect(desktopService, requestPath, requestInterface, "Response", this,
			 SLOT(response(uint, QVariantMap, QDBusMessage)))) {
		fail(QStringLiteral("Could not subscribe to the portal response."));
		return;
	}
	auto call = QDBusMessage::createMethodCall(desktopService, desktopPath, screencastInterface, method);
	auto args = arguments;
	args.append(options);
	call.setArguments(args);
	const auto attempt = generation;
	const auto expectedPath = requestPath;
	auto watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this,
		[this, attempt, expectedPath](QDBusPendingCallWatcher *watcher) {
			QDBusPendingReply<QDBusObjectPath> reply = *watcher;
			watcher->deleteLater();
			if (attempt != generation || expectedPath != requestPath || !state->alive)
				return;
			if (reply.isError())
				fail(QStringLiteral("Portal request failed: %1").arg(reply.error().message()));
			else if (reply.value().path() != expectedPath)
				fail(QStringLiteral("The portal returned an unexpected request handle."));
		});
	requestTimeout.start();
}

void PortalCapture::response(uint code, const QVariantMap &results, const QDBusMessage &message)
{
	if (message.path() != requestPath || !state->alive)
		return;
	QDBusConnection::sessionBus().disconnect(desktopService, requestPath, requestInterface, "Response", this,
						 SLOT(response(uint, QVariantMap, QDBusMessage)));
	requestTimeout.stop();
	requestPath.clear();
	if (code != 0) {
		fail(code == 1 ? QStringLiteral("Selection cancelled. The saved selection was kept.")
			       : QStringLiteral("Capture could not be started. The saved selection was kept."));
		return;
	}
	switch (step) {
	case Step::Create:
		if (results.value("session_handle").toString() != sessionPath) {
			fail(QStringLiteral("The portal returned an invalid session."));
			return;
		}
		if (!QDBusConnection::sessionBus().connect(desktopService, sessionPath, sessionInterface, "Closed",
							   this, SLOT(sessionClosed(QVariantMap, QDBusMessage)))) {
			fail(QStringLiteral("Could not monitor the capture session."));
			return;
		}
		selectSources();
		break;
	case Step::Select:
		request("Start", {QVariant::fromValue(QDBusObjectPath(sessionPath)), QString()}, {}, Step::Start);
		break;
	case Step::Start: {
		auto replacementToken = results.value("restore_token").toString();
		if (replacementToken.isEmpty()) {
			fail(QStringLiteral("The portal did not grant permission to remember this capture. "
					    "Choose again and enable restoring on future sessions."));
			return;
		}
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			// Even a malformed stream response may already have consumed the old token.
			state->restoreToken = replacementToken.toStdString();
		}
		auto streams = qdbus_cast<PortalStreams>(results.value("streams"));
		if (streams.size() != 1) {
			fail(QStringLiteral("The portal did not return exactly one capture stream."));
			return;
		}
		nodeId = streams.front().node;
		auto properties = streams.front().properties;
		nodeSerial = properties.contains("pipewire-serial")
				 ? QString::number(properties.value("pipewire-serial").toULongLong())
				 : QString();
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			// Start consumes the previous token. Save its replacement BEFORE opening video.
			// Failed/cancelled requests never reach this block or erase the old token.
			state->selectionId = properties.value("id").toString().toStdString();
			state->status = CaptureStatus::Connecting;
		}
		persist();
		openRemote();
		break;
	}
	default:
		break;
	}
}

void PortalCapture::selectSources()
{
	QVariantMap options{
	    {"types", sourceTypes}, {"multiple", false}, {"cursor_mode", cursorMode}, {"persist_mode", uint(2)}};
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		if (!choosing && !state->restoreToken.empty())
			options.insert("restore_token", QString::fromStdString(state->restoreToken));
	}
	request("SelectSources", {QVariant::fromValue(QDBusObjectPath(sessionPath))}, options, Step::Select);
}

void PortalCapture::openRemote()
{
	step = Step::Remote;
	auto call =
	    QDBusMessage::createMethodCall(desktopService, desktopPath, screencastInterface, "OpenPipeWireRemote");
	call << QVariant::fromValue(QDBusObjectPath(sessionPath)) << QVariantMap();
	const auto attempt = generation;
	auto watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
	connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, attempt](QDBusPendingCallWatcher *watcher) {
		QDBusPendingReply<QDBusUnixFileDescriptor> reply = *watcher;
		watcher->deleteLater();
		if (attempt != generation || !state->alive)
			return;
		if (reply.isError()) {
			fail(QStringLiteral("Could not open the video connection: %1").arg(reply.error().message()));
			return;
		}
		int fd = fcntl(reply.value().fileDescriptor(), F_DUPFD_CLOEXEC, 0);
		if (fd < 0) {
			fail(QStringLiteral("Could not duplicate the PipeWire connection."));
			return;
		}
		pipewire = std::make_unique<PipeWireCapture>(
		    state,
		    [this, attempt](QString error) {
			    QMetaObject::invokeMethod(
				this,
				[this, attempt, error] {
					if (generation == attempt)
						fail(error);
				},
				Qt::QueuedConnection);
		    },
		    [this, attempt] {
			    QMetaObject::invokeMethod(
				this,
				[this, attempt] {
					if (generation == attempt) {
						CaptureStatus status;
						{
							std::lock_guard<std::mutex> lock(state->mutex);
							status = state->status;
						}
						if (status == CaptureStatus::Connecting && !frameTimeout.isActive())
							frameTimeout.start();
						else if (status != CaptureStatus::Connecting)
							frameTimeout.stop();
						persist();
						refreshProperties();
					}
				},
				Qt::QueuedConnection);
		    });
		if (!pipewire->start(fd, nodeId, nodeSerial)) {
			fail(QStringLiteral("Could not connect to the PipeWire video stream."));
			return;
		}
		frameTimeout.start();
	});
}

void PortalCapture::sessionClosed(const QVariantMap &, const QDBusMessage &message)
{
	if (message.path() == sessionPath)
		fail(QStringLiteral("The capture session closed. The saved selection was kept."));
}

void PortalCapture::persist()
{
	auto source = OBSGetStrongRef(state->source);
	if (!source || obs_source_removed(source))
		return;
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		obs_data_set_string(settings, "restore_token", state->restoreToken.c_str());
		obs_data_set_string(settings, "selection_id", state->selectionId.c_str());
		obs_data_set_bool(settings, "selection_attempted", state->selectionAttempted);
		obs_data_set_int(settings, "width", state->width);
		obs_data_set_int(settings, "height", state->height);
	}
	if (obs_frontend_get_main_window())
		obs_frontend_save();
}

void PortalCapture::refreshProperties()
{
	auto source = OBSGetStrongRef(state->source);
	if (source && !obs_source_removed(source))
		obs_source_update_properties(source);
}
