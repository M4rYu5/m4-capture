#include "kwin-fixture.hpp"
#include "obs-capture.hpp"
#include "video-fixture.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-nix-platform.h>

#include <QApplication>
#include <QTest>
#include <wayland-client.h>

static QWidget *mainWindow = nullptr;
static obs_source_t *currentScene = nullptr;
static int saves = 0;
extern "C" void *obs_frontend_get_main_window() { return mainWindow; }
extern "C" obs_source_t *obs_frontend_get_current_scene() { return obs_source_get_ref(currentScene); }
extern "C" void obs_frontend_save() { ++saves; }
extern "C" const char *obs_module_text(const char *text) { return text; }

class CaptureTest : public QObject
{
	Q_OBJECT
	KWinFixture kwin;
	bool live = qEnvironmentVariableIsSet("KWINCAPTURE_LIVE_TEST");

	QString status(obs_source_t *source)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		QString result = calldata_string(&params, "status");
		calldata_free(&params);
		return result;
	}
	void action(obs_source_t *source, const char *name)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), name, &params);
		calldata_free(&params);
	}
	obs_source_t *source(const char *name, const char *app = "test.app", bool raise = false, bool automatic = true)
	{
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", app);
		obs_data_set_bool(settings, "raise_on_focus", raise);
		obs_data_set_bool(settings, "auto_start", automatic);
		return obs_source_create("waylandconnect_source", name, settings, nullptr);
	}
	QColor renderedPixel(obs_source_t *source)
	{
		obs_enter_graphics();
		auto target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		auto stage = gs_stagesurface_create(160, 90, GS_RGBA);
		QColor result;
		if (gs_texrender_begin(target, 160, 90)) {
			gs_projection_push();
			vec4 clear{};
			gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
			gs_ortho(0, 160, 0, 90, -100, 100);
			obs_source_video_render(source);
			gs_projection_pop();
			gs_texrender_end(target);
			gs_stage_texture(stage, gs_texrender_get_texture(target));
			uint8_t *bytes = nullptr;
			uint32_t stride = 0;
			if (gs_stagesurface_map(stage, &bytes, &stride)) {
				result = QColor(bytes[0], bytes[1], bytes[2], bytes[3]);
				gs_stagesurface_unmap(stage);
			}
		}
		gs_stagesurface_destroy(stage);
		gs_texrender_destroy(target);
		obs_leave_graphics();
		return result;
	}

      private slots:
	void initTestCase()
	{
		if (!live) {
			QVERIFY(kwin.start());
			qputenv("WAYLAND_DISPLAY", kwin.socketPath().toUtf8());
		}
		initialize_capture();
		if (!live) {
			kwin.add({"probe", "probe.app", "Probe", false});
			QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 3000);
			kwin.clear();
		}
	}
	void init()
	{
		kwin.reject = true;
		kwin.deferCapture = false;
	}
	void cleanup()
	{
		mainWindow = nullptr;
		currentScene = nullptr;
		// OBS 32 canvases own public scenes/groups in addition to our test refs.
		// Remove those canvas refs before tearing down their child sources.
		std::vector<OBSSource> scenes;
		auto collect = [](void *data, obs_source_t *source) {
			if (obs_source_is_scene(source) || obs_source_is_group(source)) {
				auto &items = *static_cast<std::vector<OBSSource> *>(data);
				if (std::none_of(items.begin(), items.end(),
						 [source](const auto &item) { return item == source; }))
					items.emplace_back(source);
			}
			return true;
		};
		obs_enum_sources(collect, &scenes);
		obs_enum_scenes(collect, &scenes);
		for (const auto &source : scenes)
			obs_canvas_scene_remove(obs_group_or_scene_from_source(source));
		scenes.clear();
		if (!live)
			kwin.clear();
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
		QVERIFY(obs_property_enabled(obs_properties_get(props, "connect")));
		QCOMPARE(QString(obs_property_long_description(obs_properties_get(props, "status"))),
			 QString("Status.connecting"));
		int requests = kwin.captureRequests;
		QVERIFY(!obs_property_button_clicked(obs_properties_get(props, "connect"), target));
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		obs_properties_destroy(props);
		action(target, "stop");
		QTRY_COMPARE(status(target), QString("idle"));
		QCOMPARE(updates, 0);
		props = obs_source_properties(target);
		QVERIFY(obs_property_enabled(obs_properties_get(props, "connect")));
		QCOMPARE(QString(obs_property_long_description(obs_properties_get(props, "status"))),
			 QString("Status.idle"));
		QVERIFY(!obs_property_button_clicked(obs_properties_get(props, "refresh_windows"), target));
		QCOMPARE(updates, 1);
		obs_properties_destroy(props);
	}

	void settingsAndPickerRoundTrip()
	{
		kwin.add({"pick", "test.app", "Window title", false});
		QTRY_VERIFY(capture_windows().size() == 1);
		OBSSourceAutoRelease target = source("Settings", "", true, false);
		OBSDataAutoRelease settings = obs_source_get_settings(target);
		auto props = obs_source_properties(target);
		auto picker = obs_properties_get(props, "selected_window");
		QVERIFY(picker);
		QVERIFY(obs_properties_get(props, "disconnect_on_title_mismatch"));
		QVERIFY(!obs_data_get_bool(settings, "disconnect_on_title_mismatch"));
		obs_data_set_string(settings, "selected_window", "pick");
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		obs_data_set_string(settings, "title_filter", "title");
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", true);
		obs_source_update(target, settings);
		obs_properties_destroy(props);
		QTest::qWait(80);
		OBSDataAutoRelease saved = obs_save_source(target);
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QCOMPARE(QString(obs_data_get_string(config, "app_id")), QString("test.app"));
		QCOMPARE(QString(obs_data_get_string(config, "title_filter")), QString("title"));
		QVERIFY(obs_data_get_bool(config, "raise_on_focus"));
		QVERIFY(!obs_data_get_bool(config, "capture_cursor"));
		QVERIFY(!obs_data_get_bool(config, "auto_start"));
		QVERIFY(obs_data_get_bool(config, "disconnect_on_title_mismatch"));
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
		action(target, "stop");
		QTRY_COMPARE(status(target), QString("idle"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
	}

	void titleMismatchDisconnects_data()
	{
		QTest::addColumn<bool>("enabled");
		QTest::addColumn<bool>("automatic");
		QTest::newRow("enabled-automatic") << true << true;
		QTest::newRow("enabled-manual") << true << false;
		QTest::newRow("enable-during-capture") << false << true;
	}

	void titleMismatchDisconnects()
	{
		QFETCH(bool, enabled);
		QFETCH(bool, automatic);
		TestVideo video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"editor", "test.app", "Other", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "selected_window", "editor");
		obs_data_set_string(settings, "title_filter", "Editor");
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", enabled);
		obs_data_set_bool(settings, "auto_reconnect", automatic);
		OBSSourceAutoRelease target =
		    obs_source_create("waylandconnect_source", "Title matching", settings, nullptr);
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
		QTRY_COMPARE(status(target), automatic ? QString("waiting") : QString("idle"));
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		QCOMPARE(obs_source_get_height(target), uint32_t(90));
		QCOMPARE(kwin.captureRequests, requests);
		kwin.title("editor", "Editor");
		QTRY_VERIFY(capture_windows().front().title == "Editor");
		if (!automatic) {
			QCOMPARE(status(target), QString("idle"));
			QCOMPARE(kwin.captureRequests, requests);
			action(target, "connect");
		}
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QCOMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(renderedPixel(target).alpha(), 255);
	}

	void titleMismatchWithAlternateWindow_data()
	{
		QTest::addColumn<bool>("automatic");
		QTest::newRow("automatic") << true;
		QTest::newRow("manual") << false;
	}

	void titleMismatchWithAlternateWindow()
	{
		QFETCH(bool, automatic);
		TestVideo video;
		QVERIFY(video.startCapture(kwin));
		kwin.add({"editor", "test.app", "Editor", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "title_filter", "Editor");
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", true);
		obs_data_set_bool(settings, "auto_reconnect", automatic);
		OBSSourceAutoRelease target =
		    obs_source_create("waylandconnect_source", "Alternate window", settings, nullptr);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		kwin.add({"other", "test.app", "Editor two", false});
		QTRY_COMPARE(capture_windows().size(), 2);
		int requests = kwin.captureRequests;
		QCOMPARE(kwin.requestedWindow, QString("editor"));
		kwin.title("editor", "Other");
		if (!automatic) {
			QTRY_COMPARE(status(target), QString("idle"));
			QCOMPARE(renderedPixel(target).alpha(), 0);
			QCOMPARE(kwin.captureRequests, requests);
			kwin.focus("other");
			QTest::qWait(100);
			QCOMPARE(status(target), QString("idle"));
			QCOMPARE(kwin.captureRequests, requests);
			action(target, "connect");
		}
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
		OBSSourceAutoRelease target =
		    obs_source_create("waylandconnect_source", "Cursor toggle", settings, nullptr);
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
		int requests = kwin.captureRequests;
		OBSDataAutoRelease settings = obs_source_get_settings(target);
		for (const char *filter : {"E", "Ed", "Editor"}) {
			obs_data_set_string(settings, "title_filter", filter);
			obs_source_update(target, settings);
			QTest::qWait(100);
			QCOMPARE(kwin.captureRequests, requests);
			QCOMPARE(status(target), QString("capturing"));
			QCOMPARE(renderedPixel(target).alpha(), 255);
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
		QCOMPARE(updates, 0);
	}

	void liveKWinCapture()
	{
		if (!live)
			QSKIP("Opt-in test captures only its own temporary Wayland window.");
		auto makeWindow = [] {
			auto w = std::make_unique<QWidget>(nullptr, Qt::FramelessWindowHint);
			w->setWindowTitle("KWinCapture live check");
			w->setFixedSize(160, 90);
			w->setStyleSheet("background-color: rgb(224, 128, 32)");
			w->show();
			return w;
		};
		auto window = makeWindow();
		QTRY_VERIFY_WITH_TIMEOUT(
		    ([&] {
			    auto windows = capture_windows();
			    return std::any_of(windows.begin(), windows.end(),
					       [](const WindowInfo &w) { return w.appId == "kwincapture-test"; });
		    })(),
		    5000);
		OBSSourceAutoRelease target = source("Live target", "kwincapture-test");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 8000);
		auto pixel = renderedPixel(target);
		QCOMPARE(pixel.alpha(), 255);
		QVERIFY(pixel.red() > pixel.green());
		uint32_t width = obs_source_get_width(target), height = obs_source_get_height(target);
		window.reset();
		QTRY_VERIFY_WITH_TIMEOUT(status(target) != "capturing", 3000);
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), width);
		QCOMPARE(obs_source_get_height(target), height);
		window = makeWindow();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 8000);
		QCOMPARE(renderedPixel(target).alpha(), 255);
		action(target, "stop");
		QTRY_COMPARE(status(target), QString("idle"));
	}
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setDesktopFileName("kwincapture-test");
	auto display = wl_display_connect(nullptr);
	if (!display) {
		qCritical("Tests need a Wayland session for libobs graphics.");
		return 1;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
	obs_set_nix_platform_display(display);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;
	obs_video_info video{};
	video.graphics_module = "libobs-opengl";
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.output_width = 320;
	video.base_height = video.output_height = 240;
	video.output_format = VIDEO_FORMAT_RGBA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
		return 1;
	register_kwincapture_source();
	CaptureTest test;
	int result = QTest::qExec(&test, argc, argv);
	shutdown_capture();
	obs_shutdown();
	wl_display_disconnect(display);
	return result;
}

#include "capture-test.moc"
