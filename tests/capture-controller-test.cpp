#include "capture.hpp"
#include "kwin-connection.hpp"
#include "kwin-fixture.hpp"
#include "video-fixture.hpp"

#include <QEvent>
#include <QSignalSpy>
#include <QTest>
#include <vector>

class CaptureControllerTest : public QObject
{
	Q_OBJECT
	KWinFixture kwin;
	const QByteArray originalDisplay = qgetenv("WAYLAND_DISPLAY");
	const bool hadDisplay = qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
	std::vector<std::shared_ptr<CaptureState>> captures;

	std::shared_ptr<CaptureState> capture(CaptureConfig config = {})
	{
		if (config.rule.appId.isEmpty())
			config.rule.appId = "test.app";
		auto state = std::make_shared<CaptureState>();
		state->configure(config);
		captures.push_back(state);
		queue_capture_action(state, CaptureAction::Configure);
		return state;
	}
	QString status(const std::shared_ptr<CaptureState> &state)
	{
		return QString::fromUtf8(capture_status_name(state->diagnostics().status));
	}

      private slots:
	void initTestCase()
	{
		QVERIFY(kwin.start());
		qputenv("WAYLAND_DISPLAY", kwin.socketPath().toUtf8());
		initialize_capture();
		kwin.add({"probe", "probe.app", "Probe", false});
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 3000);
		kwin.clear();
	}

	void init()
	{
		kwin.reject = true;
		kwin.deferCapture = false;
	}

	void cleanup()
	{
		for (const auto &state : captures)
			release_capture(state);
		captures.clear();
		QCoreApplication::sendPostedEvents();
		kwin.clear();
		QTRY_VERIFY(capture_windows().isEmpty());
	}

	void cleanupTestCase()
	{
		shutdown_capture();
		if (hadDisplay)
			qputenv("WAYLAND_DISPLAY", originalDisplay);
		else
			qunsetenv("WAYLAND_DISPLAY");
	}

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

	void stopDisarmsAndConnectRearms()
	{
		auto target = capture();
		QTRY_COMPARE(status(target), QString("waiting"));
		queue_capture_action(target, CaptureAction::Stop);
		QTRY_COMPARE(status(target), QString("idle"));
		int requests = kwin.captureRequests;
		kwin.add({"manual", "test.app", "Open", false});
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		queue_capture_action(target, CaptureAction::Connect);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
	}

	void automaticReconnectCanBeDisabled()
	{
		kwin.add({"no-retry", "test.app", "App", false});
		CaptureConfig config;
		config.autoReconnect = false;
		auto target = capture(config);
		int requests = kwin.captureRequests;
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 3000);
		QTRY_COMPARE(status(target), QString("failed"));
		kwin.remove("no-retry");
		kwin.add({"new-no-retry", "test.app", "Returned", false});
		QTest::qWait(2100);
		QCOMPARE(kwin.captureRequests, requests + 1);
		queue_capture_action(target, CaptureAction::Connect);
		QTRY_COMPARE(kwin.captureRequests, requests + 2);
	}

	void reenablingAutomaticReconnectRetriesFailedCapture()
	{
		kwin.add({"retry", "test.app", "Failed capture", false});
		CaptureConfig config;
		config.autoReconnect = false;
		auto target = capture(config);
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		kwin.deferCapture = true;
		config = target->configuration();
		config.autoReconnect = true;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(status(target), QString("connecting"));
	}

	void enablingStartupCaptureStartsImmediately()
	{
		kwin.deferCapture = true;
		kwin.add({"startup", "test.app", "Startup toggle", false});
		CaptureConfig config;
		config.autoStart = false;
		auto target = capture(config);
		int requests = kwin.captureRequests;
		QTest::qWait(100);
		QCOMPARE(status(target), QString("idle"));
		QCOMPARE(kwin.captureRequests, requests);
		config = target->configuration();
		config.autoStart = true;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(status(target), QString("connecting"));
		config.autoStart = false;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTest::qWait(100);
		QCOMPARE(status(target), QString("connecting"));
		QCOMPARE(kwin.captureRequests, requests + 1);
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
		config.autoReconnect = false;
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
			config.rule.disconnectOnTitleMismatch = true;
			break;
		case 6:
			config.cursor = false;
			break;
		}
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow, change == 1 ? QString("other-app") : QString("original"));
		QCOMPARE(status(target), QString("connecting"));
	}

	void disablingAutomaticReconnectCancelsPendingRetry()
	{
		kwin.add({"pending-retry", "test.app", "Pending retry", false});
		auto target = capture();
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		auto config = target->configuration();
		config.autoReconnect = false;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTest::qWait(100);
		kwin.deferCapture = true;
		kwin.focus("pending-retry");
		QTest::qWait(2200);
		QCOMPARE(kwin.captureRequests, requests);
		QCOMPARE(status(target), QString("failed"));
		config.autoReconnect = true;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
	}

	void disablingAutomaticReconnectCancelsWaitingAfterLoss()
	{
		TestVideo video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"original", "test.app", "Original", false});
		auto target = capture();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.remove("original");
		QTRY_COMPARE(status(target), QString("waiting"));
		int requests = kwin.captureRequests;
		auto config = target->configuration();
		config.autoReconnect = false;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTRY_COMPARE(status(target), QString("idle"));
		kwin.add({"returned", "test.app", "Returned", false});
		QTRY_COMPARE(capture_windows().size(), 1);
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		config.autoReconnect = true;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
	}

	void disablingAutomaticReconnectPreservesInitialWait()
	{
		auto target = capture();
		QTRY_COMPARE(status(target), QString("waiting"));
		int requests = kwin.captureRequests;
		auto config = target->configuration();
		config.autoReconnect = false;
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		kwin.deferCapture = true;
		kwin.add({"first-target", "test.app", "First target", false});
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(status(target), QString("connecting"));
	}

	void backendDisconnectRespectsReconnectSetting_data()
	{
		QTest::addColumn<bool>("automatic");
		QTest::newRow("automatic") << true;
		QTest::newRow("manual") << false;
	}

	void backendDisconnectRespectsReconnectSetting()
	{
		QFETCH(bool, automatic);
		TestVideo video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"backend", "test.app", "Backend reconnect", false});
		CaptureConfig config;
		config.autoReconnect = automatic;
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
		if (!automatic) {
			QCOMPARE(status(target), QString("failed"));
			QCOMPARE(kwin.captureRequests, requests);
			queue_capture_action(target, CaptureAction::Connect);
		}
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
		auto controller = std::make_unique<KWinCapture>(state, &backend, nullptr);
		QPointer<KWinCapture> controllerGuard = controller.get();
		controller->action(CaptureAction::Configure);
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
		// No controller or changed() observer participates in stream teardown.
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
		auto controller = std::make_unique<KWinCapture>(state, backend.get(), nullptr);
		controller->action(CaptureAction::Configure);
		QTRY_COMPARE(kwin.requestedWindow, QString("controller"));
		backend.reset();
		QTRY_COMPARE(state->diagnostics().status, CaptureStatus::Failed);
		controller->action(CaptureAction::Connect);
		QCOMPARE(state->diagnostics().status, CaptureStatus::Failed);
		controller.reset();
	}

	void configurationEditsPreserveManualStop_data()
	{
		QTest::addColumn<bool>("startupDisabled");
		QTest::newRow("api-stop") << false;
		QTest::newRow("startup-disabled") << true;
	}

	void configurationEditsPreserveManualStop()
	{
		QFETCH(bool, startupDisabled);
		CaptureConfig initial;
		initial.autoStart = !startupDisabled;
		auto target = capture(initial);
		if (!startupDisabled) {
			QTRY_COMPARE(status(target), QString("waiting"));
			queue_capture_action(target, CaptureAction::Stop);
		}
		QTRY_COMPARE(status(target), QString("idle"));
		auto config = target->configuration();
		config.cursor = false;
		config.rule.title = "Edited";
		target->configure(config);
		queue_capture_action(target, CaptureAction::Configure);
		kwin.add({"edited", "test.app", "Edited", false});
		QTRY_COMPARE(capture_windows().size(), 1);
		QCOMPARE(status(target), QString("idle"));
		queue_capture_action(target, CaptureAction::Connect);
		QTRY_COMPARE(kwin.requestedWindow, QString("edited"));
	}

	void pausedStreamResumesWithoutReselecting()
	{
		TestVideo video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"paused", "test.app", "Paused stream", false});
		auto target = capture();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		int requests = kwin.captureRequests;
		QVERIFY(video.setProducerActive(false));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("paused"), 3000);
		VideoFrame frame;
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QVERIFY(video.setProducerActive(true));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests);
	}
};

QTEST_GUILESS_MAIN(CaptureControllerTest)
#include "capture-controller-test.moc"
