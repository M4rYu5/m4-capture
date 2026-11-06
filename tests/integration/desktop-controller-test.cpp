#include "capture-test-fixture.hpp"
#include "video-capture-fixture.hpp"

class DesktopControllerTest : public CaptureTestFixture
{
	Q_OBJECT
      private slots:
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
		auto backend = [&] {
			ScopedEnvironment selectedDisplay("WAYLAND_DISPLAY");
			selectedDisplay.set(server.socketPath().toUtf8());
			return KWinConnection(nullptr);
		}();
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
};

QTEST_GUILESS_MAIN(DesktopControllerTest)
#include "desktop-controller-test.moc"
