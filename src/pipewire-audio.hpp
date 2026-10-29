#pragma once

#include <QString>
#include <cstdint>
#include <functional>
#include <memory>

// Stereo planar float samples; pointers are valid only during the callback.
struct AudioFrame {
	const float *data[2];
	uint32_t frames;
	uint32_t sampleRate;
	uint64_t timestamp;
};

class PipeWireAudioCapture
{
      public:
	PipeWireAudioCapture(QString appId, uint32_t pid, std::function<void(const AudioFrame &)> output,
			     std::function<void(QString)> failed);
	~PipeWireAudioCapture();
	PipeWireAudioCapture(const PipeWireAudioCapture &) = delete;
	PipeWireAudioCapture &operator=(const PipeWireAudioCapture &) = delete;
	bool start();

      private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};
