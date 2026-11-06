#include "kwin-connection.hpp"
#include "kwin-test-fixture.hpp"
#include <QSignalSpy>

class KWinConnectionTest : public KWinTestFixture
{
	Q_OBJECT
      private slots:
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
		auto backend = [&] {
			ScopedEnvironment selectedDisplay("WAYLAND_DISPLAY");
			selectedDisplay.set(legacy.socketPath().toUtf8());
			return KWinConnection(nullptr);
		}();
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
};

QTEST_GUILESS_MAIN(KWinConnectionTest)
#include "kwin-connection-test.moc"
