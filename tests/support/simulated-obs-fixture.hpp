#pragma once

#include "kwin-fixture.hpp"
#include "obs-test-fixture.hpp"
#include "scoped-environment.hpp"

// Source behavior against simulated capture servers, with real libobs graphics.
class SimulatedObsFixture : public ObsTestFixture
{
	Q_OBJECT
      protected:
	KWinFixture kwin;
	ScopedEnvironment display{"WAYLAND_DISPLAY"};

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
	void init() { kwin.resetRequests(); }
	void cleanup()
	{
		cleanupObs();
		kwin.clear();
		kwin.clearDisplays();
		QTRY_VERIFY(capture_windows().isEmpty());
		QTRY_VERIFY(capture_displays().isEmpty());
	}
	void cleanupTestCase()
	{
		drain_obs_test_tasks();
		shutdown_capture();
	}
};
