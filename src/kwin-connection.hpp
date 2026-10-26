#pragma once

#include "window-rule.hpp"

#include <QObject>
#include <QSocketNotifier>
#include <QTimer>

#include <map>
#include <memory>

struct wl_display;
struct wl_registry;
struct org_kde_plasma_window_management;
struct zkde_screencast_unstable_v1;
struct zkde_screencast_stream_unstable_v1;

class KWinStream : public QObject
{
	Q_OBJECT
      public:
	KWinStream(zkde_screencast_unstable_v1 *manager, const QString &uuid, bool cursor, QObject *parent);
	~KWinStream() override;
      signals:
	void ready(uint32_t node, const QString &serial);
	void closed(const QString &reason);

      private:
	zkde_screencast_stream_unstable_v1 *stream = nullptr;
	QString serial;
};

class KWinConnection : public QObject
{
	Q_OBJECT
      public:
	explicit KWinConnection(QObject *parent);
	~KWinConnection() override;
	QList<WindowInfo> windows() const;
	bool available() const;
	QString error() const { return message; }
	KWinStream *capture(const QString &uuid, bool cursor, QObject *parent);
	void flush();
      signals:
	void changed();

      private:
	struct Window;
	std::map<QString, std::unique_ptr<Window>> entries;
	wl_display *display = nullptr;
	wl_registry *registry = nullptr;
	org_kde_plasma_window_management *management = nullptr;
	zkde_screencast_unstable_v1 *screencast = nullptr;
	QSocketNotifier *reader = nullptr;
	QSocketNotifier *writer = nullptr;
	QTimer retry;
	QString message;
	uint32_t managementName = 0;
	uint32_t screencastName = 0;
	bool initialized = false;
	bool notificationPending = false;
	void open();
	void close();
	void disconnected(const QString &reason);
	void notify();
	void addWindow(const QString &uuid);
	static void global(void *, wl_registry *, uint32_t, const char *, uint32_t);
	static void globalRemoved(void *, wl_registry *, uint32_t);
};
