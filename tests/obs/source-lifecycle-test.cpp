#include "simulated-obs-fixture.hpp"
#include "video-capture-fixture.hpp"

class SourceLifecycleTest : public SimulatedObsFixture
{
	Q_OBJECT
      private slots:
	void premultipliedAlphaSurvivesSceneRendering_data()
	{
		QTest::addColumn<uint32_t>("alpha");
		QTest::newRow("transparent") << 0u;
		QTest::newRow("quarter-opacity") << 64u;
		QTest::newRow("half-opacity") << 128u;
		QTest::newRow("opaque") << 255u;
	}

	void premultipliedAlphaSurvivesSceneRendering()
	{
		QFETCH(uint32_t, alpha);
		// KWin supplies premultiplied pixels: red and alpha have the same value.
		VideoCaptureFixture video((alpha << 24) | (alpha << 16));
		QVERIFY(video.startCapture(kwin));
		kwin.add({"translucent", "test.app", "Translucent window", false});
		OBSSourceAutoRelease target = source("Alpha capture");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		OBSSceneAutoRelease scene = obs_scene_create("Alpha scene");
		obs_scene_add(scene, target);
		auto pixel = renderedPixel(obs_scene_get_source(scene));
		QCOMPARE(pixel.red(), int(alpha));
		QCOMPARE(pixel.green(), 0);
		QCOMPARE(pixel.blue(), 0);
		QCOMPARE(pixel.alpha(), int(alpha));
	}

	void videoReconnectsAndKeepsSizeAndCursorChoice()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"first", "test.app", "First title", false});
		OBSSourceAutoRelease target = source("Video target");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.cursorMode, uint32_t(2));
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		auto pixel = renderedPixel(target);
		QCOMPARE(pixel.alpha(), 255);
		QVERIFY(pixel.red() > pixel.green());
		int requests = kwin.captureRequests;
		kwin.title("first", "Changed title");
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		kwin.remove("first");
		QTRY_VERIFY(status(target) != "capturing");
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		QCOMPARE(obs_source_get_height(target), uint32_t(90));
		video.producer.stop();
		QVERIFY(video.producer.start());
		kwin.videoNode = video.producer.nodeId();
		kwin.videoSerial = video.producer.serial();
		kwin.add({"second", "test.app", "Reopened", false});
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.requestedWindow, QString("second"));
		OBSDataAutoRelease settings = obs_source_get_settings(target);
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_source_update(target, settings);
		QTRY_COMPARE_WITH_TIMEOUT(kwin.cursorMode, uint32_t(1), 3000);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		obs_data_set_string(settings, "app_id", "");
		obs_source_update(target, settings);
		QTRY_COMPARE(status(target), QString("idle"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
	}

	void legacyManualSettingsDoNotDisableCaptureOrReconnect()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"original", "test.app", "Original window", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_bool(settings, "auto_start", false);
		obs_data_set_bool(settings, "auto_reconnect", false);
		OBSSourceAutoRelease target = createSource("Legacy settings", settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.remove("original");
		QTRY_COMPARE(status(target), QString("waiting"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
		kwin.add({"reopened", "test.app", "Reopened window", false});
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.requestedWindow, QString("reopened"));
		QCOMPARE(renderedPixel(target).alpha(), 255);
		OBSDataAutoRelease saved = obs_save_source(target);
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QVERIFY(!obs_data_has_user_value(config, "auto_start"));
		QVERIFY(!obs_data_has_user_value(config, "auto_reconnect"));
	}

	void choosingWindowWhileWaitingStartsCapture()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"original", "test.app", "Original", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		OBSSourceAutoRelease target = createSource("Select after stop", settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.remove("original");
		QTRY_COMPARE(status(target), QString("waiting"));
		int requests = kwin.captureRequests;
		kwin.add({"chosen", "other.app", "Chosen", false});
		QTRY_COMPARE(capture_windows().size(), 1);
		auto props = obs_source_properties(target);
		obs_data_set_string(settings, "selected_window", "chosen");
		QVERIFY(obs_property_modified(obs_properties_get(props, "selected_window"), settings));
		obs_source_update(target, settings);
		obs_properties_destroy(props);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow, QString("chosen"));
		QCOMPARE(renderedPixel(target).alpha(), 255);
	}

	void titleMismatchDisconnects_data()
	{
		QTest::addColumn<bool>("enabled");
		QTest::newRow("enabled") << true;
		QTest::newRow("enable-during-capture") << false;
	}

	void titleMismatchDisconnects()
	{
		QFETCH(bool, enabled);
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"editor", "test.app", "Other", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "selected_window", "editor");
		obs_data_set_string(settings, "title_filter", "Editor");
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", enabled);
		OBSSourceAutoRelease target = createSource("Title matching", settings);
		QTRY_COMPARE(status(target), QString("waiting"));
		kwin.title("editor", "Editor");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		int requests = kwin.captureRequests;
		QCOMPARE(renderedPixel(target).alpha(), 255);
		kwin.title("editor", "Editor - document");
		QTRY_VERIFY(capture_windows().front().title == "Editor - document");
		QCOMPARE(status(target), QString("capturing"));
		QCOMPARE(kwin.captureRequests, requests);
		kwin.title("editor", "Other");
		QTRY_VERIFY(capture_windows().front().title == "Other");
		if (!enabled) {
			QCOMPARE(status(target), QString("capturing"));
			QCOMPARE(renderedPixel(target).alpha(), 255);
			obs_data_set_bool(settings, "disconnect_on_title_mismatch", true);
			obs_source_update(target, settings);
		}
		QTRY_COMPARE(status(target), QString("waiting"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		QCOMPARE(obs_source_get_height(target), uint32_t(90));
		QCOMPARE(kwin.captureRequests, requests);
		kwin.title("editor", "Editor");
		QTRY_VERIFY(capture_windows().front().title == "Editor");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(renderedPixel(target).alpha(), 255);
	}

	void titleMismatchWithAlternateWindow()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"editor", "test.app", "Editor", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "title_filter", "Editor");
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", true);
		OBSSourceAutoRelease target = createSource("Alternate window", settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.add({"other", "test.app", "Editor two", false});
		QTRY_COMPARE(capture_windows().size(), 2);
		int requests = kwin.captureRequests;
		QCOMPARE(kwin.requestedWindow, QString("editor"));
		kwin.title("editor", "Other");
		QTRY_COMPARE(kwin.requestedWindow, QString("other"));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(renderedPixel(target).alpha(), 255);
	}

	void cursorToggleKeepsCurrentWindow_data()
	{
		QTest::addColumn<QString>("preferred");
		QTest::newRow("application-rule") << QString();
		QTest::newRow("saved-window") << QString("first");
		QTest::newRow("stale-saved-window") << QString("closed");
	}

	void cursorToggleKeepsCurrentWindow()
	{
		QFETCH(QString, preferred);
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"first", "test.app", "First", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "selected_window", preferred.toUtf8().constData());
		OBSSourceAutoRelease target = createSource("Cursor toggle", settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.add({"other", "test.app", "Second", false});
		QTRY_COMPARE(capture_windows().size(), 2);
		int requests = kwin.captureRequests;
		for (bool cursor : {false, true}) {
			obs_data_set_bool(settings, "capture_cursor", cursor);
			obs_source_update(target, settings);
			++requests;
			QTRY_COMPARE(kwin.captureRequests, requests);
			QCOMPARE(kwin.requestedWindow, QString("first"));
			QCOMPARE(kwin.cursorMode, uint32_t(cursor ? 2 : 1));
			QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
			QCOMPARE(renderedPixel(target).alpha(), 255);
		}
	}

	void propertyEditsOnlyRestartCaptureWhenNeeded()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"editor", "test.app", "Editor", false});
		OBSSourceAutoRelease target = source("Editing properties");
		int updates = 0;
		auto updated = [](void *data, calldata_t *) { ++*static_cast<int *>(data); };
		OBSSignal updateSignal(obs_source_get_signal_handler(target), "update_properties", updated, &updates);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		const int initialUpdates = updates;
		int requests = kwin.captureRequests;
		OBSDataAutoRelease settings = obs_source_get_settings(target);
		for (const char *filter : {"E", "Ed", "Editor"}) {
			obs_data_set_string(settings, "title_filter", filter);
			obs_source_update(target, settings);
			QTest::qWait(100);
			QCOMPARE(kwin.captureRequests, requests);
			QCOMPARE(status(target), QString("capturing"));
			QCOMPARE(renderedPixel(target).alpha(), 255);
			QCOMPARE(updates, initialUpdates);
		}
		obs_data_set_int(settings, "title_match", int(TitleMatch::Exact));
		obs_data_set_bool(settings, "case_sensitive", true);
		obs_data_set_bool(settings, "raise_on_focus", true);
		obs_source_update(target, settings);
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		kwin.add({"other", "test.app", "Other", false});
		obs_data_set_string(settings, "title_filter", "Other");
		obs_source_update(target, settings);
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow, QString("other"));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		obs_data_set_string(settings, "title_filter", "missing");
		obs_source_update(target, settings);
		QTRY_COMPARE(status(target), QString("waiting"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		QCOMPARE(obs_source_get_height(target), uint32_t(90));
		obs_data_set_string(settings, "title_filter", "Other");
		obs_source_update(target, settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.deferCapture = true;
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_source_update(target, settings);
		QTRY_COMPARE(status(target), QString("connecting"));
		QTRY_COMPARE(kwin.captureRequests, requests + 3);
		QCOMPARE(kwin.cursorMode, uint32_t(1));
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		QCOMPARE(obs_source_get_height(target), uint32_t(90));
		QVERIFY(updates > initialUpdates); // Refresh the fallback warning when capture stops/restarts.
	}

	void sceneAndPreviewVisibility()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"visible", "test.app", "Visibility target", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		OBSSourceAutoRelease target = createSource("Visibility", settings, false);
		OBSSceneAutoRelease scene = obs_scene_create("Visibility scene");
		auto sceneSource = obs_scene_get_source(scene);
		auto item = obs_scene_add(scene, target);
		QTRY_COMPARE(status(target), QString("paused"));
		int requests = kwin.captureRequests;
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		QCOMPARE(renderedPixel(target).alpha(), 0);

		obs_set_output_source(0, sceneSource);
		auto deactivate = qScopeGuard([&] { obs_set_output_source(0, nullptr); });
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(renderedPixel(target).alpha(), 255);
		obs_sceneitem_set_visible(item, false);
		// Apply OBS's deferred scene-item visibility bookkeeping.
		renderedPixel(sceneSource);
		QTRY_COMPARE(status(target), QString("paused"));
		QCOMPARE(renderedPixel(target).alpha(), 0);

		// Properties previews count even when the scene item is hidden.
		obs_source_inc_showing(target);
		obs_source_inc_showing(target);
		int previews = 2;
		auto closePreviews = qScopeGuard([&] {
			while (previews > 0) {
				obs_source_dec_showing(target);
				--previews;
			}
		});
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		obs_source_dec_showing(target);
		--previews;
		QTest::qWait(100);
		QCOMPARE(status(target), QString("capturing"));
		QCOMPARE(renderedPixel(target).alpha(), 255);
		obs_source_dec_showing(target);
		--previews;
		QTRY_COMPARE(status(target), QString("paused"));
		obs_sceneitem_set_visible(item, true);
		renderedPixel(sceneSource);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		obs_set_output_source(0, nullptr);
		deactivate.dismiss();
		QTRY_COMPARE(status(target), QString("paused"));
	}

	void hiddenSourceTracksReplacementAndResumes()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"original", "test.app", "Original", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		int requests = kwin.captureRequests;
		OBSSourceAutoRelease target = createSource("Hidden tracking", settings, false);
		QTRY_COMPARE(status(target), QString("paused"));
		kwin.remove("original");
		QTRY_COMPARE(status(target), QString("waiting"));
		kwin.add({"replacement", "test.app", "Replacement", false});
		QTRY_COMPARE(status(target), QString("paused"));
		QCOMPARE(kwin.captureRequests, requests);
		obs_source_inc_showing(target);
		auto hide = qScopeGuard([&] { obs_source_dec_showing(target); });
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.requestedWindow, QString("replacement"));
		QCOMPARE(renderedPixel(target).alpha(), 255);
	}
};

int main(int argc, char **argv) { return run_obs_tests<SourceLifecycleTest>(argc, argv); }
#include "source-lifecycle-test.moc"
