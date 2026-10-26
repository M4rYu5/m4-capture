#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>

#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QListView>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include <cstring>

namespace
{
class SourceListButtons : public QObject
{
      public:
	explicit SourceListButtons(QWidget *window) : QObject(window), window(window)
	{
		timer.setInterval(300);
		connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
		timer.start();
		refresh();
	}

	~SourceListButtons() override
	{
		if (window)
			for (auto button : window->findChildren<QToolButton *>("waylandconnectConnect"))
				delete button;
	}

      private:
	QPointer<QWidget> window;
	QTimer timer;

	void refresh()
	{
		if (!window)
			return;
		// OBS has no public API for a control beside a source row's visibility icon.
		// Use Qt introspection and AccessibleTextRole, never private C++ ABI/layouts.
		auto view = window->findChild<QListView *>("sources");
		if (!view || !view->model())
			return;
		auto model = view->model();
		for (int rowIndex = 0; rowIndex < model->rowCount(); ++rowIndex) {
			auto index = model->index(rowIndex, 0);
			auto row = view->indexWidget(index);
			if (!row)
				continue;
			auto button =
			    row->findChild<QToolButton *>("waylandconnectConnect", Qt::FindDirectChildrenOnly);
			auto name = model->data(index, Qt::AccessibleTextRole).toString().toUtf8();
			OBSSourceAutoRelease source = obs_get_source_by_name(name.constData());
			if (!source ||
			    std::strcmp(obs_source_get_unversioned_id(source), "waylandconnect_source") != 0) {
				delete button;
				continue;
			}
			if (!button) {
				auto layout = qobject_cast<QBoxLayout *>(row->layout());
				auto checkboxes = row->findChildren<QCheckBox *>(QString(), Qt::FindDirectChildrenOnly);
				QCheckBox *eye = nullptr;
				for (auto checkbox : checkboxes)
					if (checkbox->property("class").toString().contains("indicator-visibility"))
						eye = checkbox;
				// OBS 32 rows end with visibility and lock, with an optional group expander.
				if (!eye && checkboxes.size() >= 2)
					eye = checkboxes[checkboxes.size() - 2];
				if (!layout || !eye || layout->indexOf(eye) < 0)
					continue;
				button = new QToolButton(row);
				button->setObjectName("waylandconnectConnect");
				button->setAutoRaise(true);
				button->setIcon(QIcon::fromTheme("network-connect",
								 row->style()->standardIcon(QStyle::SP_BrowserReload)));
				button->setIconSize(QSize(16, 16));
				button->setFixedWidth(24);
				button->setAccessibleName(obs_module_text("Source.Connect"));
				layout->insertWidget(layout->indexOf(eye), button);
				connect(button, &QToolButton::clicked, button, [button] {
					auto uuid = button->property("source_uuid").toString().toUtf8();
					OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.constData());
					if (!source || std::strcmp(obs_source_get_unversioned_id(source),
								   "waylandconnect_source") != 0)
						return;
					calldata_t params = {};
					proc_handler_call(obs_source_get_proc_handler(source), "connect", &params);
					calldata_free(&params);
				});
			}
			button->setProperty("source_uuid", obs_source_get_uuid(source));
			calldata_t params = {};
			proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
			std::string key = std::string("Status.") + calldata_string(&params, "status");
			button->setToolTip(QStringLiteral("%1\n%2").arg(obs_module_text("Source.Connect"),
									obs_module_text(key.c_str())));
			button->setEnabled(!calldata_bool(&params, "busy"));
			calldata_free(&params);
		}
	}
};

QPointer<SourceListButtons> buttons;
} // namespace

void initialize_source_list()
{
	if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
		return;
	auto window = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (window && !buttons)
		buttons = new SourceListButtons(window);
}

void shutdown_source_list() { delete buttons.data(); }
