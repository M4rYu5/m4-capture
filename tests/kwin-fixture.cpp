#include "kwin-fixture.hpp"
#include "plasma-window-management-server-protocol.h"
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

bool KWinFixture::start()
{
	display = wl_display_create();
	if (!display || wl_display_add_socket(display, socketPath().toUtf8().constData()) < 0)
		return false;
	wl_global_create(display, &org_kde_plasma_window_management_interface, 17, this, bindManagement);
	wl_global_create(display, &zkde_screencast_unstable_v1_interface, 6, this, bindScreencast);
	auto loop = wl_display_get_event_loop(display);
	notifier = new QSocketNotifier(wl_event_loop_get_fd(loop), QSocketNotifier::Read, this);
	connect(notifier, &QSocketNotifier::activated, this, [this, loop] {
		wl_event_loop_dispatch(loop, 0);
		flush();
	});
	return true;
}

void KWinFixture::flush() { wl_display_flush_clients(display); }

void KWinFixture::disconnectClients() { wl_display_destroy_clients(display); }

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
			auto stream = wl_resource_create(client, &zkde_screencast_stream_unstable_v1_interface,
							 wl_resource_get_version(manager), id);
			static const struct zkde_screencast_stream_unstable_v1_interface implementation{
			    [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};
			wl_resource_set_implementation(stream, &implementation, self, [](wl_resource *r) {
				static_cast<KWinFixture *>(wl_resource_get_user_data(r))->streams.erase(r);
			});
			self->streams[stream] = QString::fromUtf8(uuid);
			self->requestedWindow = QString::fromUtf8(uuid);
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
		};
		return i;
	}();
	wl_resource_set_implementation(resource, &implementation, data, nullptr);
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
