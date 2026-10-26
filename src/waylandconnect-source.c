#include <obs-module.h>

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define MAX_DIMENSION 8192

struct waylandconnect_source {
	uint32_t width;
	uint32_t height;
};

static const char *waylandconnect_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Source.Name");
}

static uint32_t clamp_dimension(long long value)
{
	if (value < 1)
		return 1;
	if (value > MAX_DIMENSION)
		return MAX_DIMENSION;
	return (uint32_t)value;
}

static void waylandconnect_update(void *data, obs_data_t *settings)
{
	struct waylandconnect_source *source = data;
	source->width = clamp_dimension(obs_data_get_int(settings, "width"));
	source->height = clamp_dimension(obs_data_get_int(settings, "height"));
}

static void *waylandconnect_create(obs_data_t *settings, obs_source_t *obs_source)
{
	UNUSED_PARAMETER(obs_source);
	struct waylandconnect_source *source = bzalloc(sizeof(*source));
	waylandconnect_update(source, settings);
	return source;
}

static void waylandconnect_destroy(void *data) { bfree(data); }

static void waylandconnect_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", DEFAULT_WIDTH);
	obs_data_set_default_int(settings, "height", DEFAULT_HEIGHT);
}

static obs_properties_t *waylandconnect_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_text(properties, "status", obs_module_text("Source.Status"), OBS_TEXT_INFO);
	obs_properties_add_int(properties, "width", obs_module_text("Source.Width"), 1, MAX_DIMENSION, 1);
	obs_properties_add_int(properties, "height", obs_module_text("Source.Height"), 1, MAX_DIMENSION, 1);
	return properties;
}

static uint32_t waylandconnect_width(void *data) { return ((struct waylandconnect_source *)data)->width; }

static uint32_t waylandconnect_height(void *data) { return ((struct waylandconnect_source *)data)->height; }

static void waylandconnect_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	const struct waylandconnect_source *source = data;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct vec4 color;

	/* A visible placeholder confirms that the plugin's render callback works. */
	vec4_set(&color, 0.05f, 0.45f, 0.40f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
	while (gs_effect_loop(effect, "Solid"))
		gs_draw_sprite(NULL, 0, source->width, source->height);
}

struct obs_source_info waylandconnect_source_info = {
    .id = "waylandconnect_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
    .get_name = waylandconnect_get_name,
    .create = waylandconnect_create,
    .destroy = waylandconnect_destroy,
    .get_defaults = waylandconnect_defaults,
    .get_properties = waylandconnect_properties,
    .update = waylandconnect_update,
    .get_width = waylandconnect_width,
    .get_height = waylandconnect_height,
    .video_render = waylandconnect_render,
    .icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE,
};
