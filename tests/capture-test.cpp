#include "dmabuf-fixture.hpp"
#include "kwin-fixture.hpp"
#include "obs-capture.hpp"
#include "obs-dmabuf.hpp"
#include "video-fixture.hpp"
#include <QScopeGuard>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-nix-platform.h>

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QTest>
#include <ctime>
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
	std::vector<OBSWeakSource> shownSources;

	obs_source_t *createSource(const char *name, obs_data_t *settings, bool showing = true)
	{
		auto source = obs_source_create("waylandconnect_source", name, settings, nullptr);
		if (source && showing) {
			// Model a properties preview for tests that render a standalone source.
			obs_source_inc_showing(source);
			shownSources.emplace_back(OBSGetWeakRef(source));
		}
		return source;
	}

	QString status(obs_source_t *source)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		QString result = calldata_string(&params, "status");
		calldata_free(&params);
		return result;
	}
	obs_source_t *source(const char *name, const char *app = "test.app", bool raise = false)
	{
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", app);
		obs_data_set_bool(settings, "raise_on_focus", raise);
		return createSource(name, settings);
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
		for (const auto &weak : shownSources)
			if (auto source = OBSGetStrongRef(weak))
				obs_source_dec_showing(source);
		shownSources.clear();
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
		obs_data_set_string(settings, "selected_window", "pick");
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		obs_data_set_string(settings, "title_filter", "title");
		obs_data_set_bool(settings, "capture_cursor", false);
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", false);
		obs_source_update(target, settings);
		obs_properties_destroy(props);
		QTest::qWait(80);
		OBSDataAutoRelease saved = obs_save_source(target);
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QCOMPARE(QString(obs_data_get_string(config, "app_id")), QString("test.app"));
		QCOMPARE(QString(obs_data_get_string(config, "title_filter")), QString("title"));
		QVERIFY(obs_data_get_bool(config, "raise_on_focus"));
		QVERIFY(!obs_data_get_bool(config, "capture_cursor"));
		QVERIFY(!obs_data_get_bool(config, "disconnect_on_title_mismatch"));
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
		VideoFrame expected, actual;
		QCOMPARE(convert_video_frame(buffer, format, expected), FrameResult::Ready);
		QCOMPARE(capture->copy(input.buffer, input.format, actual), FrameResult::Ready);
		QCOMPARE(actual.width, expected.width);
		QCOMPARE(actual.height, expected.height);
		QVERIFY(actual.pixels.empty());
		obs_enter_graphics();
		auto stage = gs_stagesurface_create(actual.width, actual.height, GS_BGRA);
		gs_stage_texture(stage, obs_frame_texture(actual));
		uint8_t *bytes = nullptr;
		uint32_t stride = 0;
		bool mapped = gs_stagesurface_map(stage, &bytes, &stride);
		QByteArray pixels;
		if (mapped) {
			for (uint32_t y = 0; y < actual.height; ++y)
				pixels.append(reinterpret_cast<char *>(bytes + y * stride), actual.width * 4);
			gs_stagesurface_unmap(stage);
		}
		gs_stagesurface_destroy(stage);
		obs_leave_graphics();
		QVERIFY(mapped);
		QCOMPARE(pixels,
			 QByteArray(reinterpret_cast<const char *>(expected.pixels.data()), expected.pixels.size()));
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

		obs_source_inc_active(sceneSource);
		auto deactivate = qScopeGuard([&] { obs_source_dec_active(sceneSource); });
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
		obs_source_dec_active(sceneSource);
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
		QTRY_VERIFY_WITH_TIMEOUT(([&] {
						 auto windows = capture_windows();
						 return std::any_of(
						     windows.begin(), windows.end(),
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
		window.reset();
		QTRY_VERIFY(status(target) != "capturing");
	}

	void liveImportFailureFallsBackToCpu()
	{
		if (!live)
			QSKIP("Opt-in test captures only its own temporary Wayland window.");
		auto gpu = create_obs_dmabuf_capture();
		if (!gpu)
			QSKIP("DMA-BUF import is unavailable.");
		class RejectImport : public DmaBufCapture
		{
		      public:
			std::shared_ptr<DmaBufCapture> delegate;
			std::atomic<int> rejected{0};
			explicit RejectImport(std::shared_ptr<DmaBufCapture> gpu) : delegate(std::move(gpu))
			{
				formats = delegate->formats;
				explicitSync = delegate->explicitSync;
			}
			FrameResult copy(const spa_buffer &buffer, const spa_video_info_raw &, VideoFrame &) override
			{
				++rejected;
				delegate->discard(buffer);
				return FrameResult::UnmappedBuffer;
			}
			void discard(const spa_buffer &buffer) override { delegate->discard(buffer); }
		};
		auto reject = std::make_shared<RejectImport>(gpu);
		QWidget window;
		window.setWindowTitle("KWinCapture fallback check");
		window.setFixedSize(160, 90);
		window.setStyleSheet("background-color: rgb(224, 128, 32)");
		window.show();
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		CaptureCallbacks callbacks;
		callbacks.gpu = reject;
		auto state = std::make_shared<CaptureState>(160, 90, callbacks);
		auto cleanup = qScopeGuard([&] { release_capture(state); });
		CaptureConfig config;
		config.rule.appId = "kwincapture-test";
		state->configure(config);
		queue_capture_update(state);
		QTRY_COMPARE_WITH_TIMEOUT(state->frames->mode(), FrameBuffer::Mode::Cpu, 12000);
		QVERIFY(reject->rejected.load() > 0);
		VideoFrame frame;
		QCOMPARE(state->frames->take(frame), FrameBuffer::Update::Updated);
		QVERIFY(!frame.pixels.empty());
		QVERIFY(!frame.gpu);
		QVERIFY(!reject->enabled.load());
		auto rejected = reject->rejected.load();
		auto sequence = state->frames->sequence();
		config.cursor = false;
		state->configure(config);
		queue_capture_update(state);
		QTRY_VERIFY_WITH_TIMEOUT(state->frames->sequence() > sequence, 5000);
		QCOMPARE(state->frames->mode(), FrameBuffer::Mode::Cpu);
		QCOMPARE(reject->rejected.load(), rejected);
	}

	void liveHiddenCapture()
	{
		if (!live)
			QSKIP("Opt-in test captures only its own temporary Wayland window.");
		class CountingImport : public DmaBufCapture
		{
		      public:
			std::shared_ptr<DmaBufCapture> delegate;
			std::atomic<uint64_t> copies{0}, discards{0};
			explicit CountingImport(std::shared_ptr<DmaBufCapture> gpu) : delegate(std::move(gpu))
			{
				formats = delegate->formats;
				explicitSync = delegate->explicitSync;
			}
			FrameResult copy(const spa_buffer &buffer, const spa_video_info_raw &format,
					 VideoFrame &output) override
			{
				++copies;
				return delegate->copy(buffer, format, output);
			}
			void discard(const spa_buffer &buffer) override
			{
				++discards;
				delegate->discard(buffer);
			}
		};
		QWidget window(nullptr, Qt::FramelessWindowHint);
		window.setWindowTitle("KWinCapture hidden source check");
		window.setFixedSize(160, 90);
		window.setStyleSheet("background-color: rgb(220, 40, 80)");
		window.show();
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		auto gpu = create_obs_dmabuf_capture();
		auto counter = gpu ? std::make_shared<CountingImport>(gpu) : nullptr;
		CaptureCallbacks callbacks;
		callbacks.gpu = counter;
		auto state = std::make_shared<CaptureState>(160, 90, callbacks);
		auto cleanup = qScopeGuard([&] { release_capture(state); });
		CaptureConfig config;
		config.rule.appId = "kwincapture-test";
		state->configure(config);
		queue_capture_update(state);
		QTRY_COMPARE_WITH_TIMEOUT(state->diagnostics().status, CaptureStatus::Capturing, 8000);
		if (counter) {
			QCOMPARE(state->frames->mode(), FrameBuffer::Mode::DmaBuf);
			QVERIFY(counter->copies.load() > 0);
		}
		state->setShowing(false);
		queue_capture_update(state);
		QTRY_COMPARE(state->diagnostics().status, CaptureStatus::Paused);
		auto sequence = state->frames->sequence();
		auto copies = counter ? counter->copies.load() : 0;
		auto discards = counter ? counter->discards.load() : 0;
		// Repeated compositor updates while hidden must not perform capture work.
		for (int i = 0; i < 10; ++i) {
			window.setStyleSheet(QString("background-color: rgb(40, %1, 80)").arg(210 + i));
			QTest::qWait(30);
		}
		QCOMPARE(state->frames->sequence(), sequence);
		if (counter) {
			QCOMPARE(counter->copies.load(), copies);
			QCOMPARE(counter->discards.load(), discards);
		}
		// The window is now static. Reopening must still deliver its new green frame.
		state->setShowing(true);
		queue_capture_update(state);
		QTRY_VERIFY_WITH_TIMEOUT(state->frames->sequence() > sequence, 8000);
		VideoFrame frame;
		QCOMPARE(state->frames->take(frame), FrameBuffer::Update::Updated);
		QColor pixel;
		if (!frame.gpu) {
			pixel = QColor(frame.pixels[2], frame.pixels[1], frame.pixels[0], frame.pixels[3]);
		} else {
			obs_enter_graphics();
			auto stage = gs_stagesurface_create(frame.width, frame.height, GS_BGRA);
			gs_stage_texture(stage, obs_frame_texture(frame));
			uint8_t *bytes = nullptr;
			uint32_t stride = 0;
			if (gs_stagesurface_map(stage, &bytes, &stride)) {
				pixel = QColor(bytes[2], bytes[1], bytes[0], bytes[3]);
				gs_stagesurface_unmap(stage);
			}
			gs_stagesurface_destroy(stage);
			obs_leave_graphics();
		}
		QVERIFY(pixel.isValid());
		QCOMPARE(pixel.alpha(), 255);
		QVERIFY(pixel.green() > pixel.red());
	}

	void liveFrameRateProfile()
	{
		if (!live)
			QSKIP("Opt-in test captures only its own temporary animated window.");
		QWidget window;
		window.setWindowTitle("KWinCapture frame rate check");
		window.setWindowFlags(Qt::FramelessWindowHint);
		window.setFixedSize(160, 90);
		window.show();
		QTimer animate;
		animate.setTimerType(Qt::PreciseTimer);
		int tick = 0;
		connect(&animate, &QTimer::timeout, &window, [&] {
			window.setStyleSheet(QString("background-color: rgb(%1, 80, 40)").arg(80 + (++tick % 120)));
		});
		animate.start(2);
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		OBSSourceAutoRelease target = source("Frame rate target", "kwincapture-test");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 8000);
		auto frames = [&] {
			calldata_t data{};
			proc_handler_call(obs_source_get_proc_handler(target), "get_status", &data);
			auto count = calldata_int(&data, "frames");
			calldata_free(&data);
			return count;
		};
		obs_video_info original{};
		QVERIFY(obs_get_video_info(&original));
		auto restore = qScopeGuard([&] { obs_reset_video(&original); });
		for (bool changeRate : {false, true}) {
			obs_video_info video{};
			QVERIFY(obs_get_video_info(&video));
			if (changeRate) {
				video.fps_num = video.fps_num > 30 * video.fps_den ? 30 : 60;
				video.fps_den = 1;
				QCOMPARE(obs_reset_video(&video), OBS_VIDEO_SUCCESS);
			}
			QTest::qWait(500);
			QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
			auto start = frames();
			QElapsedTimer elapsed;
			elapsed.start();
			QTest::qWait(1500);
			const double seconds = elapsed.nsecsElapsed() / 1e9;
			const auto prepared = frames() - start;
			qInfo("FRAME_RATE_PROFILE prepared=%lld seconds=%.3f output=%u/%u fps=%.2f",
			      (long long)prepared, seconds, video.fps_num, video.fps_den, prepared / seconds);
			QVERIFY(prepared > 0);
			QCOMPARE(renderedPixel(target).alpha(), 255);
		}
	}

	void capturePerformance()
	{
		if (!live || !qEnvironmentVariableIsSet("KWINCAPTURE_BENCHMARK_WIDTH"))
			QSKIP("Opt-in benchmark captures only its own animated window.");
		const int width = qEnvironmentVariableIntValue("KWINCAPTURE_BENCHMARK_WIDTH");
		const int height = width == 3840 ? 2160 : 1080;
		class AnimatedWindow : public QWidget
		{
		      public:
			int tick = 0;
			void paintEvent(QPaintEvent *) override
			{
				QPainter painter(this);
				painter.fillRect(rect(), QColor(80 + tick % 120, 80, 40));
			}
		} window;
		window.setWindowFlags(Qt::FramelessWindowHint);
		window.setWindowTitle("KWinCapture performance check");
		window.setFixedSize(width, height);
		window.show();
		QTest::qWait(200);
		window.setFixedSize(width, height);
		window.resize(width, height);
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		OBSSourceAutoRelease target = source("Performance target", "kwincapture-test");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 15000);
		QCOMPARE(obs_source_get_width(target), uint32_t(width));
		QCOMPARE(obs_source_get_height(target), uint32_t(height));
		obs_enter_graphics();
		auto render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		QTimer animate;
		const int animationInterval = qEnvironmentVariableIntValue("KWINCAPTURE_BENCHMARK_ANIMATION_MS");
		if (animationInterval > 0) {
			animate.setTimerType(Qt::PreciseTimer);
			connect(&animate, &QTimer::timeout, &window, [&] {
				++window.tick;
				window.update();
			});
			animate.start(animationInterval);
		}
		QTimer timer;
		timer.setTimerType(Qt::PreciseTimer);
		int draws = 0;
		connect(&timer, &QTimer::timeout, this, [&] {
			if (!animate.isActive()) {
				++window.tick;
				window.update();
			}
			obs_enter_graphics();
			gs_texrender_reset(render);
			if (gs_texrender_begin(render, width, height)) {
				gs_projection_push();
				gs_ortho(0, width, 0, height, -100, 100);
				obs_source_video_render(target);
				gs_projection_pop();
				gs_texrender_end(render);
				gs_flush();
				++draws;
			}
			obs_leave_graphics();
		});
		timer.start(16);
		QTest::qWait(2000);
		auto frames = [&] {
			calldata_t data{};
			proc_handler_call(obs_source_get_proc_handler(target), "get_status", &data);
			auto count = calldata_int(&data, "frames");
			calldata_free(&data);
			return count;
		};
		auto startFrames = frames();
		draws = 0;
		timespec startCpu{}, endCpu{};
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &startCpu);
		QElapsedTimer elapsed;
		elapsed.start();
		QTest::qWait(10000);
		const double seconds = elapsed.nsecsElapsed() / 1e9;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &endCpu);
		const double cpu = endCpu.tv_sec - startCpu.tv_sec + (endCpu.tv_nsec - startCpu.tv_nsec) / 1e9;
		const auto captured = frames() - startFrames;
		timer.stop();
		calldata_t diagnostic{};
		proc_handler_call(obs_source_get_proc_handler(target), "get_status", &diagnostic);
		qInfo("BENCHMARK_PATH %s", calldata_bool(&diagnostic, "gpu") ? "dmabuf" : "cpu");
		calldata_free(&diagnostic);
		qInfo("BENCHMARK %dx%d seconds=%.3f frames=%lld capture_fps=%.2f draws=%d cpu_seconds=%.3f "
		      "cpu_percent=%.2f cpu_ms_per_frame=%.3f",
		      width, height, seconds, (long long)captured, captured / seconds, draws, cpu, cpu / seconds * 100,
		      cpu * 1000 / captured);
		QVERIFY(captured > 0);
		obs_enter_graphics();
		gs_texrender_destroy(render);
		obs_leave_graphics();
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
	video.fps_num = qEnvironmentVariableIntValue("KWINCAPTURE_TEST_FPS_NUM");
	video.fps_den = qEnvironmentVariableIntValue("KWINCAPTURE_TEST_FPS_DEN");
	if (!video.fps_num)
		video.fps_num = qEnvironmentVariableIsSet("KWINCAPTURE_BENCHMARK_WIDTH") ? 60 : 30;
	if (!video.fps_den)
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
