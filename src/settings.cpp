/*
 * Vertical+ settings: per-profile JSON plus a private source whose
 * properties dialog is the settings UI (no custom Qt dialog needed).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#include <util/bmem.h>
#include <util/platform.h>

namespace vplus {

static Settings g_settings;
static obs_data_t *g_extra = nullptr;
static obs_source_t *g_settings_source = nullptr;

Settings &settings()
{
	return g_settings;
}

obs_data_t *settings_extra()
{
	if (!g_extra)
		g_extra = obs_data_create();
	return g_extra;
}

static std::string settings_file_path()
{
	char *profile = obs_frontend_get_current_profile_path();
	if (!profile)
		return {};
	std::string path = std::string(profile) + "/vertical-plus.json";
	bfree(profile);
	return path;
}

static void read_from_data(Settings &s, obs_data_t *d)
{
	s.width = (uint32_t)obs_data_get_int(d, "width");
	s.height = (uint32_t)obs_data_get_int(d, "height");
	s.audio_track = (int)obs_data_get_int(d, "audio_track");
	s.enforce_reserved_track = obs_data_get_bool(d, "enforce_reserved_track");
	s.idle_parking = obs_data_get_bool(d, "idle_parking");
	s.link_visibility = obs_data_get_bool(d, "link_visibility");
	s.link_all_scenes = obs_data_get_bool(d, "link_all_scenes");
	s.fps_divisor = (uint32_t)obs_data_get_int(d, "fps_divisor");
	s.video_encoder = obs_data_get_string(d, "video_encoder");
	s.video_bitrate = (int)obs_data_get_int(d, "video_bitrate");
	s.audio_bitrate = (int)obs_data_get_int(d, "audio_bitrate");
	s.server = obs_data_get_string(d, "server");
	s.key = obs_data_get_string(d, "key");
	s.record_path = obs_data_get_string(d, "record_path");
	s.record_format = obs_data_get_string(d, "record_format");
	s.start_with_main_stream = obs_data_get_bool(d, "start_with_main_stream");
	s.start_with_main_record = obs_data_get_bool(d, "start_with_main_record");
	s.transition_ms = (uint32_t)obs_data_get_int(d, "transition_ms");
	s.current_scene = obs_data_get_string(d, "current_scene");

	/* Clamp to sane values. */
	if (s.width < 128 || s.width > 4096)
		s.width = 1080;
	if (s.height < 128 || s.height > 4096)
		s.height = 1920;
	if (s.audio_track < 1 || s.audio_track > (int)MAX_AUDIO_MIXES)
		s.audio_track = 6;
	if (s.fps_divisor < 1 || s.fps_divisor > 4)
		s.fps_divisor = 1;
}

static void write_to_data(const Settings &s, obs_data_t *d)
{
	obs_data_set_int(d, "width", s.width);
	obs_data_set_int(d, "height", s.height);
	obs_data_set_int(d, "audio_track", s.audio_track);
	obs_data_set_bool(d, "enforce_reserved_track", s.enforce_reserved_track);
	obs_data_set_bool(d, "idle_parking", s.idle_parking);
	obs_data_set_bool(d, "link_visibility", s.link_visibility);
	obs_data_set_bool(d, "link_all_scenes", s.link_all_scenes);
	obs_data_set_int(d, "fps_divisor", s.fps_divisor);
	obs_data_set_string(d, "video_encoder", s.video_encoder.c_str());
	obs_data_set_int(d, "video_bitrate", s.video_bitrate);
	obs_data_set_int(d, "audio_bitrate", s.audio_bitrate);
	obs_data_set_string(d, "server", s.server.c_str());
	obs_data_set_string(d, "key", s.key.c_str());
	obs_data_set_string(d, "record_path", s.record_path.c_str());
	obs_data_set_string(d, "record_format", s.record_format.c_str());
	obs_data_set_bool(d, "start_with_main_stream", s.start_with_main_stream);
	obs_data_set_bool(d, "start_with_main_record", s.start_with_main_record);
	obs_data_set_int(d, "transition_ms", s.transition_ms);
	obs_data_set_string(d, "current_scene", s.current_scene.c_str());
}

static void set_defaults(obs_data_t *d)
{
	const Settings def;
	obs_data_set_default_int(d, "width", def.width);
	obs_data_set_default_int(d, "height", def.height);
	obs_data_set_default_int(d, "audio_track", def.audio_track);
	obs_data_set_default_bool(d, "enforce_reserved_track", def.enforce_reserved_track);
	obs_data_set_default_bool(d, "idle_parking", def.idle_parking);
	obs_data_set_default_bool(d, "link_visibility", def.link_visibility);
	obs_data_set_default_bool(d, "link_all_scenes", def.link_all_scenes);
	obs_data_set_default_int(d, "fps_divisor", def.fps_divisor);
	obs_data_set_default_string(d, "video_encoder", def.video_encoder.c_str());
	obs_data_set_default_int(d, "video_bitrate", def.video_bitrate);
	obs_data_set_default_int(d, "audio_bitrate", def.audio_bitrate);
	obs_data_set_default_string(d, "record_format", def.record_format.c_str());
	obs_data_set_default_int(d, "transition_ms", def.transition_ms);
}

void settings_load()
{
	std::string path = settings_file_path();
	obs_data_t *d = path.empty() ? nullptr : obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!d)
		d = obs_data_create();
	set_defaults(d);
	g_settings = Settings();
	read_from_data(g_settings, d);

	if (g_extra)
		obs_data_release(g_extra);
	g_extra = obs_data_get_obj(d, "extra");
	if (!g_extra)
		g_extra = obs_data_create();

	obs_data_release(d);
	vlog(LOG_INFO, "settings loaded (%ux%u, reserved track %d)", g_settings.width, g_settings.height,
	     g_settings.audio_track);
}

void settings_save()
{
	std::string path = settings_file_path();
	if (path.empty())
		return;
	obs_data_t *d = obs_data_create();
	write_to_data(g_settings, d);
	obs_data_set_obj(d, "extra", settings_extra());
	if (!obs_data_save_json_safe(d, path.c_str(), "tmp", "bak"))
		vlog(LOG_WARNING, "could not save %s", path.c_str());
	obs_data_release(d);
}

/* ------------------------------------------------------------------ */
/* Settings source: its properties dialog is the settings window.     */

static const char *ss_get_name(void *)
{
	return obs_module_text("VerticalPlus.Settings");
}

static void *ss_create(obs_data_t *, obs_source_t *source)
{
	return source;
}

static void ss_destroy(void *) {}

static bool reserve_now_clicked(obs_properties_t *, obs_property_t *, void *)
{
	int changed = reserved_track_enforce();
	vlog(LOG_INFO, "reserved track enforced on %d source(s)", changed);
	return false;
}

static obs_properties_t *ss_get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *canvas = obs_properties_create();
	obs_properties_add_int(canvas, "width", obs_module_text("Width"), 128, 4096, 2);
	obs_properties_add_int(canvas, "height", obs_module_text("Height"), 128, 4096, 2);
	obs_properties_add_int(canvas, "transition_ms", obs_module_text("TransitionMs"), 0, 5000, 50);
	obs_properties_add_bool(canvas, "idle_parking", obs_module_text("IdleParking"));
	obs_properties_add_group(props, "canvas", obs_module_text("Group.Canvas"), OBS_GROUP_NORMAL, canvas);

	obs_properties_t *audio = obs_properties_create();
	obs_properties_add_int(audio, "audio_track", obs_module_text("AudioTrack"), 1, MAX_AUDIO_MIXES, 1);
	obs_properties_add_bool(audio, "enforce_reserved_track", obs_module_text("EnforceReservedTrack"));
	obs_properties_add_button2(audio, "reserve_now", obs_module_text("ReserveNow"), reserve_now_clicked, nullptr);
	obs_properties_add_int(audio, "audio_bitrate", obs_module_text("AudioBitrate"), 64, 320, 32);
	obs_properties_add_group(props, "audio", obs_module_text("Group.Audio"), OBS_GROUP_NORMAL, audio);

	obs_properties_t *vis = obs_properties_create();
	obs_properties_add_bool(vis, "link_visibility", obs_module_text("LinkVisibility"));
	obs_properties_add_bool(vis, "link_all_scenes", obs_module_text("LinkAllScenes"));
	obs_properties_add_group(props, "links", obs_module_text("Group.Links"), OBS_GROUP_NORMAL, vis);

	obs_properties_t *enc = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(enc, "video_encoder", obs_module_text("VideoEncoder"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("EncoderAuto"), "auto");
	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		obs_property_list_add_string(list, obs_encoder_get_display_name(id), id);
	}
	obs_properties_add_int(enc, "video_bitrate", obs_module_text("VideoBitrate"), 500, 50000, 250);
	obs_property_t *div = obs_properties_add_list(enc, "fps_divisor", obs_module_text("FpsDivisor"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(div, obs_module_text("FpsFull"), 1);
	obs_property_list_add_int(div, obs_module_text("FpsHalf"), 2);
	obs_property_list_add_int(div, obs_module_text("FpsThird"), 3);
	obs_properties_add_group(props, "encoder", obs_module_text("Group.Encoder"), OBS_GROUP_NORMAL, enc);

	obs_properties_t *stream = obs_properties_create();
	obs_properties_add_text(stream, "server", obs_module_text("Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(stream, "key", obs_module_text("StreamKey"), OBS_TEXT_PASSWORD);
	obs_properties_add_bool(stream, "start_with_main_stream", obs_module_text("StartWithMainStream"));
	obs_properties_add_group(props, "stream", obs_module_text("Group.Stream"), OBS_GROUP_NORMAL, stream);

	obs_properties_t *rec = obs_properties_create();
	obs_properties_add_path(rec, "record_path", obs_module_text("RecordPath"), OBS_PATH_DIRECTORY, nullptr,
				nullptr);
	obs_property_t *fmt = obs_properties_add_list(rec, "record_format", obs_module_text("RecordFormat"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(fmt, "Hybrid MP4", "mp4");
	obs_property_list_add_string(fmt, "MKV", "mkv");
	obs_property_list_add_string(fmt, "MOV", "mov");
	obs_properties_add_bool(rec, "start_with_main_record", obs_module_text("StartWithMainRecord"));
	obs_properties_add_group(props, "record", obs_module_text("Group.Record"), OBS_GROUP_NORMAL, rec);

	return props;
}

static void ss_get_defaults(obs_data_t *d)
{
	set_defaults(d);
}

static void ss_update(void *, obs_data_t *d)
{
	const Settings old = g_settings;
	Settings s = g_settings;
	read_from_data(s, d);
	s.current_scene = old.current_scene; /* not edited here */
	g_settings = s;
	settings_save();

	if (old.width != s.width || old.height != s.height)
		canvas::apply_video_settings();
	if (old.width != s.width || old.height != s.height || old.video_encoder != s.video_encoder ||
	    old.video_bitrate != s.video_bitrate || old.audio_bitrate != s.audio_bitrate ||
	    old.fps_divisor != s.fps_divisor || old.audio_track != s.audio_track || old.server != s.server ||
	    old.key != s.key)
		outputs::reset_encoders();
	if (old.enforce_reserved_track != s.enforce_reserved_track || old.audio_track != s.audio_track)
		reserved_track_watch(s.enforce_reserved_track);
	if (old.link_visibility != s.link_visibility || old.link_all_scenes != s.link_all_scenes)
		links::refresh();
	canvas::update_parking();
}

void settings_register_source()
{
	obs_source_info info = {};
	info.id = VPLUS_SETTINGS_SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_CAP_DISABLED;
	info.get_name = ss_get_name;
	info.create = ss_create;
	info.destroy = ss_destroy;
	info.get_properties = ss_get_properties;
	info.get_defaults = ss_get_defaults;
	info.update = ss_update;
	obs_register_source(&info);
}

void settings_open_dialog()
{
	if (!g_settings_source)
		g_settings_source =
			obs_source_create_private(VPLUS_SETTINGS_SOURCE_ID, obs_module_text("VerticalPlus.Settings"),
						  nullptr);
	if (!g_settings_source)
		return;

	/* Load current values into the source without triggering update. */
	obs_data_t *d = obs_source_get_settings(g_settings_source);
	obs_data_clear(d);
	write_to_data(g_settings, d);
	obs_data_release(d);

	obs_frontend_open_source_properties(g_settings_source);
}

void settings_shutdown()
{
	if (g_settings_source) {
		obs_source_release(g_settings_source);
		g_settings_source = nullptr;
	}
	if (g_extra) {
		obs_data_release(g_extra);
		g_extra = nullptr;
	}
}

} // namespace vplus
