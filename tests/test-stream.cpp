/*
 * Headless test of the Vertical+ stream output (src/outputs.cpp) against
 * real libobs + obs-outputs/obs-x264/obs-ffmpeg, streaming the vertical
 * canvas to a local RTMP server (ffmpeg -listen), plus the error paths.
 *
 * Needs: libobs + libobs-opengl + those plugin modules, ffmpeg/ffprobe, a
 * display (xvfb-run). See the build notes at the end of this file.
 */
#include "vplus.hpp"

#include <obs-nix-platform.h>
#include <util/platform.h>
#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

/* ---- stubs for the parts of the plugin not under test ---- */
static obs_canvas_t *g_canvas = nullptr;
static vplus::Settings g_settings;
namespace vplus {
Settings &settings()
{
	return g_settings;
}
void run_on_ui(std::function<void()> fn)
{
	fn();
}
namespace canvas {
obs_canvas_t *get()
{
	return g_canvas;
}
void update_parking() {}
} // namespace canvas
} // namespace vplus
extern "C" {
config_t *obs_frontend_get_profile_config(void)
{
	return nullptr;
}
obs_output_t *obs_frontend_get_streaming_output(void)
{
	return nullptr;
}
const char *obs_module_text(const char *k)
{
	return k;
}
}

/* ---- a 1080x1920 test picture and a test tone ---- */
static const char *box_name(void *)
{
	return "box";
}
static void *box_create(obs_data_t *, obs_source_t *s)
{
	return s;
}
static void box_destroy(void *) {}
static uint32_t box_w(void *)
{
	return 1080;
}
static uint32_t box_h(void *)
{
	return 1920;
}
static void box_render(void *, gs_effect_t *) {}

struct Tone {
	obs_source_t *src;
	std::atomic<bool> run{true};
	std::thread th;
};
static const char *tone_name(void *)
{
	return "tone";
}
static void *tone_create(obs_data_t *, obs_source_t *src)
{
	auto *t = new Tone();
	t->src = src;
	t->th = std::thread([t]() {
		float buf[480];
		for (int i = 0; i < 480; i++)
			buf[i] = 0.2f * (float)sin(i * 2 * 3.14159265 * 440 / 48000);
		uint64_t ts = os_gettime_ns();
		while (t->run) {
			obs_source_audio a = {};
			a.data[0] = (uint8_t *)buf;
			a.data[1] = (uint8_t *)buf;
			a.frames = 480;
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

static std::atomic<int> g_started{0}, g_failed{0}, g_stopped{0};
static int g_fail = 0;

static void check(const char *what, bool ok, const std::string &detail = {})
{
	printf("  %-52s %s%s%s\n", what, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
	if (!ok)
		g_fail++;
}

static bool wait_for(std::atomic<int> &counter, int target, int ms)
{
	for (int i = 0; i < ms / 50; i++) {
		if (counter >= target)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return counter >= target;
}

static void load_module(const char *dir, const char *name)
{
	obs_module_t *m = nullptr;
	std::string bin = std::string(dir) + "/" + name + ".so";
	std::string data = std::string(getenv("OBS_SRC")) + "/plugins/" + name + "/data";
	if (obs_open_module(&m, bin.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(m))
		printf("could not load %s\n", bin.c_str());
}

int main()
{
	Display *dpy = XOpenDisplay(nullptr);
	if (!dpy)
		return 2;
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(dpy);
	if (!obs_startup("en-US", nullptr, nullptr))
		return 2;
	obs_audio_info ai = {48000, SPEAKERS_STEREO};
	obs_reset_audio(&ai);
	obs_video_info ovi = {};
	ovi.graphics_module = getenv("OBS_TEST_GL");
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 1280;
	ovi.base_height = ovi.output_height = 720;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 3;

	const char *plugins = getenv("OBS_PLUGINS");
	load_module(plugins, "obs-outputs");
	load_module(plugins, "rtmp-services");
	load_module(plugins, "obs-x264");
	load_module(plugins, "obs-ffmpeg");
	obs_post_load_modules();

	obs_source_info box = {};
	box.id = "vp_box";
	box.type = OBS_SOURCE_TYPE_INPUT;
	box.output_flags = OBS_SOURCE_VIDEO;
	box.get_name = box_name;
	box.create = box_create;
	box.destroy = box_destroy;
	box.get_width = box_w;
	box.get_height = box_h;
	box.video_render = box_render;
	obs_register_source(&box);
	obs_source_info tone = {};
	tone.id = "vp_tone";
	tone.type = OBS_SOURCE_TYPE_INPUT;
	tone.output_flags = OBS_SOURCE_AUDIO;
	tone.get_name = tone_name;
	tone.create = tone_create;
	tone.destroy = tone_destroy;
	obs_register_source(&tone);

	obs_video_info vovi = ovi;
	vovi.base_width = vovi.output_width = 1080;
	vovi.base_height = vovi.output_height = 1920;
	g_canvas = obs_canvas_create(VPLUS_CANVAS_NAME, &vovi, ACTIVATE | MIX_AUDIO | SCENE_REF);
	obs_scene_t *scene = obs_canvas_scene_create(g_canvas, "Vertical");
	obs_source_t *pic = obs_source_create("vp_box", "pic", nullptr, nullptr);
	obs_source_t *tn = obs_source_create("vp_tone", "tone", nullptr, nullptr);
	obs_scene_add(scene, pic);
	obs_scene_add(scene, tn);
	obs_canvas_set_channel(g_canvas, 0, obs_scene_get_source(scene));

	vplus::outputs::on_event([](vplus::outputs::Event e) {
		if (e == vplus::outputs::Event::StreamStarted)
			g_started++;
		if (e == vplus::outputs::Event::StreamFailed)
			g_failed++;
		if (e == vplus::outputs::Event::StreamStopped)
			g_stopped++;
	});

	vplus::Settings &s = g_settings;
	s.video_encoder = "obs_x264";
	s.video_bitrate = 2500;
	s.audio_track = 6;

	printf("Settings checks\n");
	s.platform = "tiktok";
	s.server = "";
	s.key = "abc";
	check("TikTok with no server URL refuses to start", !vplus::outputs::start_stream());
	check("  and says the server is missing",
	      vplus::outputs::stream_error().find("StreamError.NoServer") == 0, vplus::outputs::stream_error());
	s.platform = "youtube";
	s.key = "   ";
	check("YouTube with blank key refuses to start", !vplus::outputs::start_stream());
	check("  and says the key is missing", vplus::outputs::stream_error().find("StreamError.NoKey") == 0);
	s.platform = "custom";
	s.server = "abcd-1234-efgh";
	s.key = "rtmp://example.com/live";
	check("Server and key swapped is caught", !vplus::outputs::start_stream());
	check("  with a format hint", vplus::outputs::stream_error() == "StreamError.ServerFormat");

	printf("Connection refused (nothing listening)\n");
	s.server = "rtmp://127.0.0.1:19351/live";
	s.key = "testkey";
	check("start_stream accepts the settings", vplus::outputs::start_stream());
	const bool failed = wait_for(g_failed, 1, 15000);
	check("stream reports a failure", failed);
	check("  explaining it could not connect",
	      vplus::outputs::stream_error().find("StreamError.ConnectFailed") == 0, vplus::outputs::stream_error());
	check("  and is not left running", !vplus::outputs::streaming() && !vplus::outputs::stream_connecting());

	printf("Live stream to a local RTMP server\n");
	const char *out = "/tmp/vplus-stream-test.flv";
	remove(out);
	std::string cmd = std::string("ffmpeg -hide_banner -loglevel error -y -listen 1 -timeout 20 ") +
			  "-i rtmp://127.0.0.1:19350/live/testkey -t 5 -c copy -f flv " + out + " &";
	if (system(cmd.c_str()) != 0)
		printf("could not start ffmpeg\n");
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));

	s.server = "  rtmp://127.0.0.1:19350/live  "; /* stray spaces from copy/paste */
	s.key = "testkey\n";
	check("start_stream", vplus::outputs::start_stream());
	check("connects", wait_for(g_started, 1, 15000));
	check("streaming() is true", vplus::outputs::streaming());
	std::this_thread::sleep_for(std::chrono::seconds(7));
	vplus::outputs::stop_stream();
	check("stops cleanly", wait_for(g_stopped, 1, 10000));
	check("no error after a normal stop", vplus::outputs::stream_error().empty(), vplus::outputs::stream_error());
	std::this_thread::sleep_for(std::chrono::seconds(2));

	std::string probe = std::string("ffprobe -v error -show_entries stream=codec_type,codec_name,width,height,channels "
					"-of compact=p=0:nk=1 ") +
			    out + " 2>&1";
	std::string result;
	if (FILE *p = popen(probe.c_str(), "r")) {
		char line[256];
		while (fgets(line, sizeof(line), p))
			result += line;
		pclose(p);
	}
	printf("  server received:\n%s", result.c_str());
	check("server got H.264 video at 1080x1920", result.find("h264|video|1080|1920") != std::string::npos);
	check("server got AAC audio", result.find("aac|audio") != std::string::npos);

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");

	vplus::outputs::shutdown();
	obs_canvas_set_channel(g_canvas, 0, nullptr);
	obs_source_release(pic);
	obs_source_release(tn);
	obs_scene_release(scene);
	obs_canvas_release(g_canvas);
	obs_shutdown();
	return g_fail ? 1 : 0;
}

/*
 * Build (Linux, against a local obs-studio build with the plugins above):
 *   g++ -std=c++17 -I ../src -I <obs>/libobs -I <obs>/frontend/api test-stream.cpp ../src/outputs.cpp \
 *       -L<build>/libobs -lobs -lX11 -o bin/test-stream
 *   OBS_TEST_GL=<libobs-opengl.so> OBS_PLUGINS=<dir with obs-outputs.so etc> OBS_SRC=<obs> xvfb-run -a ./bin/test-stream
 */
