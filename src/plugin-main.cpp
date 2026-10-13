/*
 * Vertical+ for OBS Studio - module entry point and frontend wiring.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"


OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("vertical-plus", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Vertical+: a vertical canvas with isolated audio, linked visibility and automation.";
}

namespace vplus {

void run_on_ui(std::function<void()> fn)
{
	auto *heap = new std::function<void()>(std::move(fn));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			auto *f = static_cast<std::function<void()> *>(p);
			(*f)();
			delete f;
		},
		heap, false);
}

static bool g_ready = false;

static void stop_outputs_for_switch()
{
	if (outputs::any_active()) {
		vlog(LOG_INFO, "stopping vertical outputs for a scene collection or profile switch");
		outputs::stop_stream();
		outputs::stop_record();
	}
}

static void load_collection()
{
	reserved_track_watch(settings().enforce_reserved_track);
	canvas::init();
	outputs::reset_encoders();
	links::refresh();
	dock_refresh();
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		settings_load();
		load_collection();
		g_ready = true;
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		if (!g_ready)
			break;
		stop_outputs_for_switch();
		settings_save();
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		if (!g_ready)
			break;
		links::shutdown();
		canvas::shutdown();
		break;

	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		if (g_ready)
			load_collection();
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		if (!g_ready)
			break;
		stop_outputs_for_switch();
		canvas::save_hotkeys();
		settings_save();
		break;

	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		if (!g_ready)
			break;
		settings_load(); /* each profile has its own keys and sizes */
		canvas::apply_video_settings();
		canvas::refresh_hotkeys();
		load_collection();
		break;

	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		if (g_ready)
			links::refresh();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (g_ready && settings().start_with_main_stream)
			outputs::start_stream();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		if (g_ready && settings().start_with_main_stream)
			outputs::stop_stream();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (g_ready && settings().start_with_main_record)
			outputs::start_record();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (g_ready && settings().start_with_main_record)
			outputs::stop_record();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		ws::shutdown(); /* before obs-websocket can unload */
		if (g_ready) {
			canvas::save_hotkeys();
			settings_save();
		}
		outputs::shutdown();
		links::shutdown();
		canvas::unregister_hotkeys();
		canvas::destroy();
		reserved_track_watch(false);
		g_ready = false;
		break;

	default:
		break;
	}
}

static void wire_events()
{
	canvas::on_scene_changed([](const std::string &name) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "sceneName", name.c_str());
		ws::emit("SceneChanged", d);
		obs_data_release(d);
	});
	outputs::on_event([](outputs::Event e) {
		obs_data_t *d = obs_data_create();
		switch (e) {
		case outputs::Event::StreamStarted:
		case outputs::Event::StreamStopped:
			obs_data_set_bool(d, "active", e == outputs::Event::StreamStarted);
			ws::emit("StreamStateChanged", d);
			break;
		case outputs::Event::RecordStarted:
		case outputs::Event::RecordStopped:
			obs_data_set_bool(d, "active", e == outputs::Event::RecordStarted);
			obs_data_set_string(d, "outputPath", outputs::last_record_path().c_str());
			ws::emit("RecordStateChanged", d);
			break;
		}
		obs_data_release(d);
	});
}

} // namespace vplus

bool obs_module_load(void)
{
	if (obs_get_version() < MAKE_SEMANTIC_VERSION(32, 0, 0)) {
		vlog(LOG_ERROR, "OBS Studio 32.0 or newer is required (native canvases); plugin disabled");
		return false;
	}

	vplus::program_register_source();
	vplus::reframe_register_source();
	vplus::settings_register_source();

	obs_frontend_add_event_callback(vplus::on_frontend_event, nullptr);
	obs_frontend_add_tools_menu_item(
		obs_module_text("VerticalPlus.SettingsMenu"), [](void *) { vplus::settings_open_dialog(); }, nullptr);

	vlog(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	vplus::outputs::init();
	vplus::wire_events();
	vplus::ws::init();
	vplus::dock_create();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(vplus::on_frontend_event, nullptr);
	vplus::settings_shutdown();
}
