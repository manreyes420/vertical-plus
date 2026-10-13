/*
 * Main Canvas Reframe source.
 *
 * Draws a crop of the already-rendered main canvas frame, so a vertical
 * layout built from it costs a texture copy instead of rendering the game
 * capture, webcam and their filters a second time.
 *
 * Presets pan with an ease-in-out curve; two source hotkeys step through
 * them (bindings are saved with the source by libobs).
 *
 * Note: OBS renders the main canvas before plugin canvases, so the crop is
 * from the same frame. After a main video reset the order can flip, which
 * costs one frame of delay on the reframe source only.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#include <graphics/graphics.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace vplus {

struct Rect {
	float x = 0, y = 0, w = 0, h = 0;
};

static const char *kPresets[] = {"center", "left", "right", "full", "custom"};
static const size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

struct ReframeData {
	obs_source_t *self = nullptr;
	std::mutex mtx;

	std::string preset = "center";
	Rect custom;
	uint32_t duration_ms = 600;
	int64_t override_duration_ms = -1;

	Rect cur, from, to;
	float elapsed = 0.f, total = 0.f;
	bool animating = false;
	bool first = true;

	obs_hotkey_id hk_next = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_prev = OBS_INVALID_HOTKEY_ID;
};

static void main_size(uint32_t &w, uint32_t &h)
{
	obs_video_info ovi = {};
	if (obs_get_video_info(&ovi)) {
		w = ovi.base_width;
		h = ovi.base_height;
	} else {
		w = 1920;
		h = 1080;
	}
}

static Rect preset_rect(const std::string &preset, const Rect &custom)
{
	uint32_t mw, mh;
	main_size(mw, mh);
	const float W = (float)mw, H = (float)mh;
	const float cw = std::min(W, std::round(H * 9.f / 16.f)); /* 9:16 at full height */

	if (preset == "left")
		return {0.f, 0.f, cw, H};
	if (preset == "right")
		return {W - cw, 0.f, cw, H};
	if (preset == "full")
		return {0.f, 0.f, W, H};
	if (preset == "custom") {
		Rect r = custom;
		r.w = std::clamp(r.w, 16.f, W);
		r.h = std::clamp(r.h, 16.f, H);
		r.x = std::clamp(r.x, 0.f, W - r.w);
		r.y = std::clamp(r.y, 0.f, H - r.h);
		return r;
	}
	return {(W - cw) / 2.f, 0.f, cw, H}; /* center */
}

static inline float ease_in_out(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}

/* Caller holds d->mtx. */
static void retarget(ReframeData *d)
{
	Rect target = preset_rect(d->preset, d->custom);
	const uint32_t dur = d->override_duration_ms >= 0 ? (uint32_t)d->override_duration_ms : d->duration_ms;
	d->override_duration_ms = -1;

	if (d->first || dur == 0) {
		d->cur = d->to = target;
		d->animating = false;
		d->first = false;
		return;
	}
	d->from = d->cur;
	d->to = target;
	d->elapsed = 0.f;
	d->total = (float)dur / 1000.f;
	d->animating = true;
}

static const char *rf_get_name(void *)
{
	return obs_module_text("Reframe.Name");
}

static void rf_update(void *data, obs_data_t *settings)
{
	auto *d = static_cast<ReframeData *>(data);
	std::lock_guard<std::mutex> lock(d->mtx);
	d->preset = obs_data_get_string(settings, "preset");
	d->custom.x = (float)obs_data_get_int(settings, "x");
	d->custom.y = (float)obs_data_get_int(settings, "y");
	d->custom.w = (float)obs_data_get_int(settings, "w");
	d->custom.h = (float)obs_data_get_int(settings, "h");
	d->duration_ms = (uint32_t)obs_data_get_int(settings, "duration_ms");
	retarget(d);
}

static void step_preset(ReframeData *d, int dir)
{
	std::string next;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		size_t idx = 0;
		for (size_t i = 0; i < kPresetCount; i++)
			if (d->preset == kPresets[i])
				idx = i;
		/* Hotkeys step through the named framings; "custom" is skipped. */
		do {
			idx = (idx + kPresetCount + dir) % kPresetCount;
		} while (strcmp(kPresets[idx], "custom") == 0);
		next = kPresets[idx];
	}
	reframe_set_preset(d->self, next.c_str(), UINT32_MAX);
}

static void hk_next(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		step_preset(static_cast<ReframeData *>(data), +1);
}

static void hk_prev(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		step_preset(static_cast<ReframeData *>(data), -1);
}

static void *rf_create(obs_data_t *settings, obs_source_t *source)
{
	auto *d = new ReframeData();
	d->self = source;
	rf_update(d, settings);
	d->hk_next = obs_hotkey_register_source(source, "vplus.reframe.next", obs_module_text("Reframe.Next"),
						hk_next, d);
	d->hk_prev = obs_hotkey_register_source(source, "vplus.reframe.prev", obs_module_text("Reframe.Prev"),
						hk_prev, d);
	return d;
}

static void rf_destroy(void *data)
{
	delete static_cast<ReframeData *>(data);
}

static void rf_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "preset", "center");
	obs_data_set_default_int(settings, "x", 656);
	obs_data_set_default_int(settings, "y", 0);
	obs_data_set_default_int(settings, "w", 608);
	obs_data_set_default_int(settings, "h", 1080);
	obs_data_set_default_int(settings, "duration_ms", 600);
}

static bool preset_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool custom = strcmp(obs_data_get_string(settings, "preset"), "custom") == 0;
	for (const char *k : {"x", "y", "w", "h"})
		obs_property_set_visible(obs_properties_get(props, k), custom);
	return true;
}

static obs_properties_t *rf_properties(void *)
{
	uint32_t mw, mh;
	main_size(mw, mh);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "preset", obs_module_text("Reframe.Preset"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Reframe.Center"), "center");
	obs_property_list_add_string(p, obs_module_text("Reframe.Left"), "left");
	obs_property_list_add_string(p, obs_module_text("Reframe.Right"), "right");
	obs_property_list_add_string(p, obs_module_text("Reframe.Full"), "full");
	obs_property_list_add_string(p, obs_module_text("Reframe.Custom"), "custom");
	obs_property_set_modified_callback(p, preset_changed);

	obs_properties_add_int(props, "x", "X", 0, (int)mw, 1);
	obs_properties_add_int(props, "y", "Y", 0, (int)mh, 1);
	obs_properties_add_int(props, "w", obs_module_text("Width"), 16, (int)mw, 1);
	obs_properties_add_int(props, "h", obs_module_text("Height"), 16, (int)mh, 1);
	obs_properties_add_int_slider(props, "duration_ms", obs_module_text("Reframe.Duration"), 0, 3000, 50);
	return props;
}

static void rf_tick(void *data, float seconds)
{
	auto *d = static_cast<ReframeData *>(data);
	std::lock_guard<std::mutex> lock(d->mtx);
	if (!d->animating)
		return;
	d->elapsed += seconds;
	const float e = ease_in_out(d->total > 0.f ? d->elapsed / d->total : 1.f);
	d->cur.x = d->from.x + (d->to.x - d->from.x) * e;
	d->cur.y = d->from.y + (d->to.y - d->from.y) * e;
	d->cur.w = d->from.w + (d->to.w - d->from.w) * e;
	d->cur.h = d->from.h + (d->to.h - d->from.h) * e;
	if (d->elapsed >= d->total) {
		d->cur = d->to;
		d->animating = false;
	}
}

static uint32_t rf_width(void *data)
{
	auto *d = static_cast<ReframeData *>(data);
	std::lock_guard<std::mutex> lock(d->mtx);
	return (uint32_t)std::lround(d->cur.w);
}

static uint32_t rf_height(void *data)
{
	auto *d = static_cast<ReframeData *>(data);
	std::lock_guard<std::mutex> lock(d->mtx);
	return (uint32_t)std::lround(d->cur.h);
}

static void rf_render(void *data, gs_effect_t *)
{
	auto *d = static_cast<ReframeData *>(data);
	gs_texture_t *tex = obs_get_main_texture();
	if (!tex)
		return;
	/* Never sample the texture we are drawing into (reframe placed on the
	 * main canvas itself would feed back). */
	if (gs_get_render_target() == tex)
		return;

	Rect r;
	{
		std::lock_guard<std::mutex> lock(d->mtx);
		r = d->cur;
	}
	const uint32_t tw = gs_texture_get_width(tex), th = gs_texture_get_height(tex);
	uint32_t mw, mh;
	main_size(mw, mh);
	/* The main texture is at base resolution; scale if it ever differs. */
	const float sx = mw ? (float)tw / (float)mw : 1.f, sy = mh ? (float)th / (float)mh : 1.f;

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite_subregion(tex, 0, (uint32_t)std::lround(r.x * sx), (uint32_t)std::lround(r.y * sy),
					 (uint32_t)std::lround(r.w * sx), (uint32_t)std::lround(r.h * sy));
	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(prev_srgb);
}

void reframe_register_source()
{
	obs_source_info info = {};
	info.id = VPLUS_REFRAME_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = rf_get_name;
	info.create = rf_create;
	info.destroy = rf_destroy;
	info.update = rf_update;
	info.get_defaults = rf_defaults;
	info.get_properties = rf_properties;
	info.video_tick = rf_tick;
	info.video_render = rf_render;
	info.get_width = rf_width;
	info.get_height = rf_height;
	info.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
	obs_register_source(&info);
}

bool reframe_set_preset(obs_source_t *source, const char *preset, uint32_t duration_ms)
{
	if (!source || strcmp(obs_source_get_id(source), VPLUS_REFRAME_ID) != 0)
		return false;
	bool known = false;
	for (const char *p : kPresets)
		known = known || strcmp(p, preset) == 0;
	if (!known)
		return false;

	auto *d = static_cast<ReframeData *>(obs_obj_get_data(source));
	if (d && duration_ms != UINT32_MAX) {
		std::lock_guard<std::mutex> lock(d->mtx);
		d->override_duration_ms = duration_ms;
	}
	/* Persist through the source settings so it survives a restart. */
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "preset", preset);
	obs_source_update(source, s);
	obs_data_release(s);
	return true;
}

} // namespace vplus
