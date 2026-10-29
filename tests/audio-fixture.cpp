#include "audio-fixture.hpp"

#include <algorithm>
#include <spa/node/io.h>
#include <unistd.h>

TestAudio::~TestAudio()
{
	if (started)
		pw_thread_loop_stop(loop);
	if (filter)
		pw_filter_destroy(filter);
	if (core)
		pw_core_disconnect(core);
	if (context)
		pw_context_destroy(context);
	if (loop)
		pw_thread_loop_destroy(loop);
}

bool TestAudio::start(int fd, const char *appId, std::initializer_list<std::pair<const char *, float>> samples,
		      uint32_t pid)
{
	loop = pw_thread_loop_new("wc-test-audio", nullptr);
	context = loop ? pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0) : nullptr;
	if (!context) {
		close(fd);
		return false;
	}
	core = pw_context_connect_fd(context, fd, nullptr, 0);
	if (!core)
		return false;
	auto props =
	    pw_properties_new(PW_KEY_NODE_NAME, "wc-test-audio", PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS,
			      "Stream/Output/Audio", PW_KEY_APP_ID, appId, PW_KEY_NODE_WANT_DRIVER, "true", nullptr);
	pw_properties_setf(props, PW_KEY_APP_PROCESS_ID, "%u", pid);
	filter = pw_filter_new(core, "wc-test-audio", props);
	if (!filter)
		return false;
	static const pw_filter_events events = [] {
		pw_filter_events e{};
		e.version = PW_VERSION_FILTER_EVENTS;
		e.process = [](void *data, spa_io_position *position) {
			auto self = static_cast<TestAudio *>(data);
			for (auto port : self->ports) {
				auto buffer =
				    static_cast<float *>(pw_filter_get_dsp_buffer(port, position->clock.duration));
				if (buffer)
					std::fill_n(buffer, position->clock.duration, *static_cast<float *>(port));
			}
		};
		return e;
	}();
	pw_filter_add_listener(filter, &listener, &events, this);
	for (const auto &[channel, value] : samples) {
		auto port =
		    pw_filter_add_port(filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(float),
				       pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
							 channel, PW_KEY_AUDIO_CHANNEL, channel, nullptr),
				       nullptr, 0);
		if (!port)
			return false;
		*static_cast<float *>(port) = value;
		ports.push_back(port);
	}
	if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0 || pw_thread_loop_start(loop) < 0)
		return false;
	started = true;
	return true;
}

bool TestAudio::setIdentity(const char *appId, uint32_t pid, bool onClient)
{
	auto props = pw_properties_new(PW_KEY_APP_ID, appId, nullptr);
	pw_properties_setf(props, PW_KEY_APP_PROCESS_ID, "%u", pid);
	pw_thread_loop_lock(loop);
	int result;
	if (onClient) {
		result = pw_core_update_properties(core, &props->dict);
		pw_properties_set(props, PW_KEY_APP_ID, "");
		pw_properties_set(props, PW_KEY_APP_PROCESS_ID, "");
		if (result >= 0)
			result = pw_filter_update_properties(filter, nullptr, &props->dict);
	} else {
		result = pw_filter_update_properties(filter, nullptr, &props->dict);
	}
	pw_thread_loop_unlock(loop);
	pw_properties_free(props);
	return result >= 0;
}
