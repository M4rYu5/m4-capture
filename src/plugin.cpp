#include "capture.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("waylandconnect", "en-US")

void register_waylandconnect_source();
void initialize_source_list();
void shutdown_source_list();

MODULE_EXPORT const char *obs_module_name() { return "WaylandConnect"; }

MODULE_EXPORT const char *obs_module_description()
{
	return "Wayland capture with saved selection and manual reconnection";
}

bool obs_module_load()
{
	initialize_capture();
	register_waylandconnect_source();
	blog(LOG_INFO, "[WaylandConnect] version %s loaded", WAYLANDCONNECT_VERSION);
	return true;
}

void obs_module_post_load() { initialize_source_list(); }

void obs_module_unload()
{
	shutdown_source_list();
	shutdown_capture();
}
