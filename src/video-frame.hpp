#pragma once

#include <spa/buffer/buffer.h>
#include <spa/param/video/raw.h>

#include <cstdint>
#include <vector>

struct VideoFrame {
	std::vector<uint8_t> pixels;
	uint32_t width = 0;
	uint32_t height = 0;
};

enum class FrameResult { Ready, Empty, UnsupportedFormat, UnmappedBuffer, InvalidLayout };

bool supported_video_format(const spa_video_info_raw &format);
// Converts mapped pixels to packed BGRA. Empty/invalid buffers leave output unchanged.
FrameResult convert_video_frame(const spa_buffer &buffer, const spa_video_info_raw &format, VideoFrame &output);
