#include "capture.hpp"
#include "video-fixture.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs-nix-platform.h>

#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusUnixFileDescriptor>
#include <QDBusVirtualObject>
#include <QLabel>
#include <QListView>
#include <QStandardItemModel>
#include <QTest>
#include <QToolButton>

#include <unistd.h>
#include <wayland-client.h>

void register_waylandconnect_source();
void initialize_source_list();
void shutdown_source_list();

static QWidget *mainWindow = nullptr;
extern "C" void *obs_frontend_get_main_window() { return mainWindow; }
extern "C" void obs_frontend_save() {}
extern "C" const char *obs_module_text(const char *text) { return text; }

struct TestStream {
	uint node;
	QVariantMap properties;
};
using TestStreams = QList<TestStream>;
Q_DECLARE_METATYPE(TestStream)
Q_DECLARE_METATYPE(TestStreams)

static QDBusArgument &operator<<(QDBusArgument &arg, const TestStream &stream)
{
	arg.beginStructure();
	arg << stream.node << stream.properties;
	arg.endStructure();
	return arg;
}
static const QDBusArgument &operator>>(const QDBusArgument &arg, TestStream &stream)
{
	arg.beginStructure();
	arg >> stream.node >> stream.properties;
	arg.endStructure();
	return arg;
}

class FakePortal : public QDBusVirtualObject
{
	Q_OBJECT
      public:
	enum class Result { Cancel, Deny, Success, NoToken, Hold };
	Result result = Result::Cancel;
	int starts = 0;
	int closes = 0;
	QVariantMap selectedOptions;
	QString session;
	TestVideo *video = nullptr;
	QString token = "rotated-token";
	bool useSerial = false;
	QString introspect(const QString &) const override { return {}; }

	bool handleMessage(const QDBusMessage &message, const QDBusConnection &bus) override
	{
		auto args = message.arguments();
		if (message.member() == "GetAll") {
			bus.send(message.createReply(QVariantList{QVariantMap{{"version", uint(4)},
									      {"AvailableSourceTypes", uint(3)},
									      {"AvailableCursorModes", uint(3)}}}));
			return true;
		}
		if (message.member() == "Close") {
			++closes;
			bus.send(message.createReply());
			return true;
		}
		if (message.member() == "OpenPipeWireRemote") {
			if (video) {
				int fd = video->openRemote();
				QDBusUnixFileDescriptor remote(fd);
				close(fd);
				bus.send(message.createReply(QVariant::fromValue(remote)));
				return true;
			}
			bus.send(message.createErrorReply("org.freedesktop.portal.Error.Failed",
							  "test video connection failure"));
			return true;
		}
		if (args.isEmpty())
			return false;
		auto options = qdbus_cast<QVariantMap>(args.last());
		QString sender = message.service();
		sender.remove(':');
		sender.replace('.', '_');
		QString path = "/org/freedesktop/portal/desktop/request/" + sender + "/" +
			       options.value("handle_token").toString();
		QVariantMap results;
		uint code = 0;
		if (message.member() == "CreateSession") {
			session = "/org/freedesktop/portal/desktop/session/" + sender + "/" +
				  options.value("session_handle_token").toString();
			results.insert("session_handle", session);
		} else if (message.member() == "SelectSources") {
			selectedOptions = options;
		} else if (message.member() == "Start") {
			++starts;
			if (result == Result::Cancel)
				code = 1;
			else if (result == Result::Deny)
				code = 2;
			else {
				QVariantMap streamProperties{{"id", "test-window"}};
				if (video && useSerial)
					streamProperties.insert("pipewire-serial",
								QVariant::fromValue<qulonglong>(video->serial()));
				results.insert("streams", QVariant::fromValue(TestStreams{
							      {video ? video->nodeId() : 42, streamProperties}}));
				if (result != Result::NoToken)
					results.insert("restore_token", token);
			}
		} else
			return false;
		bus.send(message.createReply(QVariant::fromValue(QDBusObjectPath(path))));
		if (message.member() == "Start" && result == Result::Hold)
			return true;
		QTimer::singleShot(0, this, [bus, path, code, results] {
			auto signal = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
			signal << code << results;
			bus.send(signal);
		});
		return true;
	}
};

class CaptureTest : public QObject
{
	Q_OBJECT
	FakePortal portal;

	std::shared_ptr<CaptureState> savedState()
	{
		auto state = std::make_shared<CaptureState>();
		state->restoreToken = "original-token";
		state->selectionId = "original-window";
		state->width = 640;
		state->height = 360;
		state->pixels.resize(640 * 360 * 4, 255);
		state->hasFrame = true;
		state->status = CaptureStatus::Capturing;
		return state;
	}

	CaptureStatus status(const std::shared_ptr<CaptureState> &state)
	{
		std::lock_guard<std::mutex> lock(state->mutex);
		return state->status;
	}

	QString sourceStatus(obs_source_t *source)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		QString result = calldata_string(&params, "status");
		calldata_free(&params);
		return result;
	}

	QColor renderedPixel(obs_source_t *source)
	{
		obs_enter_graphics();
		auto target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		auto stage = gs_stagesurface_create(160, 90, GS_RGBA);
		QColor result;
		if (gs_texrender_begin(target, 160, 90)) {
			gs_projection_push();
			vec4 transparent{};
			gs_clear(GS_CLEAR_COLOR, &transparent, 0, 0);
			gs_ortho(0, 160, 0, 90, -100, 100);
			obs_source_video_render(source);
			gs_projection_pop();
			gs_texrender_end(target);
			gs_stage_texture(stage, gs_texrender_get_texture(target));
			uint8_t *bytes = nullptr;
			uint32_t stride = 0;
			if (gs_stagesurface_map(stage, &bytes, &stride)) {
				result = QColor(bytes[0], bytes[1], bytes[2], bytes[3]);
				gs_stagesurface_unmap(stage);
			}
		}
		gs_stagesurface_destroy(stage);
		gs_texrender_destroy(target);
		obs_leave_graphics();
		return result;
	}

      private slots:
	void init()
	{
		portal.video = nullptr;
		portal.token = "rotated-token";
		portal.useSerial = false;
	}

	void cleanup()
	{
		portal.video = nullptr;
		shutdown_source_list();
		mainWindow = nullptr;
	}

	void initTestCase()
	{
		qDBusRegisterMetaType<TestStream>();
		qDBusRegisterMetaType<TestStreams>();
		auto bus = QDBusConnection::sessionBus();
		QVERIFY(bus.registerService("org.freedesktop.portal.Desktop"));
		QVERIFY(
		    bus.registerVirtualObject("/org/freedesktop/portal/desktop", &portal, QDBusConnection::SubPath));
	}

	void cancelledRestoreKeepsSelectionAndSize()
	{
		portal.result = FakePortal::Result::Cancel;
		auto state = savedState();
		PortalCapture capture(state, nullptr);
		capture.start(false);
		QVERIFY(!state->hasFrame); // Clear the old frame before waiting on the portal.
		QTRY_VERIFY_WITH_TIMEOUT(status(state) == CaptureStatus::Failed, 3000);
		QCOMPARE(state->restoreToken, std::string("original-token"));
		QCOMPARE(state->selectionId, std::string("original-window"));
		QCOMPARE(state->width, uint32_t(640));
		QCOMPARE(state->height, uint32_t(360));
		QVERIFY(state->pixels.empty());
		QCOMPARE(portal.selectedOptions.value("restore_token").toString(), QString("original-token"));
	}

	void deniedNewSelectionKeepsOldTarget()
	{
		portal.result = FakePortal::Result::Deny;
		auto state = savedState();
		PortalCapture capture(state, nullptr);
		capture.start(true);
		QTRY_VERIFY_WITH_TIMEOUT(status(state) == CaptureStatus::Failed, 3000);
		QVERIFY(!portal.selectedOptions.contains("restore_token"));
		QCOMPARE(state->restoreToken, std::string("original-token"));
		QCOMPARE(state->selectionId, std::string("original-window"));
	}

	void tokenRotatesBeforeVideoConnectionFailure()
	{
		portal.result = FakePortal::Result::Success;
		auto state = savedState();
		PortalCapture capture(state, nullptr);
		capture.start(false);
		QTRY_VERIFY_WITH_TIMEOUT(status(state) == CaptureStatus::Failed, 3000);
		QCOMPARE(state->restoreToken, std::string("rotated-token"));
		QCOMPARE(state->selectionId, std::string("test-window"));
		QVERIFY(!state->hasFrame);
		QCOMPARE(state->width, uint32_t(640));
	}

	void missingPersistenceDoesNotEraseToken()
	{
		portal.result = FakePortal::Result::NoToken;
		auto state = savedState();
		PortalCapture capture(state, nullptr);
		capture.start(true);
		QTRY_VERIFY_WITH_TIMEOUT(status(state) == CaptureStatus::Failed, 3000);
		QCOMPARE(state->restoreToken, std::string("original-token"));
		QCOMPARE(state->selectionId, std::string("original-window"));
	}

	void sessionClosureClearsFrameAndPreservesSize()
	{
		portal.result = FakePortal::Result::Hold;
		auto state = savedState();
		PortalCapture capture(state, nullptr);
		int starts = portal.starts;
		capture.start(false);
		QTRY_VERIFY_WITH_TIMEOUT(portal.starts > starts, 3000);
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->pixels.resize(640 * 360 * 4);
			state->hasFrame = true;
		}
		auto signal = QDBusMessage::createSignal(portal.session, "org.freedesktop.portal.Session", "Closed");
		signal << QVariantMap();
		QDBusConnection::sessionBus().send(signal);
		QTRY_VERIFY_WITH_TIMEOUT(status(state) == CaptureStatus::Failed, 3000);
		QVERIFY(!state->hasFrame);
		QVERIFY(state->pixels.empty());
		QCOMPARE(state->restoreToken, std::string("original-token"));
		QCOMPARE(state->width, uint32_t(640));
		QCOMPARE(state->height, uint32_t(360));
	}

	void destroyingPendingCaptureClosesSession()
	{
		portal.result = FakePortal::Result::Hold;
		auto state = savedState();
		auto capture = std::make_unique<PortalCapture>(state, nullptr);
		int starts = portal.starts, closes = portal.closes;
		capture->start(false);
		QTRY_VERIFY_WITH_TIMEOUT(portal.starts > starts, 3000);
		capture.reset();
		QTRY_VERIFY_WITH_TIMEOUT(portal.closes > closes, 3000);
		QCOMPARE(state->restoreToken, std::string("original-token"));
	}

	void savedSourceLoadsWithoutChooserAndRoundTripsSelection_data()
	{
		QTest::addColumn<QString>("token");
		QTest::newRow("saved selection") << QString("saved-token");
		QTest::newRow("old empty scaffold") << QString();
	}

	void savedSourceLoadsWithoutChooserAndRoundTripsSelection()
	{
		QFETCH(QString, token);
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "restore_token", token.toUtf8().constData());
		obs_data_set_string(settings, "selection_id", "saved-window");
		obs_data_set_int(settings, "width", 800);
		obs_data_set_int(settings, "height", 600);
		OBSSourceAutoRelease source =
		    obs_source_create("waylandconnect_source", "Saved target", settings, nullptr);
		obs_source_load(source);
		int starts = portal.starts;
		auto properties = obs_source_properties(source);
		obs_properties_destroy(properties);
		QTest::qWait(50);
		QCOMPARE(portal.starts, starts);
		OBSDataAutoRelease saved = obs_save_source(source);
		auto savedSettings = obs_data_get_obj(saved, "settings");
		QCOMPARE(QString(obs_data_get_string(savedSettings, "restore_token")), token);
		QCOMPARE(QString(obs_data_get_string(savedSettings, "selection_id")), QString("saved-window"));
		QCOMPARE(obs_source_get_width(source), uint32_t(800));
		QCOMPARE(obs_source_get_height(source), uint32_t(600));
		obs_data_release(savedSettings);
	}

	void newSourceOpensChooserOnlyOnce()
	{
		portal.result = FakePortal::Result::Cancel;
		OBSSourceAutoRelease source =
		    obs_source_create("waylandconnect_source", "New target", nullptr, nullptr);
		int starts = portal.starts;
		auto properties = obs_source_properties(source);
		obs_properties_destroy(properties);
		QTRY_COMPARE_WITH_TIMEOUT(portal.starts, starts + 1, 3000);
		QTest::qWait(50);
		properties = obs_source_properties(source);
		obs_properties_destroy(properties);
		QTest::qWait(50);
		QCOMPARE(portal.starts, starts + 1);
	}

	void rowButtonGoesBeforeEyeAndTracksSource()
	{
		QWidget window;
		mainWindow = &window;
		QListView view(&window);
		view.setObjectName("sources");
		QStandardItemModel model(1, 1);
		view.setModel(&model);
		OBSSourceAutoRelease source =
		    obs_source_create("waylandconnect_source", "Row target", nullptr, nullptr);
		model.setData(model.index(0, 0), "Row target", Qt::AccessibleTextRole);
		auto row = new QWidget;
		auto layout = new QHBoxLayout(row);
		layout->addWidget(new QLabel("Row target"));
		auto eye = new QCheckBox;
		eye->setProperty("class", "checkbox-icon indicator-visibility");
		layout->addWidget(eye);
		layout->addWidget(new QCheckBox);
		view.setIndexWidget(model.index(0, 0), row);
		initialize_source_list();
		auto button = row->findChild<QToolButton *>("waylandconnectConnect");
		QVERIFY(button);
		QCOMPARE(layout->indexOf(button) + 1, layout->indexOf(eye));
		QCOMPARE(button->property("source_uuid").toString(), QString(obs_source_get_uuid(source)));
		portal.result = FakePortal::Result::Cancel;
		int starts = portal.starts;
		button->click();
		QTRY_COMPARE_WITH_TIMEOUT(portal.starts, starts + 1, 3000);
		shutdown_source_list();
		mainWindow = nullptr;
	}

	void videoRendersThenClearsOnDisconnectAndReconnects()
	{
		TestVideo video;
		QVERIFY(video.start());
		QVERIFY(video.startProducer());
		portal.video = &video;
		portal.result = FakePortal::Result::Success;
		OBSDataAutoRelease settings = obs_data_create();
		obs_data_set_string(settings, "restore_token", "initial-token");
		OBSSourceAutoRelease source =
		    obs_source_create("waylandconnect_source", "Video target", settings, nullptr);
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "connect", &params);
		QTRY_COMPARE_WITH_TIMEOUT(sourceStatus(source), QString("capturing"), 5000);
		QCOMPARE(obs_source_get_width(source), uint32_t(160));
		QCOMPARE(obs_source_get_height(source), uint32_t(90));
		auto pixel = renderedPixel(source);
		QVERIFY(pixel.isValid());
		QCOMPARE(pixel.alpha(), 255);
		QVERIFY(pixel.red() > pixel.green());
		QVERIFY(pixel.green() > pixel.blue());
		video.stopProducer();
		QTRY_COMPARE_WITH_TIMEOUT(sourceStatus(source), QString("failed"), 3000);
		QCOMPARE(obs_source_get_width(source), uint32_t(160));
		QCOMPARE(obs_source_get_height(source), uint32_t(90));
		QCOMPARE(renderedPixel(source).alpha(), 0);
		OBSDataAutoRelease saved = obs_save_source(source);
		OBSDataAutoRelease savedSettings = obs_data_get_obj(saved, "settings");
		QCOMPARE(QString(obs_data_get_string(savedSettings, "restore_token")), QString("rotated-token"));
		QVERIFY(video.startProducer());
		portal.token = "second-token";
		portal.useSerial = true;
		proc_handler_call(obs_source_get_proc_handler(source), "connect", &params);
		QTRY_COMPARE_WITH_TIMEOUT(sourceStatus(source), QString("capturing"), 5000);
		QCOMPARE(portal.selectedOptions.value("restore_token").toString(), QString("rotated-token"));
		QCOMPARE(renderedPixel(source).alpha(), 255);
		calldata_free(&params);
		portal.video = nullptr;
	}
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	auto display = wl_display_connect(nullptr);
	if (!display) {
		qCritical("Tests need access to a Wayland session for libobs initialization.");
		return 1;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
	obs_set_nix_platform_display(display);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 1;
	obs_video_info video{};
	video.graphics_module = "libobs-opengl";
	video.fps_num = 30;
	video.fps_den = 1;
	video.base_width = video.output_width = 320;
	video.base_height = video.output_height = 240;
	video.output_format = VIDEO_FORMAT_RGBA;
	video.colorspace = VIDEO_CS_709;
	video.range = VIDEO_RANGE_FULL;
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
		return 1;
	initialize_capture();
	register_waylandconnect_source();
	CaptureTest test;
	int result = QTest::qExec(&test, argc, argv);
	shutdown_source_list();
	shutdown_capture();
	obs_shutdown();
	wl_display_disconnect(display);
	return result;
}

#include "capture-test.moc"
