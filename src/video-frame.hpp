#pragma once

#include <spa/buffer/buffer.h>
#include <spa/param/video/raw.h>

#include <cstdint>
#include <memory>
#include <vector>

// GPU storage is owned by the OBS adapter; capture-core does not depend on OBS.
struct GpuFrame {
	virtual ~GpuFrame() = default;
};

struct VideoFrame {
	std::vector<uint8_t> pixels;
	uint32_t width = 0;
	uint32_t height = 0;
	std::shared_ptr<GpuFrame> gpu;
};

struct VideoLayout {
	uint32_t x = 0, y = 0, width = 0, height = 0, rotation = 0;
	bool flip = false;
	uint32_t outputWidth() const { return (rotation & 1) ? height : width; }
	uint32_t outputHeight() const { return (rotation & 1) ? width : height; }
};

VideoLayout video_layout(const spa_buffer &buffer, const spa_video_info_raw &format);

enum class FrameResult { Ready, Empty, UnsupportedFormat, UnmappedBuffer, InvalidLayout };

bool supported_video_format(const spa_video_info_raw &format);
// Converts mapped pixels to packed BGRA. Empty/invalid buffers leave output unchanged.
FrameResult convert_video_frame(const spa_buffer &buffer, const spa_video_info_raw &format, VideoFrame &output);
