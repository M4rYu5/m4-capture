#include "audio-fixture.hpp"
#include "capture-test-fixture.hpp"
#include "video-capture-fixture.hpp"
#include <QScopeGuard>

enum class BackendInterface { WindowManagement, OutputDiscovery, Screencast, Connection };
Q_DECLARE_METATYPE(BackendInterface)

class CaptureAudioTest : public CaptureTestFixture
{
	Q_OBJECT
      private slots:
	void audioOnlyDoesNotRequireScreencast()
	{
		KWinFixture audioKwin;
		QVERIFY(audioKwin.start(false));
		ScopedEnvironment selectedDisplay("WAYLAND_DISPLAY");
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		selectedDisplay.set(audioKwin.socketPath().toUtf8());
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
		remote.set("/nonexistent/m4capture-audio-test");
		audioKwin.add({"audio", "test.app", "Application audio", false});
		QTRY_COMPARE(status(target), QString("failed"));
		remote.set(daemon.socketPath().toUtf8());
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
		QTest::addColumn<BackendInterface>("interface");
		QTest::newRow("window-management") << BackendInterface::WindowManagement;
		QTest::newRow("output-discovery") << BackendInterface::OutputDiscovery;
		QTest::newRow("screencast") << BackendInterface::Screencast;
		QTest::newRow("connection") << BackendInterface::Connection;
	}

	void captureSourcesHandleInterfaceChanges()
	{
		QFETCH(BackendInterface, interface);
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		AudioProducer producer;
		QVERIFY(producer.start(video.server.openRemote(), "test.app"));
		kwin.add({"audio", "test.app", "Application audio", false});
		kwin.addDisplay({"DP-1", "Display", {0, 0, 160, 90}, 1});
		auto setAvailable = [&](bool available) {
			switch (interface) {
			case BackendInterface::WindowManagement:
				kwin.setWindowManagementAvailable(available);
				break;
			case BackendInterface::OutputDiscovery:
				kwin.setOutputDiscoveryAvailable(available);
				break;
			case BackendInterface::Screencast:
				kwin.setScreencastAvailable(available);
				break;
			case BackendInterface::Connection:
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
		const bool loseWindows =
		    interface == BackendInterface::WindowManagement || interface == BackendInterface::Connection;
		const bool loseDisplays =
		    interface == BackendInterface::OutputDiscovery || interface == BackendInterface::Connection;
		const bool loseWindowVideo = loseWindows || interface == BackendInterface::Screencast;
		const bool loseDesktopVideo = loseDisplays || interface == BackendInterface::Screencast;
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
};

QTEST_GUILESS_MAIN(CaptureAudioTest)
#include "capture-audio-test.moc"
