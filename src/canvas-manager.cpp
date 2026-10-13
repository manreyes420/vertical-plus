/*
 * Vertical+ canvas manager: finds or creates the vertical canvas, owns the
 * program source on channel 0, switches scenes, parks the canvas when idle
 * and registers hotkeys.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#include <algorithm>
#include <list>

namespace vplus::canvas {

static const uint32_t kCanvasFlags = ACTIVATE | MIX_AUDIO | SCENE_REF; /* == PROGRAM */

static obs_canvas_t *g_canvas = nullptr;  /* strong ref */
static obs_source_t *g_program = nullptr; /* strong ref, private */
static bool g_parked = true;
static bool g_preview_visible = false;
static std::string g_current;
static std::vector<SceneChangedFn> g_scene_cbs;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */

static obs_video_info make_ovi()
{
	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = settings().width;
	ovi.base_height = settings().height;
	ovi.output_width = settings().width;
	ovi.output_height = settings().height;
	return ovi;
}

static bool enum_names(void *param, obs_source_t *src)
{
	static_cast<std::vector<std::string> *>(param)->emplace_back(obs_source_get_name(src));
	return true;
}

static void notify_scene_changed()
{
	for (auto &fn : g_scene_cbs)
		fn(g_current);
}

/* An existing canvas created without MIX_AUDIO (e.g. by an older build)
 * cannot change flags, so move its scenes into a correctly flagged one. */
static obs_canvas_t *migrate_canvas(obs_canvas_t *old_canvas)
{
	obs_video_info ovi = make_ovi();
	obs_canvas_t *fresh = obs_frontend_add_canvas(VPLUS_CANVAS_NAME " (new)", &ovi, kCanvasFlags);
	if (!fresh)
		return old_canvas;

	std::vector<obs_source_t *> scenes;
	obs_canvas_enum_scenes(
		old_canvas,
		[](void *param, obs_source_t *src) {
			static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(src));
			return true;
		},
		&scenes);
	for (obs_source_t *src : scenes) {
		if (obs_scene_t *scene = obs_scene_from_source(src))
			obs_canvas_move_scene(scene, fresh);
		obs_source_release(src);
	}

	obs_frontend_remove_canvas(old_canvas);
	obs_canvas_release(old_canvas);
	obs_canvas_set_name(fresh, VPLUS_CANVAS_NAME);
	vlog(LOG_INFO, "migrated %zu scene(s) to a canvas with isolated audio", scenes.size());
	return fresh;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */

bool init()
{
	if (g_canvas)
		shutdown();

	obs_video_info ovi = make_ovi();

	g_canvas = obs_get_canvas_by_name(VPLUS_CANVAS_NAME);
	if (g_canvas) {
		if ((obs_canvas_get_flags(g_canvas) & kCanvasFlags) != kCanvasFlags)
			g_canvas = migrate_canvas(g_canvas);

		/* Canvases are saved without video info; give it one. */
		obs_video_info cur = {};
		if (!obs_canvas_get_video_info(g_canvas, &cur) || cur.base_width != ovi.base_width ||
		    cur.base_height != ovi.base_height)
			obs_canvas_reset_video(g_canvas, &ovi);
	} else {
		g_canvas = obs_frontend_add_canvas(VPLUS_CANVAS_NAME, &ovi, kCanvasFlags);
	}

	if (!g_canvas) {
		vlog(LOG_ERROR, "could not create the vertical canvas");
		return false;
	}

	if (scene_names().empty())
		create_scene(obs_module_text("DefaultSceneName"));

	if (!g_program)
		g_program = obs_source_create_private(VPLUS_PROGRAM_ID, "Vertical+ Program", nullptr);
	program_resize(g_program, settings().width, settings().height);

	/* Restore the last scene without a transition. */
	std::vector<std::string> names = scene_names();
	std::string target = settings().current_scene;
	if (std::find(names.begin(), names.end(), target) == names.end())
		target = names.empty() ? std::string() : names.front();
	if (!target.empty()) {
		if (obs_scene_t *scene = get_scene(target.c_str())) {
			obs_transition_set(transition(), obs_scene_get_source(scene));
			obs_scene_release(scene);
			g_current = target;
		}
	}

	g_parked = true;
	update_parking();
	refresh_hotkeys();
	notify_scene_changed();
	vlog(LOG_INFO, "vertical canvas ready (%ux%u, %zu scenes)", settings().width, settings().height,
	     names.size());
	return true;
}

void shutdown()
{
	save_hotkeys();
	if (g_canvas)
		obs_canvas_set_channel(g_canvas, 0, nullptr);
	g_parked = true;
	if (obs_source_t *t = transition())
		obs_transition_set(t, nullptr);
	if (g_canvas) {
		obs_canvas_release(g_canvas);
		g_canvas = nullptr;
	}
	g_current.clear();
}

/* Final teardown at module unload. */
void destroy()
{
	shutdown();
	if (g_program) {
		obs_source_release(g_program);
		g_program = nullptr;
	}
}

obs_canvas_t *get()
{
	return g_canvas;
}

obs_source_t *program()
{
	return g_program;
}

obs_source_t *transition()
{
	return program_transition(g_program);
}

void apply_video_settings()
{
	if (!g_canvas)
		return;
	if (outputs::any_active()) {
		vlog(LOG_WARNING, "canvas size change deferred: vertical outputs are running");
		return;
	}
	obs_video_info ovi = make_ovi();
	if (!obs_canvas_reset_video(g_canvas, &ovi))
		vlog(LOG_ERROR, "could not resize the vertical canvas");
	program_resize(g_program, settings().width, settings().height);
}

/* ------------------------------------------------------------------ */
/* Scenes                                                             */

std::vector<std::string> scene_names()
{
	std::vector<std::string> names;
	if (g_canvas)
		obs_canvas_enum_scenes(g_canvas, enum_names, &names);
	return names;
}

obs_scene_t *get_scene(const char *name)
{
	if (!g_canvas || !name || !*name)
		return nullptr;
	return obs_canvas_get_scene_by_name(g_canvas, name);
}

std::string current_scene()
{
	return g_current;
}

bool set_scene(const std::string &name)
{
	obs_scene_t *scene = get_scene(name.c_str());
	if (!scene)
		return false;

	obs_source_t *t = transition();
	obs_source_t *src = obs_scene_get_source(scene);
	if (settings().transition_ms == 0 || g_parked)
		obs_transition_set(t, src);
	else
		obs_transition_start(t, OBS_TRANSITION_MODE_AUTO, settings().transition_ms, src);
	obs_scene_release(scene);

	g_current = name;
	settings().current_scene = name;
	notify_scene_changed();
	links::refresh();
	return true;
}

bool create_scene(const char *name)
{
	if (!g_canvas || !name || !*name)
		return false;
	if (obs_scene_t *existing = get_scene(name)) {
		obs_scene_release(existing);
		return false;
	}
	obs_scene_t *scene = obs_canvas_scene_create(g_canvas, name);
	if (!scene)
		return false;
	obs_scene_release(scene); /* the canvas (SCENE_REF) keeps it alive */
	refresh_hotkeys();
	links::refresh();
	return true;
}

bool remove_scene(const char *name)
{
	std::vector<std::string> names = scene_names();
	if (names.size() <= 1)
		return false; /* always keep one scene */

	if (g_current == name) {
		for (const std::string &n : names)
			if (n != name) {
				set_scene(n);
				break;
			}
	}

	obs_scene_t *scene = get_scene(name);
	if (!scene)
		return false;
	obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);
	refresh_hotkeys();
	links::refresh();
	return true;
}

bool rename_scene(const char *old_name, const char *new_name)
{
	if (!new_name || !*new_name)
		return false;
	if (obs_scene_t *clash = get_scene(new_name)) {
		obs_scene_release(clash);
		return false;
	}
	obs_scene_t *scene = get_scene(old_name);
	if (!scene)
		return false;
	obs_source_set_name(obs_scene_get_source(scene), new_name);
	obs_scene_release(scene);
	if (g_current == old_name) {
		g_current = new_name;
		settings().current_scene = new_name;
	}
	refresh_hotkeys(); /* descriptions carry the name */
	return true;
}

void on_scene_changed(SceneChangedFn fn)
{
	g_scene_cbs.push_back(std::move(fn));
}

/* ------------------------------------------------------------------ */
/* Idle parking                                                       */

void set_preview_visible(bool visible)
{
	g_preview_visible = visible;
	update_parking();
}

bool parked()
{
	return g_parked;
}

void update_parking()
{
	if (!g_canvas || !g_program)
		return;

	const bool needed = !settings().idle_parking || outputs::any_active() || g_preview_visible;
	if (needed && g_parked) {
		obs_canvas_set_channel(g_canvas, 0, g_program);
		g_parked = false;
		vlog(LOG_DEBUG, "vertical canvas active");
	} else if (!needed && !g_parked) {
		obs_canvas_set_channel(g_canvas, 0, nullptr);
		g_parked = true;
		vlog(LOG_DEBUG, "vertical canvas parked");
	}
	program_activate_tracks(outputs::any_active());
}

/* ------------------------------------------------------------------ */
/* Hotkeys                                                            */

struct SceneHotkey {
	std::string uuid;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

static std::list<SceneHotkey> g_scene_hotkeys;
static obs_hotkey_pair_id g_stream_pair = OBS_INVALID_HOTKEY_PAIR_ID;
static obs_hotkey_pair_id g_record_pair = OBS_INVALID_HOTKEY_PAIR_ID;

static obs_data_t *hotkey_store()
{
	obs_data_t *extra = settings_extra();
	obs_data_t *hk = obs_data_get_obj(extra, "hotkeys");
	if (!hk) {
		hk = obs_data_create();
		obs_data_set_obj(extra, "hotkeys", hk);
	}
	return hk; /* caller releases */
}

static void scene_hotkey_pressed(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	std::string uuid = static_cast<SceneHotkey *>(data)->uuid;
	run_on_ui([uuid]() {
		obs_source_t *src = obs_get_source_by_uuid(uuid.c_str());
		if (!src)
			return;
		set_scene(obs_source_get_name(src));
		obs_source_release(src);
	});
}

static bool stream_start_hk(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || outputs::streaming())
		return false;
	run_on_ui([]() { outputs::start_stream(); });
	return true;
}

static bool stream_stop_hk(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !outputs::streaming())
		return false;
	run_on_ui([]() { outputs::stop_stream(); });
	return true;
}

static bool record_start_hk(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || outputs::recording())
		return false;
	run_on_ui([]() { outputs::start_record(); });
	return true;
}

static bool record_stop_hk(void *, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed || !outputs::recording())
		return false;
	run_on_ui([]() { outputs::stop_record(); });
	return true;
}

static void load_pair(obs_data_t *hk, obs_hotkey_pair_id id, const char *k0, const char *k1)
{
	obs_data_array_t *a0 = obs_data_get_array(hk, k0);
	obs_data_array_t *a1 = obs_data_get_array(hk, k1);
	obs_hotkey_pair_load(id, a0, a1);
	obs_data_array_release(a0);
	obs_data_array_release(a1);
}

static void save_pair(obs_data_t *hk, obs_hotkey_pair_id id, const char *k0, const char *k1)
{
	obs_data_array_t *a0 = nullptr, *a1 = nullptr;
	obs_hotkey_pair_save(id, &a0, &a1);
	obs_data_set_array(hk, k0, a0);
	obs_data_set_array(hk, k1, a1);
	obs_data_array_release(a0);
	obs_data_array_release(a1);
}

void save_hotkeys()
{
	obs_data_t *hk = hotkey_store();
	for (SceneHotkey &e : g_scene_hotkeys) {
		obs_data_array_t *arr = obs_hotkey_save(e.id);
		obs_data_set_array(hk, ("scene." + e.uuid).c_str(), arr);
		obs_data_array_release(arr);
	}
	if (g_stream_pair != OBS_INVALID_HOTKEY_PAIR_ID)
		save_pair(hk, g_stream_pair, "stream.start", "stream.stop");
	if (g_record_pair != OBS_INVALID_HOTKEY_PAIR_ID)
		save_pair(hk, g_record_pair, "record.start", "record.stop");
	obs_data_release(hk);
}

static bool enum_scene_sources(void *param, obs_source_t *src)
{
	static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(src));
	return true;
}

void refresh_hotkeys()
{
	save_hotkeys();
	for (SceneHotkey &e : g_scene_hotkeys)
		obs_hotkey_unregister(e.id);
	g_scene_hotkeys.clear();

	obs_data_t *hk = hotkey_store();

	if (g_stream_pair == OBS_INVALID_HOTKEY_PAIR_ID) {
		g_stream_pair = obs_hotkey_pair_register_frontend(
			"vplus.stream.start", obs_module_text("Hotkey.StartStream"), "vplus.stream.stop",
			obs_module_text("Hotkey.StopStream"), stream_start_hk, stream_stop_hk, nullptr, nullptr);
		load_pair(hk, g_stream_pair, "stream.start", "stream.stop");
	}
	if (g_record_pair == OBS_INVALID_HOTKEY_PAIR_ID) {
		g_record_pair = obs_hotkey_pair_register_frontend(
			"vplus.record.start", obs_module_text("Hotkey.StartRecord"), "vplus.record.stop",
			obs_module_text("Hotkey.StopRecord"), record_start_hk, record_stop_hk, nullptr, nullptr);
		load_pair(hk, g_record_pair, "record.start", "record.stop");
	}

	if (g_canvas) {
		std::vector<obs_source_t *> scenes;
		obs_canvas_enum_scenes(g_canvas, enum_scene_sources, &scenes);
		for (obs_source_t *src : scenes) {
			SceneHotkey &e = g_scene_hotkeys.emplace_back();
			e.uuid = obs_source_get_uuid(src);
			std::string name = "vplus.scene." + e.uuid;
			std::string desc = std::string(obs_module_text("Hotkey.SwitchTo")) + " " +
					   obs_source_get_name(src);
			e.id = obs_hotkey_register_frontend(name.c_str(), desc.c_str(), scene_hotkey_pressed, &e);

			obs_data_array_t *arr = obs_data_get_array(hk, ("scene." + e.uuid).c_str());
			if (arr) {
				obs_hotkey_load(e.id, arr);
				obs_data_array_release(arr);
			}
			obs_source_release(src);
		}
	}
	obs_data_release(hk);
}

void unregister_hotkeys()
{
	save_hotkeys();
	for (SceneHotkey &e : g_scene_hotkeys)
		obs_hotkey_unregister(e.id);
	g_scene_hotkeys.clear();
	if (g_stream_pair != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(g_stream_pair);
	if (g_record_pair != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(g_record_pair);
	g_stream_pair = g_record_pair = OBS_INVALID_HOTKEY_PAIR_ID;
}

} // namespace vplus::canvas
