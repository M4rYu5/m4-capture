#pragma once

#include "capture.hpp"
#include <obs.hpp>

void register_kwincapture_source();
CaptureConfig read_capture_config(obs_data_t *settings);
void write_capture_config(obs_data_t *settings, const CaptureConfig &config);
void write_capture_state(obs_data_t *settings, const CaptureState &state);
void set_capture_defaults(obs_data_t *settings);
std::shared_ptr<CaptureState> create_obs_capture_state(obs_source_t *source, obs_data_t *settings);
bool raise_source_items(obs_scene_t *scene, obs_source_t *source);
