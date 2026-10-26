#include "video-frame.hpp"

#include <QTest>
#include <spa/buffer/meta.h>

namespace
{
struct FrameFixture {
	std::vector<uint8_t> bytes{1, 20, 30, 40, 2, 20, 30, 40, 3, 20, 30, 40,
				   4, 20, 30, 40, 5, 20, 30, 40, 6, 20, 30, 40};
	spa_video_info_raw format{};
	spa_chunk chunk{};
	spa_data plane{};
	spa_meta_region crop{};
	spa_meta_videotransform transform{};
	spa_meta metadata[2]{{SPA_META_VideoCrop, sizeof(crop), &crop},
			     {SPA_META_VideoTransform, sizeof(transform), &transform}};
	spa_buffer buffer{};
	FrameFixture()
	{
		format.format = SPA_VIDEO_FORMAT_BGRA;
		format.size = {3, 2};
		chunk.size = bytes.size();
		chunk.stride = 12;
		plane.type = SPA_DATA_MemPtr;
		plane.data = bytes.data();
		plane.maxsize = bytes.size();
		plane.chunk = &chunk;
		buffer.n_datas = 1;
		buffer.datas = &plane;
		buffer.n_metas = 2;
		buffer.metas = metadata;
	}
};

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
		FrameFixture input;
		input.transform.transform = transform;
		VideoFrame output;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), expected);
		QCOMPARE(output.width, (transform & 1) ? 2u : 3u);
		QCOMPARE(output.height, (transform & 1) ? 3u : 2u);
	}

	void pixelFormats_data()
	{
		QTest::addColumn<int>("format");
		QTest::addColumn<QByteArray>("expected");
		QTest::newRow("bgra") << int(SPA_VIDEO_FORMAT_BGRA) << QByteArray::fromHex("01141e28");
		QTest::newRow("bgrx") << int(SPA_VIDEO_FORMAT_BGRx) << QByteArray::fromHex("01141eff");
		QTest::newRow("rgba") << int(SPA_VIDEO_FORMAT_RGBA) << QByteArray::fromHex("1e140128");
		QTest::newRow("rgbx") << int(SPA_VIDEO_FORMAT_RGBx) << QByteArray::fromHex("1e1401ff");
	}
	void pixelFormats()
	{
		QFETCH(int, format);
		QFETCH(QByteArray, expected);
		FrameFixture input;
		input.format.format = static_cast<spa_video_format>(format);
		VideoFrame output;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(QByteArray(reinterpret_cast<const char *>(output.pixels.data()), 4), expected);
	}

	void croppingAndRotation()
	{
		FrameFixture input;
		input.crop.region = {{1, 0}, {2, 2}};
		input.transform.transform = SPA_META_TRANSFORMATION_90;
		VideoFrame output;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("03060205"));
		QCOMPARE(output.width, 2u);
		QCOMPARE(output.height, 2u);
		input.crop.region.position.x = 3; // Out-of-bounds crops are ignored.
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("030602050104"));
	}

	void negativeStride()
	{
		FrameFixture input;
		input.chunk.offset = 12;
		input.chunk.stride = -12;
		VideoFrame output;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("040506010203"));
	}

	void paddedStrideAndOffset()
	{
		FrameFixture input;
		std::vector<uint8_t> padded(36, 255);
		std::copy_n(input.bytes.data(), 12, padded.data() + 4);
		std::copy_n(input.bytes.data() + 12, 12, padded.data() + 20);
		input.plane.data = padded.data();
		input.plane.maxsize = padded.size();
		input.chunk.offset = 4;
		input.chunk.stride = 16;
		input.chunk.size = 28;
		VideoFrame output;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Ready);
		QCOMPARE(markers(output), QByteArray::fromHex("010203040506"));
	}

	void invalidAndEmptyBuffers()
	{
		FrameFixture input;
		VideoFrame output{{42}, 1, 1};
		input.chunk.size = 0;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Empty);
		input.chunk.size = 24;
		input.chunk.flags = SPA_CHUNK_FLAG_CORRUPTED;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::Empty);
		input.chunk.flags = 0;
		input.chunk.stride = 8;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.chunk.stride = -12;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.chunk.stride = 12;
		input.chunk.size = 23;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.chunk.size = 24;
		input.plane.maxsize = 23;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::InvalidLayout);
		input.plane.data = nullptr;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::UnmappedBuffer);
		input.plane.data = input.bytes.data();
		input.plane.type = SPA_DATA_DmaBuf;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::UnmappedBuffer);
		input.format.format = SPA_VIDEO_FORMAT_NV12;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::UnsupportedFormat);
		input.format.format = SPA_VIDEO_FORMAT_BGRA;
		input.format.size.width = 16385;
		QCOMPARE(convert_video_frame(input.buffer, input.format, output), FrameResult::UnsupportedFormat);
		QCOMPARE(output.pixels, std::vector<uint8_t>{42});
		QCOMPARE(output.width, 1u);
		QCOMPARE(output.height, 1u);
	}
};

QTEST_GUILESS_MAIN(VideoFrameTest)
#include "video-frame-test.moc"
