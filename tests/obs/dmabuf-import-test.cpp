#include "dmabuf-fixture.hpp"
#include "obs-dmabuf.hpp"
#include "obs-graphics.hpp"
#include "obs-test-fixture.hpp"
#include "pixel-fixture.hpp"

class DmaBufImportTest : public ObsTestFixture
{
	Q_OBJECT
	QByteArray gpuPixels(const VideoFrame &frame)
	{
		GraphicsLock lock;
		auto stage = gs_stagesurface_create(frame.width, frame.height, GS_BGRA);
		QByteArray pixels;
		if (stage) {
			gs_stage_texture(stage, obs_frame_texture(frame));
			uint8_t *bytes = nullptr;
			uint32_t stride = 0;
			if (gs_stagesurface_map(stage, &bytes, &stride)) {
				for (uint32_t y = 0; y < frame.height; ++y)
					pixels.append(reinterpret_cast<char *>(bytes + y * stride), frame.width * 4);
				gs_stagesurface_unmap(stage);
			}
			gs_stagesurface_destroy(stage);
		}
		return pixels;
	}
      private slots:
	void gpuTransformsMatchCpu_data()
	{
		QTest::addColumn<uint32_t>("transform");
		QTest::addColumn<bool>("crop");
		QTest::addColumn<bool>("opaque");
		for (uint32_t transform = 0; transform < 8; ++transform)
			for (bool crop : {false, true})
				for (bool opaque : {false, true})
					QTest::newRow(qPrintable(
					    QString("%1-crop%2-opaque%3").arg(transform).arg(crop).arg(opaque)))
					    << transform << crop << opaque;
	}

	void gpuTransformsMatchCpu()
	{
		QFETCH(uint32_t, transform);
		QFETCH(bool, crop);
		QFETCH(bool, opaque);
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf input;
		if (!input.buffer.n_datas || !supported_video_format(input.format))
			QSKIP("Driver cannot export the test texture as a supported DMA-BUF.");
		input.transform.transform = transform;
		if (crop)
			input.crop.region = {{1, 2}, {13, 7}};
		if (opaque)
			input.format.format = input.format.format == SPA_VIDEO_FORMAT_BGRA ? SPA_VIDEO_FORMAT_BGRx
											   : SPA_VIDEO_FORMAT_RGBx;
		spa_chunk chunk{};
		chunk.size = input.pixels.size();
		chunk.stride = 16 * 4;
		spa_data plane{};
		plane.type = SPA_DATA_MemPtr;
		plane.data = input.pixels.data();
		plane.maxsize = input.pixels.size();
		plane.chunk = &chunk;
		auto buffer = input.buffer;
		buffer.n_datas = 1;
		buffer.datas = &plane;
		auto format = input.format;
		format.format = opaque ? SPA_VIDEO_FORMAT_BGRx : SPA_VIDEO_FORMAT_BGRA;
		const VideoLayout expectedLayout{crop ? 1u : 0u,  crop ? 2u : 0u, crop ? 13u : 16u,
						 crop ? 7u : 12u, transform & 3,  bool(transform & 4)};
		auto expected = reference_video_pixels(buffer, format, expectedLayout);
		VideoFrame actual;
		QCOMPARE(capture->copy(input.buffer, input.format, actual), FrameResult::Ready);
		QCOMPARE(actual.width, expectedLayout.outputWidth());
		QCOMPARE(actual.height, expectedLayout.outputHeight());
		QVERIFY(actual.pixels.empty());
		QCOMPARE(gpuPixels(actual), expected);
	}

	void rendererSwitchesBetweenCpuAndDmaBuf()
	{
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf input;
		if (!input.buffer.n_datas || !supported_video_format(input.format))
			QSKIP("Driver cannot export the test texture as a supported DMA-BUF.");
		ObsVideoRenderer renderer;
		VideoFrame frame;
		for (int round = 0; round < 2; ++round) {
			QCOMPARE(capture->copy(input.buffer, input.format, frame), FrameResult::Ready);
			QCOMPARE(rendererPixels(renderer, frame), gpuPixels(frame));
			PixelFixture cpu(16, 16);
			cpu.format.format = SPA_VIDEO_FORMAT_RGBx;
			cpu.transform.transform = SPA_META_TRANSFORMATION_Flipped90;
			QCOMPARE(copy_video_frame(cpu.buffer, cpu.format, frame), FrameResult::Ready);
			QVERIFY(!frame.gpu);
			const VideoLayout expectedLayout{0, 0, 16, 16, 1, true};
			QCOMPARE(rendererPixels(renderer, frame),
				 reference_video_pixels(cpu.buffer, cpu.format, expectedLayout));
		}
	}

	void gpuImportCacheFollowsBufferLifetime()
	{
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf first, second;
		if (!first.buffer.n_datas || !second.buffer.n_datas || !supported_video_format(first.format) ||
		    !supported_video_format(second.format))
			QSKIP("Driver cannot export the test textures as supported DMA-BUFs.");
		auto before = dmabuf_import_stats();
		VideoFrame frame;
		for (int round = 0; round < 8; ++round) {
			for (auto input : {&first, &second}) {
				for (size_t i = 0; i < input->pixels.size(); i += 4) {
					input->pixels[i] = 20 + round * 20;
					input->pixels[i + 1] = input == &first ? 80 : 160;
					input->pixels[i + 2] = 230 - round * 20;
					input->pixels[i + 3] = 255;
				}
				input->upload();
				QCOMPARE(capture->copy(input->buffer, input->format, frame), FrameResult::Ready);
				QCOMPARE(gpuPixels(frame),
					 QByteArray(reinterpret_cast<const char *>(input->pixels.data()),
						    input->pixels.size()));
			}
		}
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(0));
		capture->removeBuffer(first.buffer);
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(1));
		QCOMPARE(capture->copy(second.buffer, second.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		// The same address and fds can identify a new buffer lifetime after removal.
		QCOMPARE(capture->copy(first.buffer, first.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(3));
		capture->clearImports();
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(3));
		QCOMPARE(capture->copy(second.buffer, second.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(4));
		capture.reset();
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(4));
	}

	void gpuImportCacheInvalidatesChangedLayout()
	{
		auto capture = create_obs_dmabuf_capture();
		if (!capture)
			QSKIP("GPU DMA-BUF import unavailable on this graphics backend.");
		TestDmaBuf input, replacement;
		if (!input.buffer.n_datas || !replacement.buffer.n_datas || !supported_video_format(input.format) ||
		    !supported_video_format(replacement.format))
			QSKIP("Driver cannot export the test textures as supported DMA-BUFs.");
		auto before = dmabuf_import_stats();
		VideoFrame frame;
		QCOMPARE(capture->copy(input.buffer, input.format, frame), FrameResult::Ready);
		auto opaque = input.format;
		opaque.format = opaque.format == SPA_VIDEO_FORMAT_BGRA ? SPA_VIDEO_FORMAT_BGRx : SPA_VIDEO_FORMAT_RGBx;
		QCOMPARE(capture->copy(input.buffer, opaque, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(1));
		// Crop/rotation change the draw, not the underlying image import.
		input.crop.region = {{1, 2}, {13, 7}};
		input.transform.transform = SPA_META_TRANSFORMATION_90;
		QCOMPARE(capture->copy(input.buffer, opaque, frame), FrameResult::Ready);
		QCOMPARE(frame.width, uint32_t(7));
		QCOMPARE(frame.height, uint32_t(13));
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(2));
		// Different plane storage at the same buffer address must replace the import.
		input.buffer.n_datas = replacement.buffer.n_datas;
		input.buffer.datas = replacement.buffer.datas;
		QCOMPARE(capture->copy(input.buffer, replacement.format, frame), FrameResult::Ready);
		QCOMPARE(dmabuf_import_stats().created - before.created, uint64_t(3));
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(2));
		capture->clearImports();
		QCOMPARE(dmabuf_import_stats().destroyed - before.destroyed, uint64_t(3));
	}
};

int main(int argc, char **argv) { return run_obs_tests<DmaBufImportTest>(argc, argv); }
#include "dmabuf-import-test.moc"
