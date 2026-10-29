#pragma once

#include "video-frame.hpp"
#include <graphics/graphics.h>

// Uses the current graphics context and preserves its transform matrix.
void draw_video_texture(gs_texture_t *texture, const VideoLayout &layout);

// Render-thread upload texture, reused across frames with the same native format.
class ObsVideoRenderer
{
      public:
	ObsVideoRenderer() = default;
	~ObsVideoRenderer();
	ObsVideoRenderer(const ObsVideoRenderer &) = delete;
	ObsVideoRenderer &operator=(const ObsVideoRenderer &) = delete;
	// Caller holds the OBS graphics context; unchanged frames reuse the upload.
	void render(const VideoFrame &frame, bool updated);

      private:
	gs_texture_t *texture = nullptr;
};
