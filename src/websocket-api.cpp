/*
 * obs-websocket vendor "vertical-plus".
 *
 * Call with CallVendorRequest:
 *   { "vendorName": "vertical-plus", "requestType": "SetCurrentScene",
 *     "requestData": { "sceneName": "Gameplay" } }
 *
 * Every response carries "success" (bool) and, on failure, "error".
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"
#include "external/obs-websocket-api.h"

#include <functional>

namespace vplus::ws {

static obs_websocket_vendor g_vendor = nullptr;

/* Requests arrive on the websocket thread; run them on the UI thread and
 * wait so the response can be filled in. */
static void run_sync(std::function<void()> fn)
{
	if (obs_in_task_thread(OBS_TASK_UI)) {
		fn();
		return;
	}
	obs_queue_task(
		OBS_TASK_UI, [](void *p) { (*static_cast<std::function<void()> *>(p))(); }, &fn, true);
}

static void fail(obs_data_t *res, const char *msg)
{
	obs_data_set_bool(res, "success", false);
	obs_data_set_string(res, "error", msg);
}

static void ok(obs_data_t *res)
{
	obs_data_set_bool(res, "success", true);
}

static void req_get_status(obs_data_t *, obs_data_t *res, void *)
{
	run_sync([res]() {
		obs_data_set_int(res, "width", settings().width);
		obs_data_set_int(res, "height", settings().height);
		obs_data_set_string(res, "currentScene", canvas::current_scene().c_str());
		obs_data_set_bool(res, "streaming", outputs::streaming());
		obs_data_set_bool(res, "recording", outputs::recording());
		obs_data_set_bool(res, "parked", canvas::parked());
		obs_data_set_int(res, "audioTrack", settings().audio_track);
		ok(res);
	});
}

static void req_get_scene_list(obs_data_t *, obs_data_t *res, void *)
{
	run_sync([res]() {
		obs_data_array_t *arr = obs_data_array_create();
		for (const std::string &name : canvas::scene_names()) {
			obs_data_t *item = obs_data_create();
			obs_data_set_string(item, "sceneName", name.c_str());
			if (obs_scene_t *s = canvas::get_scene(name.c_str())) {
				obs_data_set_string(item, "sceneUuid", obs_source_get_uuid(obs_scene_get_source(s)));
				obs_scene_release(s);
			}
			obs_data_array_push_back(arr, item);
			obs_data_release(item);
		}
		obs_data_set_array(res, "scenes", arr);
		obs_data_array_release(arr);
		obs_data_set_string(res, "currentScene", canvas::current_scene().c_str());
		ok(res);
	});
}

static void req_set_current_scene(obs_data_t *req, obs_data_t *res, void *)
{
	std::string name = obs_data_get_string(req, "sceneName");
	run_sync([res, name]() {
		if (canvas::set_scene(name))
			ok(res);
		else
			fail(res, "no vertical scene with that name");
	});
}

static void req_set_item_visible(obs_data_t *req, obs_data_t *res, void *)
{
	std::string scene_name = obs_data_get_string(req, "sceneName");
	std::string source_name = obs_data_get_string(req, "sourceName");
	std::string which = obs_data_get_string(req, "canvas"); /* "vertical" (default) or "main" */
	const bool visible = obs_data_get_bool(req, "visible");
	const bool link = obs_data_has_user_value(req, "link") ? obs_data_get_bool(req, "link") : true;

	run_sync([=]() {
		obs_scene_t *scene = nullptr;
		if (which == "main") {
			obs_source_t *src = obs_get_source_by_name(scene_name.c_str());
			scene = obs_scene_from_source(src);
			if (scene)
				obs_scene_get_ref(scene);
			obs_source_release(src);
		} else {
			scene = canvas::get_scene(scene_name.c_str());
		}
		if (!scene) {
			fail(res, "scene not found");
			return;
		}
		if (links::set_visible(scene, source_name.c_str(), visible, link))
			ok(res);
		else
			fail(res, "source not found in scene");
		obs_scene_release(scene);
	});
}

static void req_start_stream(obs_data_t *, obs_data_t *res, void *)
{
	run_sync([res]() {
		if (outputs::start_stream())
			ok(res);
		else
			fail(res, "could not start the vertical stream (check server and key)");
	});
}

static void req_stop_stream(obs_data_t *, obs_data_t *res, void *)
{
	run_sync([res]() {
		outputs::stop_stream();
		ok(res);
	});
}

static void req_start_record(obs_data_t *, obs_data_t *res, void *)
{
	run_sync([res]() {
		if (outputs::start_record())
			ok(res);
		else
			fail(res, "could not start the vertical recording");
	});
}

static void req_stop_record(obs_data_t *, obs_data_t *res, void *)
{
	run_sync([res]() {
		outputs::stop_record();
		obs_data_set_string(res, "outputPath", outputs::last_record_path().c_str());
		ok(res);
	});
}

static void req_set_reframe_preset(obs_data_t *req, obs_data_t *res, void *)
{
	std::string source_name = obs_data_get_string(req, "sourceName");
	std::string preset = obs_data_get_string(req, "preset");
	const uint32_t duration = obs_data_has_user_value(req, "durationMs")
					  ? (uint32_t)obs_data_get_int(req, "durationMs")
					  : UINT32_MAX; /* use the source's own duration */
	run_sync([=]() {
		obs_source_t *src = obs_get_source_by_name(source_name.c_str());
		if (src && reframe_set_preset(src, preset.c_str(), duration))
			ok(res);
		else
			fail(res, "reframe source or preset not found");
		obs_source_release(src);
	});
}

void init()
{
	g_vendor = obs_websocket_register_vendor("vertical-plus");
	if (!g_vendor) {
		vlog(LOG_INFO, "obs-websocket not available; vendor requests disabled");
		return;
	}
	obs_websocket_vendor_register_request(g_vendor, "GetStatus", req_get_status, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "GetSceneList", req_get_scene_list, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "SetCurrentScene", req_set_current_scene, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "SetItemVisible", req_set_item_visible, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "StartStream", req_start_stream, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "StopStream", req_stop_stream, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "StartRecord", req_start_record, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "StopRecord", req_stop_record, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "SetReframePreset", req_set_reframe_preset, nullptr);
	vlog(LOG_INFO, "obs-websocket vendor 'vertical-plus' registered");
}

void shutdown()
{
	/* obs-websocket may already be unloaded at this point (the crash Aitum
	 * fixed in 1.2.5), so never call into it here. Dropping the handle is
	 * enough; the vendor dies with obs-websocket. */
	g_vendor = nullptr;
}

void emit(const char *event, obs_data_t *data)
{
	if (g_vendor)
		obs_websocket_vendor_emit_event(g_vendor, event, data);
}

} // namespace vplus::ws
