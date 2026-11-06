#include "pipewire-capture.hpp"
#include "video-fixture.hpp"
#include <QTest>

class PipeWireVideoTest : public QObject
{
	Q_OBJECT
      private slots:
	void pipewireNotifiesImportLifetime()
	{
		class ImportLifetime : public DmaBufCapture
		{
		      public:
			std::atomic<uint64_t> removed{0}, cleared{0};
			FrameResult copy(const spa_buffer &, const spa_video_info_raw &, VideoFrame &) override
			{
				return FrameResult::UnmappedBuffer;
			}
			void discard(const spa_buffer &) override {}
			void removeBuffer(const spa_buffer &) override { ++removed; }
			void clearImports() override { ++cleared; }
		};
		PipeWireServer server;
		QVERIFY(server.start());
		VideoProducer video(server);
		QVERIFY(video.start());
		auto gpu = std::make_shared<ImportLifetime>();
		auto frames = std::make_shared<FrameBuffer>(160, 90);
		for (int stream = 0; stream < 2; ++stream) {
			auto cleared = gpu->cleared.load();
			auto removed = gpu->removed.load();
			auto sequence = frames->sequence();
			auto capture = std::make_unique<PipeWireCapture>(
			    frames, [](QString error) { QFAIL(qPrintable(error)); }, [](PipeWireStatus) {}, gpu);
			QVERIFY(capture->start(server.openRemote(), video.nodeId(), QString::number(video.serial())));
			QTRY_VERIFY_WITH_TIMEOUT(frames->sequence() > sequence, 5000);
			QVERIFY(gpu->cleared.load() > cleared); // Format negotiation invalidates the prior pool.
			cleared = gpu->cleared.load();
			capture.reset();
			QVERIFY(gpu->removed.load() > removed);
			QVERIFY(gpu->cleared.load() > cleared);
		}
	}
};

QTEST_GUILESS_MAIN(PipeWireVideoTest)
#include "pipewire-video-test.moc"
