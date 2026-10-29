#pragma once

#include <obs.h>

// Keep the OBS graphics context locked for this scope.
struct GraphicsLock {
	GraphicsLock() { obs_enter_graphics(); }
	~GraphicsLock() { obs_leave_graphics(); }
	GraphicsLock(const GraphicsLock &) = delete;
	GraphicsLock &operator=(const GraphicsLock &) = delete;
};
