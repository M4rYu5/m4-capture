#include "obs-capture.hpp"
#include "obs-dmabuf.hpp"

#include <algorithm>
#include <obs-frontend-api.h>
#include <obs-module.h>

CaptureConfig read_capture_config(obs_data_t *settings)
{
	CaptureConfig config;
	config.rule.appId = QString::fromUtf8(obs_data_get_string(settings, "app_id"));
	config.rule.title = QString::fromUtf8(obs_data_get_string(settings, "title_filter"));
	config.rule.mode =
	    static_cast<TitleMatch>(std::clamp<long long>(obs_data_get_int(settings, "title_match"), 0, 3));
	config.rule.caseSensitive = obs_data_get_bool(settings, "case_sensitive");
	config.rule.disconnectOnTitleMismatch = obs_data_get_bool(settings, "disconnect_on_title_mismatch");
	config.preferredWindow = QString::fromUtf8(obs_data_get_string(settings, "selected_window"));
	config.cursor = obs_data_get_bool(settings, "capture_cursor");
	config.audio = obs_data_get_bool(settings, "capture_audio");
	config.raiseOnFocus = obs_data_get_bool(settings, "raise_on_focus");
	return config;
}

CaptureConfig read_desktop_capture_config(obs_data_t *settings)
{
	CaptureConfig config;
	config.display = QString::fromUtf8(obs_data_get_string(settings, "selected_display"));
	config.cursor = obs_data_get_bool(settings, "capture_cursor");
	return config;
}

void write_capture_config(obs_data_t *settings, const CaptureConfig &config)
{
	obs_data_set_string(settings, "app_id", config.rule.appId.toUtf8().constData());
	obs_data_set_string(settings, "title_filter", config.rule.title.toUtf8().constData());
	obs_data_set_int(settings, "title_match", int(config.rule.mode));
	obs_data_set_bool(settings, "case_sensitive", config.rule.caseSensitive);
	obs_data_set_bool(settings, "disconnect_on_title_mismatch", config.rule.disconnectOnTitleMismatch);
	obs_data_set_string(settings, "selected_window", config.preferredWindow.toUtf8().constData());
	obs_data_set_bool(settings, "capture_cursor", config.cursor);
	obs_data_set_bool(settings, "capture_audio", config.audio);
	// Older scenes may have disabled these options; capture is now always automatic.
	obs_data_erase(settings, "auto_start");
	obs_data_erase(settings, "auto_reconnect");
	obs_data_set_bool(settings, "raise_on_focus", config.raiseOnFocus);
}

void write_capture_state(obs_data_t *settings, const CaptureState &state)
{
	// OBS defers video-source updates. A stream callback may save while a new
	// audio choice or target is still pending; keep those newer settings.
	if (state.mode == CaptureMode::Desktop) {
		auto config = normalize_capture_config(read_desktop_capture_config(settings), state.mode);
		obs_data_set_string(settings, "selected_display", config.display.toUtf8().constData());
		obs_data_set_bool(settings, "capture_cursor", config.cursor);
		obs_data_set_bool(settings, "capture_audio", config.audio);
		obs_data_set_bool(settings, "raise_on_focus", config.raiseOnFocus);
	} else {
		write_capture_config(settings, normalize_capture_config(read_capture_config(settings), state.mode));
	}
	if (state.mode != CaptureMode::ApplicationAudio) {
		auto size = state.frames->size();
		obs_data_set_int(settings, "width", size.width);
		obs_data_set_int(settings, "height", size.height);
	}
}

void set_capture_defaults(obs_data_t *settings, CaptureMode mode)
{
	const auto defaults = normalize_capture_config({}, mode);
	if (mode == CaptureMode::Desktop)
		obs_data_set_default_string(settings, "selected_display", "");
	if (mode != CaptureMode::ApplicationAudio) {
		obs_data_set_default_int(settings, "width", 1280);
		obs_data_set_default_int(settings, "height", 720);
	}
	obs_data_set_default_bool(settings, "capture_cursor", defaults.cursor);
	obs_data_set_default_bool(settings, "capture_audio", defaults.audio);
	obs_data_set_default_bool(settings, "raise_on_focus", defaults.raiseOnFocus);
	obs_data_set_default_bool(settings, "disconnect_on_title_mismatch", defaults.rule.disconnectOnTitleMismatch);
}

std::shared_ptr<CaptureState> create_obs_capture_state(obs_source_t *source, obs_data_t *settings, CaptureMode mode)
{
	OBSWeakSource weak = OBSGetWeakRef(source);
	CaptureCallbacks callbacks;
	callbacks.audio = [weak](const AudioFrame &frame) {
		auto source = OBSGetStrongRef(weak);
		if (!source || obs_source_removed(source))
			return;
		obs_source_audio audio{};
		audio.data[0] = reinterpret_cast<const uint8_t *>(frame.data[0]);
		audio.data[1] = reinterpret_cast<const uint8_t *>(frame.data[1]);
		audio.frames = frame.frames;
		audio.speakers = SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = frame.sampleRate;
		audio.timestamp = frame.timestamp;
		obs_source_output_audio(source, &audio);
	};
	callbacks.save = [weak](const CaptureState &state) {
		auto source = OBSGetStrongRef(weak);
		if (!source || obs_source_removed(source))
			return;
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		write_capture_state(settings, state);
		if (obs_frontend_get_main_window())
			obs_frontend_save();
	};
	callbacks.changed = [weak, mode = FrameBuffer::Mode::None](const CaptureState &state) mutable {
		auto nextMode = state.frames->mode();
		if (mode == nextMode)
			return;
		mode = nextMode;
		auto source = OBSGetStrongRef(weak);
		if (!source || obs_source_removed(source))
			return;
		obs_source_update_properties(source);
		if (mode != FrameBuffer::Mode::None)
			blog(LOG_INFO, "[M4Capture] %s capture",
			     mode == FrameBuffer::Mode::DmaBuf ? "DMA-BUF GPU" : "CPU fallback");
	};
	callbacks.focused = [weak] {
		auto source = OBSGetStrongRef(weak);
		if (!source || !obs_frontend_get_main_window())
			return;
		OBSSourceAutoRelease sceneSource = obs_frontend_get_current_scene();
		auto scene = obs_scene_from_source(sceneSource);
		if (scene && raise_source_items(scene, source))
			obs_frontend_save();
	};
	callbacks.error = [](const QString &error) { blog(LOG_WARNING, "[M4Capture] %s", error.toUtf8().constData()); };
	if (mode != CaptureMode::ApplicationAudio)
		callbacks.gpu = create_obs_dmabuf_capture();
	return std::make_shared<CaptureState>(std::clamp<long long>(obs_data_get_int(settings, "width"), 1, 16384),
					      std::clamp<long long>(obs_data_get_int(settings, "height"), 1, 16384),
					      std::move(callbacks), mode);
}
