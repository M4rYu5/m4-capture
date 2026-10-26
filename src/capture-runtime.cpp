#include "capture.hpp"
#include "kwin-connection.hpp"

#include <QCoreApplication>

namespace
{
QObject *captureContext = nullptr;
KWinConnection *connection = nullptr;
std::mutex windowsMutex;
QList<WindowInfo> windowsSnapshot;

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
	if (!captureContext || !state->isAlive())
		return;
	QMetaObject::invokeMethod(
	    captureContext,
	    [state, action] {
		    if (!state->isAlive())
			    return;
		    ensureConnection();
		    if (!state->controller)
			    new KWinCapture(state, connection, captureContext);
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
