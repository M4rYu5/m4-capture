#include "capture.hpp"
#include "kwin-fixture.hpp"
#include "video-fixture.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-nix-platform.h>

#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QListView>
#include <QStandardItemModel>
#include <QTest>
#include <QToolButton>
#include <wayland-client.h>

void register_waylandconnect_source();
void initialize_source_list();
void shutdown_source_list();

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
	bool live = qEnvironmentVariableIsSet("WAYLANDCONNECT_LIVE_TEST");
	QByteArray originalRemote = qgetenv("PIPEWIRE_REMOTE");

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
	void init() { kwin.reject = true; }
	void cleanup()
	{
		shutdown_source_list();
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
		qputenv("PIPEWIRE_REMOTE", originalRemote);
		QTest::qWait(60);
	}
	void cleanupTestCase() { shutdown_capture(); }

	void titleFiltersAndStableSelection()
	{
		WindowRule rule{"test.app", "Editor"};
		WindowInfo one{"one", "test.app", "My editor", false};
		QVERIFY(rule.matches(one));
		rule.caseSensitive = true;
		QVERIFY(!rule.matches(one));
		rule.caseSensitive = false;
		rule.mode = TitleMatch::Exact;
		QVERIFY(!rule.matches(one));
		rule.title = "MY EDITOR";
		QVERIFY(rule.matches(one));
		rule.mode = TitleMatch::Wildcard;
		rule.title = "*edit?r";
		QVERIFY(rule.matches(one));
		rule.title = "*Project*";
		QVERIFY(rule.matches({"path", "test.app", "/home/mok/Project", false}));
		rule.mode = TitleMatch::Regex;
		rule.title = "^my.*editor$";
		QVERIFY(rule.matches(one));
		rule.title = "[";
		QVERIFY(!rule.error().isEmpty());
		QVERIFY(!rule.matches(one));
		rule.title.clear();
		QVERIFY(rule.matches(one));
		bool ambiguous;
		WindowInfo two{"two", "test.app", "Other", true};
		QCOMPARE(select_window({one, two}, rule, {}, {}, &ambiguous), QString());
		QVERIFY(ambiguous);
		QCOMPARE(select_window({one, two}, rule, {}, "one", &ambiguous), QString("one"));
		rule.title = "does not match";
		QCOMPARE(select_window({one, two}, rule, "one", {}, &ambiguous), QString("one"));
		QCOMPARE(select_window({two}, rule, "one", {}, &ambiguous), QString());
	}

	void absentWindowWaitsWithoutCaptureRequests()
	{
		OBSSourceAutoRelease target = source("Absent target");
		int requests = kwin.captureRequests;
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("waiting"), 3000);
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		QCOMPARE(renderedPixel(target).alpha(), 0);
		kwin.add({"arriving", "test.app", "App opened", false});
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 3000);
		QCOMPARE(kwin.requestedWindow, QString("arriving"));
	}

	void stopDisarmsAndConnectRearms()
	{
		OBSSourceAutoRelease target = source("Manual stop");
		QTRY_COMPARE(status(target), QString("waiting"));
		action(target, "stop");
		QTRY_COMPARE(status(target), QString("idle"));
		int requests = kwin.captureRequests;
		kwin.add({"manual", "test.app", "Open", false});
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		action(target, "connect");
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
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
		obs_data_set_string(settings, "selected_window", "pick");
		QVERIFY(obs_property_modified(picker, settings));
		QCOMPARE(QString(obs_data_get_string(settings, "app_id")), QString("test.app"));
		obs_data_set_string(settings, "title_filter", "title");
		obs_data_set_bool(settings, "capture_cursor", false);
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
		QVERIFY(video.start());
		QVERIFY(video.startProducer());
		qputenv("PIPEWIRE_REMOTE", video.socketPath().toUtf8());
		kwin.reject = false;
		kwin.videoNode = video.nodeId();
		kwin.videoSerial = video.serial();
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

	void automaticReconnectCanBeDisabled()
	{
		kwin.add({"no-retry", "test.app", "App", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_bool(settings, "auto_reconnect", false);
		OBSSourceAutoRelease target =
		    obs_source_create("waylandconnect_source", "No auto retry", settings, nullptr);
		int requests = kwin.captureRequests;
		QTRY_COMPARE_WITH_TIMEOUT(kwin.captureRequests, requests + 1, 3000);
		QTRY_COMPARE(status(target), QString("failed"));
		kwin.remove("no-retry");
		kwin.add({"new-no-retry", "test.app", "Returned", false});
		QTest::qWait(2100);
		QCOMPARE(kwin.captureRequests, requests + 1);
		action(target, "connect");
		QTRY_COMPARE(kwin.captureRequests, requests + 2);
	}

	void rowButtonGoesBeforeEyeAndStartsCapture()
	{
		QWidget window;
		mainWindow = &window;
		QListView view(&window);
		view.setObjectName("sources");
		QStandardItemModel model(1, 1);
		view.setModel(&model);
		OBSSourceAutoRelease target = source("Row target", "test.app", false, false);
		kwin.add({"row", "test.app", "App", false});
		model.setData(model.index(0, 0), "Row target", Qt::AccessibleTextRole);
		auto row = new QWidget;
		auto layout = new QHBoxLayout(row);
		layout->addWidget(new QLabel("Row target"));
		auto eye = new QCheckBox;
		eye->setProperty("class", "checkbox-icon indicator-visibility");
		layout->addWidget(eye);
		layout->addWidget(new QCheckBox);
		view.setIndexWidget(model.index(0, 0), row);
		initialize_source_list();
		auto button = row->findChild<QToolButton *>("waylandconnectConnect");
		QVERIFY(button);
		QCOMPARE(layout->indexOf(button) + 1, layout->indexOf(eye));
		QCOMPARE(button->property("source_uuid").toString(), QString(obs_source_get_uuid(target)));
		int requests = kwin.captureRequests;
		QTest::qWait(100);
		QCOMPARE(kwin.captureRequests, requests);
		button->click();
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		shutdown_source_list();
		mainWindow = nullptr;
	}

	void liveKWinCapture()
	{
		if (!live)
			QSKIP("Opt-in test captures only its own temporary Wayland window.");
		auto makeWindow = [] {
			auto w = std::make_unique<QWidget>(nullptr, Qt::FramelessWindowHint);
			w->setWindowTitle("KCow live check");
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
					       [](const WindowInfo &w) { return w.appId == "waylandconnect-test"; });
		    })(),
		    5000);
		OBSSourceAutoRelease target = source("Live target", "waylandconnect-test");
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
	app.setDesktopFileName("waylandconnect-test");
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
	register_waylandconnect_source();
	CaptureTest test;
	int result = QTest::qExec(&test, argc, argv);
	shutdown_source_list();
	shutdown_capture();
	obs_shutdown();
	wl_display_disconnect(display);
	return result;
}

#include "capture-test.moc"
