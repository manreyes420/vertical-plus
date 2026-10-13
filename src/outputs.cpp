/*
 * Vertical stream and recording outputs.
 * One video encoder (bound to the vertical canvas) and one audio encoder
 * (bound to the reserved track) are shared by both outputs.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"
#include "platforms.hpp"

#include <util/config-file.h>
#include <util/platform.h>

#include <atomic>
#include <mutex>
#include <cstring>

namespace vplus::outputs {

static obs_encoder_t *g_venc = nullptr;
static obs_encoder_t *g_aenc = nullptr;
static obs_service_t *g_service = nullptr;
static obs_output_t *g_stream = nullptr;
static obs_output_t *g_record = nullptr;

static std::atomic<bool> g_stream_active{false};
static std::atomic<bool> g_record_active{false};
static std::atomic<int> g_starting{0};
static bool g_encoders_dirty = true;
static std::string g_last_record_path;
static std::atomic<bool> g_reconnecting{false};
static std::mutex g_error_mutex;
static std::string g_stream_error; /* why the last start failed or the stream dropped */
static std::vector<EventFn> g_event_cbs;

/* ------------------------------------------------------------------ */
/* Encoders                                                           */

static bool encoder_available(const char *id)
{
	return id && *id && obs_get_encoder_codec(id) != nullptr;
}

static std::string pick_video_encoder(obs_data_t **inherit_settings)
{
	*inherit_settings = nullptr;
	const std::string &wanted = settings().video_encoder;
	if (wanted != "auto" && encoder_available(wanted.c_str()))
		return wanted;

	/* 1. Same encoder as the main stream, with its tuning. */
	if (obs_output_t *main = obs_frontend_get_streaming_output()) {
		obs_encoder_t *enc = obs_output_get_video_encoder(main);
		if (enc && strcmp(obs_encoder_get_codec(enc), "h264") == 0) {
			*inherit_settings = obs_encoder_get_settings(enc);
			std::string id = obs_encoder_get_id(enc);
			obs_output_release(main);
			return id;
		}
		obs_output_release(main);
	}

	/* 2. A hardware H.264 encoder. */
	for (const char *id : {"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2"})
		if (encoder_available(id))
			return id;
	const char *id;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++)
		if (strncmp(id, "com.apple.videotoolbox", 22) == 0 && strcmp(obs_get_encoder_codec(id), "h264") == 0)
			return id;

	/* 3. Software. */
	return "obs_x264";
}

static void release_encoders()
{
	if (g_venc) {
		obs_encoder_release(g_venc);
		g_venc = nullptr;
	}
	if (g_aenc) {
		obs_encoder_release(g_aenc);
		g_aenc = nullptr;
	}
}

static bool ensure_encoders()
{
	if (g_venc && g_aenc && !g_encoders_dirty)
		return true;
	if (any_active() && g_venc && g_aenc)
		return true; /* keep running ones; new settings apply next time */

	release_encoders();
	obs_canvas_t *canvas = canvas::get();
	if (!canvas || !obs_canvas_has_video(canvas))
		return false;

	obs_data_t *vs = nullptr;
	const std::string venc_id = pick_video_encoder(&vs);
	if (!vs)
		vs = obs_data_create();
	obs_data_set_string(vs, "rate_control", "CBR");
	obs_data_set_int(vs, "bitrate", settings().video_bitrate);
	obs_data_set_int(vs, "keyint_sec", 2);

	g_venc = obs_video_encoder_create(venc_id.c_str(), "Vertical+ Video", vs, nullptr);
	obs_data_release(vs);
	if (!g_venc) {
		vlog(LOG_ERROR, "could not create video encoder '%s'", venc_id.c_str());
		return false;
	}
	if (settings().fps_divisor > 1)
		obs_encoder_set_frame_rate_divisor(g_venc, settings().fps_divisor);
	obs_encoder_set_video(g_venc, obs_canvas_get_video(canvas));

	const char *aenc_id = "ffmpeg_aac";
#ifdef __APPLE__
	if (encoder_available("CoreAudio_AAC"))
		aenc_id = "CoreAudio_AAC";
#endif
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", settings().audio_bitrate);
	const size_t mix_idx = (size_t)(settings().audio_track - 1);
	g_aenc = obs_audio_encoder_create(aenc_id, "Vertical+ Audio", as, mix_idx, nullptr);
	obs_data_release(as);
	if (!g_aenc) {
		vlog(LOG_ERROR, "could not create audio encoder '%s'", aenc_id);
		release_encoders();
		return false;
	}
	obs_encoder_set_audio(g_aenc, obs_get_audio());

	g_encoders_dirty = false;
	vlog(LOG_INFO, "encoders: %s %d kbps (fps / %u), %s %d kbps on track %d", venc_id.c_str(),
	     settings().video_bitrate, settings().fps_divisor, aenc_id, settings().audio_bitrate,
	     settings().audio_track);
	return true;
}

void reset_encoders()
{
	g_encoders_dirty = true;
	if (!any_active())
		release_encoders();
}

/* ------------------------------------------------------------------ */
/* Signals                                                            */

static void emit(Event e)
{
	for (auto &fn : g_event_cbs)
		fn(e);
}

static void set_stream_error(const std::string &msg)
{
	std::lock_guard<std::mutex> lock(g_error_mutex);
	g_stream_error = msg;
}

/* Plain-language reason for an output stop code. */
static std::string describe_stop(long long code, const char *detail)
{
	std::string msg;
	switch (code) {
	case OBS_OUTPUT_BAD_PATH:
		msg = obs_module_text("StreamError.BadPath");
		break;
	case OBS_OUTPUT_CONNECT_FAILED:
		msg = obs_module_text("StreamError.ConnectFailed");
		break;
	case OBS_OUTPUT_INVALID_STREAM:
		msg = obs_module_text("StreamError.InvalidStream");
		break;
	case OBS_OUTPUT_DISCONNECTED:
		msg = obs_module_text("StreamError.Disconnected");
		break;
	case OBS_OUTPUT_UNSUPPORTED:
		msg = obs_module_text("StreamError.Unsupported");
		break;
	case OBS_OUTPUT_ENCODE_ERROR:
		msg = obs_module_text("StreamError.EncodeError");
		break;
	default:
		msg = obs_module_text("StreamError.Unknown");
	}
	if (detail && *detail)
		msg += std::string("\n\n") + detail;
	return msg;
}

static void stream_reconnect(void *, calldata_t *)
{
	g_reconnecting = true;
	vlog(LOG_WARNING, "vertical stream lost its connection; reconnecting");
	run_on_ui([]() { emit(Event::StreamReconnecting); });
}

static void stream_reconnect_success(void *, calldata_t *)
{
	g_reconnecting = false;
	vlog(LOG_INFO, "vertical stream reconnected");
	run_on_ui([]() { emit(Event::StreamStarted); });
}

static void stream_started(void *, calldata_t *)
{
	set_stream_error({});
	g_reconnecting = false;
	g_stream_active = true;
	vlog(LOG_INFO, "vertical stream connected");
	g_starting--;
	run_on_ui([]() {
		canvas::update_parking();
		emit(Event::StreamStarted);
	});
}

static void stream_stopped(void *, calldata_t *cd)
{
	const bool was_starting = !g_stream_active;
	g_stream_active = false;
	if (was_starting)
		g_starting--;
	g_reconnecting = false;
	const long long code = calldata_int(cd, "code");
	if (code != OBS_OUTPUT_SUCCESS) {
		obs_output_t *out = static_cast<obs_output_t *>(calldata_ptr(cd, "output"));
		const char *detail = out ? obs_output_get_last_error(out) : nullptr;
		vlog(LOG_WARNING, "vertical stream stopped with code %lld%s%s", code, detail ? ": " : "",
		     detail ? detail : "");
		set_stream_error(describe_stop(code, detail));
	}
	run_on_ui([code]() {
		canvas::update_parking();
		emit(code == OBS_OUTPUT_SUCCESS ? Event::StreamStopped : Event::StreamFailed);
	});
}

static void record_started(void *, calldata_t *)
{
	g_record_active = true;
	g_starting--;
	run_on_ui([]() {
		canvas::update_parking();
		emit(Event::RecordStarted);
	});
}

static void record_stopped(void *, calldata_t *cd)
{
	const bool was_starting = !g_record_active;
	g_record_active = false;
	if (was_starting)
		g_starting--;
	const long long code = calldata_int(cd, "code");
	if (code != OBS_OUTPUT_SUCCESS)
		vlog(LOG_WARNING, "vertical recording stopped with code %lld", code);
	run_on_ui([]() {
		canvas::update_parking();
		emit(Event::RecordStopped);
	});
}

static void connect_output(obs_output_t *out, signal_callback_t on_start, signal_callback_t on_stop)
{
	signal_handler_t *sh = obs_output_get_signal_handler(out);
	signal_handler_connect(sh, "start", on_start, nullptr);
	signal_handler_connect(sh, "stop", on_stop, nullptr);
}

static void disconnect_output(obs_output_t *out, signal_callback_t on_start, signal_callback_t on_stop)
{
	signal_handler_t *sh = obs_output_get_signal_handler(out);
	signal_handler_disconnect(sh, "start", on_start, nullptr);
	signal_handler_disconnect(sh, "stop", on_stop, nullptr);
}

/* ------------------------------------------------------------------ */
/* Stream                                                             */

bool start_stream()
{
	if (g_stream_active || (g_stream && obs_output_active(g_stream)))
		return true;

	const platforms::Platform *platform = platforms::find(settings().platform);
	const std::string server = platforms::resolve_server(settings().platform, settings().server);
	const std::string key = platforms::trim(settings().key);
	if (server.empty()) {
		vlog(LOG_WARNING, "vertical stream not started: no server URL set");
		set_stream_error(std::string(obs_module_text("StreamError.NoServer")) + "\n\n" +
				 obs_module_text(platform->helpKey));
		return false;
	}
	if (!platforms::looks_like_ingest_url(server)) {
		vlog(LOG_WARNING, "vertical stream not started: server URL is not rtmp:// or rtmps://");
		set_stream_error(obs_module_text("StreamError.ServerFormat"));
		return false;
	}
	if (key.empty()) {
		vlog(LOG_WARNING, "vertical stream not started: no stream key set");
		set_stream_error(std::string(obs_module_text("StreamError.NoKey")) + "\n\n" +
				 obs_module_text(platform->helpKey));
		return false;
	}
	/* Twitch takes one stream per channel: a second connection with the
	 * same key knocks the main stream off. Twitch's vertical view is sent
	 * with the main stream instead (Enhanced Broadcasting / Dual Format). */
	if (strcmp(platform->id, "twitch") == 0) {
		bool mainIsTwitch = false;
		if (obs_service_t *svc = obs_frontend_get_streaming_service()) {
			obs_data_t *sd = obs_service_get_settings(svc);
			const char *name = obs_data_get_string(sd, "service");
			mainIsTwitch = name && strstr(name, "Twitch") != nullptr;
			obs_data_release(sd);
		}
		if (mainIsTwitch && obs_frontend_streaming_active()) {
			vlog(LOG_WARNING, "vertical stream not started: main stream is already live on Twitch");
			set_stream_error(obs_module_text("StreamError.TwitchTwice"));
			return false;
		}
	}

	set_stream_error({});
	vlog(LOG_INFO, "starting vertical stream (%s)", platform->id);

	g_starting++;
	canvas::update_parking(); /* make sure the canvas renders */

	if (!ensure_encoders()) {
		g_starting--;
		canvas::update_parking();
		set_stream_error(obs_module_text("StreamError.EncodeError"));
		return false;
	}

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", server.c_str());
	obs_data_set_string(ss, "key", key.c_str());
	if (!g_service)
		g_service = obs_service_create_private("rtmp_custom", "Vertical+ Service", ss);
	else
		obs_service_update(g_service, ss);
	obs_data_release(ss);

	if (!g_stream) {
		g_stream = obs_output_create("rtmp_output", "Vertical+ Stream", nullptr, nullptr);
		if (!g_stream) {
			g_starting--;
			canvas::update_parking();
			return false;
		}
		connect_output(g_stream, stream_started, stream_stopped);
		signal_handler_t *sh = obs_output_get_signal_handler(g_stream);
		signal_handler_connect(sh, "reconnect", stream_reconnect, nullptr);
		signal_handler_connect(sh, "reconnect_success", stream_reconnect_success, nullptr);
	}

	config_t *cfg = obs_frontend_get_profile_config();
	const int retries = cfg ? (int)config_get_uint(cfg, "Output", "MaxRetries") : 25;
	const int delay = cfg ? (int)config_get_uint(cfg, "Output", "RetryDelay") : 2;
	obs_output_set_reconnect_settings(g_stream, retries > 0 ? retries : 25, delay > 0 ? delay : 2);

	obs_output_set_service(g_stream, g_service);
	obs_output_set_video_encoder(g_stream, g_venc);
	obs_output_set_audio_encoder(g_stream, g_aenc, 0);

	if (!obs_output_start(g_stream)) {
		const char *err = obs_output_get_last_error(g_stream);
		vlog(LOG_ERROR, "vertical stream failed to start: %s", err ? err : "unknown error");
		set_stream_error(describe_stop(OBS_OUTPUT_ERROR, err));
		g_starting--;
		canvas::update_parking();
		return false;
	}
	return true;
}

bool stream_connecting()
{
	return !g_stream_active && g_stream && obs_output_active(g_stream);
}

bool stream_reconnecting()
{
	return g_reconnecting;
}

std::string stream_error()
{
	std::lock_guard<std::mutex> lock(g_error_mutex);
	return g_stream_error;
}

void stop_stream()
{
	if (g_stream && (g_stream_active || obs_output_active(g_stream)))
		obs_output_stop(g_stream);
}

bool streaming()
{
	return g_stream_active;
}

/* ------------------------------------------------------------------ */
/* Recording                                                          */

static std::string main_record_folder()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return {};
	const char *mode = config_get_string(cfg, "Output", "Mode");
	const bool adv = mode && strcmp(mode, "Advanced") == 0;
	const char *path = adv ? config_get_string(cfg, "AdvOut", "RecFilePath")
			       : config_get_string(cfg, "SimpleOutput", "FilePath");
	return path ? path : "";
}

static std::string make_record_path(const char *ext)
{
	std::string folder = settings().record_path.empty() ? main_record_folder() : settings().record_path;
	if (folder.empty())
		folder = ".";

	config_t *cfg = obs_frontend_get_profile_config();
	const char *fmt = cfg ? config_get_string(cfg, "Output", "FilenameFormatting") : nullptr;
	std::string format = (fmt && *fmt) ? fmt : "%CCYY-%MM-%DD %hh-%mm-%ss";
	format += " vertical";

	char *name = os_generate_formatted_filename(ext, true, format.c_str());
	std::string path = folder + "/" + (name ? name : "vertical." + std::string(ext));
	bfree(name);
	return path;
}

bool start_record()
{
	if (g_record_active)
		return true;

	g_starting++;
	canvas::update_parking();
	if (!ensure_encoders()) {
		g_starting--;
		canvas::update_parking();
		return false;
	}

	const std::string &f = settings().record_format;
	const char *output_id = f == "mkv" ? "ffmpeg_muxer" : f == "mov" ? "mov_output" : "mp4_output";
	const char *ext = f == "mkv" ? "mkv" : f == "mov" ? "mov" : "mp4";

	/* Recreate if the format changed since the last recording. */
	if (g_record && strcmp(obs_output_get_id(g_record), output_id) != 0) {
		disconnect_output(g_record, record_started, record_stopped);
		obs_output_release(g_record);
		g_record = nullptr;
	}
	if (!g_record) {
		g_record = obs_output_create(output_id, "Vertical+ Recording", nullptr, nullptr);
		if (!g_record) {
			g_starting--;
			canvas::update_parking();
			return false;
		}
		connect_output(g_record, record_started, record_stopped);
	}

	g_last_record_path = make_record_path(ext);
	obs_data_t *rs = obs_data_create();
	obs_data_set_string(rs, "path", g_last_record_path.c_str());
	obs_output_update(g_record, rs);
	obs_data_release(rs);

	obs_output_set_video_encoder(g_record, g_venc);
	obs_output_set_audio_encoder(g_record, g_aenc, 0);

	if (!obs_output_start(g_record)) {
		const char *err = obs_output_get_last_error(g_record);
		vlog(LOG_ERROR, "vertical recording failed to start: %s", err ? err : "unknown error");
		g_starting--;
		canvas::update_parking();
		return false;
	}
	vlog(LOG_INFO, "vertical recording to %s", g_last_record_path.c_str());
	return true;
}

void stop_record()
{
	if (g_record && (g_record_active || obs_output_active(g_record)))
		obs_output_stop(g_record);
}

bool recording()
{
	return g_record_active;
}

std::string last_record_path()
{
	return g_last_record_path;
}

bool any_active()
{
	return g_stream_active || g_record_active || g_starting > 0;
}

void on_event(EventFn fn)
{
	g_event_cbs.push_back(std::move(fn));
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */

void init() {}

void shutdown()
{
	if (g_stream) {
		obs_output_force_stop(g_stream);
		disconnect_output(g_stream, stream_started, stream_stopped);
		signal_handler_t *sh = obs_output_get_signal_handler(g_stream);
		signal_handler_disconnect(sh, "reconnect", stream_reconnect, nullptr);
		signal_handler_disconnect(sh, "reconnect_success", stream_reconnect_success, nullptr);
		obs_output_release(g_stream);
		g_stream = nullptr;
	}
	if (g_record) {
		obs_output_force_stop(g_record);
		disconnect_output(g_record, record_started, record_stopped);
		obs_output_release(g_record);
		g_record = nullptr;
	}
	g_stream_active = g_record_active = false;
	g_starting = 0;
	if (g_service) {
		obs_service_release(g_service);
		g_service = nullptr;
	}
	release_encoders();
	g_event_cbs.clear();
}

} // namespace vplus::outputs
