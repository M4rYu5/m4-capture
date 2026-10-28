#pragma once

#include "video-frame.hpp"
#include <atomic>

struct DmaBufFormat {
	uint32_t format;
	std::vector<uint64_t> modifiers;
};

// Called on the PipeWire thread, or during teardown after that thread joins.
// copy() must finish reading, or arrange a GPU release fence, before returning:
// the producer can then reuse its buffer.
class DmaBufCapture
{
      public:
	virtual ~DmaBufCapture() = default;
	std::vector<DmaBufFormat> formats;
	bool explicitSync = false;
	// Remember a failed GPU path across automatic stream reconnections.
	std::atomic<bool> enabled{true};
	virtual FrameResult copy(const spa_buffer &buffer, const spa_video_info_raw &format, VideoFrame &output) = 0;
	// Metadata-only/dropped buffers still need their release points signaled.
	virtual void discard(const spa_buffer &buffer) = 0;
	// Buffer identities are valid until removal. Drop retained imports before
	// their identity can be reused, and clear the pool on renegotiation/teardown.
	virtual void removeBuffer(const spa_buffer &buffer) = 0;
	virtual void clearImports() = 0;
};
