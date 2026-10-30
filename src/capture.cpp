#include "capture.hpp"
#include "kwin-connection.hpp"
#include "pipewire-capture.hpp"

#include <QThread>
#include <algorithm>

namespace
{
bool requiresTargetSelection(const CaptureConfig &a, const CaptureConfig &b)
{
	return a.target != b.target || a.display != b.display || a.rule.appId != b.rule.appId ||
	       a.rule.title != b.rule.title || a.rule.mode != b.rule.mode ||
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
	if (next.target == CaptureConfig::Target::Desktop || !next.audio || requiresTargetSelection(next, config))
		audioEnabled = false;
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
	if (!next) {
		frames->setEnabled(false);
		audioEnabled = false;
	}
}

bool CaptureState::isShowing() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return showing;
}

void CaptureState::setStatus(CaptureStatus next, const QString &detail, uint64_t generation, const QString &target)
{
	std::lock_guard<std::mutex> lock(mutex);
	diagnostic = {next, detail, generation, target};
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

M4Capture::M4Capture(std::shared_ptr<CaptureState> state, KWinConnection *backend, QObject *parent)
    : QObject(parent), state(std::move(state)), backend(backend)
{
	Q_ASSERT(QThread::currentThread() == thread());
	Q_ASSERT(!this->state->controller);
	this->state->controller = this;
	retry.setSingleShot(true);
	retry.setInterval(2000);
	deadline.setSingleShot(true);
	deadline.setInterval(15000);
	audioRetry.setSingleShot(true);
	audioRetry.setInterval(2000);
	connect(&audioRetry, &QTimer::timeout, this, [this] { reconcile(); });
	connect(&retry, &QTimer::timeout, this, [this] { reconcile(); });
	connect(&deadline, &QTimer::timeout, this, [this] {
		if (pipewire)
			pipewire->disableGpu();
		failed(QStringLiteral("No video frames were received."));
	});
	connect(backend, &KWinConnection::windowsChanged, this, [this] {
		if (config.target == CaptureConfig::Target::Window)
			reconcile();
	});
	connect(backend, &KWinConnection::displaysChanged, this, [this] {
		if (config.target == CaptureConfig::Target::Desktop)
			reconcile();
	});
}

M4Capture::~M4Capture()
{
	stopAudio();
	stopStream();
}

void M4Capture::update()
{
	Q_ASSERT(QThread::currentThread() == thread());
	applyConfiguration();
	reconcile();
}

void M4Capture::applyConfiguration()
{
	auto next = state->configuration();
	bool selectTarget = requiresTargetSelection(next, config);
	next.rule = config.rule.updated(next.rule);
	bool restart = next.cursor != config.cursor || next.target != config.target;
	if (next.target == CaptureConfig::Target::Desktop) {
		stopAudio();
		next.audio = false;
		next.raiseOnFocus = false;
		restart |= next.display != config.display;
	} else if (selectTarget) {
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

void M4Capture::stopStream()
{
	++generation;
	deadline.stop();
	pipewire.reset();
	stream.reset();
	if (backend)
		backend->flush();
	state->frames->clear();
}

void M4Capture::status(CaptureStatus next, const QString &detail)
{
	state->setStatus(next, detail, generation,
			 config.target == CaptureConfig::Target::Desktop ? config.display : currentWindowUuid);
	if (state->callbacks.changed)
		state->callbacks.changed(*state);
}

void M4Capture::focused(const WindowInfo &window)
{
	if (window.active && !wasFocused && config.raiseOnFocus && state->callbacks.focused)
		state->callbacks.focused();
	wasFocused = window.active;
}

void M4Capture::reconcile()
{
	if (!state->isAlive()) {
		// Source deletion is queued; stop producing frames while awaiting controller deletion.
		stopAudio();
		stopStream();
		return;
	}
	const bool showing = state->isShowing();
	if (!showing) {
		retry.stop();
		stopAudio();
		if (stream)
			stopStream();
	} else if (!state->frames->isEnabled()) {
		// A hide/show pair can arrive before this queued update. Always retire
		// the old producer before allowing publication or rendering again.
		stopAudio();
		stopStream();
		retry.stop();
		std::lock_guard<std::mutex> lock(state->mutex);
		if (state->showing)
			state->frames->setEnabled(true);
	}
	if (config.target == CaptureConfig::Target::Window && config.rule.appId.isEmpty()) {
		stopAudio();
		status(CaptureStatus::Idle, QStringLiteral("Choose a window or enter its application ID."));
		return;
	}
	if (!backend ||
	    !(config.target == CaptureConfig::Target::Desktop ? backend->desktopAvailable() : backend->available())) {
		stopAudio();
		auto error = backend ? backend->error() : QStringLiteral("The KWin connection was destroyed.");
		if (error.isEmpty())
			error = QStringLiteral("KWin desktop screencast or output discovery is unavailable.");
		if (stream)
			failed(error);
		else
			status(CaptureStatus::Failed, error);
		return;
	}
	if (config.target == CaptureConfig::Target::Desktop)
		reconcileDesktop(showing);
	else
		reconcileWindow(showing);
}

void M4Capture::reconcileWindow(bool showing)
{
	if (!config.rule.error().isEmpty()) {
		stopAudio();
		status(CaptureStatus::Failed, config.rule.error());
		return;
	}
	const auto windows = backend->windows();
	bool ambiguous;
	auto match = select_window(windows, config.rule, currentWindowUuid, config.preferredWindow, &ambiguous);
	if (stream && match != currentWindowUuid)
		stopStream();
	if (match.isEmpty()) {
		stopAudio();
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
	for (const auto &window : windows)
		if (window.uuid == currentWindowUuid)
			updateAudio(window);
}

void M4Capture::reconcileDesktop(bool showing)
{
	QList<DisplayInfo> displays;
	for (const auto &display : backend->displays()) {
		if (!config.display.isEmpty() && display.name != config.display)
			continue;
		displays.append(display);
	}
	const bool sameLayout = displays.size() == desktopDisplays.size() &&
				std::equal(displays.cbegin(), displays.cend(), desktopDisplays.cbegin(),
					   [](const auto &a, const auto &b) { return a.sameCaptureLayout(b); });
	if (!sameLayout) {
		stopStream();
		retry.stop();
		desktopDisplays = std::move(displays);
	}
	if (desktopDisplays.isEmpty()) {
		status(CaptureStatus::Waiting, QStringLiteral("Waiting for the selected display."));
		return;
	}
	if (!showing) {
		status(CaptureStatus::Paused);
		return;
	}
	if (!stream && !retry.isActive())
		startStream();
}

void M4Capture::stopAudio()
{
	state->audioEnabled = false;
	++audioGeneration;
	audioRetry.stop();
	audio.reset();
	audioAppId.clear();
	audioPid = 0;
}

void M4Capture::updateAudio(const WindowInfo &window)
{
	if (!config.audio || !state->callbacks.audio) {
		stopAudio();
		return;
	}
	if (audio && (audioAppId != window.appId || audioPid != window.pid))
		stopAudio();
	if (audio) {
		state->audioEnabled = true;
		return;
	}
	if (audioRetry.isActive())
		return;
	audioAppId = window.appId;
	audioPid = window.pid;
	const auto attempt = audioGeneration;
	audio = std::make_unique<PipeWireAudioCapture>(
	    window.appId, window.pid,
	    [state = state](const AudioFrame &frame) {
		    if (state->isAlive() && state->audioEnabled)
			    state->callbacks.audio(frame);
	    },
	    [this, attempt](QString error) {
		    QMetaObject::invokeMethod(
			this, [this, attempt, error] { audioFailed(attempt, error); }, Qt::QueuedConnection);
	    });
	state->audioEnabled = true;
	if (!audio->start())
		audioFailed(attempt, QStringLiteral("Could not connect to PipeWire for application audio."));
}

void M4Capture::audioFailed(uint64_t attempt, const QString &error)
{
	if (attempt != audioGeneration || !state->isAlive())
		return;
	stopAudio();
	if (state->callbacks.error)
		state->callbacks.error(error);
	if (config.audio && state->isShowing())
		audioRetry.start();
}

void M4Capture::startStream()
{
	status(CaptureStatus::Connecting);
	stream.reset(config.target == CaptureConfig::Target::Desktop
			 ? backend->captureDesktop(config.display, config.cursor, this)
			 : backend->capture(currentWindowUuid, config.cursor, this));
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

void M4Capture::streamReady(uint64_t attempt, uint32_t node, const QString &serial)
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

void M4Capture::streamFailed(uint64_t attempt, const QString &error)
{
	if (generation == attempt && state->isAlive())
		failed(error);
}

void M4Capture::streamStatusChanged(uint64_t attempt, PipeWireStatus next)
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

void M4Capture::failed(const QString &error)
{
	stopStream();
	status(CaptureStatus::Failed, error);
	if (state->callbacks.error)
		state->callbacks.error(error);
	persist();
	if (state->isShowing())
		retry.start();
}

void M4Capture::persist()
{
	if (state->callbacks.save)
		state->callbacks.save(*state);
}
