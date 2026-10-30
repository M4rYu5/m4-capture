#include "dmabuf-fixture.hpp"
#include "obs-dmabuf.hpp"
#include "obs-graphics.hpp"
#include "obs-test-fixture.hpp"

#include <QElapsedTimer>
#include <QPainter>
#include <QTimer>
#include <algorithm>
#include <ctime>

class LiveCaptureTest : public ObsTestFixture
{
	Q_OBJECT

	int64_t frameCount(obs_source_t *source)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		auto result = calldata_int(&params, "frames");
		calldata_free(&params);
		return result;
	}
      private slots:
	void initTestCase()
	{
		if (!qEnvironmentVariableIsSet("M4CAPTURE_LIVE_TEST"))
			QSKIP("Set M4CAPTURE_LIVE_TEST=1 to capture temporary test windows on KDE Plasma Wayland.");
		initialize_capture();
	}
	void cleanup()
	{
		cleanupObs();
		QTest::qWait(60);
	}
	void cleanupTestCase() { shutdown_capture(); }

	void liveM4Capture()
	{
		auto initialImports = dmabuf_import_stats();
		auto makeWindow = [] {
			auto w = std::make_unique<QWidget>(nullptr, Qt::FramelessWindowHint);
			w->setWindowTitle("M4Capture live check");
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
						     [](const WindowInfo &w) { return w.appId == "m4capture-test"; });
					 })(),
					 5000);
		OBSSourceAutoRelease target = source("Live target", "m4capture-test");
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 8000);
		auto pixel = renderedPixel(target);
		QCOMPARE(pixel.alpha(), 255);
		QVERIFY(pixel.red() > pixel.green());
		auto resizeImports = dmabuf_import_stats();
		window->setFixedSize(320, 180);
		QTRY_COMPARE_WITH_TIMEOUT(obs_source_get_width(target), uint32_t(320), 5000);
		QTRY_COMPARE_WITH_TIMEOUT(obs_source_get_height(target), uint32_t(180), 5000);
		if (resizeImports.created > initialImports.created)
			QTRY_VERIFY(dmabuf_import_stats().destroyed > resizeImports.destroyed);
		QCOMPARE(renderedPixel(target).alpha(), 255);
		uint32_t width = obs_source_get_width(target), height = obs_source_get_height(target);
		window.reset();
		QTRY_VERIFY_WITH_TIMEOUT(status(target) != "capturing", 3000);
		QCOMPARE(renderedPixel(target).alpha(), 0);
		QCOMPARE(obs_source_get_width(target), width);
		QCOMPARE(obs_source_get_height(target), height);
		QTRY_COMPARE(dmabuf_import_stats().created - initialImports.created,
			     dmabuf_import_stats().destroyed - initialImports.destroyed);
		window = makeWindow();
		QTRY_COMPARE_WITH_TIMEOUT(status(target), QString("capturing"), 8000);
		QCOMPARE(obs_source_get_width(target), uint32_t(160));
		QCOMPARE(obs_source_get_height(target), uint32_t(90));
		QCOMPARE(renderedPixel(target).alpha(), 255);
		window.reset();
		QTRY_VERIFY(status(target) != "capturing");
		QTRY_COMPARE(dmabuf_import_stats().created - initialImports.created,
			     dmabuf_import_stats().destroyed - initialImports.destroyed);
	}

	void liveImportFailureFallsBackToCpu()
	{
		auto gpu = create_obs_dmabuf_capture();
		if (!gpu)
			QSKIP("DMA-BUF import is unavailable.");
		class RejectImport : public DmaBufCapture
		{
		      public:
			std::shared_ptr<DmaBufCapture> delegate;
			std::atomic<int> rejected{0};
			std::atomic<bool> rejectNext{false};
			explicit RejectImport(std::shared_ptr<DmaBufCapture> gpu) : delegate(std::move(gpu))
			{
				formats = delegate->formats;
				explicitSync = delegate->explicitSync;
			}
			FrameResult copy(const spa_buffer &buffer, const spa_video_info_raw &format,
					 VideoFrame &output) override
			{
				if (!rejectNext)
					return delegate->copy(buffer, format, output);
				++rejected;
				delegate->discard(buffer);
				return FrameResult::UnmappedBuffer;
			}
			void discard(const spa_buffer &buffer) override { delegate->discard(buffer); }
			void removeBuffer(const spa_buffer &buffer) override { delegate->removeBuffer(buffer); }
			void clearImports() override { delegate->clearImports(); }
		};
		auto reject = std::make_shared<RejectImport>(gpu);
		auto initialImports = dmabuf_import_stats();
		QWidget window;
		window.setWindowTitle("M4Capture fallback check");
		window.setFixedSize(160, 90);
		window.setStyleSheet("background-color: rgb(224, 128, 32)");
		window.show();
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 5000);
		CaptureCallbacks callbacks;
		callbacks.gpu = reject;
		auto state = std::make_shared<CaptureState>(160, 90, callbacks);
		auto cleanup = qScopeGuard([&] { release_capture(state); });
		CaptureConfig config;
		config.rule.appId = "m4capture-test";
		state->configure(config);
		queue_capture_update(state);
		QTRY_COMPARE_WITH_TIMEOUT(state->frames->mode(), FrameBuffer::Mode::DmaBuf, 8000);
		QVERIFY(dmabuf_import_stats().created > initialImports.created);
		reject->rejectNext = true;
		window.setStyleSheet("background-color: rgb(32, 128, 224)");
		QTRY_COMPARE_WITH_TIMEOUT(state->frames->mode(), FrameBuffer::Mode::Cpu, 12000);
		QCOMPARE(dmabuf_import_stats().created - initialImports.created,
			 dmabuf_import_stats().destroyed - initialImports.destroyed);
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
		auto initialImports = dmabuf_import_stats();
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
			void removeBuffer(const spa_buffer &buffer) override { delegate->removeBuffer(buffer); }
			void clearImports() override { delegate->clearImports(); }
		};
		QWidget window(nullptr, Qt::FramelessWindowHint);
		window.setWindowTitle("M4Capture hidden source check");
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
		config.rule.appId = "m4capture-test";
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
		QCOMPARE(dmabuf_import_stats().created - initialImports.created,
			 dmabuf_import_stats().destroyed - initialImports.destroyed);
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
		ObsVideoRenderer renderer;
		auto pixels = rendererPixels(renderer, frame);
		QCOMPARE(pixels.size(), qsizetype(frame.width) * frame.height * 4);
		QColor pixel{uint8_t(pixels[2]), uint8_t(pixels[1]), uint8_t(pixels[0]), uint8_t(pixels[3])};
		QVERIFY(pixel.isValid());
		QCOMPARE(pixel.alpha(), 255);
		QVERIFY(pixel.green() > pixel.red());
	}

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

int main(int argc, char **argv) { return run_obs_tests<LiveCaptureTest>(argc, argv); }

#include "live-capture-test.moc"
