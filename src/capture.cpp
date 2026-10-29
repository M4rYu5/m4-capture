#include "capture.hpp"
#include "kwin-connection.hpp"
#include "pipewire-capture.hpp"

#include <QThread>

namespace
{
bool requiresTargetSelection(const CaptureConfig &a, const CaptureConfig &b)
{
	return a.rule.appId != b.rule.appId || a.rule.title != b.rule.title || a.rule.mode != b.rule.mode ||
	       a.rule.caseSensitive != b.rule.caseSensitive || a.preferredWindow != b.preferredWindow;
}
} // namespace

CaptureState::CaptureState(uint32_t width, uint32_t height, CaptureCallbacks callbacks)
    : frames(std::make_shared<FrameBuffer>(width, height)), callbacks(std::move(callbacks))
{
}

void CaptureState::configure(const CaptureConfig &next)
{
	std::lock_guard<std::mutex> lock(mutex);
	config = next;
}

CaptureConfig CaptureState::configuration() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return config;
}

CaptureDiagnostics CaptureState::diagnostics() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return diagnostic;
}

void CaptureState::setShowing(bool next)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		showing = next;
	}
	// Disable publication immediately, including any copy already in flight.
	// Only the controller re-enables frames, after joining the old stream.
	if (!next)
		frames->setEnabled(false);
}

bool CaptureState::isShowing() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return showing;
}

void CaptureState::setStatus(CaptureStatus next, const QString &detail)
{
	std::lock_guard<std::mutex> lock(mutex);
	diagnostic = {next, detail};
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
	case CaptureStatus::Paused:
		return "paused";
	case CaptureStatus::Failed:
		return "failed";
	default:
		return "idle";
	}
}

KWinCapture::KWinCapture(std::shared_ptr<CaptureState> state, KWinConnection *backend, QObject *parent)
    : QObject(parent), state(std::move(state)), backend(backend)
{
	Q_ASSERT(QThread::currentThread() == thread());
	Q_ASSERT(!this->state->controller);
	this->state->controller = this;
	retry.setSingleShot(true);
	retry.setInterval(2000);
	deadline.setSingleShot(true);
	deadline.setInterval(15000);
	connect(&retry, &QTimer::timeout, this, [this] { reconcile(); });
	connect(&deadline, &QTimer::timeout, this, [this] {
		if (pipewire)
			pipewire->disableGpu();
		failed(QStringLiteral("No video frames were received."));
	});
	connect(backend, &KWinConnection::changed, this, [this] { reconcile(); });
}

KWinCapture::~KWinCapture() { stopStream(); }

void KWinCapture::update()
{
	Q_ASSERT(QThread::currentThread() == thread());
	applyConfiguration();
	reconcile();
}

void KWinCapture::applyConfiguration()
{
	auto next = state->configuration();
	bool selectTarget = requiresTargetSelection(next, config);
	next.rule = config.rule.updated(next.rule);
	bool restart = next.cursor != config.cursor;
	if (selectTarget) {
		bool ambiguous;
		const auto windows = backend ? backend->windows() : QList<WindowInfo>{};
		auto match = select_window(windows, next.rule, {}, next.preferredWindow, &ambiguous);
		restart |= match != currentWindowUuid || !next.rule.error().isEmpty();
	}
	if (!config.raiseOnFocus && next.raiseOnFocus)
		wasFocused = false;
	config = next;
	if (selectTarget || restart)
		retry.stop();
	if (restart) {
		stopStream();
		if (selectTarget)
			currentWindowUuid.clear();
		wasFocused = false;
	}
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
	state->setStatus(next, detail);
	if (state->callbacks.changed)
		state->callbacks.changed(*state);
}

void KWinCapture::focused(const WindowInfo &window)
{
	if (window.active && !wasFocused && config.raiseOnFocus && state->callbacks.focused)
		state->callbacks.focused();
	wasFocused = window.active;
}

void KWinCapture::reconcile()
{
	if (!state->isAlive()) {
		// Source deletion is queued; stop producing frames while awaiting controller deletion.
		stopStream();
		return;
	}
	const bool showing = state->isShowing();
	if (!showing) {
		retry.stop();
		if (stream)
			stopStream();
	} else if (!state->frames->isEnabled()) {
		// A hide/show pair can arrive before this queued update. Always retire
		// the old producer before allowing publication or rendering again.
		stopStream();
		retry.stop();
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->showing)
			state->frames->setEnabled(true);
	}
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
	const auto windows = backend->windows();
	bool ambiguous;
	auto match = select_window(windows, config.rule, currentWindowUuid, config.preferredWindow, &ambiguous);
	if (stream && match != currentWindowUuid)
		stopStream();
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
	if (!showing) {
		status(CaptureStatus::Paused);
		return;
	}
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
	if (generation != attempt || !state->isAlive() || !state->isShowing())
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
	    },
	    state->callbacks.gpu);
	if (!pipewire->start(-1, node, serial)) {
		failed(QStringLiteral("Could not connect to the KWin PipeWire stream."));
		return;
	}
	deadline.start();
}

void KWinCapture::streamFailed(uint64_t attempt, const QString &error)
{
	if (generation == attempt && state->isAlive())
		failed(error);
}

void KWinCapture::streamStatusChanged(uint64_t attempt, PipeWireStatus next)
{
	if (generation != attempt || !state->isAlive() || !state->isShowing())
		return;
	switch (next) {
	case PipeWireStatus::Connecting:
		status(CaptureStatus::Connecting);
		break;
	case PipeWireStatus::Paused:
		status(CaptureStatus::Paused);
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
	if (state->callbacks.error)
		state->callbacks.error(error);
	persist();
	if (state->isShowing())
		retry.start();
}

void KWinCapture::persist()
{
	if (state->callbacks.save)
		state->callbacks.save(*state);
}
