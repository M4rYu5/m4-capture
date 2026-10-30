#include "audio-fixture.hpp"
#include "dmabuf-fixture.hpp"
#include "kwin-fixture.hpp"
#include "obs-dmabuf.hpp"
#include "obs-graphics.hpp"
#include "obs-test-fixture.hpp"
#include "pixel-fixture.hpp"
#include "video-fixture.hpp"

#include <algorithm>
#include <cmath>

class CaptureTest : public ObsTestFixture
{
	Q_OBJECT
	KWinFixture kwin;

	QByteArray gpuPixels(const VideoFrame &frame)
	{
		GraphicsLock lock;
		auto stage = gs_stagesurface_create(frame.width, frame.height, GS_BGRA);
		QByteArray pixels;
		if (stage) {
			gs_stage_texture(stage, obs_frame_texture(frame));
			uint8_t *bytes = nullptr;
			uint32_t stride = 0;
			if (gs_stagesurface_map(stage, &bytes, &stride)) {
				for (uint32_t y = 0; y < frame.height; ++y)
					pixels.append(reinterpret_cast<char *>(bytes + y * stride), frame.width * 4);
				gs_stagesurface_unmap(stage);
			}
			gs_stagesurface_destroy(stage);
		}
		return pixels;
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
		cleanupObs();
		kwin.clear();
		kwin.clearDisplays();
		QTest::qWait(60);
	}
	void cleanupTestCase() { shutdown_capture(); }

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
		TestVideo video;
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
		QCOMPARE(video.audioCapturesCreated(), uint32_t(0));
		QCOMPARE(kwin.requestedRegion, QRect(-1280, 0, 3200, 1080));
		auto props = obs_source_properties(target);
		auto picker = obs_properties_get(props, "selected_display");
		QVERIFY(picker);
		QCOMPARE(obs_property_list_item_count(picker), size_t(3));
		QVERIFY(!obs_properties_get(props, "capture_audio"));
		QVERIFY(!obs_properties_get(props, "selected_window"));
		QVERIFY(!obs_properties_get(props, "app_id"));
		QVERIFY(!obs_properties_get(props, "raise_on_focus"));
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
		QVERIFY(obs_source_get_output_flags(target) & OBS_SOURCE_AUDIO);
		obs_data_set_string(settings, "selected_window", "pick");
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		obs_data_set_string(settings, "title_filter", "title");
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_data_set_bool(settings, "capture_audio", true);
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", false);
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
	}

	void applicationAudioFollowsSettingsVisibilityAndTarget()
	{
		TestVideo video;
		QVERIFY(video.startCapture(kwin));
		TestAudio first, second, unrelated;
		QVERIFY(first.start(video.openRemote(), "test.app"));
		QVERIFY(second.start(video.openRemote(), "other.app", {{"FL", -.3f}, {"FR", .4f}}));
		QVERIFY(unrelated.start(video.openRemote(), "unrelated.app"));
		kwin.add({"first", "test.app", "First", false, 12345});
		kwin.add({"second", "other.app", "Second", false, 12346});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		bool showing = true;
		obs_source_inc_showing(target);
		auto hide = qScopeGuard([&] {
			if (showing)
				obs_source_dec_showing(target);
		});
		struct Samples {
			std::atomic<uint64_t> count{0};
			std::atomic<float> left{0}, right{0};
		} samples;
		auto receive = [](void *data, obs_source_t *, const audio_data *audio, bool) {
			auto &samples = *static_cast<Samples *>(data);
			if (audio->frames && audio->data[0] && audio->data[1]) {
				samples.left = reinterpret_cast<const float *>(audio->data[0])[audio->frames / 2];
				samples.right = reinterpret_cast<const float *>(audio->data[1])[audio->frames / 2];
				++samples.count;
			}
		};
		obs_source_add_audio_capture_callback(target, receive, &samples);
		auto detach = qScopeGuard([&] { obs_source_remove_audio_capture_callback(target, receive, &samples); });
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), uint64_t(0));
		int requests = kwin.captureRequests;
		qputenv("PIPEWIRE_REMOTE", "/nonexistent/m4capture-audio-test");
		auto restoreRemote = qScopeGuard([&] { qputenv("PIPEWIRE_REMOTE", video.socketPath().toUtf8()); });
		obs_data_set_bool(settings, "capture_audio", true);
		obs_source_update(target, settings);
		QTest::qWait(200);
		QCOMPARE(samples.count.load(), uint64_t(0));
		QCOMPARE(status(target), QString("capturing"));
		QCOMPARE(kwin.captureRequests, requests);
		qputenv("PIPEWIRE_REMOTE", video.socketPath().toUtf8());
		QTRY_VERIFY_WITH_TIMEOUT(samples.count > 0, 5000);
		QTRY_VERIFY(std::abs(samples.left.load() - .125f) < .0001f);
		QVERIFY(std::abs(samples.right.load() + .25f) < .0001f);
		obs_data_set_bool(settings, "capture_audio", false);
		obs_source_update(target, settings);
		QTest::qWait(100);
		auto count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		QCOMPARE(kwin.captureRequests, requests);
		obs_data_set_bool(settings, "capture_audio", true);
		obs_source_update(target, settings);
		QTRY_VERIFY(samples.count > count);
		obs_source_dec_showing(target);
		showing = false;
		QTRY_COMPARE(status(target), QString("paused"));
		count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		obs_source_inc_showing(target);
		showing = true;
		QTRY_VERIFY(samples.count > count);
		obs_data_set_string(settings, "app_id", "other.app");
		obs_source_update(target, settings);
		QTRY_VERIFY2(std::abs(samples.left.load() + .3f) < .0001f,
			     qPrintable(QString("left=%1 count=%2 app=%3 status=%4")
					    .arg(samples.left.load())
					    .arg(samples.count.load())
					    .arg(obs_data_get_string(settings, "app_id"))
					    .arg(status(target))));
		QVERIFY(std::abs(samples.right.load() - .4f) < .0001f);
		kwin.remove("second");
		QTRY_COMPARE(status(target), QString("waiting"));
		count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		kwin.add({"reopened", "other.app", "Reopened", false, 12347});
		QTRY_VERIFY(samples.count > count);
		QTRY_VERIFY(std::abs(samples.left.load() + .3f) < .0001f);
	}

	void savingPreservesPendingAudioAndTargetEdits()
	{
		OBSSourceAutoRelease target = source("Pending edits", "test.app");
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_bool(settings, "capture_audio", true);
		obs_data_set_string(settings, "app_id", "other.app");
		obs_source_update(target, settings);
		// Save before OBS's video thread applies the deferred update callback.
		OBSDataAutoRelease saved = obs_save_source(target);
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QVERIFY(obs_data_get_bool(config, "capture_audio"));
		QCOMPARE(QString(obs_data_get_string(config, "app_id")), QString("other.app"));
	}

	void mixerEntryFollowsAudioSetting()
	{
		OBSDataAutoRelease settings = obs_data_create();
		OBSSourceAutoRelease target = createSource("Optional mixer entry", settings, false);
		QVERIFY(!obs_source_audio_active(target));
		std::atomic<int> activated{0}, deactivated{0};
		auto increment = [](void *data, calldata_t *) { ++*static_cast<std::atomic<int> *>(data); };
		OBSSignal activate(obs_source_get_signal_handler(target), "audio_activate", increment, &activated);
		OBSSignal deactivate(obs_source_get_signal_handler(target), "audio_deactivate", increment,
				     &deactivated);
		obs_data_set_bool(settings, "capture_audio", true);
		obs_source_update(target, settings);
		QTRY_VERIFY(obs_source_audio_active(target));
		QTRY_COMPARE(activated.load(), 1);
		obs_source_update(target, settings);
		QTest::qWait(80);
		QCOMPARE(activated.load(), 1);
		obs_data_set_bool(settings, "capture_audio", false);
		obs_source_update(target, settings);
		QTRY_VERIFY(!obs_source_audio_active(target));
		QTRY_COMPARE(deactivated.load(), 1);

		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease enabled = createSource("Saved audio capture", settings, false);
		QVERIFY(obs_source_audio_active(enabled));
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
		TestVideo video;
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

	void focusRaisesInsideEachContainerOnly()
	{
		QWidget window;
		mainWindow = &window;
		OBSSceneAutoRelease scene = obs_scene_create("Focus scene");
		currentScene = obs_scene_get_source(scene);
		OBSSourceAutoRelease target = source("Raise target", "test.app", true);
		OBSSourceAutoRelease filler = source("Other item", "");
		auto direct = obs_scene_add(scene, target);
		auto group = obs_scene_add_group(scene, "Group");
		auto groupScene = obs_sceneitem_group_get_scene(group);
		auto grouped = obs_scene_add(groupScene, target);
		auto sibling = obs_scene_add(groupScene, filler);
		auto nested = obs_scene_add_group(groupScene, "Nested");
		auto nestedScene = obs_sceneitem_group_get_scene(nested);
		auto nestedItem = obs_scene_add(nestedScene, target);
		obs_scene_add(nestedScene, filler);
		obs_sceneitem_set_visible(grouped, false);
		obs_sceneitem_set_locked(grouped, true);
		int groupPosition = obs_sceneitem_get_order_position(group);
		kwin.add({"focused", "test.app", "App", false});
		QTRY_VERIFY(kwin.requestedWindow == "focused");
		kwin.focus("focused");
		QTRY_COMPARE(obs_sceneitem_get_order_position(direct), 1);
		QCOMPARE(obs_sceneitem_get_order_position(grouped), 2);
		QCOMPARE(obs_sceneitem_get_order_position(nestedItem), 1);
		QCOMPARE(obs_sceneitem_get_scene(grouped), groupScene);
		QCOMPARE(obs_sceneitem_get_scene(nestedItem), nestedScene);
		QVERIFY(!obs_sceneitem_visible(grouped));
		QVERIFY(obs_sceneitem_locked(grouped));
		// The group retains its place relative to its siblings; only the matching root item moved past it.
		QCOMPARE(obs_sceneitem_get_order_position(group), groupPosition - 1);
		int position = obs_sceneitem_get_order_position(grouped);
		kwin.focus("");
		QTest::qWait(80);
		QCOMPARE(obs_sceneitem_get_order_position(grouped), position);
		obs_sceneitem_set_order_position(grouped, 0);
		kwin.focus("focused");
		QTRY_COMPARE(obs_sceneitem_get_order_position(grouped), 2);
		QVERIFY(obs_sceneitem_get_order_position(sibling) < 2);
	}

	void focusOptionDefaultsOff()
	{
		QWidget window;
		mainWindow = &window;
		OBSSceneAutoRelease scene = obs_scene_create("No raise scene");
		currentScene = obs_scene_get_source(scene);
		OBSSourceAutoRelease target = source("No raise");
		OBSSourceAutoRelease filler = source("Filler", "");
		auto item = obs_scene_add(scene, target);
		obs_scene_add(scene, filler);
		kwin.add({"off", "test.app", "App", false});
		QTRY_VERIFY(kwin.requestedWindow == "off");
		kwin.focus("off");
		QTest::qWait(100);
		QCOMPARE(obs_sceneitem_get_order_position(item), 0);
	}

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
		TestVideo video((alpha << 24) | (alpha << 16));
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
		TestVideo video;
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
		video.stopProducer();
		QVERIFY(video.startProducer());
		kwin.videoNode = video.nodeId();
		kwin.videoSerial = video.serial();
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
		TestVideo video;
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
		TestVideo video;
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
		TestVideo video;
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
		TestVideo video;
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
		TestVideo video;
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
		TestVideo video;
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

	void cpuFallbackWarning()
	{
		TestVideo video;
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

	void gpuTransformsMatchCpu_data()
	{
		QTest::addColumn<uint32_t>("transform");
		QTest::addColumn<bool>("crop");
		QTest::addColumn<bool>("opaque");
		for (uint32_t transform = 0; transform < 8; ++transform)
			for (bool crop : {false, true})
				for (bool opaque : {false, true})
					QTest::newRow(qPrintable(
					    QString("%1-crop%2-opaque%3").arg(transform).arg(crop).arg(opaque)))
					    << transform << crop << opaque;
	}

	void gpuTransformsMatchCpu()
	{
		QFETCH(uint32_t, transform);
		QFETCH(bool, crop);
		QFETCH(bool, opaque);
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf input;
		if (!input.buffer.n_datas || !supported_video_format(input.format))
			QSKIP("Driver cannot export the test texture as a supported DMA-BUF.");
		input.transform.transform = transform;
		if (crop)
			input.crop.region = {{1, 2}, {13, 7}};
		if (opaque)
			input.format.format = input.format.format == SPA_VIDEO_FORMAT_BGRA ? SPA_VIDEO_FORMAT_BGRx
											   : SPA_VIDEO_FORMAT_RGBx;
		spa_chunk chunk{};
		chunk.size = input.pixels.size();
		chunk.stride = 16 * 4;
		spa_data plane{};
		plane.type = SPA_DATA_MemPtr;
		plane.data = input.pixels.data();
		plane.maxsize = input.pixels.size();
		plane.chunk = &chunk;
		auto buffer = input.buffer;
		buffer.n_datas = 1;
		buffer.datas = &plane;
		auto format = input.format;
		format.format = opaque ? SPA_VIDEO_FORMAT_BGRx : SPA_VIDEO_FORMAT_BGRA;
		auto expected = reference_video_pixels(buffer, format);
		auto layout = video_layout(buffer, format);
		VideoFrame actual;
		QCOMPARE(capture->copy(input.buffer, input.format, actual), FrameResult::Ready);
		QCOMPARE(actual.width, layout.outputWidth());
		QCOMPARE(actual.height, layout.outputHeight());
		QVERIFY(actual.pixels.empty());
		QCOMPARE(gpuPixels(actual), expected);
	}

	void cpuFramesRenderNatively_data()
	{
		QTest::addColumn<int>("format");
		QTest::addColumn<uint32_t>("transform");
		QTest::addColumn<bool>("crop");
		QTest::addColumn<bool>("bottomUp");
		for (auto format :
		     {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx})
			for (uint32_t transform = 0; transform < 8; ++transform)
				for (bool crop : {false, true})
					for (bool bottomUp : {false, true})
						QTest::newRow(qPrintable(QString("%1-transform%2-crop%3-bottomUp%4")
									     .arg(format)
									     .arg(transform)
									     .arg(crop)
									     .arg(bottomUp)))
						    << int(format) << transform << crop << bottomUp;
	}

	void cpuFramesRenderNatively()
	{
		QFETCH(int, format);
		QFETCH(uint32_t, transform);
		QFETCH(bool, crop);
		QFETCH(bool, bottomUp);
		PixelFixture input(5, 3);
		input.format.format = static_cast<spa_video_format>(format);
		input.transform.transform = transform;
		if (crop)
			input.crop.region = {{1, 1}, {3, 2}};
		// Mix transparent, translucent, and fully opaque pixels, plus arbitrary x bytes.
		for (size_t i = 0; i < input.bytes.size(); i += 4) {
			if (i % 12 == 0)
				std::fill_n(input.bytes.data() + i, 4, 0);
			else if (i % 12 == 4)
				input.bytes[i + 3] = 255;
		}
		std::vector<uint8_t> padded(82, 0);
		for (size_t row = 0; row < 3; ++row)
			std::copy_n(input.bytes.data() + row * 20, 20, padded.data() + 1 + row * 27);
		input.plane.data = padded.data();
		input.plane.maxsize = padded.size();
		input.chunk.offset = bottomUp ? 55 : 1;
		input.chunk.stride = bottomUp ? -27 : 27;
		input.chunk.size = 74;
		auto expected = reference_video_pixels(input.buffer, input.format);
		VideoFrame frame;
		QCOMPARE(copy_video_frame(input.buffer, input.format, frame), FrameResult::Ready);
		// PipeWire can immediately overwrite the input once the CPU copy is published.
		std::fill(padded.begin(), padded.end(), 0);
		FrameBuffer frames;
		QVERIFY(frames.publish(frame));
		VideoFrame output;
		QCOMPARE(frames.take(output), FrameBuffer::Update::Updated);
		ObsVideoRenderer renderer;
		QCOMPARE(rendererPixels(renderer, output), expected);
		QCOMPARE(frames.take(output), FrameBuffer::Update::Unchanged);
		QCOMPARE(rendererPixels(renderer, output, false), expected);
	}

	void cpuFormatAndStorageChanges()
	{
		ObsVideoRenderer renderer;
		VideoFrame frame;
		// All have the same displayed size; texture format/storage dimensions differ.
		for (auto format : {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx,
				    SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA}) {
			for (uint32_t transform : {0u, 1u, 2u, 3u}) {
				PixelFixture input((transform & 1) ? 3 : 5, (transform & 1) ? 5 : 3);
				input.format.format = format;
				input.transform.transform = transform;
				QCOMPARE(copy_video_frame(input.buffer, input.format, frame), FrameResult::Ready);
				QCOMPARE(frame.width, 5u);
				QCOMPARE(frame.height, 3u);
				QCOMPARE(rendererPixels(renderer, frame),
					 reference_video_pixels(input.buffer, input.format));
			}
		}
	}

	void rendererSwitchesBetweenCpuAndDmaBuf()
	{
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf input;
		if (!input.buffer.n_datas || !supported_video_format(input.format))
			QSKIP("Driver cannot export the test texture as a supported DMA-BUF.");
		ObsVideoRenderer renderer;
		VideoFrame frame;
		for (int round = 0; round < 2; ++round) {
			QCOMPARE(capture->copy(input.buffer, input.format, frame), FrameResult::Ready);
			QCOMPARE(rendererPixels(renderer, frame), gpuPixels(frame));
			PixelFixture cpu(16, 16);
			cpu.format.format = SPA_VIDEO_FORMAT_RGBx;
			cpu.transform.transform = SPA_META_TRANSFORMATION_Flipped90;
			QCOMPARE(copy_video_frame(cpu.buffer, cpu.format, frame), FrameResult::Ready);
			QVERIFY(!frame.gpu);
			QCOMPARE(rendererPixels(renderer, frame), reference_video_pixels(cpu.buffer, cpu.format));
		}
	}

	void gpuImportCacheFollowsBufferLifetime()
	{
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf first, second;
		if (!first.buffer.n_datas || !second.buffer.n_datas || !supported_video_format(first.format) ||
		    !supported_video_format(second.format))
			QSKIP("Driver cannot export the test textures as supported DMA-BUFs.");
		auto before = dmabuf_import_stats();
		VideoFrame frame;
		for (int round = 0; round < 8; ++round) {
			for (auto input : {&first, &second}) {
				for (size_t i = 0; i < input->pixels.size(); i += 4) {
					input->pixels[i] = 20 + round * 20;
					input->pixels[i + 1] = input == &first ? 80 : 160;
					input->pixels[i + 2] = 230 - round * 20;
					input->pixels[i + 3] = 255;
				}
				input->upload();
				QCOMPARE(capture->copy(input->buffer, input->format, frame), FrameResult::Ready);
				QCOMPARE(gpuPixels(frame),
					 QByteArray(reinterpret_cast<const char *>(input->pixels.data()),
						    input->pixels.size()));
			}
		}
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(0));
		capture->removeBuffer(first.buffer);
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(1));
		QCOMPARE(capture->copy(second.buffer, second.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		// The same address and fds can identify a new buffer lifetime after removal.
		QCOMPARE(capture->copy(first.buffer, first.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(3));
		capture->clearImports();
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(3));
		QCOMPARE(capture->copy(second.buffer, second.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(4));
		capture.reset();
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(4));
	}

	void gpuImportCacheInvalidatesChangedLayout()
	{
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf input, replacement;
		if (!input.buffer.n_datas || !replacement.buffer.n_datas || !supported_video_format(input.format) ||
		    !supported_video_format(replacement.format))
			QSKIP("Driver cannot export the test textures as supported DMA-BUFs.");
		auto before = dmabuf_import_stats();
		VideoFrame frame;
		QCOMPARE(capture->copy(input.buffer, input.format, frame), FrameResult::Ready);
		auto opaque = input.format;
		opaque.format = opaque.format == SPA_VIDEO_FORMAT_BGRA ? SPA_VIDEO_FORMAT_BGRx : SPA_VIDEO_FORMAT_RGBx;
		QCOMPARE(capture->copy(input.buffer, opaque, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(1));
		// Crop/rotation change the draw, not the underlying image import.
		input.crop.region = {{1, 2}, {13, 7}};
		input.transform.transform = SPA_META_TRANSFORMATION_90;
		QCOMPARE(capture->copy(input.buffer, opaque, frame), FrameResult::Ready);
		QCOMPARE(frame.width, uint32_t(7));
		QCOMPARE(frame.height, uint32_t(13));
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		// Different plane storage at the same buffer address must replace the import.
		input.buffer.n_datas = replacement.buffer.n_datas;
		input.buffer.datas = replacement.buffer.datas;
		QCOMPARE(capture->copy(input.buffer, replacement.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(3));
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(2));
		capture->clearImports();
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(3));
	}

	void sceneAndPreviewVisibility()
	{
		TestVideo video;
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
		TestVideo video;
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

int main(int argc, char **argv) { return run_obs_tests<CaptureTest>(argc, argv); }

#include "capture-test.moc"
