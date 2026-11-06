#include "audio-fixture.hpp"
#include "capture-test-fixture.hpp"
#include "capture.hpp"
#include "kwin-connection.hpp"
#include "kwin-fixture.hpp"
#include "pipewire-capture.hpp"
#include "video-capture-fixture.hpp"

#include <QEvent>
#include <QSignalSpy>
#include <QTest>
#include <vector>

class CaptureControllerTest : public CaptureTestFixture
{
	Q_OBJECT
      private slots:
	void absentWindowWaitsWithoutCaptureRequests()
	{
		auto target = capture();
		int requests = kwin.captureRequests;
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("waiting"), 3000);
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		VideoFrame frame;
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		kwin.add({"arriving", "test.app", "App opened", false});
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 3000);
		QCOMPARE(kwin.requestedWindow, QString("arriving"));
	}

	void desktopTracksCombinedBoundsAndCursor()
	{
		kwin.deferCapture = true;
		kwin.addDisplay({"DP-1", "Left", {-1280, -200, 1280, 1024}, 1});
		kwin.addDisplay({"DP-2", "Main", {0, 0, 1920, 1080}, 2});
		QTRY_COMPARE(capture_displays().size(), 2);
		CaptureConfig config;
		auto target = capture(config, CaptureMode::Desktop);
		QTRY_COMPARE(status(target), QString("connecting"));
		QTRY_COMPARE(kwin.requestedRegion, QRect(-1280, -200, 3200, 1280));
		QCOMPARE(kwin.requestedScale, int32_t(0));
		QCOMPARE(kwin.cursorMode, uint32_t(2));
		int requests = kwin.captureRequests;
		kwin.add({"unrelated", "test.app", "Unrelated window", true});
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		config.cursor = false;
		target->configure(config);
		queue_capture_update(target);
		++requests;
		QTRY_COMPARE(kwin.captureRequests, requests);
		QCOMPARE(kwin.cursorMode, uint32_t(1));
		kwin.addDisplay({"DP-1", "Left", {-1280, 0, 1280, 1024}, 1});
		QTRY_COMPARE(kwin.requestedRegion, QRect(-1280, 0, 3200, 1080));
		++requests;
		QCOMPARE(kwin.captureRequests, requests);
		kwin.removeDisplay("DP-1");
		QTRY_COMPARE(kwin.requestedRegion, QRect(0, 0, 1920, 1080));
		++requests;
		QCOMPARE(kwin.captureRequests, requests);
	}

	void desktopSupportsOlderProtocolsWithoutWindowManagement()
	{
		KWinFixture legacy;
		legacy.windowManagement = false;
		legacy.outputManagerVersion = 2;
		legacy.displayVersion = 2;
		legacy.screencastVersion = 3;
		legacy.deferCapture = true;
		QVERIFY(legacy.start());
		legacy.addDisplay({"DP-1", "Scaled display", {-100, 50, 1920, 1080}, 2});
		qputenv("WAYLAND_DISPLAY", legacy.socketPath().toUtf8());
		KWinConnection backend(nullptr);
		qputenv("WAYLAND_DISPLAY", kwin.socketPath().toUtf8());
		QTRY_VERIFY(backend.desktopAvailable());
		QVERIFY(!backend.available());
		QTRY_COMPARE(backend.displays().size(), 1);
		QCOMPARE(backend.displays().front().geometry, QRect(-100, 50, 1920, 1080));
		std::unique_ptr<KWinStream> stream(backend.captureDesktop({}, true, nullptr));
		QVERIFY(stream);
		backend.flush();
		QTRY_COMPARE(legacy.captureRequests, 1);
		QCOMPARE(legacy.requestedRegion, QRect(-100, 50, 1920, 1080));
		QCOMPARE(legacy.requestedScale, int32_t(512));
	}

	void desktopWaitsForSelectedMonitorAndReconnects()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.addDisplay({"DP-1", "Main", {0, 0, 1920, 1080}, 1});
		CaptureConfig config;
		config.display = "HDMI-A-1";
		auto target = capture(config, CaptureMode::Desktop);
		QTRY_COMPARE(status(target), QString("waiting"));
		int requests = kwin.captureRequests;
		kwin.addDisplay({"HDMI-A-1", "Other", {1920, 0, 1280, 720}, 1});
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		++requests;
		QCOMPARE(kwin.captureRequests, requests);
		QCOMPARE(kwin.requestedDisplay, QString("HDMI-A-1"));
		kwin.removeDisplay("HDMI-A-1");
		QTRY_COMPARE(status(target), QString("waiting"));
		VideoFrame frame;
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QCOMPARE(kwin.captureRequests, requests);
		kwin.addDisplay({"HDMI-A-1", "Other", {1920, 0, 1280, 720}, 1});
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		++requests;
		QCOMPARE(kwin.captureRequests, requests);
		target->setShowing(false);
		queue_capture_update(target);
		QTRY_COMPARE(status(target), QString("paused"));
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		kwin.disconnectClients();
		QTRY_VERIFY(capture_displays().isEmpty());
		QTRY_COMPARE_WITH_TIMEOUT(capture_displays().size(), 2, 5000);
		QTRY_COMPARE(status(target), QString("paused"));
		QCOMPARE(kwin.captureRequests, requests);
		target->setShowing(true);
		queue_capture_update(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		++requests;
		QCOMPARE(kwin.captureRequests, requests);
	}

	void desktopPublishesCoreScaleChanges_data()
	{
		QTest::addColumn<uint32_t>("outputVersion");
		QTest::newRow("xdg-output-v2") << uint32_t(2);
		QTest::newRow("xdg-output-v3") << uint32_t(3);
	}

	void desktopPublishesCoreScaleChanges()
	{
		QFETCH(uint32_t, outputVersion);
		KWinFixture server;
		server.outputManagerVersion = outputVersion;
		server.screencastVersion = 3;
		server.deferCapture = true;
		QVERIFY(server.start());
		server.addDisplay({"DP-1", "Display", {0, 0, 1920, 1080}, 1});
		qputenv("WAYLAND_DISPLAY", server.socketPath().toUtf8());
		KWinConnection backend(nullptr);
		qputenv("WAYLAND_DISPLAY", kwin.socketPath().toUtf8());
		QTRY_COMPARE(backend.displays().size(), 1);
		auto state = std::make_shared<CaptureState>(1280, 720, CaptureCallbacks{}, CaptureMode::Desktop);
		CaptureConfig config;
		state->configure(config);
		M4Capture controller(state, &backend, nullptr);
		controller.update();
		QTRY_COMPARE(server.captureRequests, 1);
		QCOMPARE(server.requestedScale, int32_t(256));
		server.displayScale("DP-1", 2);
		QTRY_COMPARE(backend.displays().front().scale, 2);
		QTRY_COMPARE(server.captureRequests, 2);
		QCOMPARE(server.requestedScale, int32_t(512));
		const auto layout = backend.displays().front();
		server.displayDescription("DP-1", "Renamed display");
		QTRY_COMPARE(backend.displays().front().description, QString("Renamed display"));
		QVERIFY(backend.displays().front().sameCaptureLayout(layout));
		QVERIFY(!(backend.displays().front() == layout));
		QCOMPARE(server.captureRequests, 2);
		if (outputVersion == 2) {
			// A core batch must not publish an unfinished XDG geometry batch.
			server.displayGeometry("DP-1", {100, 0, 1920, 1080}, false);
			server.displayScale("DP-1", 3);
			QTRY_COMPARE(backend.displays().front().scale, 3);
			QCOMPARE(backend.displays().front().geometry, layout.geometry);
			QTRY_COMPARE(server.captureRequests, 3);
			QCOMPARE(server.requestedRegion, layout.geometry);
			server.displayGeometry("DP-1", {100, 0, 1920, 1080});
			QTRY_COMPARE(server.captureRequests, 4);
			QCOMPARE(server.requestedRegion, QRect(100, 0, 1920, 1080));
		}
	}

	void windowAndDisplayNotificationsAreIndependent()
	{
		kwin.add({"independent", "test.app", "Editor", false});
		kwin.addDisplay({"DP-1", "Display", {0, 0, 1920, 1080}, 1});
		KWinConnection backend(nullptr);
		QTRY_COMPARE(backend.windows().size(), 1);
		QTRY_COMPARE(backend.displays().size(), 1);
		QSignalSpy windowsChanged(&backend, &KWinConnection::windowsChanged);
		QSignalSpy displaysChanged(&backend, &KWinConnection::displaysChanged);
		const auto displays = backend.displays();
		kwin.title("independent", "New title");
		QTRY_COMPARE(backend.windows().front().title, QString("New title"));
		QVERIFY(windowsChanged.count() > 0);
		QCOMPARE(displaysChanged.count(), 0);
		QCOMPARE(backend.displays().constData(), displays.constData());
		const auto windows = backend.windows();
		windowsChanged.clear();
		kwin.displayScale("DP-1", 2);
		QTRY_COMPARE(backend.displays().front().scale, 2);
		QVERIFY(displaysChanged.count() > 0);
		QCOMPARE(windowsChanged.count(), 0);
		QCOMPARE(backend.windows().constData(), windows.constData());
		displaysChanged.clear();
		kwin.displayScale("DP-1", 2);
		QTest::qWait(60);
		QCOMPARE(displaysChanged.count(), 0);
	}

	void captureModeNormalizesConfiguration_data()
	{
		QTest::addColumn<CaptureMode>("mode");
		QTest::newRow("window") << CaptureMode::Window;
		QTest::newRow("desktop") << CaptureMode::Desktop;
		QTest::newRow("application-audio") << CaptureMode::ApplicationAudio;
	}

	void captureModeNormalizesConfiguration()
	{
		QFETCH(CaptureMode, mode);
		CaptureState state(1, 1, {}, mode);
		const bool audioOnly = mode == CaptureMode::ApplicationAudio;
		QCOMPARE(state.configuration().audio, audioOnly);
		QCOMPARE(state.configuration().cursor, !audioOnly);
		QVERIFY(!state.configuration().followFocus);
		CaptureConfig config;
		config.rule.appId = "test.app";
		config.raiseOnFocus = true;
		config.followFocus = true;
		state.configure(config);
		auto normalized = state.configuration();
		QCOMPARE(normalized.audio, audioOnly);
		QCOMPARE(normalized.cursor, !audioOnly);
		QCOMPARE(normalized.raiseOnFocus, mode == CaptureMode::Window);
		QCOMPARE(normalized.followFocus, mode == CaptureMode::Window);
		QCOMPARE(normalized.rule.appId, mode == CaptureMode::Desktop ? QString{} : config.rule.appId);
	}

	void windowSnapshotsAreSharedAndRetained()
	{
		KWinConnection backend(nullptr);
		kwin.add({"snapshot", "test.app", "Editor", false});
		QTRY_COMPARE(backend.windows().size(), 1);
		const auto original = backend.windows();
		const auto shared = backend.windows();
		QCOMPARE(original.constData(), shared.constData());
		kwin.title("snapshot", "Changed");
		QTRY_COMPARE(backend.windows().front().title, QString("Changed"));
		const auto changed = backend.windows();
		QCOMPARE(changed.constData(), backend.windows().constData());
		QCOMPARE(original.front().title, QString("Editor"));
		QCOMPARE(shared.front().title, QString("Editor"));
		kwin.focus("snapshot");
		QTRY_VERIFY(backend.windows().front().active);
		QVERIFY(!changed.front().active);
		kwin.add({"second", "test.app", "Second", false});
		QTRY_COMPARE(backend.windows().size(), 2);
		kwin.remove("snapshot");
		QTRY_COMPARE(backend.windows().size(), 1);
		QCOMPARE(backend.windows().front().uuid, QString("second"));
	}

	void pipewireNotifiesImportLifetime()
	{
		class ImportLifetime : public DmaBufCapture
		{
		      public:
			std::atomic<uint64_t> removed{0}, cleared{0};
			FrameResult copy(const spa_buffer &, const spa_video_info_raw &, VideoFrame &) override
			{
				return FrameResult::UnmappedBuffer;
			}
			void discard(const spa_buffer &) override {}
			void removeBuffer(const spa_buffer &) override { ++removed; }
			void clearImports() override { ++cleared; }
		};
		VideoCaptureFixture video;
		QVERIFY(video.server.start());
		QVERIFY(video.producer.start());
		auto gpu = std::make_shared<ImportLifetime>();
		auto frames = std::make_shared<FrameBuffer>(160, 90);
		for (int stream = 0; stream < 2; ++stream) {
			auto cleared = gpu->cleared.load();
			auto removed = gpu->removed.load();
			auto sequence = frames->sequence();
			auto capture = std::make_unique<PipeWireCapture>(
			    frames, [](QString error) { QFAIL(qPrintable(error)); }, [](PipeWireStatus) {}, gpu);
			QVERIFY(capture->start(video.server.openRemote(), video.producer.nodeId(),
					       QString::number(video.producer.serial())));
			QTRY_VERIFY_WITH_TIMEOUT(frames->sequence() > sequence, 5000);
			QVERIFY(gpu->cleared.load() > cleared); // Format negotiation invalidates the prior pool.
			cleared = gpu->cleared.load();
			capture.reset();
			QVERIFY(gpu->removed.load() > removed);
			QVERIFY(gpu->cleared.load() > cleared);
		}
	}

	void followsFocusedMatchingWindow_data()
	{
		QTest::addColumn<bool>("disconnect");
		QTest::newRow("continuous-title-matching") << true;
		QTest::newRow("keep-changed-title") << false;
	}

	void followsFocusedMatchingWindow()
	{
		QFETCH(bool, disconnect);
		kwin.deferCapture = true;
		kwin.add({"first", "test.app", "Editor one", false});
		kwin.add({"second", "test.app", "Editor two", true});
		kwin.add({"wrong-title", "test.app", "Settings", false});
		kwin.add({"wrong-app", "other.app", "Editor three", false});
		QTRY_COMPARE(capture_windows().size(), 4);
		CaptureConfig config;
		config.rule = {"test.app", "Editor"};
		config.rule.disconnectOnTitleMismatch = disconnect;
		config.preferredWindow = "first";
		auto target = capture(config);
		QTRY_COMPARE(status(target), QString("connecting"));
		QTRY_COMPARE(kwin.requestedWindow, QString("first"));
		int requests = kwin.captureRequests;
		config.followFocus = true;
		target->configure(config);
		queue_capture_update(target);
		++requests;
		QTRY_COMPARE(kwin.captureRequests, requests);
		QCOMPARE(kwin.requestedWindow, QString("second"));
		for (const char *uuid : {"wrong-title", "wrong-app", "second"}) {
			kwin.focus(uuid);
			QTest::qWait(60);
			QCOMPARE(kwin.captureRequests, requests);
			QCOMPARE(kwin.requestedWindow, QString("second"));
		}
		kwin.focus("first");
		++requests;
		QTRY_COMPARE(kwin.captureRequests, requests);
		QCOMPARE(kwin.requestedWindow, QString("first"));
		// Switching follows focus without changing the saved picker preference.
		QCOMPARE(target->configuration().preferredWindow, QString("first"));
		kwin.focus("second");
		++requests;
		QTRY_COMPARE(kwin.captureRequests, requests);
		QCOMPARE(kwin.requestedWindow, QString("second"));
		config.followFocus = false;
		target->configure(config);
		queue_capture_update(target);
		kwin.focus("first");
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		QCOMPARE(kwin.requestedWindow, QString("second"));
	}

	void focusResolvesAmbiguousSelection()
	{
		kwin.deferCapture = true;
		kwin.add({"first", "test.app", "Editor one", false});
		kwin.add({"second", "test.app", "Editor two", false});
		QTRY_COMPARE(capture_windows().size(), 2);
		CaptureConfig config;
		config.followFocus = true;
		auto target = capture(config);
		QTRY_COMPARE(status(target), QString("waiting"));
		int requests = kwin.captureRequests;
		kwin.focus("second");
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow, QString("second"));
		kwin.remove("second");
		QTRY_COMPARE(kwin.captureRequests, requests + 2);
		QCOMPARE(kwin.requestedWindow, QString("first"));
	}

	void focusedTargetDoesNotInheritRetryDelay()
	{
		kwin.add({"first", "test.app", "Editor one", true});
		kwin.add({"second", "test.app", "Editor two", false});
		CaptureConfig config;
		config.followFocus = true;
		auto target = capture(config);
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		kwin.deferCapture = true;
		kwin.focus("second");
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 1000);
		QCOMPARE(kwin.requestedWindow, QString("second"));
		QCOMPARE(status(target), QString("connecting"));
	}

	void failedCaptureRetriesWithDelay()
	{
		kwin.add({"retry", "test.app", "Retry target", false});
		auto target = capture();
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		kwin.deferCapture = true;
		kwin.focus("retry");
		QTest::qWait(200);
		QCOMPARE(kwin.captureRequests, requests);
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 3500);
		QCOMPARE(status(target), QString("connecting"));
	}

	void releasedCaptureCancelsRetry()
	{
		kwin.add({"released", "test.app", "Released target", false});
		auto target = capture();
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		release_capture(target);
		captures.clear();
		kwin.deferCapture = true;
		kwin.focus("released");
		QTest::qWait(2200);
		QCOMPARE(kwin.captureRequests, requests);
	}

	void configurationChangesResumeFailedCapture_data()
	{
		QTest::addColumn<int>("change");
		QTest::newRow("selected-window") << 0;
		QTest::newRow("application") << 1;
		QTest::newRow("title-filter") << 2;
		QTest::newRow("title-match") << 3;
		QTest::newRow("case-sensitive") << 4;
		QTest::newRow("title-policy") << 5;
		QTest::newRow("cursor") << 6;
	}

	void configurationChangesResumeFailedCapture()
	{
		QFETCH(int, change);
		kwin.add({"original", "test.app", "Editor", false});
		CaptureConfig config;
		config.rule.title = "Editor";
		auto target = capture(config);
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		kwin.deferCapture = true;
		config = target->configuration();
		switch (change) {
		case 0:
			config.preferredWindow = "original";
			break;
		case 1:
			kwin.add({"other-app", "other.app", "Editor", false});
			config.rule.appId = "other.app";
			QTRY_COMPARE(capture_windows().size(), 2);
			break;
		case 2:
			config.rule.title = "Edit";
			break;
		case 3:
			config.rule.mode = TitleMatch::Exact;
			break;
		case 4:
			config.rule.caseSensitive = true;
			break;
		case 5:
			config.rule.disconnectOnTitleMismatch = !config.rule.disconnectOnTitleMismatch;
			break;
		case 6:
			config.cursor = false;
			break;
		}
		target->configure(config);
		queue_capture_update(target);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow, change == 1 ? QString("other-app") : QString("original"));
		QCOMPARE(status(target), QString("connecting"));
	}

	void backendDisconnectAutomaticallyRecovers()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"backend", "test.app", "Backend reconnect", false});
		CaptureConfig config;
		auto target = capture(config);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		int requests = kwin.captureRequests;
		kwin.disconnectClients();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("failed"), 1000);
		QVERIFY(capture_windows().isEmpty());
		VideoFrame frame;
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QCOMPARE(target->frames->size().width, uint32_t(160));
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
		QVERIFY(target->frames->take(frame) != FrameBuffer::Update::Empty);
	}

	void releasedCaptureDisconnectsBeforeControllerDeletion()
	{
		kwin.deferCapture = true;
		kwin.add({"closing", "test.app", "Closing", false});
		KWinConnection backend(nullptr);
		QTRY_VERIFY(backend.available());
		QTRY_COMPARE(backend.windows().size(), 1);
		auto state = std::make_shared<CaptureState>();
		CaptureConfig config;
		config.rule.appId = "test.app";
		state->configure(config);
		auto controller = std::make_unique<M4Capture>(state, &backend, nullptr);
		QPointer<M4Capture> controllerGuard = controller.get();
		controller->update();
		QTRY_COMPARE(kwin.requestedWindow, QString("closing"));
		QSocketNotifier *reader = nullptr;
		for (auto notifier : backend.findChildren<QSocketNotifier *>())
			if (notifier->type() == QSocketNotifier::Read)
				reader = notifier;
		QVERIFY(reader);
		controller.release();
		release_capture(state);
		kwin.disconnectClients();
		// Force the socket event ahead of the queued controller deletion to reproduce the race.
		QEvent socketEvent(QEvent::SockAct);
		QCoreApplication::sendEvent(reader, &socketEvent);
		QVERIFY(!backend.available());
		QCoreApplication::sendPostedEvents();
		QVERIFY(!controllerGuard);
	}

	void streamOutlivesBackend_data()
	{
		QTest::addColumn<bool>("disconnectFirst");
		QTest::newRow("connection-destroyed") << false;
		QTest::newRow("connection-disconnected") << true;
	}

	void streamOutlivesBackend()
	{
		QFETCH(bool, disconnectFirst);
		kwin.deferCapture = true;
		kwin.add({"retained", "test.app", "Retained stream", false});
		auto backend = std::make_unique<KWinConnection>(nullptr);
		QTRY_VERIFY(backend->available());
		// No controller or change observer participates in stream teardown.
		std::unique_ptr<KWinStream> stream(backend->capture("retained", true, nullptr));
		QVERIFY(stream);
		QSignalSpy closed(stream.get(), &KWinStream::closed);
		backend->flush();
		QTRY_COMPARE(kwin.requestedWindow, QString("retained"));
		if (disconnectFirst) {
			kwin.disconnectClients();
			QTRY_VERIFY(!backend->available());
			QCOMPARE(closed.count(), 1);
		}
		backend.reset();
		QCOMPARE(closed.count(), 1);
		stream.reset();
	}

	void controllerOutlivesBackend()
	{
		kwin.deferCapture = true;
		kwin.add({"controller", "test.app", "Controller lifetime", false});
		auto backend = std::make_unique<KWinConnection>(nullptr);
		QTRY_VERIFY(backend->available());
		QTRY_COMPARE(backend->windows().size(), 1);
		auto state = std::make_shared<CaptureState>();
		CaptureConfig config;
		config.rule.appId = "test.app";
		state->configure(config);
		auto controller = std::make_unique<M4Capture>(state, backend.get(), nullptr);
		controller->update();
		QTRY_COMPARE(kwin.requestedWindow, QString("controller"));
		backend.reset();
		QTRY_COMPARE(state->diagnostics().status, CaptureStatus::Failed);
		controller->update();
		QCOMPARE(state->diagnostics().status, CaptureStatus::Failed);
		controller.reset();
	}

	void hiddenCaptureRequiresFreshFrames()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"visibility", "test.app", "Visibility", false});
		auto target = capture();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		VideoFrame frame;
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Updated);
		auto oldPixels = frame.pixels;
		target->setShowing(false);
		queue_capture_update(target);
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QTRY_COMPARE(status(target), QString("paused"));
		auto sequence = target->frames->sequence();
		int requests = kwin.captureRequests;
		QTest::qWait(150);
		QCOMPARE(target->frames->sequence(), sequence);
		QVERIFY(video.producer.setActive(false));
		video.producer.setColor(0xff20c040u);
		target->setShowing(true);
		queue_capture_update(target);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QTest::qWait(100);
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QCOMPARE(target->frames->sequence(), sequence);
		QVERIFY(video.producer.setActive(true));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Updated);
		QVERIFY(frame.pixels != oldPixels);
		// A queued update must not collapse a rapid hide/show into old-stream reuse.
		requests = kwin.captureRequests;
		target->setShowing(false);
		target->setShowing(true);
		queue_capture_update(target);
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
	}

	void audioOnlyDoesNotRequireScreencast()
	{
		KWinFixture audioKwin;
		QVERIFY(audioKwin.start(false));
		const auto previousDisplay = qgetenv("WAYLAND_DISPLAY");
		const auto previousRemote = qgetenv("PIPEWIRE_REMOTE");
		const bool hadRemote = qEnvironmentVariableIsSet("PIPEWIRE_REMOTE");
		auto restore = qScopeGuard([&] {
			qputenv("WAYLAND_DISPLAY", previousDisplay);
			if (hadRemote)
				qputenv("PIPEWIRE_REMOTE", previousRemote);
			else
				qunsetenv("PIPEWIRE_REMOTE");
		});
		qputenv("WAYLAND_DISPLAY", audioKwin.socketPath().toUtf8());
		KWinConnection backend(nullptr);
		QTRY_VERIFY(backend.windowsAvailable());
		QVERIFY(!backend.available());
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		AudioProducer producer;
		QVERIFY(producer.start(daemon.openRemote(), "test.app"));
		std::atomic<uint64_t> samples{0};
		CaptureCallbacks callbacks;
		callbacks.audio = [&](const AudioFrame &frame) {
			if (frame.data[0][0] == .125f && frame.data[1][0] == -.25f)
				++samples;
		};
		auto target = std::make_shared<CaptureState>(1, 1, std::move(callbacks), CaptureMode::ApplicationAudio);
		CaptureConfig config;
		config.rule.appId = "test.app";
		target->configure(config);
		M4Capture controller(target, &backend, nullptr);
		controller.update();
		QCOMPARE(status(target), QString("waiting"));
		// A failed audio connection retries without requesting a video stream.
		qputenv("PIPEWIRE_REMOTE", "/nonexistent/m4capture-audio-test");
		audioKwin.add({"audio", "test.app", "Application audio", false});
		QTRY_COMPARE(status(target), QString("failed"));
		qputenv("PIPEWIRE_REMOTE", daemon.socketPath().toUtf8());
		QTRY_VERIFY_WITH_TIMEOUT(samples.load() > 0, 5000);
		QCOMPARE(status(target), QString("capturing"));
		QCOMPARE(audioKwin.captureRequests, 0);
		QCOMPARE(target->frames->sequence(), uint64_t(0));
		// Frame-buffer changes do not control audio lifecycle.
		target->frames->setEnabled(false);
		controller.update();
		QTest::qWait(100);
		QCOMPARE(daemon.audioCapturesCreated(), uint32_t(1));
		target->frames->setEnabled(true);
		target->setShowing(false);
		target->setShowing(true);
		controller.update();
		QTRY_COMPARE(daemon.audioCapturesCreated(), uint32_t(2));
		target->setShowing(false);
		controller.update();
		QCOMPARE(status(target), QString("paused"));
		auto count = samples.load();
		QTest::qWait(100);
		QCOMPARE(samples.load(), count);
		target->setShowing(true);
		controller.update();
		QTRY_VERIFY(samples.load() > count);
		audioKwin.remove("audio");
		QTRY_COMPARE(status(target), QString("waiting"));
		count = samples.load();
		QTest::qWait(100);
		QCOMPARE(samples.load(), count);
		audioKwin.add({"reopened", "test.app", "Reopened", false});
		QTRY_VERIFY(samples.load() > count);
		QCOMPARE(audioKwin.captureRequests, 0);
		QCOMPARE(target->frames->sequence(), uint64_t(0));
	}

	void captureSourcesHandleInterfaceChanges_data()
	{
		QTest::addColumn<int>("interface");
		QTest::newRow("window-management") << 0;
		QTest::newRow("output-discovery") << 1;
		QTest::newRow("screencast") << 2;
		QTest::newRow("connection") << 3;
	}

	void captureSourcesHandleInterfaceChanges()
	{
		QFETCH(int, interface);
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		AudioProducer producer;
		QVERIFY(producer.start(video.server.openRemote(), "test.app"));
		kwin.add({"audio", "test.app", "Application audio", false});
		kwin.addDisplay({"DP-1", "Display", {0, 0, 160, 90}, 1});
		auto setAvailable = [&](bool available) {
			switch (interface) {
			case 0:
				kwin.setWindowManagementAvailable(available);
				break;
			case 1:
				kwin.setOutputDiscoveryAvailable(available);
				break;
			case 2:
				kwin.setScreencastAvailable(available);
				break;
			case 3:
				if (!available)
					kwin.disconnectClients();
				break;
			}
		};
		auto restoreInterface = qScopeGuard([&] { setAvailable(true); });
		KWinConnection backend(nullptr);
		QTRY_VERIFY(backend.available() && backend.desktopAvailable());
		QTRY_COMPARE(backend.windows().size(), 1);
		QTRY_COMPARE(backend.displays().size(), 1);
		std::atomic<uint64_t> samples{0};
		CaptureCallbacks callbacks;
		callbacks.audio = [&](const AudioFrame &frame) {
			if (frame.data[0][0] == .125f)
				++samples;
		};
		auto audioState =
		    std::make_shared<CaptureState>(1, 1, std::move(callbacks), CaptureMode::ApplicationAudio);
		auto windowState = std::make_shared<CaptureState>();
		auto desktopState = std::make_shared<CaptureState>(160, 90, CaptureCallbacks{}, CaptureMode::Desktop);
		CaptureConfig config;
		config.rule.appId = "test.app";
		config.display = "DP-1";
		audioState->configure(config);
		windowState->configure(config);
		desktopState->configure(config);
		M4Capture audioController(audioState, &backend, nullptr);
		M4Capture windowController(windowState, &backend, nullptr);
		M4Capture desktopController(desktopState, &backend, nullptr);
		audioController.update();
		windowController.update();
		desktopController.update();
		QTRY_VERIFY(samples.load() > 0);
		QTRY_COMPARE(status(windowState), QString("capturing"));
		QTRY_COMPARE(status(desktopState), QString("capturing"));
		QTRY_COMPARE(video.server.audioCapturesCreated(), uint32_t(1));
		const auto windowGeneration = windowState->diagnostics().generation;
		const auto desktopGeneration = desktopState->diagnostics().generation;
		const bool loseWindows = interface == 0 || interface == 3;
		const bool loseDisplays = interface == 1 || interface == 3;
		const bool loseWindowVideo = loseWindows || interface == 2;
		const bool loseDesktopVideo = loseDisplays || interface == 2;
		setAvailable(false);
		QTRY_COMPARE(backend.windowsAvailable(), !loseWindows);
		QTRY_COMPARE(backend.displaysAvailable(), !loseDisplays);
		QTRY_COMPARE(backend.available(), !loseWindowVideo);
		QTRY_COMPARE(backend.desktopAvailable(), !loseDesktopVideo);
		QTRY_COMPARE(backend.windows().size(), loseWindows ? 0 : 1);
		QTRY_COMPARE(backend.displays().size(), loseDisplays ? 0 : 1);
		QTRY_COMPARE(status(windowState), loseWindowVideo ? QString("failed") : QString("capturing"));
		QTRY_COMPARE(status(desktopState), loseDesktopVideo ? QString("failed") : QString("capturing"));
		QTRY_COMPARE(status(audioState), loseWindows ? QString("failed") : QString("capturing"));
		VideoFrame frame;
		if (loseWindowVideo)
			QCOMPARE(windowState->frames->take(frame), FrameBuffer::Update::Empty);
		else
			QCOMPARE(windowState->diagnostics().generation, windowGeneration);
		if (loseDesktopVideo)
			QCOMPARE(desktopState->frames->take(frame), FrameBuffer::Update::Empty);
		else
			QCOMPARE(desktopState->diagnostics().generation, desktopGeneration);
		auto count = samples.load();
		QTest::qWait(100);
		if (loseWindows)
			QCOMPARE(samples.load(), count);
		else
			QVERIFY(samples.load() > count);
		setAvailable(true);
		QTRY_VERIFY_WITH_TIMEOUT(backend.available() && backend.desktopAvailable(), 5000);
		QTRY_COMPARE_WITH_TIMEOUT(status(windowState), QString("capturing"), 5000);
		QTRY_COMPARE_WITH_TIMEOUT(status(desktopState), QString("capturing"), 5000);
		QTRY_VERIFY(samples.load() > count);
		QCOMPARE(video.server.audioCapturesCreated(), loseWindows ? uint32_t(2) : uint32_t(1));
		if (!loseWindowVideo)
			QCOMPARE(windowState->diagnostics().generation, windowGeneration);
		if (!loseDesktopVideo)
			QCOMPARE(desktopState->diagnostics().generation, desktopGeneration);
	}

	void audioSurvivesVideoRestarts()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		AudioProducer producer;
		QVERIFY(producer.start(video.server.openRemote(), "test.app"));
		kwin.add({"audio", "test.app", "Application audio", false});
		KWinConnection backend(nullptr);
		QTRY_COMPARE(backend.windows().size(), 1);
		std::atomic<uint64_t> samples{0};
		CaptureCallbacks callbacks;
		callbacks.audio = [&](const AudioFrame &frame) {
			if (frame.data[0][0] == .125f)
				++samples;
		};
		auto target = std::make_shared<CaptureState>(160, 90, std::move(callbacks));
		CaptureConfig config;
		config.rule.appId = "test.app";
		config.audio = true;
		target->configure(config);
		M4Capture controller(target, &backend, nullptr);
		controller.update();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_VERIFY(samples.load() > 0);
		QTRY_COMPARE(video.server.audioCapturesCreated(), uint32_t(1));

		int requests = kwin.captureRequests;
		config.cursor = false;
		target->configure(config);
		controller.update();
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QTRY_COMPARE(status(target), QString("capturing"));
		QCOMPARE(video.server.audioCapturesCreated(), uint32_t(1));

		kwin.reject = true;
		config.cursor = true;
		target->configure(config);
		controller.update();
		QTRY_COMPARE(status(target), QString("failed"));
		auto count = samples.load();
		QTest::qWait(150);
		QVERIFY(samples.load() > count);
		QCOMPARE(video.server.audioCapturesCreated(), uint32_t(1));
		requests = kwin.captureRequests;
		kwin.reject = false;
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 3500);
		QTRY_COMPARE(status(target), QString("capturing"));
		QCOMPARE(video.server.audioCapturesCreated(), uint32_t(1));

		// Collapsed hide/show updates still discard queued audio from the old producer.
		target->setShowing(false);
		target->setShowing(true);
		controller.update();
		QTRY_COMPARE(video.server.audioCapturesCreated(), uint32_t(2));
		QTRY_COMPARE(status(target), QString("capturing"));
	}

	void hiddenCaptureRetainsBackendTrackingWithoutRetries()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.reject = true;
		kwin.add({"retry-hidden", "test.app", "Hidden retry", false});
		auto target = capture();
		QTRY_COMPARE(status(target), QString("failed"));
		target->setShowing(false);
		queue_capture_update(target);
		QTRY_COMPARE(status(target), QString("paused"));
		int requests = kwin.captureRequests;
		kwin.reject = false;
		QTest::qWait(2200);
		QCOMPARE(kwin.captureRequests, requests);
		kwin.disconnectClients();
		QTRY_VERIFY_WITH_TIMEOUT(capture_windows().isEmpty(), 1000);
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		QTRY_COMPARE(status(target), QString("paused"));
		QCOMPARE(kwin.captureRequests, requests);
		target->setShowing(true);
		queue_capture_update(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
	}

	void pausedStreamResumesWithoutReselecting()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"paused", "test.app", "Paused stream", false});
		auto target = capture();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		int requests = kwin.captureRequests;
		QVERIFY(video.producer.setActive(false));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("paused"), 3000);
		VideoFrame frame;
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QVERIFY(video.producer.setActive(true));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests);
	}
};

QTEST_GUILESS_MAIN(CaptureControllerTest)
#include "capture-controller-test.moc"
