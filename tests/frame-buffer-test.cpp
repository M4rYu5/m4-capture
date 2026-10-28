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
		VideoFrame input{{1, 2, 3, 4}, 1, 1};
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
		VideoFrame input{{1, 2, 3, 4}, 1, 1};
		QVERIFY(buffer.publish(input));
		VideoFrame output;
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Updated);
		input = {{5, 6, 7, 8, 9, 10, 11, 12}, 2, 1};
		QVERIFY(buffer.publish(input));
		buffer.clear();
		QCOMPARE(buffer.take(output), FrameBuffer::Update::Empty);
		QCOMPARE(buffer.size().width, 2u);
		QCOMPARE(buffer.size().height, 1u);
		input = {{13, 14, 15, 16, 17, 18, 19, 20}, 2, 1};
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
};

QTEST_GUILESS_MAIN(FrameBufferTest)
#include "frame-buffer-test.moc"
