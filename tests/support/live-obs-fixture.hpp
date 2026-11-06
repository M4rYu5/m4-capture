#pragma once

#include "obs-test-fixture.hpp"
#include <algorithm>

class LiveObsFixture : public ObsTestFixture
{
	Q_OBJECT
      protected:
	int64_t frameCount(obs_source_t *source)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		auto result = calldata_int(&params, "frames");
		calldata_free(&params);
		return result;
	}
	int64_t captureGeneration(obs_source_t *source)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		auto result = calldata_int(&params, "generation");
		calldata_free(&params);
		return result;
	}
	bool capturingTargetAfter(obs_source_t *source, const QString &target, int64_t generation)
	{
		calldata_t params{};
		proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
		bool result = QString::fromUtf8(calldata_string(&params, "status")) == "capturing" &&
			      calldata_int(&params, "generation") > generation &&
			      QString::fromUtf8(calldata_string(&params, "target")) == target;
		calldata_free(&params);
		return result;
	}

      private slots:
	void initTestCase()
	{
		if (!qEnvironmentVariableIsSet("M4CAPTURE_LIVE_TEST"))
			QSKIP("Set M4CAPTURE_LIVE_TEST=1 to enable real compositor capture.");
		initialize_capture();
	}
	void cleanup() { cleanupObs(); }
	void cleanupTestCase()
	{
		drain_obs_test_tasks();
		shutdown_capture();
	}
};
