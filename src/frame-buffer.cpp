#include "frame-buffer.hpp"

FrameBuffer::FrameBuffer(uint32_t width, uint32_t height) : latest{{}, width, height} {}

bool FrameBuffer::publish(VideoFrame &frame)
{
	std::lock_guard<std::mutex> lock(mutex);
	bool changed = !available || latest.width != frame.width || latest.height != frame.height;
	latest.pixels.swap(frame.pixels);
	latest.width = frame.width;
	latest.height = frame.height;
	available = true;
	dirty = true;
	return changed;
}

FrameBuffer::Update FrameBuffer::take(VideoFrame &frame)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!available)
		return Update::Empty;
	if (!dirty)
		return Update::Unchanged;
	frame.pixels.swap(latest.pixels);
	frame.width = latest.width;
	frame.height = latest.height;
	dirty = false;
	return Update::Updated;
}

void FrameBuffer::clear()
{
	std::lock_guard<std::mutex> lock(mutex);
	available = false;
	dirty = false;
	latest.pixels.clear();
}

FrameBuffer::Size FrameBuffer::size() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return {latest.width, latest.height};
}
