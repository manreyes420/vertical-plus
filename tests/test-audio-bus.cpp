/*
 * Headless test of the Vertical+ isolated audio bus against real libobs.
 *
 * Three constant "tone" sources stand in for real audio:
 *   mic   = 0.1  on BOTH canvases (main + vertical)
 *   game  = 0.4  on main only
 *   music = 0.2  on vertical only
 *
 * Main outputs listen to track 1, vertical to track 6. Expected:
 *   track 1 = mic + game  = 0.5   (music must not leak in)
 *   track 6 = mic + music = 0.3   (mic once, game never)
 *
 * Build against a libobs + libobs-opengl build from obs-studio (Linux):
 *   g++ -std=c++17 -I ../src -I <obs>/libobs -I <obs>/frontend/api \
 *       test-audio-bus.cpp ../src/audio-bus.cpp -L<build>/libobs -lobs -lX11 -o bin/test-audio-bus
 * Run under a display (xvfb-run works) with:
 *   OBS_TEST_GL=<path to libobs-opengl.so> ./bin/test-audio-bus
 * libobs finds its effect files at <exe dir>/../share/obs/libobs/.
 * Set VP_CONTROL=1 to run the naive setup (scene straight on an
 * audio-mixing canvas) and watch it leak.
 */
#include "vplus.hpp"

#include <obs-nix-platform.h>
#include <util/platform.h>
#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

/* ---- stubs for the parts of the plugin not under test ---- */
namespace vplus {
static Settings g_test_settings;
Settings &settings()
{
	return g_test_settings;
}
void run_on_ui(std::function<void()> fn)
{
	fn();
}
} // namespace vplus
extern "C" config_t *obs_frontend_get_profile_config(void)
{
	return nullptr;
}

/* ---- tone source ---- */
struct Tone {
	obs_source_t *src;
	float value;
	std::atomic<bool> run{true};
	std::thread th;
};

static void *tone_create(obs_data_t *s, obs_source_t *src)
{
	auto *t = new Tone();
	t->src = src;
	t->value = (float)obs_data_get_double(s, "value");
	t->th = std::thread([t]() {
		const uint32_t frames = 480; /* 10 ms */
		float buf[480];
		for (float &f : buf)
			f = t->value;
		uint64_t ts = os_gettime_ns();
		while (t->run) {
			obs_source_audio a = {};
			a.data[0] = (uint8_t *)buf;
			a.data[1] = (uint8_t *)buf;
			a.frames = frames;
			a.speakers = SPEAKERS_STEREO;
			a.format = AUDIO_FORMAT_FLOAT_PLANAR;
			a.samples_per_sec = 48000;
			a.timestamp = ts;
			obs_source_output_audio(t->src, &a);
			ts += 10000000ULL;
			os_sleepto_ns(ts);
		}
	});
	return t;
}
static void tone_destroy(void *d)
{
	auto *t = static_cast<Tone *>(d);
	t->run = false;
	t->th.join();
	delete t;
}
static const char *tone_name(void *)
{
	return "tone";
}

/* ---- minimal transition (registered as fade_transition) ---- */
static obs_source_t *g_tr_self = nullptr;
static const char *tr_name(void *)
{
	return "test transition";
}
static void *tr_create(obs_data_t *, obs_source_t *s)
{
	g_tr_self = s;
	return s;
}
static void tr_destroy(void *) {}
static void tr_draw(void *, gs_texture_t *, gs_texture_t *, float, uint32_t, uint32_t) {}
static void tr_render(void *d, gs_effect_t *)
{
	obs_transition_video_render(static_cast<obs_source_t *>(d), tr_draw);
}
static float mix_a(void *, float t)
{
	return 1.0f - t;
}
static float mix_b(void *, float t)
{
	return t;
}
static bool tr_audio(void *d, uint64_t *ts, obs_source_audio_mix *audio, uint32_t mixers, size_t ch, size_t sr)
{
	return obs_transition_audio_render(static_cast<obs_source_t *>(d), ts, audio, mixers, ch, sr, mix_a, mix_b);
}

/* ---- measurement ---- */
struct Meter {
	std::mutex m;
	double sum = 0;
	uint64_t n = 0;
	void reset()
	{
		std::lock_guard<std::mutex> l(m);
		sum = 0;
		n = 0;
	}
	double mean()
	{
		std::lock_guard<std::mutex> l(m);
		return n ? sum / (double)n : 0.0;
	}
};
static Meter g_track1, g_track6;
static void meter_cb(void *p, size_t, audio_data *data)
{
	auto *m = static_cast<Meter *>(p);
	const float *f = (const float *)data->data[0];
	std::lock_guard<std::mutex> l(m->m);
	for (uint32_t i = 0; i < data->frames; i++)
		m->sum += f[i];
	m->n += data->frames;
}

static int g_fail = 0;
static void expect(const char *what, double got, double want)
{
	const bool ok = std::fabs(got - want) < 0.01;
	printf("  %-48s got %.3f  want %.3f  %s\n", what, got, want, ok ? "PASS" : "FAIL");
	if (!ok)
		g_fail++;
}

static void measure(const char *label, double want1, double want6)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(800));
	g_track1.reset();
	g_track6.reset();
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	printf("%s\n", label);
	expect("track 1 (main stream)", g_track1.mean(), want1);
	expect("track 6 (vertical stream)", g_track6.mean(), want6);
}

int main()
{
	Display *dpy = XOpenDisplay(nullptr);
	if (!dpy) {
		fprintf(stderr, "no X display\n");
		return 2;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(dpy);

	if (!obs_startup("en-US", nullptr, nullptr))
		return 2;
	obs_add_data_path(getenv("OBS_TEST_DATA"));

	obs_audio_info ai = {48000, SPEAKERS_STEREO};
	if (!obs_reset_audio(&ai))
		return 3;

	obs_video_info ovi = {};
	ovi.graphics_module = getenv("OBS_TEST_GL");
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 640;
	ovi.base_height = ovi.output_height = 360;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	int vr = obs_reset_video(&ovi);
	if (vr != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed: %d\n", vr);
		return 4;
	}

	obs_source_info tone = {};
	tone.id = "vp_tone";
	tone.type = OBS_SOURCE_TYPE_INPUT;
	tone.output_flags = OBS_SOURCE_AUDIO;
	tone.get_name = tone_name;
	tone.create = tone_create;
	tone.destroy = tone_destroy;
	obs_register_source(&tone);

	obs_source_info tr = {};
	tr.id = "fade_transition";
	tr.type = OBS_SOURCE_TYPE_TRANSITION;
	tr.get_name = tr_name;
	tr.create = tr_create;
	tr.destroy = tr_destroy;
	tr.video_render = tr_render;
	tr.audio_render = tr_audio;
	obs_register_source(&tr);

	vplus::program_register_source();
	vplus::settings().audio_track = 6;
	vplus::settings().width = 360;
	vplus::settings().height = 640;
	vplus::reserved_track_watch(true); /* strips track 6 from new sources */

	auto make_tone = [](const char *name, double v) {
		obs_data_t *s = obs_data_create();
		obs_data_set_double(s, "value", v);
		obs_source_t *src = obs_source_create("vp_tone", name, s, nullptr);
		obs_data_release(s);
		return src;
	};
	obs_source_t *mic = make_tone("mic", 0.1);
	obs_source_t *game = make_tone("game", 0.4);
	obs_source_t *music = make_tone("music", 0.2);
	printf("mixers after create: mic=0x%x game=0x%x music=0x%x (track 6 = 0x20 must be clear)\n",
	       obs_source_get_audio_mixers(mic), obs_source_get_audio_mixers(game),
	       obs_source_get_audio_mixers(music));

	/* Main canvas */
	obs_scene_t *main_scene = obs_scene_create("Main");
	obs_scene_add(main_scene, mic);
	obs_scene_add(main_scene, game);
	obs_set_output_source(0, obs_scene_get_source(main_scene));

	/* Vertical canvas exactly as the plugin creates it */
	obs_video_info vovi = ovi;
	vovi.base_width = vovi.output_width = 360;
	vovi.base_height = vovi.output_height = 640;
	obs_canvas_t *vcanvas = obs_canvas_create(VPLUS_CANVAS_NAME, &vovi, ACTIVATE | MIX_AUDIO | SCENE_REF);
	obs_scene_t *vscene = obs_canvas_scene_create(vcanvas, "Vertical");
	obs_scene_add(vscene, mic);
	obs_sceneitem_t *music_item = obs_scene_add(vscene, music);

	obs_source_t *program = obs_source_create_private(VPLUS_PROGRAM_ID, "Vertical+ Program", nullptr);
	obs_transition_set(vplus::program_transition(program), obs_scene_get_source(vscene));
	const bool control = getenv("VP_CONTROL") != nullptr;
	if (control) {
		/* Control: a plain audio-mixing canvas with the scene on channel 0
		 * (no bus). This is the naive approach and should leak. */
		printf("CONTROL RUN: scene directly on an audio-mixing canvas\n");
		obs_canvas_set_channel(vcanvas, 0, obs_scene_get_source(vscene));
	} else {
		obs_canvas_set_channel(vcanvas, 0, program);
	}

	/* Like the vertical audio encoder + main encoder being active. */
	vplus::program_activate_tracks(true);
	obs_add_raw_audio_callback(0, nullptr, meter_cb, &g_track1);
	obs_add_raw_audio_callback(5, nullptr, meter_cb, &g_track6);

	measure("1. mic shared, game main-only, music vertical-only", 0.5, 0.3);

	obs_sceneitem_set_visible(music_item, false);
	measure("2. music hidden on vertical", 0.5, 0.1);

	obs_sceneitem_set_visible(music_item, true);
	obs_sceneitem_t *vmic = obs_scene_find_source(vscene, "mic");
	obs_sceneitem_set_visible(vmic, false);
	measure("3. mic hidden on vertical only", 0.5, 0.2);

	obs_sceneitem_set_visible(vmic, true);
	obs_source_set_volume(mic, 0.5f);
	measure("4. mic fader at 50% (applies to both)", 0.45, 0.25);
	obs_source_set_volume(mic, 1.0f);

	obs_source_set_muted(music, true);
	measure("5. music muted", 0.5, 0.1);
	obs_source_set_muted(music, false);

	obs_canvas_set_channel(vcanvas, 0, nullptr); /* idle parking */
	measure("6. vertical parked", 0.5, 0.0);
	if (getenv("VP_QUICK_EXIT")) { printf("skip teardown\n"); obs_shutdown(); return g_fail ? 1 : 0; }

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");

	obs_remove_raw_audio_callback(0, meter_cb, &g_track1);
	obs_remove_raw_audio_callback(5, meter_cb, &g_track6);
	vplus::program_activate_tracks(false);
	vplus::reserved_track_watch(false);
	obs_set_output_source(0, nullptr);
	/* Same order as canvas::shutdown() + canvas::destroy() in the plugin. */
	obs_canvas_set_channel(vcanvas, 0, nullptr);
	obs_transition_set(vplus::program_transition(program), nullptr);
	obs_source_release(program);
	obs_scene_release(vscene);
	obs_canvas_release(vcanvas);
	obs_scene_release(main_scene);
	obs_source_release(mic);
	obs_source_release(game);
	obs_source_release(music);
	obs_shutdown();
	return g_fail ? 1 : 0;
}
