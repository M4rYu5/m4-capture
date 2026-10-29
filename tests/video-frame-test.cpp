#include "pixel-fixture.hpp"

#include <QTest>
#include <spa/buffer/meta.h>

namespace
{
QByteArray markers(const VideoFrame &frame)
{
	QByteArray result;
	for (size_t i = 0; i < frame.pixels.size(); i += 4)
		result.append(char(frame.pixels[i]));
	return result;
}
} // namespace

class VideoFrameTest : public QObject
{
	Q_OBJECT
      private slots:
	void transforms_data()
	{
		QTest::addColumn<uint32_t>("transform");
		QTest::addColumn<QByteArray>("expected");
		QTest::newRow("none") << 0u << QByteArray::fromHex("010203040506");
		QTest::newRow("90") << 1u << QByteArray::fromHex("030602050104");
		QTest::newRow("180") << 2u << QByteArray::fromHex("060504030201");
		QTest::newRow("270") << 3u << QByteArray::fromHex("040105020603");
		QTest::newRow("flip") << 4u << QByteArray::fromHex("030201060504");
		QTest::newRow("flip90") << 5u << QByteArray::fromHex("010402050306");
		QTest::newRow("flip180") << 6u << QByteArray::fromHex("040506010203");
		QTest::newRow("flip270") << 7u << QByteArray::fromHex("060305020401");
	}
	void transforms()
	{
		QFETCH(uint32_t, transform);
		QFETCH(QByteArray, expected);
		PixelFixture input;
		input.transform.transform = transform;
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("010203040506"));
		QCOMPARE(output.layout.width, 3u);
		QCOMPARE(output.layout.height, 2u);
		QCOMPARE(output.layout.rotation, transform & 3);
		QCOMPARE(output.layout.flip, bool(transform & 4));
		// The scalar reference also checks the expected display ordering.
		auto displayed = reference_video_pixels(input.buffer, input.format);
		QByteArray displayedMarkers;
		for (qsizetype i = 0; i < displayed.size(); i += 4)
			displayedMarkers.append(displayed[i]);
		QCOMPARE(displayedMarkers, expected);
		QCOMPARE(output.width, (transform & 1) ? 2u : 3u);
		QCOMPARE(output.height, (transform & 1) ? 3u : 2u);
	}

	void pixelFormats_data()
	{
		QTest::addColumn<int>("format");
		QTest::addColumn<QByteArray>("expected");
		QTest::newRow("bgra") << int(SPA_VIDEO_FORMAT_BGRA) << QByteArray::fromHex("01141e28");
		QTest::newRow("bgrx") << int(SPA_VIDEO_FORMAT_BGRx) << QByteArray::fromHex("01141e28");
		QTest::newRow("rgba") << int(SPA_VIDEO_FORMAT_RGBA) << QByteArray::fromHex("01141e28");
		QTest::newRow("rgbx") << int(SPA_VIDEO_FORMAT_RGBx) << QByteArray::fromHex("01141e28");
	}
	void pixelFormats()
	{
		QFETCH(int, format);
		QFETCH(QByteArray, expected);
		PixelFixture input;
		input.format.format = static_cast<spa_video_format>(format);
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(QByteArray(reinterpret_cast<const char *>(output.pixels.data()), 4), expected);
		QCOMPARE(int(output.format), format);
	}

	void croppingAndRotation()
	{
		PixelFixture input;
		input.crop.region = {{1, 0}, {2, 2}};
		input.transform.transform = SPA_META_TRANSFORMATION_90;
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("02030506"));
		QCOMPARE(output.width, 2u);
		QCOMPARE(output.height, 2u);
		input.crop.region.position.x = 3; // Out-of-bounds crops are ignored.
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("010203040506"));
	}

	void fullWidthCropWithOffset()
	{
		PixelFixture input(3, 4);
		std::vector<uint8_t> offsetBytes(input.bytes.size() + 1, 255);
		std::copy(input.bytes.begin(), input.bytes.end(), offsetBytes.begin() + 1);
		input.plane.data = offsetBytes.data();
		input.plane.maxsize = offsetBytes.size();
		input.chunk.offset = 1;
		input.crop.region = {{0, 1}, {3, 2}};
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(output.pixels, std::vector<uint8_t>(input.bytes.begin() + 12, input.bytes.begin() + 36));
		QCOMPARE(output.width, 3u);
		QCOMPARE(output.height, 2u);
	}

	void negativeStride()
	{
		PixelFixture input;
		input.chunk.offset = 12;
		input.chunk.stride = -12;
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("040506010203"));
	}

	void paddedStrideAndOffset()
	{
		PixelFixture input;
		std::vector<uint8_t> padded(36, 255);
		std::copy_n(input.bytes.data(), 12, padded.data() + 4);
		std::copy_n(input.bytes.data() + 12, 12, padded.data() + 20);
		input.plane.data = padded.data();
		input.plane.maxsize = padded.size();
		input.chunk.offset = 4;
		input.chunk.stride = 16;
		input.chunk.size = 28;
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("010203040506"));
	}

	void opaqueCroppedRows_data()
	{
		QTest::addColumn<bool>("bottomUp");
		QTest::newRow("positive-stride") << false;
		QTest::newRow("negative-stride") << true;
	}

	void opaqueCroppedRows()
	{
		QFETCH(bool, bottomUp);
		PixelFixture input;
		input.format.format = SPA_VIDEO_FORMAT_BGRx;
		// Offset each row by one byte to exercise unaligned, padded input.
		std::vector<uint8_t> padded(30, 0);
		std::copy_n(input.bytes.data(), 12, padded.data() + 1);
		std::copy_n(input.bytes.data() + 12, 12, padded.data() + 17);
		input.plane.data = padded.data();
		input.plane.maxsize = padded.size();
		input.chunk.offset = bottomUp ? 17 : 1;
		input.chunk.stride = bottomUp ? -16 : 16;
		input.chunk.size = 28;
		input.crop.region = {{1, 0}, {2, 2}};
		VideoFrame output;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex(bottomUp ? "05060203" : "02030506"));
		QCOMPARE(output.width, 2u);
		QCOMPARE(output.height, 2u);
		for (size_t i = 0; i < output.pixels.size(); i += 4) {
			QCOMPARE(output.pixels[i + 1], uint8_t(20));
			QCOMPARE(output.pixels[i + 2], uint8_t(30));
			QCOMPARE(output.pixels[i + 3], uint8_t(40));
		}
	}

	void invalidAndEmptyBuffers()
	{
		PixelFixture input;
		VideoFrame output{{42}, 1, 1};
		input.chunk.size = 0;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Empty);
		input.chunk.size = 24;
		input.chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::Empty);
		input.chunk.flags = 0;
		input.chunk.stride = 8;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.chunk.stride = -12;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.chunk.stride = 12;
		input.chunk.size = 23;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.chunk.size = 24;
		input.plane.maxsize = 23;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.plane.data = nullptr;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::UnmappedBuffer);
		input.plane.data = input.bytes.data();
		input.plane.type = SPA_DATA_DmaBuf;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::UnmappedBuffer);
		input.format.format = SPA_VIDEO_FORMAT_NV12;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::UnsupportedFormat);
		input.format.format = SPA_VIDEO_FORMAT_BGRA;
		input.format.size.width = 16385;
		QCOMPARE(copy_video_frame(input.buffer, input.format, output), FrameResult::UnsupportedFormat);
		QCOMPARE(output.pixels, std::vector<uint8_t>{42});
		QCOMPARE(output.width, 1u);
		QCOMPARE(output.height, 1u);
	}
};

QTEST_GUILESS_MAIN(VideoFrameTest)
#include "video-frame-test.moc"
