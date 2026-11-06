#include "simulated-obs-fixture.hpp"
#include "video-capture-fixture.hpp"

class FocusOrderTest : public SimulatedObsFixture
{
	Q_OBJECT
      private slots:
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

	void focusSwitchesLiveCaptureAndRaisesSource()
	{
		VideoCaptureFixture video;
		QVERIFY(video.startCapture(kwin));
		QWidget window;
		mainWindow = &window;
		OBSSceneAutoRelease scene = obs_scene_create("Follow focus scene");
		currentScene = obs_scene_get_source(scene);
		kwin.add({"first", "test.app", "Editor one", true});
		kwin.add({"second", "test.app", "Editor two", false});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_string(settings, "selected_window", "first");
		obs_data_set_bool(settings, "follow_focus", true);
		obs_data_set_bool(settings, "raise_on_focus", true);
		OBSSourceAutoRelease target = createSource("Follow focus", settings);
		OBSSourceAutoRelease filler = source("Filler", "");
		auto item = obs_scene_add(scene, target);
		obs_scene_add(scene, filler);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_COMPARE(renderedPixel(target).alpha(), 255);
		int requests = kwin.captureRequests;
		obs_sceneitem_set_order_position(item, 0);
		kwin.focus("second");
		QTRY_COMPARE(kwin.captureRequests, requests + 1);
		QCOMPARE(kwin.requestedWindow, QString("second"));
		QTRY_COMPARE(obs_sceneitem_get_order_position(item), 1);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_COMPARE(renderedPixel(target).alpha(), 255);
		OBSDataAutoRelease saved = obs_save_source(target);
		OBSDataAutoRelease config = obs_data_get_obj(saved, "settings");
		QCOMPARE(QString(obs_data_get_string(config, "selected_window")), QString("first"));
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
};

int main(int argc, char **argv) { return run_obs_tests<FocusOrderTest>(argc, argv); }
#include "focus-order-test.moc"
