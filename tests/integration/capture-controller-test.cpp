#include "capture-test-fixture.hpp"
#include "video-capture-fixture.hpp"
#include <QEvent>

enum class ConfigChange { SelectedWindow, Application, TitleFilter, TitleMatch, CaseSensitive, TitlePolicy, Cursor };
Q_DECLARE_METATYPE(ConfigChange)

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
		QTest::addColumn<ConfigChange>("change");
		QTest::newRow("selected-window") << ConfigChange::SelectedWindow;
		QTest::newRow("application") << ConfigChange::Application;
		QTest::newRow("title-filter") << ConfigChange::TitleFilter;
		QTest::newRow("title-match") << ConfigChange::TitleMatch;
		QTest::newRow("case-sensitive") << ConfigChange::CaseSensitive;
		QTest::newRow("title-policy") << ConfigChange::TitlePolicy;
		QTest::newRow("cursor") << ConfigChange::Cursor;
	}

	void configurationChangesResumeFailedCapture()
	{
		QFETCH(ConfigChange, change);
		kwin.add({"original", "test.app", "Editor", false});
		CaptureConfig config;
		config.rule.title = "Editor";
		auto target = capture(config);
		QTRY_COMPARE(status(target), QString("failed"));
		int requests = kwin.captureRequests;
		kwin.deferCapture = true;
		config = target->configuration();
		switch (change) {
		case ConfigChange::SelectedWindow:
			config.preferredWindow = "original";
			break;
		case ConfigChange::Application:
			kwin.add({"other-app", "other.app", "Editor", false});
			config.rule.appId = "other.app";
			QTRY_COMPARE(capture_windows().size(), 2);
			break;
		case ConfigChange::TitleFilter:
			config.rule.title = "Edit";
			break;
		case ConfigChange::TitleMatch:
			config.rule.mode = TitleMatch::Exact;
			break;
		case ConfigChange::CaseSensitive:
			config.rule.caseSensitive = true;
			break;
		case ConfigChange::TitlePolicy:
			config.rule.disconnectOnTitleMismatch = !config.rule.disconnectOnTitleMismatch;
			break;
		case ConfigChange::Cursor:
			config.cursor = false;
			break;
		}
		target->configure(config);
		queue_capture_update(target);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow,
			 change == ConfigChange::Application ? QString("other-app") : QString("original"));
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
