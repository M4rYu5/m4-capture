#include "obs-capture.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("m4capture", "en-US")

MODULE_EXPORT const char *obs_module_name() { return "M4Capture"; }

MODULE_EXPORT const char *obs_module_description()
{
	return "An OBS Studio window capture plugin for KDE Plasma Wayland with automatic reconnection.";
}

bool obs_module_load()
{
	initialize_capture();
	register_m4capture_source();
	blog(LOG_INFO, "[M4Capture] version %s loaded", M4CAPTURE_VERSION);
	return true;
}

void obs_module_unload() { shutdown_capture(); }
