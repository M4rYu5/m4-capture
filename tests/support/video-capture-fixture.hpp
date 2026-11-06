#pragma once

#include "kwin-fixture.hpp"
#include "scoped-environment.hpp"
#include "video-fixture.hpp"

// Convenience composition for tests exercising the full simulated capture path.
struct VideoCaptureFixture {
	PipeWireServer server;
	VideoProducer producer;
	ScopedEnvironment remote{"PIPEWIRE_REMOTE"};
	explicit VideoCaptureFixture(uint32_t bgra = 0xffe08020u) : producer(server, bgra) {}
	bool startCapture(KWinFixture &kwin)
	{
		if (!server.start() || !producer.start())
			return false;
		remote.set(server.socketPath().toUtf8());
		kwin.reject = false;
		kwin.videoNode = producer.nodeId();
		kwin.videoSerial = producer.serial();
		return true;
	}
};
