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

struct VideoLayout {
	uint32_t x = 0, y = 0, width = 0, height = 0, rotation = 0;
	bool flip = false;
	uint32_t outputWidth() const { return (rotation & 1) ? height : width; }
	uint32_t outputHeight() const { return (rotation & 1) ? width : height; }
};

struct VideoFrame {
	std::vector<uint8_t> pixels;
	// Display dimensions after the transform; CPU storage uses layout.width/height.
	uint32_t width = 0;
	uint32_t height = 0;
	std::shared_ptr<GpuFrame> gpu;
	VideoLayout layout{};
	spa_video_format format = SPA_VIDEO_FORMAT_BGRA;
};

VideoLayout video_layout(const spa_buffer &buffer, const spa_video_info_raw &format);

enum class FrameResult { Ready, Empty, UnsupportedFormat, UnmappedBuffer, InvalidLayout };

bool supported_video_format(const spa_video_info_raw &format);
// Copies cropped rows in native byte order; drawing applies alpha and transforms.
// Empty/invalid buffers leave output unchanged.
FrameResult copy_video_frame(const spa_buffer &buffer, const spa_video_info_raw &format, VideoFrame &output);
