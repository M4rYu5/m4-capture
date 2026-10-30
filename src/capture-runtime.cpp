#include "capture.hpp"
#include "kwin-connection.hpp"

#include <QCoreApplication>

namespace
{
QObject *captureContext = nullptr;
KWinConnection *connection = nullptr;
std::mutex snapshotsMutex;
QList<WindowInfo> windowsSnapshot;
QList<DisplayInfo> displaysSnapshot;

void ensureConnection()
{
	if (connection)
		return;
	connection = new KWinConnection(captureContext);
	QObject::connect(connection, &KWinConnection::windowsChanged, captureContext, [] {
		std::lock_guard<std::mutex> lock(snapshotsMutex);
		windowsSnapshot = connection->windows();
	});
	QObject::connect(connection, &KWinConnection::displaysChanged, captureContext, [] {
		std::lock_guard<std::mutex> lock(snapshotsMutex);
		displaysSnapshot = connection->displays();
	});
}
} // namespace

void initialize_capture()
{
	if (captureContext || !QCoreApplication::instance())
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
	std::lock_guard<std::mutex> lock(snapshotsMutex);
	windowsSnapshot.clear();
	displaysSnapshot.clear();
}

QList<WindowInfo> capture_windows()
{
	std::lock_guard<std::mutex> lock(snapshotsMutex);
	return windowsSnapshot;
}

QList<DisplayInfo> capture_displays()
{
	std::lock_guard<std::mutex> lock(snapshotsMutex);
	return displaysSnapshot;
}

void queue_capture_update(const std::shared_ptr<CaptureState> &state)
{
	if (!captureContext || !state->isAlive())
		return;
	QMetaObject::invokeMethod(
	    captureContext,
	    [state] {
		    if (!state->isAlive())
			    return;
		    ensureConnection();
		    if (!state->controller)
			    new M4Capture(state, connection, captureContext);
		    state->controller->update();
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
