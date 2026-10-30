#include "obs-test-fixture.hpp"
#include "obs-graphics.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <algorithm>

QWidget *mainWindow = nullptr;
obs_source_t *currentScene = nullptr;
static int saves = 0;
extern "C" void *obs_frontend_get_main_window() { return mainWindow; }
extern "C" obs_source_t *obs_frontend_get_current_scene() { return obs_source_get_ref(currentScene); }
extern "C" void obs_frontend_save() { ++saves; }
extern "C" const char *obs_module_text(const char *text) { return text; }

ObsAudioRecorder::ObsAudioRecorder(obs_source_t *source) : source(source)
{
	obs_source_add_audio_capture_callback(source, receive, this);
}

ObsAudioRecorder::~ObsAudioRecorder() { obs_source_remove_audio_capture_callback(source, receive, this); }

void ObsAudioRecorder::receive(void *data, obs_source_t *, const audio_data *audio, bool)
{
	auto &recorder = *static_cast<ObsAudioRecorder *>(data);
	if (audio->frames && audio->data[0] && audio->data[1]) {
		recorder.left = reinterpret_cast<const float *>(audio->data[0])[audio->frames / 2];
		recorder.right = reinterpret_cast<const float *>(audio->data[1])[audio->frames / 2];
		++recorder.count;
	}
}

obs_source_t *ObsTestFixture::createSource(const char *name, obs_data_t *settings, bool showing, const char *id)
{
	auto source = obs_source_create(id, name, settings, nullptr);
	if (source && showing) {
		// Model a properties preview for tests that render a standalone source.
		obs_source_inc_showing(source);
		shownSources.emplace_back(OBSGetWeakRef(source));
	}
	return source;
}

QString ObsTestFixture::status(obs_source_t *source)
{
	calldata_t params{};
	proc_handler_call(obs_source_get_proc_handler(source), "get_status", &params);
	QString result = calldata_string(&params, "status");
	calldata_free(&params);
	return result;
}

obs_source_t *ObsTestFixture::source(const char *name, const char *app, bool raise)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "app_id", app);
	obs_data_set_bool(settings, "raise_on_focus", raise);
	return createSource(name, settings);
}

QColor ObsTestFixture::renderedPixel(obs_source_t *source)
{
	GraphicsLock lock;
	auto target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	auto stage = gs_stagesurface_create(160, 90, GS_RGBA);
	QColor result;
	if (gs_texrender_begin(target, 160, 90)) {
		gs_projection_push();
		vec4 clear{};
		gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
		gs_ortho(0, 160, 0, 90, -100, 100);
		obs_source_video_render(source);
		gs_projection_pop();
		gs_texrender_end(target);
		gs_stage_texture(stage, gs_texrender_get_texture(target));
		uint8_t *bytes = nullptr;
		uint32_t stride = 0;
		if (gs_stagesurface_map(stage, &bytes, &stride)) {
			result = QColor(bytes[0], bytes[1], bytes[2], bytes[3]);
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(target);
	return result;
}

QByteArray ObsTestFixture::rendererPixels(ObsVideoRenderer &renderer, const VideoFrame &frame, bool updated)
{
	GraphicsLock lock;
	auto target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	auto stage = gs_stagesurface_create(frame.width, frame.height, GS_BGRA);
	QByteArray pixels;
	if (gs_texrender_begin(target, frame.width, frame.height)) {
		gs_projection_push();
		gs_matrix_push();
		gs_matrix_identity();
		gs_ortho(0, frame.width, 0, frame.height, -100, 100);
		vec4 clear{};
		gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
		renderer.render(frame, updated);
		gs_matrix_pop();
		gs_projection_pop();
		gs_texrender_end(target);
		gs_stage_texture(stage, gs_texrender_get_texture(target));
		uint8_t *bytes = nullptr;
		uint32_t stride = 0;
		if (gs_stagesurface_map(stage, &bytes, &stride)) {
			for (uint32_t y = 0; y < frame.height; ++y)
				pixels.append(reinterpret_cast<char *>(bytes + y * stride), frame.width * 4);
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(target);
	return pixels;
}

void ObsTestFixture::cleanupObs()
{
	for (const auto &weak : shownSources)
		if (auto source = OBSGetStrongRef(weak))
			obs_source_dec_showing(source);
	shownSources.clear();
	mainWindow = nullptr;
	currentScene = nullptr;
	// OBS 32 canvases own public scenes/groups in addition to our test refs.
	// Remove those canvas refs before tearing down their child sources.
	std::vector<OBSSource> scenes;
	auto collect = [](void *data, obs_source_t *source) {
		if (obs_source_is_scene(source) || obs_source_is_group(source)) {
			auto &items = *static_cast<std::vector<OBSSource> *>(data);
			if (std::none_of(items.begin(), items.end(),
					 [source](const auto &item) { return item == source; }))
				items.emplace_back(source);
		}
		return true;
	};
	obs_enum_sources(collect, &scenes);
	obs_enum_scenes(collect, &scenes);
	for (const auto &source : scenes)
		obs_canvas_scene_remove(obs_group_or_scene_from_source(source));
	scenes.clear();
}
