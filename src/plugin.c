#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("waylandconnect", "en-US")

extern struct obs_source_info waylandconnect_source_info;

MODULE_EXPORT const char *obs_module_name(void) { return "WaylandConnect"; }

MODULE_EXPORT const char *obs_module_description(void)
{
	return "WaylandConnect window capture plugin (development scaffold)";
}

bool obs_module_load(void)
{
	obs_register_source(&waylandconnect_source_info);
	blog(LOG_INFO, "[WaylandConnect] version %s loaded (capture not implemented)", WAYLANDCONNECT_VERSION);
	return true;
}
