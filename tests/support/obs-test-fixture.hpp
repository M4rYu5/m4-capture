#pragma once

#include "obs-capture.hpp"
#include "obs-video.hpp"

#include <QApplication>
#include <QColor>
#include <QScopeGuard>
#include <QTest>
#include <QWidget>
#include <obs-nix-platform.h>
#include <wayland-client.h>

#include <atomic>
#include <cmath>
#include <vector>

// Frontend state supplied by the integration tests.
extern QWidget *mainWindow;
extern obs_source_t *currentScene;

// Finish OBS source destruction and its queued Qt capture releases while graphics is alive.
void drain_obs_test_tasks();

class ObsSourceVisibility
{
      public:
	explicit ObsSourceVisibility(obs_source_t *source) : source(source) { setShowing(true); }
	~ObsSourceVisibility() { setShowing(false); }
	ObsSourceVisibility(const ObsSourceVisibility &) = delete;
	ObsSourceVisibility &operator=(const ObsSourceVisibility &) = delete;
	void setShowing(bool next)
	{
		if (next == showing)
			return;
		if (next)
			obs_source_inc_showing(source);
		else
			obs_source_dec_showing(source);
		showing = next;
	}

      private:
	OBSSource source;
	bool showing = false;
};

// Records OBS audio delivery and unregisters its callback before releasing the source.
class ObsAudioRecorder
{
      public:
	explicit ObsAudioRecorder(obs_source_t *source);
	~ObsAudioRecorder();
	ObsAudioRecorder(const ObsAudioRecorder &) = delete;
	ObsAudioRecorder &operator=(const ObsAudioRecorder &) = delete;
	bool matches(float expectedLeft, float expectedRight) const
	{
		return count.load() > 0 && std::abs(left.load() - expectedLeft) < .0001f &&
		       std::abs(right.load() - expectedRight) < .0001f;
	}
	std::atomic<uint64_t> count{0};
	std::atomic<float> left{0}, right{0};

      private:
	OBSSource source;
	static void receive(void *data, obs_source_t *, const audio_data *audio, bool);
};

class ObsTestFixture : public QObject
{
	Q_OBJECT
      protected:
	obs_source_t *createSource(const char *name, obs_data_t *settings, bool showing = true,
				   const char *id = "m4capture_source");
	QString status(obs_source_t *source);
	obs_source_t *source(const char *name, const char *app = "test.app", bool raise = false);
	QColor renderedPixel(obs_source_t *source);
	QByteArray rendererPixels(ObsVideoRenderer &renderer, const VideoFrame &frame, bool updated = true);
	void cleanupObs();

      private:
	std::vector<OBSWeakSource> shownSources;

      private slots:
	void cleanup() { cleanupObs(); }
};

template <typename Test> int run_obs_tests(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setDesktopFileName("m4capture-test");
	auto display = wl_display_connect(nullptr);
	if (!display) {
		qCritical("Tests need a Wayland session for libobs graphics.");
		return 1;
	}
	auto disconnectDisplay = qScopeGuard([&] { wl_display_disconnect(display); });
	obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
	obs_set_nix_platform_display(display);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;
	auto shutdownObs = qScopeGuard([] { obs_shutdown(); });
	obs_video_info video{};
	video.graphics_module = "libobs-opengl";
	video.fps_num = qEnvironmentVariableIntValue("M4CAPTURE_TEST_FPS_NUM");
	video.fps_den = qEnvironmentVariableIntValue("M4CAPTURE_TEST_FPS_DEN");
	if (!video.fps_num)
		video.fps_num = qEnvironmentVariableIsSet("M4CAPTURE_BENCHMARK_WIDTH") ? 60 : 30;
	if (!video.fps_den)
		video.fps_den = 1;
	video.base_width = video.output_width = 320;
	video.base_height = video.output_height = 240;
	video.output_format = VIDEO_FORMAT_RGBA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
		return 1;
	obs_audio_info audio{};
	audio.samples_per_sec = 48000;
	audio.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&audio))
		return 1;
	register_m4capture_source();
	register_m4capture_desktop_source();
	register_m4capture_audio_source();
	Test test;
	int result = QTest::qExec(&test, argc, argv);
	drain_obs_test_tasks();
	shutdown_capture();
	return result;
}
