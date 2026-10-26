#include "obs-capture.hpp"

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("waylandconnect", "en-US")

MODULE_EXPORT const char *obs_module_name() { return "KWinCapture"; }

MODULE_EXPORT const char *obs_module_description()
{
	return "An OBS Studio window capture plugin for KDE Plasma Wayland with automatic reconnection.";
}

bool obs_module_load()
{
	initialize_capture();
	register_kwincapture_source();
	blog(LOG_INFO, "[KWinCapture] version %s loaded", KWINCAPTURE_VERSION);
	return true;
}

void obs_module_unload() { shutdown_capture(); }
