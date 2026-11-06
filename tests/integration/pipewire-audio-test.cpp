#include "audio-fixture.hpp"
#include "pipewire-audio.hpp"
#include "pipewire-server.hpp"
#include "scoped-environment.hpp"

#include <QScopeGuard>
#include <QTest>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <unistd.h>
#include <vector>

class PipeWireAudioTest : public QObject
{
	Q_OBJECT
	std::mutex mutex;
	float left = 0, right = 0;
	uint32_t rate = 0;
	uint64_t timestamp = 0, callbacks = 0;
	QString error;

	void received(const AudioFrame &frame)
	{
		std::lock_guard lock(mutex);
		left = frame.data[0][frame.frames / 2];
		right = frame.data[1][frame.frames / 2];
		rate = frame.sampleRate;
		timestamp = frame.timestamp;
		++callbacks;
	}
	bool samples(float l, float r)
	{
		std::lock_guard lock(mutex);
		return std::abs(left - l) < .0001f && std::abs(right - r) < .0001f && rate == 48000 && timestamp > 0;
	}
	void failed(QString message)
	{
		std::lock_guard lock(mutex);
		error = message;
	}

      private slots:
	void init()
	{
		left = right = 0;
		rate = timestamp = callbacks = 0;
		error.clear();
	}
	void cleanup() { QVERIFY2(error.isEmpty(), qPrintable(error)); }

	void selectsMixesAndReconnects()
	{
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set(daemon.socketPath().toUtf8());
		AudioProducer unrelated;
		QVERIFY(unrelated.start(daemon.openRemote(), "unrelated.app", {{"FL", .8f}, {"FR", .8f}}));
		PipeWireAudioCapture capture(
		    "test.app", 0, [this](auto &f) { received(f); }, [this](auto e) { failed(e); });
		QVERIFY(capture.start());
		QTest::qWait(200);
		{
			std::lock_guard lock(mutex);
			QCOMPARE(left, 0.f);
			QCOMPARE(right, 0.f);
		}
		auto first = std::make_unique<AudioProducer>();
		QVERIFY(first->start(daemon.openRemote(), "test.app"));
		QTRY_VERIFY_WITH_TIMEOUT(samples(.125f, -.25f), 5000);
		AudioProducer second;
		QVERIFY(second.start(daemon.openRemote(), "test.app", {{"FL", .25f}, {"FR", .125f}}));
		QTRY_VERIFY_WITH_TIMEOUT(samples(.375f, -.125f), 5000);
		first.reset();
		QTRY_VERIFY_WITH_TIMEOUT(samples(.25f, .125f), 5000);
		first = std::make_unique<AudioProducer>();
		QVERIFY(first->start(daemon.openRemote(), "test.app"));
		QTRY_VERIFY_WITH_TIMEOUT(samples(.375f, -.125f), 5000);
	}

	void capturesProcessAndChildren()
	{
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set(daemon.socketPath().toUtf8());
		QProcess child;
		child.start("sleep", {"30"});
		QVERIFY(child.waitForStarted());
		auto stopChild = qScopeGuard([&] {
			child.terminate();
			child.waitForFinished();
		});
		AudioProducer first, second, unrelated;
		QVERIFY(first.start(daemon.openRemote(), "foreign.app", {{"FL", .1f}, {"FR", .2f}}, getpid()));
		QVERIFY(second.start(daemon.openRemote(), "child.app", {{"FL", .2f}, {"FR", .1f}}, child.processId()));
		QVERIFY(unrelated.start(daemon.openRemote(), "unrelated.app"));
		PipeWireAudioCapture capture(
		    "window.app", getpid(), [this](auto &f) { received(f); }, [this](auto e) { failed(e); });
		QVERIFY(capture.start());
		QTRY_VERIFY_WITH_TIMEOUT(samples(.3f, .3f), 5000);
	}

	void downmixesMonoAndSurround()
	{
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set(daemon.socketPath().toUtf8());
		AudioProducer producer;
		QVERIFY(producer.start(daemon.openRemote(), "test.app",
				       {{"MONO", .1f}, {"FC", .2f}, {"RL", .3f}, {"SR", .4f}}));
		PipeWireAudioCapture capture(
		    "test.app", 0, [this](auto &f) { received(f); }, [this](auto e) { failed(e); });
		QVERIFY(capture.start());
		QTRY_VERIFY_WITH_TIMEOUT(samples(.1f + .5f * .7071f, .1f + .6f * .7071f), 5000);
	}

	void identityChangesInvalidateMatches_data()
	{
		QTest::addColumn<bool>("onClient");
		QTest::newRow("node") << false;
		QTest::newRow("client") << true;
	}

	void identityChangesInvalidateMatches()
	{
		QFETCH(bool, onClient);
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set(daemon.socketPath().toUtf8());
		// Keep the graph running when the stream under test is unmatched.
		AudioProducer baseline;
		QVERIFY(baseline.start(daemon.openRemote(), "test.app", {{"FL", .25f}, {"FR", .125f}}));
		AudioProducer producer;
		QVERIFY(producer.start(daemon.openRemote(), "foreign.app"));
		PipeWireAudioCapture capture(
		    "test.app", getpid(), [this](auto &f) { received(f); }, [this](auto e) { failed(e); });
		QVERIFY(capture.start());
		QTRY_VERIFY(samples(.25f, .125f));
		QVERIFY(producer.setIdentity("test.app", 1, onClient));
		QTRY_VERIFY(samples(.375f, -.125f));
		QVERIFY(producer.setIdentity("foreign.app", 1, onClient));
		QTRY_VERIFY(samples(.25f, .125f));
		QVERIFY(producer.setIdentity("foreign.app", getpid(), onClient));
		QTRY_VERIFY(samples(.375f, -.125f));
		QVERIFY(producer.setIdentity("foreign.app", 1, onClient));
		QTRY_VERIFY(samples(.25f, .125f));
	}

	void slowConsumerDoesNotBlockCapture()
	{
		PipeWireServer daemon;
		QVERIFY(daemon.start());
		ScopedEnvironment remote("PIPEWIRE_REMOTE");
		remote.set(daemon.socketPath().toUtf8());
		AudioProducer producer;
		QVERIFY(producer.start(daemon.openRemote(), "test.app"));
		std::mutex deliveryMutex;
		std::condition_variable ready;
		std::atomic<bool> blocked{false};
		bool released = false, stable = false;
		std::vector<uint64_t> timestamps;
		auto capture = std::make_unique<PipeWireAudioCapture>(
		    "test.app", 0,
		    [&](const AudioFrame &frame) {
			    if (std::abs(frame.data[0][0] - .125f) > .0001f)
				    return;
			    std::unique_lock lock(deliveryMutex);
			    timestamps.push_back(frame.timestamp);
			    if (timestamps.size() == 1) {
				    blocked = true;
				    ready.wait(lock, [&] { return released; });
				    stable = std::abs(frame.data[0][0] - .125f) < .0001f;
			    }
		    },
		    [this](auto e) { failed(e); });
		// Release the callback before capture destruction, including assertion failures.
		auto unblock = qScopeGuard([&] {
			std::lock_guard lock(deliveryMutex);
			released = true;
			ready.notify_one();
		});
		QVERIFY(capture->start());
		QTRY_VERIFY(blocked.load());
		QTest::qWait(500);
		{
			std::lock_guard lock(deliveryMutex);
			released = true;
		}
		ready.notify_one();
		auto delivered = [&] {
			std::lock_guard lock(deliveryMutex);
			return timestamps.size();
		};
		QTRY_VERIFY(delivered() >= 10);
		capture.reset();
		QVERIFY(stable);
		// Buffered audio advanced while delivery was blocked; overflow dropped new
		// blocks and kept their original timestamps rather than overwriting old data.
		QVERIFY(timestamps[7] - timestamps[0] < 300000000);
		QVERIFY(timestamps[8] - timestamps[7] > 100000000);
		for (size_t i = 1; i < timestamps.size(); ++i)
			QVERIFY(timestamps[i] > timestamps[i - 1]);
	}
};

QTEST_GUILESS_MAIN(PipeWireAudioTest)
#include "pipewire-audio-test.moc"
