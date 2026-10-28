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
		queue_capture_update(state);
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
		TestVideo video;
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
		auto controller = std::make_unique<KWinCapture>(state, &backend, nullptr);
		QPointer<KWinCapture> controllerGuard = controller.get();
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
		TestVideo video;
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
		QVERIFY(video.setProducerActive(false));
		video.setColor(0xff20c040u);
		target->setShowing(true);
		queue_capture_update(target);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QTest::qWait(100);
		QCOMPARE(target->frames->take(frame), FrameBuffer::Update::Empty);
		QCOMPARE(target->frames->sequence(), sequence);
		QVERIFY(video.setProducerActive(true));
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

	void hiddenCaptureRetainsBackendTrackingWithoutRetries()
	{
		TestVideo video;
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
