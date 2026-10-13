/*
 * Headless test of preview move/resize against real libobs: for each kind
 * of scene item transform the dock can meet, resize it with the same code
 * the dock uses and check where OBS actually puts it.
 *
 * Build like tests/test-audio-bus.cpp (needs libobs + libobs-opengl and a display):
 *   g++ -std=c++17 -I ../src -I <obs>/libobs test-preview-edit.cpp -L<build>/libobs -lobs -lX11 -o bin/test-preview-edit
 *   OBS_TEST_GL=<path to libobs-opengl.so> xvfb-run -a ./bin/test-preview-edit
 */
#include "item-edit.hpp"

#include <obs-nix-platform.h>
#include <X11/Xlib.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace vplus;

static int g_fail = 0;

static const char *box_name(void *)
{
	return "test box";
}
static void *box_create(obs_data_t *, obs_source_t *s)
{
	return s;
}
static void box_destroy(void *) {}
static uint32_t box_w(void *)
{
	return 1280;
}
static uint32_t box_h(void *)
{
	return 720;
}
static void box_render(void *, gs_effect_t *) {}

static void wait_ticks()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(150));
}

static void expect_rect(const char *what, const xform::Rect &got, const xform::Rect &want)
{
	const float e = std::max({std::fabs(got.x - want.x), std::fabs(got.y - want.y), std::fabs(got.w - want.w),
				  std::fabs(got.h - want.h)});
	const bool ok = e <= 1.01f; /* OBS rounds scaled sizes to whole pixels */
	printf("  %-44s got %7.1f %7.1f %7.1f x %6.1f  want %7.1f %7.1f %7.1f x %6.1f  %s\n", what, got.x, got.y, got.w,
	       got.h, want.x, want.y, want.w, want.h, ok ? "PASS" : "FAIL");
	if (!ok)
		g_fail++;
}

struct Case {
	const char *name;
	obs_transform_info info;
	int handle;
	float dragX, dragY; /* pointer offset from the handle's start position */
	bool freeAspect;
};

static void run_case(obs_sceneitem_t *item, const Case &c)
{
	obs_sceneitem_set_info2(item, &c.info);
	obs_sceneitem_crop crop = {};
	obs_sceneitem_set_crop(item, &crop);
	wait_ticks();

	const xform::Rect r0 = item_rect(item);
	obs_transform_info info0;
	obs_sceneitem_get_info2(item, &info0);

	float fx, fy;
	xform::handle_fraction(c.handle, fx, fy);
	const float px = r0.x + r0.w * fx + c.dragX, py = r0.y + r0.h * fy + c.dragY;
	const xform::Rect want = xform::resize(r0, c.handle, px, py, c.freeAspect, 8.f);

	apply_resize(item, info0, r0, want);
	wait_ticks();
	expect_rect(c.name, item_rect(item), want);
}

static obs_transform_info base_info()
{
	obs_transform_info i = {};
	vec2_set(&i.scale, 1.f, 1.f);
	i.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
	i.bounds_type = OBS_BOUNDS_NONE;
	i.bounds_alignment = OBS_ALIGN_CENTER;
	return i;
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
	ovi.base_width = ovi.output_width = 1920;
	ovi.base_height = ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS)
		return 3;

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

	obs_video_info vovi = ovi;
	vovi.base_width = vovi.output_width = 1080;
	vovi.base_height = vovi.output_height = 1920;
	obs_canvas_t *canvas = obs_canvas_create("Vertical+", &vovi, ACTIVATE | MIX_AUDIO | SCENE_REF);
	obs_scene_t *scene = obs_canvas_scene_create(canvas, "Vertical");
	obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));

	obs_source_t *src = obs_source_create("vp_box", "cam", nullptr, nullptr);
	obs_sceneitem_t *item = obs_scene_add(scene, src);

	printf("Resize (source 1280x720 on a 1080x1920 vertical canvas)\n");

	Case a{"scale, top-left align, corner", base_info(), xform::BottomRight, 200.f, 0.f, false};
	vec2_set(&a.info.pos, 100.f, 200.f);
	vec2_set(&a.info.scale, .5f, .5f);
	run_case(item, a);

	Case b{"scale, centre align, top-left corner", base_info(), xform::TopLeft, -100.f, -40.f, false};
	b.info.alignment = OBS_ALIGN_CENTER;
	vec2_set(&b.info.pos, 540.f, 960.f);
	vec2_set(&b.info.scale, .4f, .4f);
	run_case(item, b);

	Case c{"scale, right edge stretch", base_info(), xform::Right, -150.f, 0.f, false};
	vec2_set(&c.info.pos, 0.f, 300.f);
	vec2_set(&c.info.scale, .6f, .6f);
	run_case(item, c);

	Case d{"bounds fit (dock 'Fit'), bottom-left", base_info(), xform::BottomLeft, 300.f, -500.f, false};
	d.info.bounds_type = OBS_BOUNDS_SCALE_INNER;
	vec2_set(&d.info.pos, 0.f, 0.f);
	vec2_set(&d.info.bounds, 1080.f, 1920.f);
	run_case(item, d);

	Case e{"bounds fill + crop, bottom-right align", base_info(), xform::Top, 0.f, 300.f, false};
	e.info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
	e.info.crop_to_bounds = true;
	e.info.alignment = OBS_ALIGN_RIGHT | OBS_ALIGN_BOTTOM;
	vec2_set(&e.info.pos, 1080.f, 1920.f);
	vec2_set(&e.info.bounds, 1080.f, 960.f);
	run_case(item, e);

	Case f{"flipped horizontally, corner", base_info(), xform::BottomRight, 120.f, 60.f, false};
	vec2_set(&f.info.pos, 800.f, 100.f);
	vec2_set(&f.info.scale, -.5f, .5f);
	run_case(item, f);

	Case g{"free stretch (Shift) corner", base_info(), xform::TopRight, 200.f, 100.f, true};
	vec2_set(&g.info.pos, 50.f, 1200.f);
	vec2_set(&g.info.scale, .5f, .5f);
	run_case(item, g);

	printf("Move and centre\n");
	{
		obs_transform_info i = base_info();
		i.alignment = OBS_ALIGN_CENTER;
		vec2_set(&i.pos, 300.f, 400.f);
		vec2_set(&i.scale, .5f, .5f);
		obs_sceneitem_set_info2(item, &i);
		wait_ticks();
		place_item_at(item, 0.f, 1560.f);
		wait_ticks();
		expect_rect("place top-left at (0,1560)", item_rect(item), {0.f, 1560.f, 640.f, 360.f});
		const xform::Rect r = item_rect(item);
		place_item_at(item, (1080.f - r.w) * .5f, (1920.f - r.h) * .5f);
		wait_ticks();
		expect_rect("centre on canvas", item_rect(item), {220.f, 780.f, 640.f, 360.f});
	}

	printf("Hit testing\n");
	{
		const bool in = item_contains(item, 540.f, 960.f), out = item_contains(item, 100.f, 100.f);
		printf("  %-44s %s\n", "inside / outside", in && !out ? "PASS" : "FAIL");
		if (!(in && !out))
			g_fail++;
		obs_sceneitem_select(item, true);
		const bool sel = selected_item(scene) == item;
		select_only(scene, nullptr);
		const bool none = selected_item(scene) == nullptr;
		printf("  %-44s %s\n", "select / deselect", sel && none ? "PASS" : "FAIL");
		if (!(sel && none))
			g_fail++;
	}

	printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");

	obs_canvas_set_channel(canvas, 0, nullptr);
	obs_source_release(src);
	obs_scene_release(scene);
	obs_canvas_release(canvas);
	obs_shutdown();
	return g_fail ? 1 : 0;
}
