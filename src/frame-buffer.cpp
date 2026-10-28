#include "frame-buffer.hpp"

FrameBuffer::FrameBuffer(uint32_t width, uint32_t height) : latest{{}, width, height, {}} {}

bool FrameBuffer::publish(VideoFrame &frame)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!enabled)
		return false;
	auto nextMode = frame.gpu ? Mode::DmaBuf : Mode::Cpu;
	bool changed =
	    !available || latest.width != frame.width || latest.height != frame.height || captureMode != nextMode;
	captureMode = nextMode;
	latest.pixels.swap(frame.pixels);
	latest.gpu.swap(frame.gpu);
	latest.width = frame.width;
	latest.height = frame.height;
	available = true;
	dirty = true;
	++published;
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
	frame.gpu.swap(latest.gpu);
	frame.width = latest.width;
	frame.height = latest.height;
	dirty = false;
	return Update::Updated;
}

void FrameBuffer::clear()
{
	// GPU destruction takes the graphics lock; never do that while holding mutex.
	std::shared_ptr<GpuFrame> retired;
	std::lock_guard<std::mutex> lock(mutex);
	available = false;
	dirty = false;
	latest.pixels.clear();
	retired.swap(latest.gpu);
}

FrameBuffer::Size FrameBuffer::size() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return {latest.width, latest.height};
}

uint64_t FrameBuffer::sequence() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return published;
}

FrameBuffer::Mode FrameBuffer::mode() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return !available ? Mode::None : captureMode;
}

bool FrameBuffer::isEnabled() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return enabled;
}

void FrameBuffer::setEnabled(bool next)
{
	std::shared_ptr<GpuFrame> retired;
	std::lock_guard<std::mutex> lock(mutex);
	enabled = next;
	if (!enabled) {
		available = false;
		dirty = false;
		latest.pixels.clear();
		retired.swap(latest.gpu);
	}
}
