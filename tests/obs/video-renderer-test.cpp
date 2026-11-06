#include "obs-test-fixture.hpp"
#include "pixel-fixture.hpp"
#include <algorithm>

class VideoRendererTest : public ObsTestFixture
{
	Q_OBJECT
      private slots:
	void cpuFramesRenderNatively_data()
	{
		QTest::addColumn<int>("format");
		QTest::addColumn<uint32_t>("transform");
		QTest::addColumn<bool>("crop");
		QTest::addColumn<bool>("bottomUp");
		for (auto format :
		     {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx})
			for (uint32_t transform = 0; transform < 8; ++transform)
				for (bool crop : {false, true})
					for (bool bottomUp : {false, true})
						QTest::newRow(qPrintable(QString("%1-transform%2-crop%3-bottomUp%4")
									     .arg(format)
									     .arg(transform)
									     .arg(crop)
									     .arg(bottomUp)))
						    << int(format) << transform << crop << bottomUp;
	}

	void cpuFramesRenderNatively()
	{
		QFETCH(int, format);
		QFETCH(uint32_t, transform);
		QFETCH(bool, crop);
		QFETCH(bool, bottomUp);
		PixelFixture input(5, 3);
		input.format.format = static_cast<spa_video_format>(format);
		input.transform.transform = transform;
		if (crop)
			input.crop.region = {{1, 1}, {3, 2}};
		// Mix transparent, translucent, and fully opaque pixels, plus arbitrary x bytes.
		for (size_t i = 0; i < input.bytes.size(); i += 4) {
			if (i % 12 == 0)
				std::fill_n(input.bytes.data() + i, 4, 0);
			else if (i % 12 == 4)
				input.bytes[i + 3] = 255;
		}
		constexpr uint32_t rowPadding = 7, byteOffset = 1;
		auto &padded = input.paddedRows(rowPadding, byteOffset, bottomUp);
		auto expected = reference_video_pixels(input.buffer, input.format);
		VideoFrame frame;
		QCOMPARE(copy_video_frame(input.buffer, input.format, frame), FrameResult::Ready);
		// PipeWire can immediately overwrite the input once the CPU copy is published.
		std::fill(padded.begin(), padded.end(), 0);
		FrameBuffer frames;
		QVERIFY(frames.publish(frame));
		VideoFrame output;
		QCOMPARE(frames.take(output), FrameBuffer::Update::Updated);
		ObsVideoRenderer renderer;
		QCOMPARE(rendererPixels(renderer, output), expected);
		QCOMPARE(frames.take(output), FrameBuffer::Update::Unchanged);
		QCOMPARE(rendererPixels(renderer, output, false), expected);
	}

	void cpuFormatAndStorageChanges()
	{
		ObsVideoRenderer renderer;
		VideoFrame frame;
		// All have the same displayed size; texture format/storage dimensions differ.
		for (auto format : {SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx,
				    SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA}) {
			for (uint32_t transform : {0u, 1u, 2u, 3u}) {
				PixelFixture input((transform & 1) ? 3 : 5, (transform & 1) ? 5 : 3);
				input.format.format = format;
				input.transform.transform = transform;
				QCOMPARE(copy_video_frame(input.buffer, input.format, frame), FrameResult::Ready);
				QCOMPARE(frame.width, 5u);
				QCOMPARE(frame.height, 3u);
				QCOMPARE(rendererPixels(renderer, frame),
					 reference_video_pixels(input.buffer, input.format));
			}
		}
	}
};

int main(int argc, char **argv) { return run_obs_tests<VideoRendererTest>(argc, argv); }
#include "video-renderer-test.moc"
