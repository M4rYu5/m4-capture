#pragma once

#include "kwin-connection.hpp"
#include "kwin-fixture.hpp"
#include "scoped-environment.hpp"

#include <QTest>

class KWinTestFixture : public QObject
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
	}
	void init() { kwin.resetRequests(); }
	void cleanup()
	{
		kwin.clear();
		kwin.clearDisplays();
	}
};
