/*
 * Vertical+ for OBS Studio
 * A second, vertical (9:16) canvas with its own isolated audio, linked
 * visibility, a cheap reframe source and full automation access.
 *
 * Requires OBS Studio 32.0 or newer (native canvas API).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#define VPLUS_CANVAS_NAME "Vertical+"
#define VPLUS_PROGRAM_ID "vertical_plus_program"
#define VPLUS_REFRAME_ID "vertical_plus_reframe"
#define VPLUS_SETTINGS_SOURCE_ID "vertical_plus_settings"

#define vlog(level, format, ...) blog(level, "[vertical-plus] " format, ##__VA_ARGS__)

namespace vplus {

/* ------------------------------------------------------------------ */
/* Settings (settings.cpp)                                            */
/* Stored per OBS profile in <profile>/vertical-plus.json             */

struct Settings {
	uint32_t width = 1080;
	uint32_t height = 1920;

	int audio_track = 6; /* 1-based track reserved for vertical audio */
	bool enforce_reserved_track = true;

	bool idle_parking = true;
	bool link_visibility = true;
	bool link_all_scenes = false; /* false = only the current program scenes */

	uint32_t fps_divisor = 1;
	std::string video_encoder = "auto";
	int video_bitrate = 6000; /* kbps */
	int audio_bitrate = 160;  /* kbps */

	std::string platform = "tiktok"; /* see platforms.hpp */
	std::string server;              /* used when the platform has no fixed server */
	std::string key;

	std::string record_path;           /* empty = main recording folder */
	std::string record_format = "mp4"; /* mp4 | mkv | mov */

	bool start_with_main_stream = false;
	bool start_with_main_record = false;

	uint32_t transition_ms = 300;
	std::string current_scene;
};

Settings &settings();
void settings_load();
void settings_save();
/* Free-form persistent data (hotkey bindings). Borrowed pointer. */
obs_data_t *settings_extra();
/* Private source whose properties dialog edits Settings. */
void settings_register_source();
void settings_open_dialog();
void settings_shutdown();

/* ------------------------------------------------------------------ */
/* Canvas manager (canvas-manager.cpp)                                */

namespace canvas {
bool init();     /* find or create the canvas for the loaded collection */
void shutdown(); /* release collection-specific state */
void destroy();  /* module unload */
void unregister_hotkeys();

obs_canvas_t *get();     /* borrowed, may be null */
obs_source_t *program(); /* borrowed program source on channel 0 */
obs_source_t *transition();

std::vector<std::string> scene_names();
/* Strong reference, release with obs_scene_release. */
obs_scene_t *get_scene(const char *name);
bool set_scene(const std::string &name);
std::string current_scene();
bool create_scene(const char *name);
bool remove_scene(const char *name);
bool rename_scene(const char *old_name, const char *new_name);

void apply_video_settings();
void set_preview_visible(bool visible);
void update_parking();
bool parked();

void refresh_hotkeys();
void save_hotkeys();

using SceneChangedFn = std::function<void(const std::string &)>;
void on_scene_changed(SceneChangedFn fn);
} // namespace canvas

/* ------------------------------------------------------------------ */
/* Program source / isolated audio bus (audio-bus.cpp)                */

void program_register_source();
/* The transition owned by the program source (borrowed). */
obs_source_t *program_transition(obs_source_t *program);
void program_resize(obs_source_t *program, uint32_t cx, uint32_t cy);
/* Keep all non-reserved tracks active so vertical-only sources are heard
 * while the main outputs are offline. */
void program_activate_tracks(bool active);
/* Strip the reserved track from every other audio source. */
int reserved_track_enforce();
/* Main outputs that use the reserved track ("Main stream", ...). */
std::vector<std::string> reserved_track_conflicts();
/* Applies the reserved track index from settings; optionally enforces it
 * on new sources as they are created. */
void reserved_track_watch(bool enable);

/* ------------------------------------------------------------------ */
/* Reframe source (reframe-source.cpp)                                */

void reframe_register_source();
bool reframe_set_preset(obs_source_t *source, const char *preset, uint32_t duration_ms);

/* ------------------------------------------------------------------ */
/* Linked visibility (visibility-link.cpp)                            */

namespace links {
void init();
void shutdown();
void refresh(); /* reconnect to all scenes (after load / scene added) */
/* Sets visibility and mirrors it to linked items when link == true. */
bool set_visible(obs_scene_t *scene, const char *source_name, bool visible, bool link);
bool item_link_enabled(obs_sceneitem_t *item);
void set_item_link_enabled(obs_sceneitem_t *item, bool enabled);
} // namespace links

/* ------------------------------------------------------------------ */
/* Outputs (outputs.cpp)                                              */

namespace outputs {
void init();
void shutdown();
/* Returns false when the stream cannot start; stream_error() says why. */
bool start_stream();
void stop_stream();
bool streaming();
bool stream_connecting();
bool stream_reconnecting();
/* Plain-language reason the last start failed or the stream dropped ("" if none). */
std::string stream_error();
bool start_record();
void stop_record();
bool recording();
std::string last_record_path();
bool any_active();
void reset_encoders(); /* call when settings or canvas size change */

enum class Event { StreamStarted, StreamStopped, StreamFailed, StreamReconnecting, RecordStarted, RecordStopped };
using EventFn = std::function<void(Event)>;
void on_event(EventFn fn);
} // namespace outputs

/* ------------------------------------------------------------------ */
/* obs-websocket vendor (websocket-api.cpp)                           */

namespace ws {
void init(); /* call from obs_module_post_load */
void shutdown();
void emit(const char *event, obs_data_t *data);
} // namespace ws

/* ------------------------------------------------------------------ */
/* Dock UI (dock.cpp)                                                 */

void dock_create();
void dock_refresh();

/* Run fn on the UI thread (no wait). */
void run_on_ui(std::function<void()> fn);

} // namespace vplus
