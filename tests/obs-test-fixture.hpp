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

#include <vector>

// Frontend state supplied by the integration tests.
extern QWidget *mainWindow;
extern obs_source_t *currentScene;

class ObsTestFixture : public QObject
{
      protected:
	obs_source_t *createSource(const char *name, obs_data_t *settings, bool showing = true);
	QString status(obs_source_t *source);
	obs_source_t *source(const char *name, const char *app = "test.app", bool raise = false);
	QColor renderedPixel(obs_source_t *source);
	QByteArray rendererPixels(ObsVideoRenderer &renderer, const VideoFrame &frame, bool updated = true);
	void cleanupObs();

      private:
	std::vector<OBSWeakSource> shownSources;
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
	Test test;
	int result = QTest::qExec(&test, argc, argv);
	shutdown_capture();
	return result;
}
