#pragma once

#include "video-frame.hpp"

#include <mutex>

// Single producer/consumer handoff. Pixel storage is exchanged rather than copied.
class FrameBuffer
{
      public:
	struct Size {
		uint32_t width;
		uint32_t height;
	};
	enum class Update { Empty, Unchanged, Updated };
	enum class Mode { None, Cpu, DmaBuf };

	explicit FrameBuffer(uint32_t width = 1280, uint32_t height = 720);
	// Returns true for the first frame after clear(), or a size change.
	bool publish(VideoFrame &frame);
	// Unchanged means the consumer can keep using its previously uploaded texture.
	Update take(VideoFrame &frame);
	void clear();
	Size size() const;
	uint64_t sequence() const;
	Mode mode() const;

      private:
	mutable std::mutex mutex;
	VideoFrame latest;
	bool available = false;
	bool dirty = false;
	uint64_t published = 0;
	Mode captureMode = Mode::None;
};
