#pragma once

#include "dmabuf-capture.hpp"
#include <graphics/graphics.h>

std::shared_ptr<DmaBufCapture> create_obs_dmabuf_capture();
gs_texture_t *obs_frame_texture(const VideoFrame &frame);
