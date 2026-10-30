#pragma once

#include "display-info.hpp"
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
struct zxdg_output_manager_v1;

class KWinStream : public QObject
{
	Q_OBJECT
      public:
	~KWinStream() override;
      signals:
	void ready(uint32_t node, const QString &serial);
	void closed(const QString &reason);

      private:
	friend class KWinConnection;
	KWinStream(zkde_screencast_stream_unstable_v1 *stream, QObject *parent);
	void invalidate(const QString &reason);
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
	QList<DisplayInfo> displays() const;
	bool windowsAvailable() const;
	bool displaysAvailable() const;
	bool available() const;
	bool desktopAvailable() const;
	QString error() const { return message; }
	KWinStream *capture(const QString &uuid, bool cursor, QObject *parent);
	KWinStream *captureDesktop(const QString &name, bool cursor, QObject *parent);
	void flush();
      signals:
	void windowsChanged();
	void displaysChanged();

      private:
	struct Window;
	struct Output;
	std::map<QString, std::unique_ptr<Window>> entries;
	std::map<uint32_t, std::unique_ptr<Output>> outputs;
	QList<WindowInfo> windowSnapshot;
	QList<DisplayInfo> displaySnapshot;
	// Proxies are invalidated before their display, even if wrappers outlive us.
	enum class StreamTarget { Window, Desktop, All };
	std::map<KWinStream *, StreamTarget> streams;
	wl_display *display = nullptr;
	wl_registry *registry = nullptr;
	org_kde_plasma_window_management *management = nullptr;
	zkde_screencast_unstable_v1 *screencast = nullptr;
	zxdg_output_manager_v1 *outputManager = nullptr;
	QSocketNotifier *reader = nullptr;
	QSocketNotifier *writer = nullptr;
	QTimer retry;
	QString message;
	uint32_t managementName = 0;
	uint32_t screencastName = 0;
	uint32_t outputManagerName = 0;
	bool initialized = false;
	bool windowNotificationPending = false;
	bool displayNotificationPending = false;
	void open();
	void close();
	void invalidateStreams(const QString &reason, StreamTarget target = StreamTarget::All);
	void disconnected(const QString &reason);
	void notifyWindows();
	void notifyDisplays();
	void addWindow(const QString &uuid);
	void addOutput(uint32_t name, uint32_t version);
	void describeOutput(Output &output);
	KWinStream *trackStream(zkde_screencast_stream_unstable_v1 *stream, QObject *parent, StreamTarget target);
	static void global(void *, wl_registry *, uint32_t, const char *, uint32_t);
	static void globalRemoved(void *, wl_registry *, uint32_t);
};
