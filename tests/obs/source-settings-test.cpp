#include "audio-fixture.hpp"
#include "simulated-obs-fixture.hpp"
#include "video-capture-fixture.hpp"

#include <chrono>
#include <future>

class SourceSettingsTest : public SimulatedObsFixture
{
	Q_OBJECT
      private slots:
	void statusChangesDoNotReloadProperties()
	{
		kwin.deferCapture = true;
		kwin.add({"pending", "test.app", "Pending capture", false});
		int updates = 0;
		OBSSourceAutoRelease target = source("Stable properties");
		auto handler = obs_source_get_signal_handler(target);
		auto updated = [](void *data, calldata_t *) { ++*static_cast<int *>(data); };
		OBSSignal updateSignal(handler, "update_properties", updated, &updates);
		QTRY_COMPARE(status(target), QString("connecting"));
		auto props = obs_source_properties(target);
		int requests = kwin.captureRequests;
		OBSDataAutoRelease settings = obs_source_get_settings(target);
		obs_source_update(target, settings);
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		obs_properties_destroy(props);
		kwin.remove("pending");
		QTRY_COMPARE(status(target), QString("waiting"));
		QCOMPARE(updates, 0);
		props = obs_source_properties(target);
		QVERIFY(!obs_property_button_clicked(obs_properties_get(props, "refresh_windows"), target));
		QCOMPARE(updates, 1);
		obs_properties_destroy(props);
	}

	void desktopSourceCapturesVideoAndSavesSelection()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.addDisplay({"DP-1", "Main display", {0, 0, 1920, 1080}, 1});
		kwin.addDisplay({"DP-2", "Second display", {-1280, 0, 1280, 720}, 1});
		QTRY_COMPARE(capture_displays().size(), 2);
		OBSDataAutoRelease settings = obs_data_create();
		// Even injected application-audio settings cannot enable audio on this source.
		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease target = createSource("Desktop", settings, true, "m4capture_desktop_source");
		QVERIFY(target);
		QVERIFY(!(obs_source_get_output_flags(target) & OBS_SOURCE_AUDIO));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_COMPARE(renderedPixel(target).alpha(), 255);
		QCOMPARE(video.server.audioCapturesCreated(), uint32_t(0));
		QCOMPARE(kwin.requestedRegion, QRect(-1280, 0, 3200, 1080));
		auto props = obs_source_properties(target);
		auto picker = obs_properties_get(props, "selected_display");
		QVERIFY(picker);
		QCOMPARE(obs_property_list_item_count(picker), size_t(3));
		QVERIFY(!obs_properties_get(props, "capture_audio"));
		QVERIFY(!obs_properties_get(props, "selected_window"));
		QVERIFY(!obs_properties_get(props, "app_id"));
		QVERIFY(!obs_properties_get(props, "raise_on_focus"));
		QVERIFY(!obs_properties_get(props, "follow_focus"));
		obs_properties_destroy(props);
		OBSDataAutoRelease updated = obs_source_get_settings(target);
		obs_data_set_string(updated, "selected_display", "DP-2");
		obs_data_set_bool(updated, "capture_cursor", false);
		obs_source_update(target, updated);
		QTRY_COMPARE(kwin.requestedDisplay, QString("DP-2"));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.cursorMode, uint32_t(1));
		OBSDataAutoRelease saved = obs_save_source(target);
		QCOMPARE(QString(obs_data_get_string(saved, "id")), QString("m4capture_desktop_source"));
		OBSDataAutoRelease serialized = obs_data_create_from_json(obs_data_get_json(saved));
		obs_data_set_string(serialized, "name", "Restored desktop");
		OBSSourceAutoRelease restored = obs_load_source(serialized);
		QVERIFY(restored);
		OBSDataAutoRelease restoredSettings = obs_source_get_settings(restored);
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "selected_display")), QString("DP-2"));
		QVERIFY(!obs_data_get_bool(restoredSettings, "capture_cursor"));
		QVERIFY(!obs_data_has_user_value(restoredSettings, "app_id"));
		QVERIFY(!(obs_source_get_output_flags(restored) & OBS_SOURCE_AUDIO));
		kwin.removeDisplay("DP-2");
		QTRY_COMPARE(status(target), QString("waiting"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
		props = obs_source_properties(target);
		picker = obs_properties_get(props, "selected_display");
		QCOMPARE(obs_property_list_item_count(picker), size_t(3));
		QCOMPARE(QString(obs_property_list_item_string(picker, 2)), QString("DP-2"));
		obs_properties_destroy(props);
	}

	void settingsAndPickerRoundTrip()
	{
		kwin.add({"pick", "test.app", "Window title", false});
		QTRY_VERIFY(capture_windows().size() == 1);
		OBSSourceAutoRelease target = source("Settings", "", true);
		OBSDataAutoRelease settings = obs_source_get_settings(target);
		auto props = obs_source_properties(target);
		auto picker = obs_properties_get(props, "selected_window");
		QVERIFY(picker);
		QVERIFY(!obs_properties_get(props, "auto_start"));
		QVERIFY(!obs_properties_get(props, "auto_reconnect"));
		QVERIFY(obs_properties_get(props, "disconnect_on_title_mismatch"));
		QVERIFY(obs_data_get_bool(settings, "disconnect_on_title_mismatch"));
		QVERIFY(obs_properties_get(props, "capture_audio"));
		QVERIFY(!obs_data_get_bool(settings, "capture_audio"));
		QVERIFY(obs_properties_get(props, "follow_focus"));
		QVERIFY(!obs_data_get_bool(settings, "follow_focus"));
		QVERIFY(obs_source_get_output_flags(target) & OBS_SOURCE_AUDIO);
		obs_data_set_string(settings, "selected_window", "pick");
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		obs_data_set_string(settings, "title_filter", "title");
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_data_set_bool(settings, "capture_audio", true);
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", false);
		obs_data_set_bool(settings, "follow_focus", true);
		obs_source_update(target, settings);
		obs_properties_destroy(props);
		QTest::qWait(80);
		OBSDataAutoRelease saved = obs_save_source(target);
		QCOMPARE(QString(obs_data_get_string(saved, "id")), QString("m4capture_source"));
		QCOMPARE(QString(obs_data_get_string(saved, "versioned_id")), QString("m4capture_source"));
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QCOMPARE(QString(obs_data_get_string(config, "app_id")), QString("test.app"));
		QCOMPARE(QString(obs_data_get_string(config, "title_filter")), QString("title"));
		QVERIFY(obs_data_get_bool(config, "raise_on_focus"));
		QVERIFY(!obs_data_get_bool(config, "capture_cursor"));
		QVERIFY(obs_data_get_bool(config, "capture_audio"));
		QVERIFY(!obs_data_get_bool(config, "disconnect_on_title_mismatch"));
		QVERIFY(obs_data_get_bool(config, "follow_focus"));
		// Reload the serialized source, including its renamed type and saved settings.
		OBSDataAutoRelease serialized = obs_data_create_from_json(obs_data_get_json(saved));
		obs_data_set_string(serialized, "name", "Restored settings");
		OBSSourceAutoRelease restored = obs_load_source(serialized);
		QVERIFY(restored);
		QCOMPARE(QString(obs_source_get_id(restored)), QString("m4capture_source"));
		OBSDataAutoRelease restoredSettings = obs_source_get_settings(restored);
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "app_id")), QString("test.app"));
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "selected_window")), QString("pick"));
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "title_filter")), QString("title"));
		QVERIFY(obs_data_get_bool(restoredSettings, "raise_on_focus"));
		QVERIFY(!obs_data_get_bool(restoredSettings, "capture_cursor"));
		QVERIFY(obs_data_get_bool(restoredSettings, "capture_audio"));
		QVERIFY(!obs_data_get_bool(restoredSettings, "disconnect_on_title_mismatch"));
		QVERIFY(obs_data_get_bool(restoredSettings, "follow_focus"));
	}

	void audioOnlySettingsAndPickerRoundTrip()
	{
		kwin.add({"audio-pick", "test.app", "Audio window", false});
		QTRY_COMPARE(capture_windows().size(), 1);
		OBSDataAutoRelease settings = obs_data_create();
		OBSSourceAutoRelease target =
		    obs_source_create("m4capture_audio_source", "Audio only settings", settings, nullptr);
		QVERIFY(target);
		QCOMPARE(obs_source_get_output_flags(target), uint32_t(OBS_SOURCE_AUDIO));
		QVERIFY(obs_source_audio_active(target));
		QCOMPARE(obs_source_get_width(target), uint32_t(0));
		QCOMPARE(obs_source_get_height(target), uint32_t(0));
		auto props = obs_source_properties(target);
		auto destroyProps = qScopeGuard([&] { obs_properties_destroy(props); });
		QVERIFY(!obs_properties_get(props, "capture_audio"));
		QVERIFY(!obs_properties_get(props, "capture_cursor"));
		QVERIFY(!obs_properties_get(props, "follow_focus"));
		QVERIFY(!obs_properties_get(props, "raise_on_focus"));
		QVERIFY(!obs_properties_get(props, "cpu_fallback"));
		auto picker = obs_properties_get(props, "selected_window");
		QVERIFY(picker);
		obs_data_set_string(settings, "selected_window", "audio-pick");
		// OBS applies audio-source settings before invoking the property's callback.
		obs_source_update(target, settings);
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		obs_data_set_string(settings, "title_filter", "Audio");
		obs_data_set_int(settings, "title_match", int(TitleMatch::Contains));
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", true);
		// Hidden video/audio switches cannot change this source's purpose.
		obs_data_set_bool(settings, "capture_audio", false);
		obs_data_set_bool(settings, "capture_cursor", true);
		obs_data_set_bool(settings, "follow_focus", true);
		obs_data_set_bool(settings, "raise_on_focus", true);
		obs_source_update(target, settings);
		QVERIFY(obs_source_audio_active(target));
		OBSDataAutoRelease saved = obs_save_source(target);
		QCOMPARE(QString(obs_data_get_string(saved, "id")), QString("m4capture_audio_source"));
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QVERIFY(obs_data_get_bool(config, "capture_audio"));
		QVERIFY(!obs_data_get_bool(config, "capture_cursor"));
		QVERIFY(!obs_data_get_bool(config, "raise_on_focus"));
		QVERIFY(!obs_data_get_bool(config, "follow_focus"));
		OBSDataAutoRelease serialized = obs_data_create_from_json(obs_data_get_json(saved));
		obs_data_set_string(serialized, "name", "Restored audio only");
		OBSSourceAutoRelease restored = obs_load_source(serialized);
		QVERIFY(restored);
		QCOMPARE(QString(obs_source_get_id(restored)), QString("m4capture_audio_source"));
		QVERIFY(obs_source_audio_active(restored));
		OBSDataAutoRelease restoredSettings = obs_source_get_settings(restored);
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "app_id")), QString("test.app"));
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "selected_window")), QString("audio-pick"));
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "title_filter")), QString("Audio"));
		QVERIFY(obs_data_get_bool(restoredSettings, "disconnect_on_title_mismatch"));
	}

	void savingPreservesPendingAudioAndTargetEdits()
	{
		OBSSourceAutoRelease target = source("Pending edits", "test.app");
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "capture_audio", true);
		obs_data_set_string(settings, "app_id", "other.app");
		{
			struct VideoThreadPause {
				std::promise<void> entered, resume;
				std::future<void> resumed = resume.get_future();
			} pause;
			auto entered = pause.entered.get_future();
			// Release and join the graphics task even if an assertion fails before it starts.
			auto resumeVideo = qScopeGuard([&] {
				pause.resume.set_value();
				obs_queue_task(OBS_TASK_GRAPHICS, [](void *) {}, nullptr, true);
			});
			obs_queue_task(
			    OBS_TASK_GRAPHICS,
			    [](void *data) {
				    auto &pause = *static_cast<VideoThreadPause *>(data);
				    pause.entered.set_value();
				    pause.resumed.wait();
			    },
			    &pause, false);
			QVERIFY(entered.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
			obs_source_update(target, settings);
			QVERIFY(!obs_source_audio_active(target)); // The source update is still deferred.
			OBSDataAutoRelease saved = obs_save_source(target);
			OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
			QVERIFY(obs_data_get_bool(config, "capture_audio"));
			QCOMPARE(QString(obs_data_get_string(config, "app_id")), QString("other.app"));
			QVERIFY(!obs_source_audio_active(target));
		}
		QTRY_VERIFY(obs_source_audio_active(target));
	}

	void choosingWindowReconcilesTitleFilter_data()
	{
		QTest::addColumn<bool>("differentApp");
		QTest::addColumn<bool>("filterMatches");
		QTest::newRow("same-app-compatible") << false << true;
		QTest::newRow("same-app-conflicting") << false << false;
		QTest::newRow("other-app-compatible") << true << true;
		QTest::newRow("other-app-conflicting") << true << false;
	}

	void choosingWindowReconcilesTitleFilter()
	{
		QFETCH(bool, differentApp);
		QFETCH(bool, filterMatches);
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"original", "test.app", "Editor", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "selected_window", "original");
		obs_data_set_string(settings, "title_filter", "Editor");
		OBSSourceAutoRelease target = createSource("Window selection", settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		int requests = kwin.captureRequests;
		auto appId = differentApp ? "other.app" : "test.app";
		kwin.add({"chosen", appId, filterMatches ? "Editor - Other" : "Other", false});
		QTRY_COMPARE(capture_windows().size(), 2);
		auto props = obs_source_properties(target);
		obs_data_set_string(settings, "selected_window", "chosen");
		bool modified = obs_property_modified(obs_properties_get(props, "selected_window"), settings);
		obs_source_update(target, settings);
		obs_properties_destroy(props);
		QVERIFY(modified);
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString(appId));
		QCOMPARE(QString(obs_data_get_string(settings, "title_filter")),
			 filterMatches ? QString("Editor") : QString());
		QTRY_COMPARE(kwin.requestedWindow, QString("chosen"));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(renderedPixel(target).alpha(), 255);
	}

	void clearingWindowSelectionStopsCapture_data()
	{
		QTest::addColumn<bool>("audioOnly");
		QTest::newRow("window") << false;
		QTest::newRow("application-audio") << true;
	}

	void clearingWindowSelectionStopsCapture()
	{
		QFETCH(bool, audioOnly);
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		AudioProducer audio;
		QVERIFY(audio.start(video.server.openRemote(), "test.app"));
		kwin.add({"chosen", "test.app", "Editor", false});
		QTRY_COMPARE(capture_windows().size(), 1);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "title_filter", "Editor");
		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease target = createSource("Clear window selection", settings, true,
							   audioOnly ? "m4capture_audio_source" : "m4capture_source");
		ObsAudioRecorder samples(target);
		auto props = obs_source_properties(target);
		auto destroyProps = qScopeGuard([&] { obs_properties_destroy(props); });
		auto picker = obs_properties_get(props, "selected_window");
		QVERIFY(picker);
		obs_data_set_string(settings, "selected_window", "chosen");
		obs_source_update(target, settings);
		QVERIFY(obs_property_modified(picker, settings));
		obs_source_update(target, settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(samples.count.load() > 0, 5000);
		if (!audioOnly)
			QTRY_COMPARE(renderedPixel(target).alpha(), 255);

		obs_data_set_string(settings, "selected_window", "");
		obs_source_update(target, settings);
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString());
		QCOMPARE(QString(obs_data_get_string(settings, "title_filter")), QString());
		obs_source_update(target, settings);
		QTRY_COMPARE(status(target), QString("idle"));
		if (!audioOnly)
			QCOMPARE(renderedPixel(target).alpha(), 0);
		QTest::qWait(100);
		auto count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);

		OBSDataAutoRelease saved = obs_save_source(target);
		obs_data_set_string(saved, "name", "Restored cleared selection");
		OBSSourceAutoRelease restored = obs_load_source(saved);
		QVERIFY(restored);
		OBSDataAutoRelease restoredSettings = obs_source_get_settings(restored);
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "selected_window")), QString());
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "app_id")), QString());
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "title_filter")), QString());
		QTRY_COMPARE(status(restored), QString("idle"));

		// Refreshing an unchanged placeholder must preserve a manually entered target.
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "title_filter", "Editor");
		obs_source_update(target, settings);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QVERIFY(!obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		QCOMPARE(QString(obs_data_get_string(settings, "title_filter")), QString("Editor"));
	}

	void desktopSelectionReturnsToAllDisplays()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.addDisplay({"DP-1", "Main display", {0, 0, 1920, 1080}, 1});
		kwin.addDisplay({"DP-2", "Second display", {-1280, 0, 1280, 720}, 1});
		QTRY_COMPARE(capture_displays().size(), 2);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "selected_display", "DP-2");
		OBSSourceAutoRelease target =
		    createSource("Reset display selection", settings, true, "m4capture_desktop_source");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.requestedDisplay, QString("DP-2"));
		int requests = kwin.captureRequests;

		auto props = obs_source_properties(target);
		auto destroyProps = qScopeGuard([&] { obs_properties_destroy(props); });
		auto picker = obs_properties_get(props, "selected_display");
		QVERIFY(picker);
		QCOMPARE(QString(obs_property_list_item_string(picker, 0)), QString());
		obs_data_set_string(settings, "selected_display", "");
		obs_property_modified(picker, settings);
		obs_source_update(target, settings);
		QTRY_COMPARE(kwin.requestedRegion, QRect(-1280, 0, 3200, 1080));
		QCOMPARE(kwin.requestedDisplay, QString());
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);

		OBSDataAutoRelease saved = obs_save_source(target);
		obs_data_set_string(saved, "name", "Restored all displays");
		OBSSourceAutoRelease restored = obs_load_source(saved);
		QVERIFY(restored);
		OBSDataAutoRelease restoredSettings = obs_source_get_settings(restored);
		QCOMPARE(QString(obs_data_get_string(restoredSettings, "selected_display")), QString());

		kwin.removeDisplay("DP-2");
		QTRY_COMPARE(kwin.requestedRegion, QRect(0, 0, 1920, 1080));
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
	}

	void cpuFallbackWarning()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"fallback", "test.app", "CPU capture", false});
		OBSSourceAutoRelease target = source("CPU warning");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		auto props = obs_source_properties(target);
		auto warning = obs_properties_first(props);
		QCOMPARE(QString(obs_property_name(warning)), QString("cpu_fallback"));
		QVERIFY(obs_property_visible(warning));
		QCOMPARE(obs_property_text_info_type(warning), OBS_TEXT_INFO_WARNING);
		QVERIFY(obs_property_next(&warning));
		QCOMPARE(QString(obs_property_name(warning)), QString("selected_window"));
		obs_properties_destroy(props);
		kwin.remove("fallback");
		QTRY_VERIFY(status(target) != "capturing");
		props = obs_source_properties(target);
		QVERIFY(!obs_property_visible(obs_properties_get(props, "cpu_fallback")));
		obs_properties_destroy(props);
	}
};

int main(int argc, char **argv) { return run_obs_tests<SourceSettingsTest>(argc, argv); }
#include "source-settings-test.moc"
