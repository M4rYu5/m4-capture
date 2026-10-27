#include "obs-capture.hpp"

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
	config.raiseOnFocus = obs_data_get_bool(settings, "raise_on_focus");
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
	// Older scenes may have disabled these options; capture is now always automatic.
	obs_data_erase(settings, "auto_start");
	obs_data_erase(settings, "auto_reconnect");
	obs_data_set_bool(settings, "raise_on_focus", config.raiseOnFocus);
}

void write_capture_state(obs_data_t *settings, const CaptureState &state)
{
	write_capture_config(settings, state.configuration());
	auto size = state.frames->size();
	obs_data_set_int(settings, "width", size.width);
	obs_data_set_int(settings, "height", size.height);
}

void set_capture_defaults(obs_data_t *settings)
{
	const CaptureConfig defaults;
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
	obs_data_set_default_bool(settings, "capture_cursor", defaults.cursor);
	obs_data_set_default_bool(settings, "raise_on_focus", defaults.raiseOnFocus);
	obs_data_set_default_bool(settings, "disconnect_on_title_mismatch", defaults.rule.disconnectOnTitleMismatch);
}

std::shared_ptr<CaptureState> create_obs_capture_state(obs_source_t *source, obs_data_t *settings)
{
	OBSWeakSource weak = OBSGetWeakRef(source);
	CaptureCallbacks callbacks;
	callbacks.save = [weak](const CaptureState &state) {
		auto source = OBSGetStrongRef(weak);
		if (!source || obs_source_removed(source))
			return;
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		write_capture_state(settings, state);
		if (obs_frontend_get_main_window())
			obs_frontend_save();
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
	callbacks.error = [](const QString &error) { blog(LOG_WARNING, "[KWinCapture] %s", error.toUtf8().constData()); };
	return std::make_shared<CaptureState>(std::clamp<long long>(obs_data_get_int(settings, "width"), 1, 16384),
					      std::clamp<long long>(obs_data_get_int(settings, "height"), 1, 16384),
					      std::move(callbacks));
}
