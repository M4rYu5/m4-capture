#include "dmabuf-fixture.hpp"
#include "live-obs-fixture.hpp"
#include "obs-graphics.hpp"
#include <QElapsedTimer>
#include <QPainter>
#include <QTimer>
#include <ctime>

class CaptureBenchmark : public LiveObsFixture
{
	Q_OBJECT
      private slots:
	void liveFrameRateProfile()
	{
		QWidget window;
		window.setWindowTitle("M4Capture frame rate check");
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
		OBSSourceAutoRelease target = source("Frame rate target", "m4capture-test");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 8000);
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
			auto start = frameCount(target);
			QElapsedTimer elapsed;
			elapsed.start();
			QTest::qWait(1500);
			const double seconds = elapsed.nsecsElapsed() / 1e9;
			const auto prepared = frameCount(target) - start;
			qInfo("FRAME_RATE_PROFILE prepared=%lld seconds=%.3f output=%u/%u fps=%.2f",
			      (long long)prepared, seconds, video.fps_num, video.fps_den, prepared / seconds);
			QVERIFY(prepared > 0);
			QCOMPARE(renderedPixel(target).alpha(), 255);
		}
	}

	void capturePerformance()
	{
		if (!qEnvironmentVariableIsSet("M4CAPTURE_BENCHMARK_WIDTH"))
			QSKIP("Opt-in benchmark captures only its own animated window.");
		const int width = qEnvironmentVariableIntValue("M4CAPTURE_BENCHMARK_WIDTH");
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
		window.setWindowTitle("M4Capture performance check");
		window.setFixedSize(width, height);
		window.show();
		QTest::qWait(200);
		window.setFixedSize(width, height);
		window.resize(width, height);
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		OBSSourceAutoRelease target = source("Performance target", "m4capture-test");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 15000);
		QCOMPARE(obs_source_get_width(target), uint32_t(width));
		QCOMPARE(obs_source_get_height(target), uint32_t(height));
		gs_texrender_t *render = nullptr;
		{
			GraphicsLock lock;
			render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		}
		auto destroyRender = qScopeGuard([&] {
			GraphicsLock lock;
			gs_texrender_destroy(render);
		});
		QVERIFY(render);
		QTimer animate;
		const int animationInterval = qEnvironmentVariableIntValue("M4CAPTURE_BENCHMARK_ANIMATION_MS");
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
			GraphicsLock lock;
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
		});
		timer.start(16);
		QTest::qWait(2000);
		auto startFrames = frameCount(target);
		auto startImports = dmabuf_import_stats();
		draws = 0;
		timespec startCpu{}, endCpu{};
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &startCpu);
		QElapsedTimer elapsed;
		elapsed.start();
		QTest::qWait(10000);
		const double seconds = elapsed.nsecsElapsed() / 1e9;
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &endCpu);
		const double cpu = endCpu.tv_sec - startCpu.tv_sec + (endCpu.tv_nsec - startCpu.tv_nsec) / 1e9;
		const auto captured = frameCount(target) - startFrames;
		auto endImports = dmabuf_import_stats();
		timer.stop();
		calldata_t diagnostic{};
		proc_handler_call(obs_source_get_proc_handler(target), "get_status", &diagnostic);
		qInfo("BENCHMARK_PATH %s", calldata_bool(&diagnostic, "gpu") ? "dmabuf" : "cpu");
		qInfo("BENCHMARK_IMPORTS created=%llu destroyed=%llu",
		      (unsigned long long)(endImports.created - startImports.created),
		      (unsigned long long)(endImports.destroyed - startImports.destroyed));
		calldata_free(&diagnostic);
		qInfo("BENCHMARK %dx%d seconds=%.3f frames=%lld capture_fps=%.2f draws=%d cpu_seconds=%.3f "
		      "cpu_percent=%.2f cpu_ms_per_frame=%.3f",
		      width, height, seconds, (long long)captured, captured / seconds, draws, cpu, cpu / seconds * 100,
		      cpu * 1000 / captured);
		QVERIFY(captured > 0);
	}
};

int main(int argc, char **argv) { return run_obs_tests<CaptureBenchmark>(argc, argv); }
#include "capture-benchmark.moc"
