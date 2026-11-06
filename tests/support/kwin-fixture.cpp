#include "kwin-fixture.hpp"
#include "plasma-window-management-server-protocol.h"
#include "xdg-output-unstable-v1-server-protocol.h"
#include "zkde-screencast-unstable-v1-server-protocol.h"
#include <wayland-server.h>

KWinFixture::~KWinFixture()
{
	delete notifier;
	if (display) {
		wl_display_destroy_clients(display);
		wl_display_destroy(display);
	}
}

bool KWinFixture::start(bool advertiseScreencast)
{
	display = wl_display_create();
	if (!display || wl_display_add_socket(display, socketPath().toUtf8().constData()) < 0)
		return false;
	setWindowManagementAvailable(windowManagement);
	setScreencastAvailable(advertiseScreencast);
	setOutputDiscoveryAvailable(true);
	auto loop = wl_display_get_event_loop(display);
	notifier = new QSocketNotifier(wl_event_loop_get_fd(loop), QSocketNotifier::Read, this);
	connect(notifier, &QSocketNotifier::activated, this, [this, loop] {
		wl_event_loop_dispatch(loop, 0);
		flush();
	});
	return true;
}

void KWinFixture::flush() { wl_display_flush_clients(display); }

void KWinFixture::resetRequests()
{
	reject = true;
	deferCapture = false;
	videoNode = 42;
	videoSerial = 0;
	captureRequests = 0;
	cursorMode = 0;
	requestedWindow.clear();
	requestedDisplay.clear();
	requestedRegion = {};
	requestedScale = 0;
}

void KWinFixture::disconnectClients() { wl_display_destroy_clients(display); }

void KWinFixture::setScreencastAvailable(bool available)
{
	if (available && !screencast)
		screencast = wl_global_create(display, &zkde_screencast_unstable_v1_interface, screencastVersion, this,
					      bindScreencast);
	else if (!available && screencast) {
		wl_global_destroy(screencast);
		screencast = nullptr;
	}
	flush();
}

void KWinFixture::setWindowManagementAvailable(bool available)
{
	if (available && !management)
		management =
		    wl_global_create(display, &org_kde_plasma_window_management_interface, 17, this, bindManagement);
	else if (!available && management) {
		wl_global_destroy(management);
		management = nullptr;
	}
	flush();
}

void KWinFixture::setOutputDiscoveryAvailable(bool available)
{
	if (available && !outputManager)
		outputManager = wl_global_create(display, &zxdg_output_manager_v1_interface, outputManagerVersion, this,
						 bindOutputManager);
	else if (!available && outputManager) {
		wl_global_destroy(outputManager);
		outputManager = nullptr;
	}
	flush();
}

void KWinFixture::bindManagement(wl_client *client, void *data, uint32_t version, uint32_t id)
{
	auto self = static_cast<KWinFixture *>(data);
	auto resource = wl_resource_create(client, &org_kde_plasma_window_management_interface, version, id);
	static const struct org_kde_plasma_window_management_interface implementation = [] {
		struct org_kde_plasma_window_management_interface i{};
		i.get_window_by_uuid = [](wl_client *client, wl_resource *manager, uint32_t id, const char *uuid) {
			auto self = static_cast<KWinFixture *>(wl_resource_get_user_data(manager));
			auto found = self->windows.find(QString::fromUtf8(uuid));
			auto window = wl_resource_create(client, &org_kde_plasma_window_interface, 17, id);
			static const struct org_kde_plasma_window_interface implementation = [] {
				struct org_kde_plasma_window_interface i{};
				i.destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); };
				return i;
			}();
			wl_resource_set_implementation(window, &implementation, self, [](wl_resource *r) {
				auto self = static_cast<KWinFixture *>(wl_resource_get_user_data(r));
				for (auto &[uuid, w] : self->windows)
					w.resources.removeAll(r);
			});
			if (found == self->windows.end()) {
				org_kde_plasma_window_send_unmapped(window);
				return;
			}
			auto &w = found->second;
			w.resources.append(window);
			org_kde_plasma_window_send_app_id_changed(window, w.info.appId.toUtf8().constData());
			org_kde_plasma_window_send_title_changed(window, w.info.title.toUtf8().constData());
			org_kde_plasma_window_send_pid_changed(window, w.info.pid);
			org_kde_plasma_window_send_state_changed(
			    window, w.info.active ? ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE : 0);
			org_kde_plasma_window_send_initial_state(window);
		};
		return i;
	}();
	wl_resource_set_implementation(resource, &implementation, self, [](wl_resource *r) {
		static_cast<KWinFixture *>(wl_resource_get_user_data(r))->managers.removeAll(r);
	});
	self->managers.append(resource);
	for (const auto &[uuid, window] : self->windows)
		org_kde_plasma_window_management_send_window_with_uuid(resource, 0, uuid.toUtf8().constData());
}

void KWinFixture::bindScreencast(wl_client *client, void *data, uint32_t version, uint32_t id)
{
	auto resource = wl_resource_create(client, &zkde_screencast_unstable_v1_interface, version, id);
	static const struct zkde_screencast_unstable_v1_interface implementation = [] {
		struct zkde_screencast_unstable_v1_interface i{};
		i.destroy = [](wl_client *, wl_resource *resource) { wl_resource_destroy(resource); };
		i.stream_window = [](wl_client *client, wl_resource *manager, uint32_t id, const char *uuid,
				     uint32_t cursor) {
			auto self = static_cast<KWinFixture *>(wl_resource_get_user_data(manager));
			self->requestedWindow = QString::fromUtf8(uuid);
			self->requestStream(client, manager, id, self->requestedWindow, cursor);
		};
		i.stream_output = [](wl_client *client, wl_resource *manager, uint32_t id, wl_resource *output,
				     uint32_t cursor) {
			auto self = static_cast<KWinFixture *>(wl_resource_get_user_data(manager));
			self->requestedDisplay = static_cast<Output *>(wl_resource_get_user_data(output))->info.name;
			self->requestedRegion = {};
			self->requestStream(client, manager, id, self->requestedDisplay, cursor);
		};
		i.stream_region = [](wl_client *client, wl_resource *manager, uint32_t id, int32_t x, int32_t y,
				     uint32_t width, uint32_t height, wl_fixed_t scale, uint32_t cursor) {
			auto self = static_cast<KWinFixture *>(wl_resource_get_user_data(manager));
			self->requestedDisplay.clear();
			self->requestedRegion = QRect(x, y, width, height);
			self->requestedScale = scale;
			self->requestStream(client, manager, id, {}, cursor);
		};
		return i;
	}();
	wl_resource_set_implementation(resource, &implementation, data, nullptr);
}

void KWinFixture::requestStream(wl_client *client, wl_resource *manager, uint32_t id, const QString &target,
				uint32_t cursor)
{
	auto self = this;
	auto stream = wl_resource_create(client, &zkde_screencast_stream_unstable_v1_interface,
					 wl_resource_get_version(manager), id);
	static const struct zkde_screencast_stream_unstable_v1_interface implementation{
	    [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
	wl_resource_set_implementation(stream, &implementation, self, [](wl_resource *r) {
		static_cast<KWinFixture *>(wl_resource_get_user_data(r))->streams.erase(r);
	});
	self->streams[stream] = target;
	self->cursorMode = cursor;
	++self->captureRequests;
	if (self->deferCapture)
		return;
	if (self->reject)
		zkde_screencast_stream_unstable_v1_send_failed(stream, "test capture error");
	else {
		if (self->videoSerial)
			zkde_screencast_stream_unstable_v1_send_serial(stream, self->videoSerial >> 32,
								       self->videoSerial & 0xffffffffu);
		zkde_screencast_stream_unstable_v1_send_created(stream, self->videoNode);
	}
}

void KWinFixture::add(const WindowInfo &info)
{
	windows[info.uuid] = {info, {}};
	for (auto manager : managers)
		org_kde_plasma_window_management_send_window_with_uuid(manager, 0, info.uuid.toUtf8().constData());
	flush();
}

void KWinFixture::remove(const QString &uuid)
{
	auto found = windows.find(uuid);
	if (found == windows.end())
		return;
	for (auto resource : found->second.resources)
		org_kde_plasma_window_send_unmapped(resource);
	for (const auto &[stream, target] : streams)
		if (target == uuid)
			zkde_screencast_stream_unstable_v1_send_closed(stream);
	windows.erase(found);
	flush();
}

void KWinFixture::focus(const QString &uuid)
{
	for (auto &[key, w] : windows) {
		w.info.active = key == uuid;
		for (auto resource : w.resources)
			org_kde_plasma_window_send_state_changed(
			    resource, w.info.active ? ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE : 0);
	}
	flush();
}

void KWinFixture::clear()
{
	QStringList ids;
	for (const auto &[uuid, window] : windows)
		ids.append(uuid);
	for (const auto &uuid : ids)
		remove(uuid);
}

void KWinFixture::title(const QString &uuid, const QString &title)
{
	auto &w = windows.at(uuid);
	w.info.title = title;
	for (auto resource : w.resources)
		org_kde_plasma_window_send_title_changed(resource, title.toUtf8().constData());
	flush();
}

void KWinFixture::addDisplay(const DisplayInfo &info)
{
	for (auto &output : outputs)
		if (output->global && output->info.name == info.name) {
			output->info = info;
			publishOutput(*output);
			flush();
			return;
		}
	auto output = std::make_unique<Output>();
	output->owner = this;
	output->info = info;
	output->global = wl_global_create(display, &wl_output_interface, displayVersion, output.get(), bindOutput);
	outputs.push_back(std::move(output));
	flush();
}

void KWinFixture::removeDisplay(const QString &name)
{
	for (auto &output : outputs)
		if (output->global && output->info.name == name) {
			wl_global_destroy(output->global);
			output->global = nullptr;
		}
	for (const auto &[stream, target] : streams)
		if (target == name)
			zkde_screencast_stream_unstable_v1_send_closed(stream);
	flush();
}

void KWinFixture::displayScale(const QString &name, int scale)
{
	for (auto &output : outputs)
		if (output->global && output->info.name == name) {
			output->info.scale = scale;
			for (auto resource : output->resources) {
				wl_output_send_scale(resource, scale);
				wl_output_send_done(resource);
			}
		}
	flush();
}

void KWinFixture::displayGeometry(const QString &name, const QRect &geometry, bool done)
{
	for (auto &output : outputs)
		if (output->global && output->info.name == name) {
			output->info.geometry = geometry;
			for (const auto &[description, resource] : output->descriptions) {
				zxdg_output_v1_send_logical_position(description, geometry.x(), geometry.y());
				zxdg_output_v1_send_logical_size(description, geometry.width(), geometry.height());
				if (done) {
					if (wl_resource_get_version(description) < 3)
						zxdg_output_v1_send_done(description);
					else
						wl_output_send_done(resource);
				}
			}
		}
	flush();
}

void KWinFixture::displayDescription(const QString &name, const QString &description)
{
	for (auto &output : outputs)
		if (output->global && output->info.name == name) {
			output->info.description = description;
			for (auto resource : output->resources)
				if (wl_resource_get_version(resource) >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION) {
					wl_output_send_description(resource, description.toUtf8().constData());
					wl_output_send_done(resource);
				}
		}
	flush();
}

void KWinFixture::clearDisplays()
{
	for (const auto &output : outputs)
		if (output->global)
			removeDisplay(output->info.name);
}

void KWinFixture::bindOutput(wl_client *client, void *data, uint32_t version, uint32_t id)
{
	auto output = static_cast<Output *>(data);
	auto resource = wl_resource_create(client, &wl_output_interface, version, id);
	static const struct wl_output_interface implementation{
	    [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
	wl_resource_set_implementation(resource, &implementation, output, [](wl_resource *r) {
		static_cast<Output *>(wl_resource_get_user_data(r))->resources.removeAll(r);
	});
	output->resources.append(resource);
	if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
		wl_output_send_name(resource, output->info.name.toUtf8().constData());
		wl_output_send_description(resource, output->info.description.toUtf8().constData());
	}
	output->owner->publishOutput(*output);
}

void KWinFixture::bindOutputManager(wl_client *client, void *data, uint32_t version, uint32_t id)
{
	auto resource = wl_resource_create(client, &zxdg_output_manager_v1_interface, version, id);
	static const struct zxdg_output_manager_v1_interface implementation = [] {
		struct zxdg_output_manager_v1_interface i{};
		i.destroy = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); };
		i.get_xdg_output = [](wl_client *client, wl_resource *manager, uint32_t id, wl_resource *wlOutput) {
			auto output = static_cast<Output *>(wl_resource_get_user_data(wlOutput));
			auto description =
			    wl_resource_create(client, &zxdg_output_v1_interface, wl_resource_get_version(manager), id);
			static const struct zxdg_output_v1_interface implementation{
			    [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
			wl_resource_set_implementation(description, &implementation, output, [](wl_resource *r) {
				static_cast<Output *>(wl_resource_get_user_data(r))->descriptions.erase(r);
			});
			output->descriptions[description] = wlOutput;
			zxdg_output_v1_send_name(description, output->info.name.toUtf8().constData());
			zxdg_output_v1_send_description(description, output->info.description.toUtf8().constData());
			output->owner->publishOutput(*output);
		};
		return i;
	}();
	wl_resource_set_implementation(resource, &implementation, data, nullptr);
}

void KWinFixture::publishOutput(Output &output)
{
	const auto &info = output.info;
	for (auto resource : output.resources) {
		wl_output_send_geometry(resource, info.geometry.x(), info.geometry.y(), 600, 340,
					WL_OUTPUT_SUBPIXEL_UNKNOWN, "Test", "Display", WL_OUTPUT_TRANSFORM_NORMAL);
		wl_output_send_mode(resource, WL_OUTPUT_MODE_CURRENT, info.geometry.width() * info.scale,
				    info.geometry.height() * info.scale, 60000);
		wl_output_send_scale(resource, info.scale);
	}
	for (const auto &[description, resource] : output.descriptions) {
		zxdg_output_v1_send_logical_position(description, info.geometry.x(), info.geometry.y());
		zxdg_output_v1_send_logical_size(description, info.geometry.width(), info.geometry.height());
		if (wl_resource_get_version(description) < 3)
			zxdg_output_v1_send_done(description);
	}
	for (auto resource : output.resources)
		wl_output_send_done(resource);
}
