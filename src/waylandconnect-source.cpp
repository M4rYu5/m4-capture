#include "capture.hpp"

#include <obs-module.h>

#include <algorithm>

struct WaylandSource {
	std::shared_ptr<CaptureState> state = std::make_shared<CaptureState>();
	gs_texture_t *texture = nullptr;
	uint32_t textureWidth = 0;
	uint32_t textureHeight = 0;
};

static const char *sourceName(void *) { return obs_module_text("Source.Name"); }

static void requestConnect(void *data, calldata_t *)
{
	queue_capture_action(static_cast<WaylandSource *>(data)->state, CaptureAction::Connect);
}

static void requestChoose(void *data, calldata_t *)
{
	queue_capture_action(static_cast<WaylandSource *>(data)->state, CaptureAction::Choose);
}

static void getStatus(void *data, calldata_t *result)
{
	auto state = static_cast<WaylandSource *>(data)->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	calldata_set_string(result, "status", capture_status_name(state->status));
	calldata_set_bool(result, "busy",
			  state->status == CaptureStatus::Choosing || state->status == CaptureStatus::Connecting);
}

static void *sourceCreate(obs_data_t *settings, obs_source_t *source)
{
	auto data = new WaylandSource;
	data->state->source = OBSGetWeakRef(source);
	data->state->width = std::clamp<long long>(obs_data_get_int(settings, "width"), 1, 16384);
	data->state->height = std::clamp<long long>(obs_data_get_int(settings, "height"), 1, 16384);
	data->state->restoreToken = obs_data_get_string(settings, "restore_token");
	data->state->selectionId = obs_data_get_string(settings, "selection_id");
	data->state->selectionAttempted = obs_data_get_bool(settings, "selection_attempted");
	auto handler = obs_source_get_proc_handler(source);
	proc_handler_add(handler, "void connect()", requestConnect, data);
	proc_handler_add(handler, "void choose_capture()", requestChoose, data);
	proc_handler_add(handler, "void get_status(out string status, out bool busy)", getStatus, data);
	return data;
}

static void sourceDestroy(void *opaque)
{
	auto data = static_cast<WaylandSource *>(opaque);
	release_capture(data->state);
	if (data->texture) {
		obs_enter_graphics();
		gs_texture_destroy(data->texture);
		obs_leave_graphics();
	}
	delete data;
}

static void sourceDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 1280);
	obs_data_set_default_int(settings, "height", 720);
}

static void sourceLoad(void *opaque, obs_data_t *)
{
	// Loading a saved scene must never open the picker, including old empty sources.
	auto state = static_cast<WaylandSource *>(opaque)->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	state->selectionAttempted = true;
}

static void sourceSave(void *opaque, obs_data_t *settings)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	obs_data_set_int(settings, "width", state->width);
	obs_data_set_int(settings, "height", state->height);
	obs_data_set_string(settings, "restore_token", state->restoreToken.c_str());
	obs_data_set_string(settings, "selection_id", state->selectionId.c_str());
	obs_data_set_bool(settings, "selection_attempted", state->selectionAttempted);
}

static bool connectClicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (data)
		requestConnect(data, nullptr);
	return false;
}

static bool chooseClicked(obs_properties_t *, obs_property_t *, void *data)
{
	if (data)
		requestChoose(data, nullptr);
	return false;
}

static obs_properties_t *sourceProperties(void *opaque)
{
	auto props = obs_properties_create();
	std::string status = obs_module_text("Status.idle");
	bool busy = false;
	bool choose = false;
	if (opaque) {
		auto state = static_cast<WaylandSource *>(opaque)->state;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			std::string key = std::string("Status.") + capture_status_name(state->status);
			status = obs_module_text(key.c_str());
			if (!state->detail.empty())
				status += "\n" + state->detail;
			busy = state->status == CaptureStatus::Choosing || state->status == CaptureStatus::Connecting;
			choose = !state->selectionAttempted && state->restoreToken.empty();
			if (choose)
				state->selectionAttempted = true;
		}
		if (choose)
			queue_capture_action(state, CaptureAction::Choose);
	}
	obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
	obs_property_set_enabled(
	    obs_properties_add_button2(props, "connect", obs_module_text("Source.Connect"), connectClicked, opaque),
	    !busy);
	obs_property_set_enabled(
	    obs_properties_add_button2(props, "choose", obs_module_text("Source.Choose"), chooseClicked, opaque),
	    !busy);
	obs_properties_add_text(props, "restore_note", obs_module_text("Source.RestoreNote"), OBS_TEXT_INFO);
	return props;
}

static uint32_t sourceWidth(void *opaque)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->width;
}

static uint32_t sourceHeight(void *opaque)
{
	auto state = static_cast<WaylandSource *>(opaque)->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	return state->height;
}

static void sourceRender(void *opaque, gs_effect_t *)
{
	auto data = static_cast<WaylandSource *>(opaque);
	auto state = data->state;
	std::lock_guard<std::mutex> lock(state->mutex);
	if (!state->hasFrame)
		return;
	if (!data->texture || data->textureWidth != state->width || data->textureHeight != state->height) {
		gs_texture_destroy(data->texture);
		data->texture = gs_texture_create(state->width, state->height, GS_BGRA, 1, nullptr, GS_DYNAMIC);
		data->textureWidth = state->width;
		data->textureHeight = state->height;
		state->frameDirty = true;
	}
	if (!data->texture)
		return;
	if (state->frameDirty) {
		gs_texture_set_image(data->texture, state->pixels.data(), state->width * 4, false);
		state->frameDirty = false;
	}
	auto effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), data->texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(data->texture, 0, state->width, state->height);
	gs_enable_framebuffer_srgb(previous);
}

void register_waylandconnect_source()
{
	obs_source_info info{};
	info.id = "waylandconnect_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = sourceName;
	info.create = sourceCreate;
	info.destroy = sourceDestroy;
	info.get_defaults = sourceDefaults;
	info.get_properties = sourceProperties;
	info.load = sourceLoad;
	info.save = sourceSave;
	info.get_width = sourceWidth;
	info.get_height = sourceHeight;
	info.video_render = sourceRender;
	info.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE;
	obs_register_source(&info);
}
