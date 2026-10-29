#pragma once

#include "video-frame.hpp"

#include <QByteArray>
#include <spa/buffer/meta.h>

struct PixelFixture {
	std::vector<uint8_t> bytes;
	spa_video_info_raw format{};
	spa_chunk chunk{};
	spa_data plane{};
	spa_meta_region crop{};
	spa_meta_videotransform transform{};
	spa_meta metadata[2]{{SPA_META_VideoCrop, sizeof(crop), &crop},
			     {SPA_META_VideoTransform, sizeof(transform), &transform}};
	spa_buffer buffer{};
	PixelFixture(uint32_t width = 3, uint32_t height = 2)
	{
		bytes.resize(size_t(width) * height * 4);
		for (size_t i = 0; i < bytes.size(); i += 4) {
			bytes[i] = uint8_t(1 + i / 4);
			bytes[i + 1] = 20;
			bytes[i + 2] = 30;
			bytes[i + 3] = 40;
		}
		format.format = SPA_VIDEO_FORMAT_BGRA;
		format.size = {width, height};
		chunk.size = bytes.size();
		chunk.stride = width * 4;
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

// Scalar reference for rendered BGRA pixels, independent of upload and GPU drawing.
inline QByteArray reference_video_pixels(const spa_buffer &buffer, const spa_video_info_raw &format)
{
	auto layout = video_layout(buffer, format);
	QByteArray result(size_t(layout.outputWidth()) * layout.outputHeight() * 4, Qt::Uninitialized);
	bool rgb = format.format == SPA_VIDEO_FORMAT_RGBA || format.format == SPA_VIDEO_FORMAT_RGBx;
	bool opaque = format.format == SPA_VIDEO_FORMAT_BGRx || format.format == SPA_VIDEO_FORMAT_RGBx;
	const auto &plane = buffer.datas[0];
	auto bytes = static_cast<const uint8_t *>(plane.data);
	auto stride = plane.chunk->stride ? int64_t(plane.chunk->stride) : int64_t(format.size.width) * 4;
	for (uint32_t row = 0; row < layout.height; ++row)
		for (uint32_t column = 0; column < layout.width; ++column) {
			uint32_t x = layout.flip ? layout.width - 1 - column : column;
			uint32_t dx = x, dy = row;
			if (layout.rotation == 1) {
				dx = row;
				dy = layout.width - 1 - x;
			} else if (layout.rotation == 2) {
				dx = layout.width - 1 - x;
				dy = layout.height - 1 - row;
			} else if (layout.rotation == 3) {
				dx = layout.height - 1 - row;
				dy = x;
			}
			auto pixel =
			    bytes + plane.chunk->offset + int64_t(layout.y + row) * stride + (layout.x + column) * 4;
			auto destination = result.data() + (size_t(dy) * layout.outputWidth() + dx) * 4;
			destination[0] = char(pixel[rgb ? 2 : 0]);
			destination[1] = char(pixel[1]);
			destination[2] = char(pixel[rgb ? 0 : 2]);
			destination[3] = opaque ? char(255) : char(pixel[3]);
		}
	return result;
}
