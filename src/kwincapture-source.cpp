#include "obs-capture.hpp"
#include "obs-video.hpp"
#include <obs-module.h>

struct WaylandSource {
	std::shared_ptr<CaptureState> state;
	OBSWeakSource source;
	VideoFrame uploadFrame;
	ObsVideoRenderer renderer;
};

static const char *sourceName(void *) { return obs_module_text("Source.Name"); }
static void getStatus(void *data, calldata_t *result)
{
	auto state = static_cast<WaylandSource *>(data)->state;
	auto diagnostic = state->diagnostics();
	calldata_set_string(result, "status", capture_status_name(diagnostic.status));
	calldata_set_bool(result, "busy", diagnostic.status == CaptureStatus::Connecting);
	calldata_set_int(result, "frames", state->frames->sequence());
	calldata_set_bool(result, "gpu", state->frames->mode() == FrameBuffer::Mode::DmaBuf);
}

static void sourceUpdate(void *opaque, obs_data_t *settings)
{
	auto data = static_cast<WaylandSource *>(opaque);
	auto config = read_capture_config(settings);
	data->state->configure(config);
	if (auto source = OBSGetStrongRef(data->source))
		obs_source_set_audio_active(source, config.audio);
	queue_capture_update(data->state);
}

static void *sourceCreate(obs_data_t *settings, obs_source_t *source)
{
	auto data = new WaylandSource;
	data->source = OBSGetWeakRef(source);
	data->state = create_obs_capture_state(source, settings);
	data->state->setShowing(false);
	sourceUpdate(data, settings);
	auto handler = obs_source_get_proc_handler(source);
	proc_handler_add(handler, "void get_status(out string status, out bool busy, out int frames, out bool gpu)",
			 getStatus, data);
	return data;
}

static void sourceDestroy(void *opaque)
{
	auto data = static_cast<WaylandSource *>(opaque);
	release_capture(data->state);
	delete data;
}

static void sourceDefaults(obs_data_t *settings) { set_capture_defaults(settings); }

static void sourceShow(void *opaque)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	state->setShowing(true);
	queue_capture_update(state);
}

static void sourceHide(void *opaque)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	state->setShowing(false);
	queue_capture_update(state);
}

static void sourceSave(void *opaque, obs_data_t *settings)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	write_capture_state(settings, *state);
}

static bool refreshClicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (data) {
		auto source = OBSGetStrongRef(static_cast<WaylandSource *>(data)->source);
		if (source)
			obs_source_update_properties(source);
	}
	return false;
}

static bool windowSelected(void *opaque, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	if (!opaque)
		return false;
	auto state = static_cast<WaylandSource *>(opaque)->state;
	auto uuid = QString::fromUtf8(obs_data_get_string(settings, "selected_window"));
	if (state->configuration().preferredWindow == uuid)
		return false;
	for (const auto &window : capture_windows())
		if (window.uuid == uuid) {
			auto rule = read_capture_config(settings).rule;
			rule.appId = window.appId;
			if (!rule.matches(window))
				obs_data_set_string(settings, "title_filter", "");
			obs_data_set_string(settings, "app_id", window.appId.toUtf8().constData());
			return true;
		}
	return false;
}

static void setHelpDescription(obs_property_t *property, const char *key)
{
	auto text = QString::fromUtf8(obs_module_text(key)).toHtmlEscaped();
	auto tooltip = QStringLiteral("<qt><table width=\"360\"><tr><td>%1</td></tr></table></qt>").arg(text).toUtf8();
	obs_property_set_long_description(property, tooltip.constData());
}

static obs_properties_t *sourceProperties(void *opaque)
{
	auto props = obs_properties_create();
	CaptureConfig config;
	bool cpuFallback = false;
	if (opaque) {
		auto state = static_cast<WaylandSource *>(opaque)->state;
		config = state->configuration();
		cpuFallback = state->frames->mode() == FrameBuffer::Mode::Cpu;
	}
	auto warning =
	    obs_properties_add_text(props, "cpu_fallback", obs_module_text("Source.CpuFallback"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);
	obs_property_text_set_info_word_wrap(warning, true);
	obs_property_set_visible(warning, cpuFallback);
	auto picker = obs_properties_add_list(props, "selected_window", obs_module_text("Source.Window"),
					      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	setHelpDescription(picker, "Source.Window.Help");
	obs_property_list_add_string(picker, obs_module_text("Source.SelectWindow"), "");
	bool found = false;
	for (const auto &window : capture_windows()) {
		auto label = QStringLiteral("%1 — %2").arg(window.appId, window.title).toUtf8();
		obs_property_list_add_string(picker, label.constData(), window.uuid.toUtf8().constData());
		found |= window.uuid == config.preferredWindow;
	}
	if (!found && !config.preferredWindow.isEmpty()) {
		auto label =
		    QStringLiteral("%1: %2").arg(obs_module_text("Source.SavedTarget"), config.rule.appId).toUtf8();
		obs_property_list_add_string(picker, label.constData(), config.preferredWindow.toUtf8().constData());
	}
	obs_property_set_modified_callback2(picker, windowSelected, opaque);
	obs_properties_add_button2(props, "refresh_windows", obs_module_text("Source.Refresh"), refreshClicked, opaque);
	obs_properties_add_text(props, "app_id", obs_module_text("Source.Application"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "title_filter", obs_module_text("Source.Title"), OBS_TEXT_DEFAULT);
	auto match = obs_properties_add_list(props, "title_match", obs_module_text("Source.Match"), OBS_COMBO_TYPE_LIST,
					     OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(match, obs_module_text("Match.Contains"), int(TitleMatch::Contains));
	obs_property_list_add_int(match, obs_module_text("Match.Exact"), int(TitleMatch::Exact));
	obs_property_list_add_int(match, obs_module_text("Match.Wildcard"), int(TitleMatch::Wildcard));
	obs_property_list_add_int(match, obs_module_text("Match.Regex"), int(TitleMatch::Regex));
	obs_properties_add_bool(props, "case_sensitive", obs_module_text("Source.CaseSensitive"));
	auto disconnect = obs_properties_add_bool(props, "disconnect_on_title_mismatch",
						  obs_module_text("Source.DisconnectOnTitleMismatch"));
	setHelpDescription(disconnect, "Source.DisconnectOnTitleMismatch.Help");
	obs_properties_add_bool(props, "capture_cursor", obs_module_text("Source.Cursor"));
	auto audio = obs_properties_add_bool(props, "capture_audio", obs_module_text("Source.Audio"));
	setHelpDescription(audio, "Source.Audio.Help");
	auto focus = obs_properties_add_bool(props, "raise_on_focus", obs_module_text("Source.RaiseOnFocus"));
	setHelpDescription(focus, "Source.RaiseOnFocus.Help");
	return props;
}

static uint32_t sourceWidth(void *opaque)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	return state->frames->size().width;
}
static uint32_t sourceHeight(void *opaque)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	return state->frames->size().height;
}

static void sourceRender(void *opaque, gs_effect_t *)
{
	auto data = static_cast<WaylandSource *>(opaque);
	auto state = data->state;
	auto update = state->frames->take(data->uploadFrame);
	if (update == FrameBuffer::Update::Empty)
		return;
	data->renderer.render(data->uploadFrame, update == FrameBuffer::Update::Updated);
}

void register_kwincapture_source()
{
	obs_source_info info{};
	// Keep saved OBS scenes compatible across plugin renames.
	info.id = "waylandconnect_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = sourceName;
	info.create = sourceCreate;
	info.destroy = sourceDestroy;
	info.get_defaults = sourceDefaults;
	info.get_properties = sourceProperties;
	info.update = sourceUpdate;
	info.save = sourceSave;
	info.get_width = sourceWidth;
	info.get_height = sourceHeight;
	info.video_render = sourceRender;
	info.show = sourceShow;
	info.hide = sourceHide;
	info.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE;
	obs_register_source(&info);
}
