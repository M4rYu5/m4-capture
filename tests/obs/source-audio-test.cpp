#include "audio-fixture.hpp"
#include "dmabuf-fixture.hpp"
#include "simulated-obs-fixture.hpp"
#include "video-capture-fixture.hpp"
#include <cmath>

namespace
{
struct ApplicationAudioInputs {
	VideoCaptureFixture video;
	AudioProducer first, second, unrelated;
	bool start(KWinFixture &kwin)
	{
		if (!video.startCapture(kwin) || !first.start(video.server.openRemote(), "test.app") ||
		    !second.start(video.server.openRemote(), "other.app", {{"FL", -.3f}, {"FR", .4f}}) ||
		    !unrelated.start(video.server.openRemote(), "unrelated.app"))
			return false;
		kwin.add({"first", "test.app", "First", false, 12345});
		kwin.add({"second", "other.app", "Second", false, 12346});
		return true;
	}
};
} // namespace

class SourceAudioTest : public SimulatedObsFixture
{
	Q_OBJECT
      private slots:
	void audioOnlyFollowsSceneVisibilityAndTarget()
	{
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set(daemon.socketPath().toUtf8());
		AudioProducer first, second, unrelated;
		QVERIFY(first.start(daemon.openRemote(), "test.app"));
		QVERIFY(second.start(daemon.openRemote(), "other.app", {{"FL", -.3f}, {"FR", .4f}}));
		QVERIFY(unrelated.start(daemon.openRemote(), "unrelated.app"));
		kwin.add({"first", "test.app", "First", false, 12345});
		kwin.add({"second", "other.app", "Second", false, 12346});
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		OBSSourceAutoRelease target =
		    obs_source_create("m4capture_audio_source", "Audio only capture", settings, nullptr);
		QVERIFY(target);
		ObsAudioRecorder samples(target);
		OBSSceneAutoRelease scene = obs_scene_create("Audio only scene");
		auto sceneSource = obs_scene_get_source(scene);
		auto item = obs_scene_add(scene, target);
		QTRY_COMPARE(status(target), QString("paused"));
		int requests = kwin.captureRequests;
		const auto imports = dmabuf_import_stats().created;
		obs_set_output_source(0, sceneSource);
		auto deactivate = qScopeGuard([&] { obs_set_output_source(0, nullptr); });
		QTRY_VERIFY_WITH_TIMEOUT(samples.count.load() > 0, 5000);
		QTRY_VERIFY(std::abs(samples.left.load() - .125f) < .0001f);
		QVERIFY(std::abs(samples.right.load() + .25f) < .0001f);
		QCOMPARE(status(target), QString("capturing"));
		obs_sceneitem_set_visible(item, false);
		renderedPixel(sceneSource);
		QTRY_COMPARE(status(target), QString("paused"));
		auto count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		obs_sceneitem_set_visible(item, true);
		renderedPixel(sceneSource);
		QTRY_VERIFY(samples.count.load() > count);
		obs_data_set_string(settings, "app_id", "other.app");
		obs_source_update(target, settings);
		QTRY_VERIFY(std::abs(samples.left.load() + .3f) < .0001f);
		QVERIFY(std::abs(samples.right.load() - .4f) < .0001f);
		obs_data_set_string(settings, "title_filter", "Second");
		obs_data_set_bool(settings, "disconnect_on_title_mismatch", true);
		obs_source_update(target, settings);
		kwin.title("second", "No longer matches");
		QTRY_COMPARE(status(target), QString("waiting"));
		count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		kwin.title("second", "Second again");
		QTRY_VERIFY(samples.count.load() > count);
		QCOMPARE(kwin.captureRequests, requests);
		QCOMPARE(dmabuf_import_stats().created, imports);
	}

	void audioIsDisabledByDefault()
	{
		ApplicationAudioInputs inputs;
		QVERIFY(inputs.start(kwin));
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		QVERIFY(target);
		ObsSourceVisibility visibility(target);
		ObsAudioRecorder samples(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), uint64_t(0));
	}

	void audioConnectionFailurePreservesVideo()
	{
		ApplicationAudioInputs inputs;
		QVERIFY(inputs.start(kwin));
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		QVERIFY(target);
		ObsSourceVisibility visibility(target);
		ObsAudioRecorder samples(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		const int requests = kwin.captureRequests;
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set("/nonexistent/m4capture-audio-test");
		obs_data_set_bool(settings, "capture_audio", true);
		obs_source_update(target, settings);
		QTest::qWait(200);
		QCOMPARE(samples.count.load(), uint64_t(0));
		QCOMPARE(status(target), QString("capturing"));
		QCOMPARE(kwin.captureRequests, requests);
		remote.set(inputs.video.server.socketPath().toUtf8());
		QTRY_VERIFY_WITH_TIMEOUT(samples.matches(.125f, -.25f), 5000);
		QCOMPARE(kwin.captureRequests, requests);
	}

	void audioSettingControlsDelivery()
	{
		ApplicationAudioInputs inputs;
		QVERIFY(inputs.start(kwin));
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		QVERIFY(target);
		ObsSourceVisibility visibility(target);
		ObsAudioRecorder samples(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(samples.matches(.125f, -.25f), 5000);
		const int requests = kwin.captureRequests;
		obs_data_set_bool(settings, "capture_audio", false);
		obs_source_update(target, settings);
		QTest::qWait(100); // Allow already queued callbacks to drain.
		const auto count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		QCOMPARE(kwin.captureRequests, requests);
		obs_data_set_bool(settings, "capture_audio", true);
		obs_source_update(target, settings);
		QTRY_VERIFY(samples.count.load() > count);
		QCOMPARE(kwin.captureRequests, requests);
	}

	void hiddenSourceStopsAudio()
	{
		ApplicationAudioInputs inputs;
		QVERIFY(inputs.start(kwin));
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		QVERIFY(target);
		ObsSourceVisibility visibility(target);
		ObsAudioRecorder samples(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(samples.matches(.125f, -.25f), 5000);
		visibility.setShowing(false);
		QTRY_COMPARE(status(target), QString("paused"));
		const auto count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		visibility.setShowing(true);
		QTRY_VERIFY(samples.count.load() > count);
	}

	void changingTargetSwitchesAudio()
	{
		ApplicationAudioInputs inputs;
		QVERIFY(inputs.start(kwin));
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "test.app");
		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		QVERIFY(target);
		ObsSourceVisibility visibility(target);
		ObsAudioRecorder samples(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(samples.matches(.125f, -.25f), 5000);
		obs_data_set_string(settings, "app_id", "other.app");
		obs_source_update(target, settings);
		QTRY_VERIFY2(samples.matches(-.3f, .4f), qPrintable(QString("left=%1 right=%2 count=%3 status=%4")
									.arg(samples.left.load())
									.arg(samples.right.load())
									.arg(samples.count.load())
									.arg(status(target))));
	}

	void reopenedTargetResumesAudio()
	{
		ApplicationAudioInputs inputs;
		QVERIFY(inputs.start(kwin));
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "app_id", "other.app");
		obs_data_set_bool(settings, "capture_audio", true);
		OBSSourceAutoRelease target = createSource("Application audio", settings, false);
		QVERIFY(target);
		ObsSourceVisibility visibility(target);
		ObsAudioRecorder samples(target);
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 5000);
		QTRY_VERIFY_WITH_TIMEOUT(samples.matches(-.3f, .4f), 5000);
		kwin.remove("second");
		QTRY_COMPARE(status(target), QString("waiting"));
		const auto count = samples.count.load();
		QTest::qWait(100);
		QCOMPARE(samples.count.load(), count);
		kwin.add({"reopened", "other.app", "Reopened", false, 12347});
		QTRY_VERIFY(samples.count.load() > count);
		QTRY_VERIFY(samples.matches(-.3f, .4f));
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
};

int main(int argc, char **argv) { return run_obs_tests<SourceAudioTest>(argc, argv); }
#include "source-audio-test.moc"
