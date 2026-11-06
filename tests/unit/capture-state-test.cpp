#include "capture.hpp"
#include <QTest>

class CaptureStateTest : public QObject
{
	Q_OBJECT
      private slots:
	void captureModeNormalizesConfiguration_data()
	{
		QTest::addColumn<CaptureMode>("mode");
		QTest::newRow("window") << CaptureMode::Window;
		QTest::newRow("desktop") << CaptureMode::Desktop;
		QTest::newRow("application-audio") << CaptureMode::ApplicationAudio;
	}

	void captureModeNormalizesConfiguration()
	{
		QFETCH(CaptureMode, mode);
		CaptureState state(1, 1, {}, mode);
		const bool audioOnly = mode == CaptureMode::ApplicationAudio;
		QCOMPARE(state.configuration().audio, audioOnly);
		QCOMPARE(state.configuration().cursor, !audioOnly);
		QVERIFY(!state.configuration().followFocus);
		CaptureConfig config;
		config.rule.appId = "test.app";
		config.raiseOnFocus = true;
		config.followFocus = true;
		state.configure(config);
		auto normalized = state.configuration();
		QCOMPARE(normalized.audio, audioOnly);
		QCOMPARE(normalized.cursor, !audioOnly);
		QCOMPARE(normalized.raiseOnFocus, mode == CaptureMode::Window);
		QCOMPARE(normalized.followFocus, mode == CaptureMode::Window);
		QCOMPARE(normalized.rule.appId, mode == CaptureMode::Desktop ? QString{} : config.rule.appId);
	}
};

QTEST_GUILESS_MAIN(CaptureStateTest)
#include "capture-state-test.moc"
