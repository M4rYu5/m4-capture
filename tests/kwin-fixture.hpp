#pragma once

#include "window-rule.hpp"
#include <QObject>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <map>

struct wl_display;
struct wl_resource;
struct wl_client;

class KWinFixture : public QObject
{
      public:
	~KWinFixture() override;
	bool start();
	void add(const WindowInfo &window);
	void remove(const QString &uuid);
	void clear();
	void focus(const QString &uuid);
	void title(const QString &uuid, const QString &title);
	void disconnectClients();
	QString socketPath() const { return directory.path() + "/wayland-test"; }
	uint32_t videoNode = 42;
	uint64_t videoSerial = 0;
	uint32_t cursorMode = 0;
	int captureRequests = 0;
	QString requestedWindow;
	bool reject = false;
	bool deferCapture = false;

      private:
	QTemporaryDir directory;
	wl_display *display = nullptr;
	QSocketNotifier *notifier = nullptr;
	struct Window {
		WindowInfo info;
		QList<wl_resource *> resources;
	};
	std::map<QString, Window> windows;
	QList<wl_resource *> managers;
	std::map<wl_resource *, QString> streams;
	void flush();
	static void bindManagement(wl_client *, void *, uint32_t, uint32_t);
	static void bindScreencast(wl_client *, void *, uint32_t, uint32_t);
};
