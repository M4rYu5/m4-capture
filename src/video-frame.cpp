#include "video-frame.hpp"

#include <spa/buffer/meta.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

bool supported_video_format(const spa_video_info_raw &format)
{
	return format.size.width && format.size.height && format.size.width <= 16384 && format.size.height <= 16384 &&
	       uint64_t(format.size.width) * format.size.height <= 64 * 1024 * 1024 &&
	       (format.format == SPA_VIDEO_FORMAT_BGRA || format.format == SPA_VIDEO_FORMAT_BGRx ||
		format.format == SPA_VIDEO_FORMAT_RGBA || format.format == SPA_VIDEO_FORMAT_RGBx);
}

static bool validLayout(const spa_data &plane, const spa_video_info_raw &format, int64_t stride)
{
	int64_t first = plane.chunk->offset;
	int64_t last = first + int64_t(format.size.height - 1) * stride;
	uint64_t end = std::max(first, last) + uint64_t(format.size.width) * 4;
	return std::min(first, last) >= 0 && end <= plane.maxsize &&
	       uint64_t(std::abs(stride)) >= uint64_t(format.size.width) * 4 &&
	       end - std::min(first, last) <= plane.chunk->size;
}

FrameResult convert_video_frame(const spa_buffer &buffer, const spa_video_info_raw &format, VideoFrame &output)
{
	if (!supported_video_format(format))
		return FrameResult::UnsupportedFormat;
	if (!buffer.n_datas)
		return FrameResult::Empty;
	const auto &plane = buffer.datas[0];
	if (!plane.data || !plane.chunk || plane.type == SPA_DATA_DmaBuf)
		return FrameResult::UnmappedBuffer;
	if (!plane.chunk->size || (plane.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED))
		return FrameResult::Empty;
	int64_t stride = plane.chunk->stride ? plane.chunk->stride : int64_t(format.size.width) * 4;
	if (!validLayout(plane, format, stride))
		return FrameResult::InvalidLayout;

	uint32_t x = 0, y = 0, width = format.size.width, height = format.size.height;
	auto crop = static_cast<const spa_meta_region *>(
	    spa_buffer_find_meta_data(&buffer, SPA_META_VideoCrop, sizeof(spa_meta_region)));
	if (crop && spa_meta_region_is_valid(crop) && crop->region.position.x >= 0 && crop->region.position.y >= 0 &&
	    uint64_t(crop->region.position.x) + crop->region.size.width <= width &&
	    uint64_t(crop->region.position.y) + crop->region.size.height <= height) {
		x = crop->region.position.x;
		y = crop->region.position.y;
		width = crop->region.size.width;
		height = crop->region.size.height;
	}
	auto transform = static_cast<const spa_meta_videotransform *>(
	    spa_buffer_find_meta_data(&buffer, SPA_META_VideoTransform, sizeof(spa_meta_videotransform)));
	uint32_t rotation = transform ? (transform->transform & 3) : 0;
	bool flip = transform && (transform->transform & 4);
	output.width = (rotation & 1) ? height : width;
	output.height = (rotation & 1) ? width : height;
	output.pixels.resize(size_t(output.width) * output.height * 4);
	bool rgb = format.format == SPA_VIDEO_FORMAT_RGBA || format.format == SPA_VIDEO_FORMAT_RGBx;
	bool opaque = format.format == SPA_VIDEO_FORMAT_BGRx || format.format == SPA_VIDEO_FORMAT_RGBx;
	auto bytes = static_cast<const uint8_t *>(plane.data);
	for (uint32_t row = 0; row < height; ++row) {
		auto input = bytes + plane.chunk->offset + int64_t(y + row) * stride + x * 4;
		if (!rotation && !flip && !rgb) {
			auto destination = output.pixels.data() + size_t(row) * width * 4;
			std::memcpy(destination, input, size_t(width) * 4);
			if (opaque)
				for (size_t alpha = 3; alpha < size_t(width) * 4; alpha += 4)
					destination[alpha] = 255;
			continue;
		}
		for (uint32_t column = 0; column < width; ++column) {
			uint32_t cx = flip ? width - 1 - column : column;
			uint32_t dx = cx, dy = row;
			if (rotation == 1) {
				dx = row;
				dy = width - 1 - cx;
			} else if (rotation == 2) {
				dx = width - 1 - cx;
				dy = height - 1 - row;
			} else if (rotation == 3) {
				dx = height - 1 - row;
				dy = cx;
			}
			auto pixel = input + column * 4;
			auto destination = output.pixels.data() + (size_t(dy) * output.width + dx) * 4;
			destination[0] = pixel[rgb ? 2 : 0];
			destination[1] = pixel[1];
			destination[2] = pixel[rgb ? 0 : 2];
			destination[3] = opaque ? 255 : pixel[3];
		}
	}
	return FrameResult::Ready;
}
