#include "frame-buffer.hpp"

#include <QTest>

class FrameBufferTest : public QObject
{
	Q_OBJECT
      private slots:
	void emptyBufferKeepsSavedDimensions()
	{
		FrameBuffer buffer(640, 480);
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Empty);
		QCOMPARE(buffer.size().width, 640u);
		QCOMPARE(buffer.size().height, 480u);
	}

	void latestFrameIsConsumedOnce()
	{
		FrameBuffer buffer;
		VideoFrame input{{1, 2, 3, 4}, 1, 1, {}};
		QVERIFY(buffer.publish(input));
		input.pixels = {5, 6, 7, 8};
		QVERIFY(!buffer.publish(input));
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QCOMPARE(output.pixels, (std::vector<uint8_t>{5, 6, 7, 8}));
		QCOMPARE(output.width, 1u);
		QCOMPARE(output.height, 1u);
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Unchanged);
		QCOMPARE(output.pixels, (std::vector<uint8_t>{5, 6, 7, 8}));
	}

	void resizeAndClearNotifyNextFrame()
	{
		FrameBuffer buffer;
		VideoFrame input{{1, 2, 3, 4}, 1, 1, {}};
		QVERIFY(buffer.publish(input));
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		input = {{5, 6, 7, 8, 9, 10, 11, 12}, 2, 1, {}};
		QVERIFY(buffer.publish(input));
		buffer.clear();
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Empty);
		QCOMPARE(buffer.size().width, 2u);
		QCOMPARE(buffer.size().height, 1u);
		input = {{13, 14, 15, 16, 17, 18, 19, 20}, 2, 1, {}};
		QVERIFY(buffer.publish(input));
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QCOMPARE(output.width, 2u);
		QCOMPARE(output.pixels, (std::vector<uint8_t>{13, 14, 15, 16, 17, 18, 19, 20}));
	}
	void gpuHandoffAndFallback()
	{
		FrameBuffer buffer;
		VideoFrame input{{}, 2, 3, std::make_shared<GpuFrame>()};
		auto texture = input.gpu;
		QVERIFY(buffer.publish(input));
		QCOMPARE(buffer.mode(), FrameBuffer::Mode::DmaBuf);
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QCOMPARE(output.gpu, texture);
		QCOMPARE(buffer.mode(), FrameBuffer::Mode::DmaBuf);
		input = {{1, 2, 3, 4}, 2, 3, {}};
		QVERIFY(buffer.publish(input));
		QCOMPARE(buffer.mode(), FrameBuffer::Mode::Cpu);
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QVERIFY(!output.gpu);
		buffer.clear();
		QCOMPARE(buffer.mode(), FrameBuffer::Mode::None);
	}

	void nativeMetadataFollowsPixels()
	{
		FrameBuffer buffer;
		VideoFrame input{{1, 2, 3, 4}, 2, 3, {}};
		input.layout = {0, 0, 3, 2, 1, true};
		input.format = SPA_VIDEO_FORMAT_RGBx;
		QVERIFY(buffer.publish(input));
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QCOMPARE(output.layout.width, 3u);
		QCOMPARE(output.layout.height, 2u);
		QCOMPARE(output.layout.rotation, 1u);
		QVERIFY(output.layout.flip);
		QCOMPARE(output.format, SPA_VIDEO_FORMAT_RGBx);
		// A same-size format/transform change must replace the whole frame.
		input.pixels = {5, 6, 7, 8};
		input.layout = {0, 0, 2, 3};
		input.format = SPA_VIDEO_FORMAT_BGRA;
		QVERIFY(!buffer.publish(input));
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QCOMPARE(output.pixels, (std::vector<uint8_t>{5, 6, 7, 8}));
		QCOMPARE(output.layout.width, 2u);
		QCOMPARE(output.layout.height, 3u);
		QCOMPARE(output.layout.rotation, 0u);
		QVERIFY(!output.layout.flip);
		QCOMPARE(output.format, SPA_VIDEO_FORMAT_BGRA);
	}

	void disabledBufferRejectsInFlightFrames()
	{
		FrameBuffer buffer;
		VideoFrame input{{1, 2, 3, 4}, 1, 1, {}};
		QVERIFY(buffer.publish(input));
		auto sequence = buffer.sequence();
		buffer.setEnabled(false);
		input = {{5, 6, 7, 8}, 1, 1, {}};
		QVERIFY(!buffer.publish(input));
		QCOMPARE(buffer.sequence(), sequence);
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Empty);
		QCOMPARE(buffer.mode(), FrameBuffer::Mode::None);
		buffer.setEnabled(true);
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Empty);
		QVERIFY(buffer.publish(input));
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		QCOMPARE(output.pixels, (std::vector<uint8_t>{5, 6, 7, 8}));
	}
};

QTEST_GUILESS_MAIN(FrameBufferTest)
#include "frame-buffer-test.moc"
