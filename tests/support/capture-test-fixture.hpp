#pragma once

#include "capture.hpp"
#include "kwin-test-fixture.hpp"

#include <vector>

class CaptureTestFixture : public KWinTestFixture
{
	Q_OBJECT
      protected:
	std::vector<std::shared_ptr<CaptureState>> captures;
	std::shared_ptr<CaptureState> capture(CaptureConfig config = {}, CaptureMode mode = CaptureMode::Window)
	{
		if (config.rule.appId.isEmpty())
			config.rule.appId = "test.app";
		auto state = std::make_shared<CaptureState>(1280, 720, CaptureCallbacks{}, mode);
		state->configure(config);
		captures.push_back(state);
		queue_capture_update(state);
		return state;
	}
	QString status(const std::shared_ptr<CaptureState> &state)
	{
		return QString::fromUtf8(capture_status_name(state->diagnostics().status));
	}

      private slots:
	void initTestCase()
	{
		QVERIFY(kwin.start());
		display.set(kwin.socketPath().toUtf8());
		initialize_capture();
		kwin.add({"probe", "probe.app", "Probe", false});
		QTRY_VERIFY_WITH_TIMEOUT(!capture_windows().isEmpty(), 3000);
		kwin.clear();
	}
	void cleanup()
	{
		for (const auto &state : captures)
			release_capture(state);
		captures.clear();
		QCoreApplication::sendPostedEvents();
		kwin.clear();
		kwin.clearDisplays();
		QTRY_VERIFY(capture_windows().isEmpty());
		QTRY_VERIFY(capture_displays().isEmpty());
	}
	void cleanupTestCase() { shutdown_capture(); }
};
