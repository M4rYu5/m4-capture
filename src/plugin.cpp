#include "obs-capture.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("waylandconnect", "en-US")

MODULE_EXPORT const char *obs_module_name() { return "KCow"; }

MODULE_EXPORT const char *obs_module_description()
{
	return "Persistent window capture for OBS Studio on KDE Plasma Wayland.";
}

bool obs_module_load()
{
	initialize_capture();
	register_waylandconnect_source();
	blog(LOG_INFO, "[KCow] version %s loaded", WAYLANDCONNECT_VERSION);
	return true;
}

void obs_module_unload() { shutdown_capture(); }
