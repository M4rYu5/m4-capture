#include "kwin-connection.hpp"

#include "plasma-window-management-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

KWinStream::KWinStream(zkde_screencast_unstable_v1 *manager, const QString &uuid, bool cursor, QObject *parent)
    : QObject(parent)
{
	stream = zkde_screencast_unstable_v1_stream_window(manager, uuid.toUtf8().constData(),
							   cursor ? ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED
								  : ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_HIDDEN);
	static const zkde_screencast_stream_unstable_v1_listener listener = [] {
		zkde_screencast_stream_unstable_v1_listener events{};
		events.closed = [](void *data, auto *) {
			emit static_cast<KWinStream *>(data)->closed(QStringLiteral("The captured window closed."));
		};
		events.created = [](void *data, auto *, uint32_t node) {
			auto self = static_cast<KWinStream *>(data);
			emit self->ready(node, self->serial);
		};
		events.failed = [](void *data, auto *, const char *error) {
			emit static_cast<KWinStream *>(data)->closed(QString::fromUtf8(error));
		};
		events.serial = [](void *data, auto *, uint32_t high, uint32_t low) {
			static_cast<KWinStream *>(data)->serial = QString::number((uint64_t(high) << 32) | low);
		};
		return events;
	}();
	zkde_screencast_stream_unstable_v1_add_listener(stream, &listener, this);
}

KWinStream::~KWinStream()
{
	if (stream)
		zkde_screencast_stream_unstable_v1_close(stream);
}

void KWinStream::invalidate(const QString &reason)
{
	if (!stream)
		return;
	zkde_screencast_stream_unstable_v1_close(stream);
	stream = nullptr;
	emit closed(reason);
}

struct KWinConnection::Window {
	KWinConnection *owner;
	org_kde_plasma_window *proxy;
	WindowInfo info;
	bool ready = false;
	bool mapped = false;
	~Window() { org_kde_plasma_window_destroy(proxy); }
};

KWinConnection::KWinConnection(QObject *parent) : QObject(parent)
{
	retry.setInterval(3000);
	connect(&retry, &QTimer::timeout, this, [this] { open(); });
	open();
}

KWinConnection::~KWinConnection() { close(); }

void KWinConnection::open()
{
	close();
	display = wl_display_connect(nullptr);
	if (!display) {
		disconnected(QStringLiteral("Could not connect to the Wayland session."));
		return;
	}
	registry = wl_display_get_registry(display);
	static const wl_registry_listener listener = [] {
		wl_registry_listener events{};
		events.global = global;
		events.global_remove = globalRemoved;
		return events;
	}();
	wl_registry_add_listener(registry, &listener, this);
	reader = new QSocketNotifier(wl_display_get_fd(display), QSocketNotifier::Read, this);
	connect(reader, &QSocketNotifier::activated, this, [this] {
		if (wl_display_dispatch(display) < 0) {
			disconnected(QStringLiteral("The KWin Wayland connection closed."));
			return;
		}
		flush();
	});
	writer = new QSocketNotifier(wl_display_get_fd(display), QSocketNotifier::Write, this);
	writer->setEnabled(false);
	connect(writer, &QSocketNotifier::activated, this, [this] { flush(); });
	auto sync = wl_display_sync(display);
	static const wl_callback_listener done = [] {
		wl_callback_listener events{};
		events.done = [](void *data, wl_callback *callback, uint32_t) {
			wl_callback_destroy(callback);
			auto self = static_cast<KWinConnection *>(data);
			self->initialized = true;
			if (!self->available())
				self->message =
				    QStringLiteral("KWin window management or screencast access is unavailable. "
						   "KDE Plasma Wayland is required.");
			else
				self->message.clear();
			self->notify();
		};
		return events;
	}();
	wl_callback_add_listener(sync, &done, this);
	retry.stop();
	flush();
}

void KWinConnection::close()
{
	initialized = false;
	delete reader;
	reader = nullptr;
	delete writer;
	writer = nullptr;
	while (!streams.isEmpty()) {
		auto stream = *streams.begin();
		streams.remove(stream);
		stream->invalidate(message.isEmpty() ? QStringLiteral("The KWin Wayland connection closed.") : message);
	}
	entries.clear();
	if (management)
		wl_proxy_destroy(reinterpret_cast<wl_proxy *>(management));
	if (screencast)
		wl_proxy_destroy(reinterpret_cast<wl_proxy *>(screencast));
	if (registry)
		wl_registry_destroy(registry);
	if (display)
		wl_display_disconnect(display);
	management = nullptr;
	screencast = nullptr;
	registry = nullptr;
	display = nullptr;
}

void KWinConnection::disconnected(const QString &reason)
{
	message = reason;
	close();
	retry.start();
	emit changed();
}

bool KWinConnection::available() const { return initialized && management && screencast; }

QList<WindowInfo> KWinConnection::windows() const
{
	QList<WindowInfo> result;
	if (!available())
		return result;
	for (const auto &[uuid, window] : entries)
		if (window->ready && window->mapped && !window->info.appId.isEmpty())
			result.append(window->info);
	return result;
}

void KWinConnection::flush()
{
	if (!display)
		return;
	if (wl_display_flush(display) < 0) {
		if (errno == EAGAIN)
			writer->setEnabled(true);
		else
			disconnected(QStringLiteral("Could not communicate with KWin."));
	} else
		writer->setEnabled(false);
}

KWinStream *KWinConnection::capture(const QString &uuid, bool cursor, QObject *parent)
{
	if (!available())
		return nullptr;
	auto stream = new KWinStream(screencast, uuid, cursor, parent);
	streams.insert(stream);
	connect(stream, &QObject::destroyed, this, [this, stream] { streams.remove(stream); });
	return stream;
}

void KWinConnection::notify()
{
	if (notificationPending)
		return;
	notificationPending = true;
	QTimer::singleShot(0, this, [this] {
		notificationPending = false;
		emit changed();
	});
}

void KWinConnection::global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	auto self = static_cast<KWinConnection *>(data);
	if (!std::strcmp(interface, org_kde_plasma_window_management_interface.name) && version >= 17) {
		self->managementName = name;
		self->management = static_cast<org_kde_plasma_window_management *>(wl_registry_bind(
		    registry, name, &org_kde_plasma_window_management_interface, std::min(version, 21u)));
		static const org_kde_plasma_window_management_listener listener = [] {
			org_kde_plasma_window_management_listener events{};
			events.show_desktop_changed = [](void *, auto *, uint32_t) {};
			events.window = [](void *, auto *, uint32_t) {};
			events.stacking_order_changed = [](void *, auto *, wl_array *) {};
			events.stacking_order_uuid_changed = [](void *, auto *, const char *) {};
			events.window_with_uuid = [](void *data, auto *, uint32_t, const char *uuid) {
				static_cast<KWinConnection *>(data)->addWindow(QString::fromUtf8(uuid));
			};
			events.stacking_order_changed_2 = [](void *, auto *) {};
			return events;
		}();
		org_kde_plasma_window_management_add_listener(self->management, &listener, self);
	} else if (!std::strcmp(interface, zkde_screencast_unstable_v1_interface.name)) {
		self->screencastName = name;
		self->screencast = static_cast<zkde_screencast_unstable_v1 *>(
		    wl_registry_bind(registry, name, &zkde_screencast_unstable_v1_interface, std::min(version, 6u)));
	}
}

void KWinConnection::globalRemoved(void *data, wl_registry *, uint32_t name)
{
	auto self = static_cast<KWinConnection *>(data);
	if (name == self->managementName || name == self->screencastName)
		QTimer::singleShot(
		    0, self, [self] { self->disconnected(QStringLiteral("KWin capture interfaces disappeared.")); });
}

void KWinConnection::addWindow(const QString &uuid)
{
	if (entries.count(uuid))
		return;
	auto window = std::make_unique<Window>();
	window->owner = this;
	window->info.uuid = uuid;
	window->proxy = org_kde_plasma_window_management_get_window_by_uuid(management, uuid.toUtf8().constData());
	window->mapped = org_kde_plasma_window_get_version(window->proxy) < 21;
	static const org_kde_plasma_window_listener listener = [] {
		org_kde_plasma_window_listener l{};
		l.title_changed = [](void *d, auto *, const char *v) {
			auto w = static_cast<Window *>(d);
			w->info.title = QString::fromUtf8(v);
			w->owner->notify();
		};
		l.app_id_changed = [](void *d, auto *, const char *v) {
			auto w = static_cast<Window *>(d);
			w->info.appId = QString::fromUtf8(v);
			w->owner->notify();
		};
		l.state_changed = [](void *d, auto *, uint32_t flags) {
			auto w = static_cast<Window *>(d);
			w->info.active = flags & ORG_KDE_PLASMA_WINDOW_MANAGEMENT_STATE_ACTIVE;
			w->owner->notify();
		};
		l.unmapped = [](void *d, auto *) {
			auto w = static_cast<Window *>(d);
			w->mapped = false;
			w->owner->notify();
			auto owner = w->owner;
			auto uuid = w->info.uuid;
			QTimer::singleShot(0, owner, [owner, uuid] {
				owner->entries.erase(uuid);
				owner->flush();
			});
		};
		l.initial_state = [](void *d, auto *) {
			auto w = static_cast<Window *>(d);
			w->ready = true;
			w->owner->notify();
		};
		l.mapped = [](void *d, auto *) {
			auto w = static_cast<Window *>(d);
			w->mapped = true;
			w->owner->notify();
		};
		l.virtual_desktop_changed = [](void *, auto *, int32_t) {};
		l.themed_icon_name_changed = [](void *, auto *, const char *) {};
		l.parent_window = [](void *, auto *, org_kde_plasma_window *) {};
		l.geometry = [](void *, auto *, int32_t, int32_t, uint32_t, uint32_t) {};
		l.icon_changed = [](void *, auto *) {};
		l.pid_changed = [](void *, auto *, uint32_t) {};
		l.virtual_desktop_entered = [](void *, auto *, const char *) {};
		l.virtual_desktop_left = [](void *, auto *, const char *) {};
		l.application_menu = [](void *, auto *, const char *, const char *) {};
		l.activity_entered = [](void *, auto *, const char *) {};
		l.activity_left = [](void *, auto *, const char *) {};
		l.resource_name_changed = [](void *, auto *, const char *) {};
		l.client_geometry = [](void *, auto *, int32_t, int32_t, uint32_t, uint32_t) {};
		return l;
	}();
	org_kde_plasma_window_add_listener(window->proxy, &listener, window.get());
	entries.emplace(uuid, std::move(window));
	flush();
}
