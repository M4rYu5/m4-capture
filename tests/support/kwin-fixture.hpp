#pragma once

#include "display-info.hpp"
#include "window-rule.hpp"
#include <QObject>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <map>
#include <memory>
#include <vector>

struct wl_display;
struct wl_resource;
struct wl_client;
struct wl_global;

class KWinFixture : public QObject
{
      public:
	~KWinFixture() override;
	bool start(bool advertiseScreencast = true);
	void resetRequests();
	void add(const WindowInfo &window);
	void remove(const QString &uuid);
	void clear();
	void focus(const QString &uuid);
	void title(const QString &uuid, const QString &title);
	void disconnectClients();
	void setScreencastAvailable(bool available);
	void setWindowManagementAvailable(bool available);
	void setOutputDiscoveryAvailable(bool available);
	void addDisplay(const DisplayInfo &display);
	void displayScale(const QString &name, int scale);
	void displayGeometry(const QString &name, const QRect &geometry, bool done = true);
	void displayDescription(const QString &name, const QString &description);
	void removeDisplay(const QString &name);
	void clearDisplays();
	QString socketPath() const { return directory.path() + "/wayland-test"; }
	uint32_t videoNode = 42;
	uint64_t videoSerial = 0;
	uint32_t cursorMode = 0;
	int captureRequests = 0;
	QString requestedWindow;
	QString requestedDisplay;
	QRect requestedRegion;
	int32_t requestedScale = 0;
	bool reject = false;
	bool deferCapture = false;
	bool windowManagement = true;
	uint32_t outputManagerVersion = 3;
	uint32_t displayVersion = 4;
	uint32_t screencastVersion = 6;

      private:
	QTemporaryDir directory;
	wl_display *display = nullptr;
	wl_global *screencast = nullptr;
	wl_global *management = nullptr;
	wl_global *outputManager = nullptr;
	QSocketNotifier *notifier = nullptr;
	struct Window {
		WindowInfo info;
		QList<wl_resource *> resources;
	};
	std::map<QString, Window> windows;
	QList<wl_resource *> managers;
	std::map<wl_resource *, QString> streams;
	struct Output {
		KWinFixture *owner;
		DisplayInfo info;
		wl_global *global;
		QList<wl_resource *> resources;
		std::map<wl_resource *, wl_resource *> descriptions;
	};
	std::vector<std::unique_ptr<Output>> outputs;
	void publishOutput(Output &output);
	void requestStream(wl_client *, wl_resource *, uint32_t, const QString &, uint32_t);
	void flush();
	static void bindManagement(wl_client *, void *, uint32_t, uint32_t);
	static void bindScreencast(wl_client *, void *, uint32_t, uint32_t);
	static void bindOutput(wl_client *, void *, uint32_t, uint32_t);
	static void bindOutputManager(wl_client *, void *, uint32_t, uint32_t);
};
