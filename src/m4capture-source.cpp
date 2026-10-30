#include "obs-capture.hpp"
#include "obs-video.hpp"
#include <obs-module.h>

struct M4CaptureSource {
	std::shared_ptr<CaptureState> state;
	OBSWeakSource source;
	VideoFrame uploadFrame;
	std::unique_ptr<ObsVideoRenderer> renderer;
};

static const char *sourceName(void *) { return obs_module_text("Source.Name"); }
static const char *desktopSourceName(void *) { return obs_module_text("Desktop.Name"); }
static const char *audioSourceName(void *) { return obs_module_text("Source.AudioName"); }
static void getStatus(void *data, calldata_t *result)
{
	auto state = static_cast<M4CaptureSource *>(data)->state;
	auto diagnostic = state->diagnostics();
	calldata_set_string(result, "status", capture_status_name(diagnostic.status));
	calldata_set_bool(result, "busy", diagnostic.status == CaptureStatus::Connecting);
	calldata_set_int(result, "frames", state->frames->sequence());
	calldata_set_bool(result, "gpu", state->frames->mode() == FrameBuffer::Mode::DmaBuf);
	calldata_set_int(result, "generation", diagnostic.generation);
	calldata_set_string(result, "target", diagnostic.target.toUtf8().constData());
}

static void sourceUpdate(void *opaque, obs_data_t *settings)
{
	auto data = static_cast<M4CaptureSource *>(opaque);
	data->state->configure(data->state->mode == CaptureMode::Desktop ? read_desktop_capture_config(settings)
									 : read_capture_config(settings));
	if (data->state->mode != CaptureMode::Desktop)
		if (auto source = OBSGetStrongRef(data->source))
			obs_source_set_audio_active(source, data->state->configuration().audio);
	queue_capture_update(data->state);
}

static void *createSource(obs_data_t *settings, obs_source_t *source, CaptureMode mode)
{
	auto data = new M4CaptureSource;
	data->source = OBSGetWeakRef(source);
	data->state = create_obs_capture_state(source, settings, mode);
	if (mode != CaptureMode::ApplicationAudio)
		data->renderer = std::make_unique<ObsVideoRenderer>();
	data->state->setShowing(false);
	sourceUpdate(data, settings);
	auto handler = obs_source_get_proc_handler(source);
	proc_handler_add(handler,
			 "void get_status(out string status, out bool busy, out int frames, out bool gpu, "
			 "out int generation, out string target)",
			 getStatus, data);
	return data;
}

static void *sourceCreate(obs_data_t *settings, obs_source_t *source)
{
	return createSource(settings, source, CaptureMode::Window);
}

static void *desktopSourceCreate(obs_data_t *settings, obs_source_t *source)
{
	return createSource(settings, source, CaptureMode::Desktop);
}

static void *audioSourceCreate(obs_data_t *settings, obs_source_t *source)
{
	return createSource(settings, source, CaptureMode::ApplicationAudio);
}

static void sourceDestroy(void *opaque)
{
	auto data = static_cast<M4CaptureSource *>(opaque);
	release_capture(data->state);
	delete data;
}

static void sourceDefaults(obs_data_t *settings) { set_capture_defaults(settings); }

static void desktopSourceDefaults(obs_data_t *settings) { set_capture_defaults(settings, CaptureMode::Desktop); }

static void audioSourceDefaults(obs_data_t *settings) { set_capture_defaults(settings, CaptureMode::ApplicationAudio); }

static void sourceShow(void *opaque)
{
	auto state = static_cast<M4CaptureSource *>(opaque)->state;
	state->setShowing(true);
	queue_capture_update(state);
}

static void sourceHide(void *opaque)
{
	auto state = static_cast<M4CaptureSource *>(opaque)->state;
	state->setShowing(false);
	queue_capture_update(state);
}

static void sourceSave(void *opaque, obs_data_t *settings)
{
	auto state = static_cast<M4CaptureSource *>(opaque)->state;
	write_capture_state(settings, *state);
}

static bool refreshClicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (data) {
		auto source = OBSGetStrongRef(static_cast<M4CaptureSource *>(data)->source);
		if (source)
			obs_source_update_properties(source);
	}
	return false;
}

static bool windowSelected(void *opaque, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	if (!opaque)
		return false;
	auto state = static_cast<M4CaptureSource *>(opaque)->state;
	auto uuid = QString::fromUtf8(obs_data_get_string(settings, "selected_window"));
	if (state->configuration().preferredWindow == uuid)
		return false;
	if (uuid.isEmpty()) {
		obs_data_set_string(settings, "app_id", "");
		obs_data_set_string(settings, "title_filter", "");
		return true;
	}
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

static obs_properties_t *captureProperties(void *opaque, CaptureMode mode)
{
	auto props = obs_properties_create();
	CaptureConfig config;
	bool cpuFallback = false;
	if (opaque) {
		auto state = static_cast<M4CaptureSource *>(opaque)->state;
		config = state->configuration();
		cpuFallback = state->frames->mode() == FrameBuffer::Mode::Cpu;
	}
	if (mode != CaptureMode::ApplicationAudio) {
		auto warning = obs_properties_add_text(props, "cpu_fallback", obs_module_text("Source.CpuFallback"),
						       OBS_TEXT_INFO);
		obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(warning, true);
		obs_property_set_visible(warning, cpuFallback);
	}
	if (mode == CaptureMode::Desktop)
		return props;
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
	if (mode == CaptureMode::Window) {
		obs_properties_add_bool(props, "capture_cursor", obs_module_text("Source.Cursor"));
		auto audio = obs_properties_add_bool(props, "capture_audio", obs_module_text("Source.Audio"));
		setHelpDescription(audio, "Source.Audio.Help");
		auto focus = obs_properties_add_bool(props, "raise_on_focus", obs_module_text("Source.RaiseOnFocus"));
		setHelpDescription(focus, "Source.RaiseOnFocus.Help");
	}
	return props;
}

static obs_properties_t *sourceProperties(void *opaque) { return captureProperties(opaque, CaptureMode::Window); }
static obs_properties_t *audioSourceProperties(void *opaque)
{
	return captureProperties(opaque, CaptureMode::ApplicationAudio);
}

static obs_properties_t *desktopSourceProperties(void *opaque)
{
	auto props = captureProperties(opaque, CaptureMode::Desktop);
	QString selected;
	if (opaque)
		selected = static_cast<M4CaptureSource *>(opaque)->state->configuration().display;
	auto picker = obs_properties_add_list(props, "selected_display", obs_module_text("Desktop.Display"),
					      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(picker, obs_module_text("Desktop.AllDisplays"), "");
	bool found = selected.isEmpty();
	for (const auto &display : capture_displays()) {
		auto label = display.description.isEmpty()
				 ? display.name
				 : QStringLiteral("%1 — %2").arg(display.name, display.description);
		obs_property_list_add_string(picker, label.toUtf8().constData(), display.name.toUtf8().constData());
		found |= display.name == selected;
	}
	if (!found) {
		auto label = QStringLiteral("%1 (%2)").arg(selected, obs_module_text("Desktop.Unavailable")).toUtf8();
		obs_property_list_add_string(picker, label.constData(), selected.toUtf8().constData());
	}
	obs_properties_add_button2(props, "refresh_displays", obs_module_text("Desktop.Refresh"), refreshClicked,
				   opaque);
	obs_properties_add_bool(props, "capture_cursor", obs_module_text("Source.Cursor"));
	return props;
}

static uint32_t sourceWidth(void *opaque)
{
	auto state = static_cast<M4CaptureSource *>(opaque)->state;
	return state->frames->size().width;
}
static uint32_t sourceHeight(void *opaque)
{
	auto state = static_cast<M4CaptureSource *>(opaque)->state;
	return state->frames->size().height;
}

static void sourceRender(void *opaque, gs_effect_t *)
{
	auto data = static_cast<M4CaptureSource *>(opaque);
	auto state = data->state;
	auto update = state->frames->take(data->uploadFrame);
	if (update == FrameBuffer::Update::Empty)
		return;
	data->renderer->render(data->uploadFrame, update == FrameBuffer::Update::Updated);
}

static obs_source_info captureSourceInfo()
{
	obs_source_info info{};
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.destroy = sourceDestroy;
	info.update = sourceUpdate;
	info.save = sourceSave;
	info.show = sourceShow;
	info.hide = sourceHide;
	return info;
}

void register_m4capture_source()
{
	auto info = captureSourceInfo();
	info.id = "m4capture_source";
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = sourceName;
	info.create = sourceCreate;
	info.get_defaults = sourceDefaults;
	info.get_properties = sourceProperties;
	info.get_width = sourceWidth;
	info.get_height = sourceHeight;
	info.video_render = sourceRender;
	info.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE;
	obs_register_source(&info);
}

void register_m4capture_desktop_source()
{
	auto info = captureSourceInfo();
	info.id = "m4capture_desktop_source";
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = desktopSourceName;
	info.create = desktopSourceCreate;
	info.get_defaults = desktopSourceDefaults;
	info.get_properties = desktopSourceProperties;
	info.get_width = sourceWidth;
	info.get_height = sourceHeight;
	info.video_render = sourceRender;
	info.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE;
	obs_register_source(&info);
}

void register_m4capture_audio_source()
{
	auto info = captureSourceInfo();
	info.id = "m4capture_audio_source";
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = audioSourceName;
	info.create = audioSourceCreate;
	info.get_defaults = audioSourceDefaults;
	info.get_properties = audioSourceProperties;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	obs_register_source(&info);
}
