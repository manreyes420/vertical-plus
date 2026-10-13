/*
 * Vertical+ program source = the isolated vertical audio bus.
 *
 * This private composite source sits on channel 0 of the vertical canvas.
 *   - Video: it renders the vertical transition (which holds the current
 *     vertical scene), so it is what the vertical canvas shows.
 *   - Audio: the vertical canvas is created with MIX_AUDIO, so this source
 *     is a root node of the global audio mix. Its audio_render writes ONLY
 *     the reserved track, summing every visible audio source on the
 *     vertical canvas straight from that source's own buffer.
 *
 * Why this is leak-free:
 *   - The transition and vertical scenes are children of this source, not
 *     root nodes, so their own mixes never reach the global tracks.
 *   - A source that exists only on vertical appears once in the audio tree,
 *     is not tagged as duplicated, and is mixed only into its parent scene,
 *     which nobody reads at the root. It is heard on the reserved track only.
 *   - A source shared with the main canvas (a mic) is tagged as duplicated by
 *     libobs and root-mixed once into its own tracks for the main outputs.
 *     The bus reads its buffer directly, so it is also on the reserved track,
 *     exactly once - as long as the shared source itself is not routed to
 *     the reserved track (reserved_track_enforce makes sure of that).
 *
 * Child audio is rendered before this source in the same audio tick because
 * libobs renders a source's active tree before the source itself.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#include <media-io/audio-io.h>
#include <util/config-file.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace vplus {

static std::atomic<int> g_reserved_mix{5}; /* 0-based mix index */

struct ProgramData {
	obs_source_t *self = nullptr;
	obs_source_t *transition = nullptr;
	std::atomic<uint32_t> cx{1080};
	std::atomic<uint32_t> cy{1920};

	/* Weak references: the program may be destroyed after OBS has already
	 * torn sources down at exit, so it must never own strong refs here. */
	std::mutex snapshot_mutex;
	std::vector<obs_weak_source_t *> snapshot;
};

static const size_t kMaxBusSources = 128;

/* ------------------------------------------------------------------ */
/* Snapshot of audible vertical sources (built on the graphics thread) */

struct CollectCtx {
	std::vector<obs_weak_source_t *> *out;
	std::vector<obs_source_t *> *seen; /* raw, for de-duplication only */
	int depth;
};

static bool collect_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<CollectCtx *>(param);
	if (!obs_sceneitem_visible(item))
		return true;

	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return true;

	if (obs_scene_t *nested = obs_group_or_scene_from_source(src)) {
		if (ctx->depth < 16) {
			CollectCtx sub{ctx->out, ctx->seen, ctx->depth + 1};
			obs_scene_enum_items(nested, collect_item, &sub);
		}
		return true;
	}

	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
		return true;
	if (!obs_source_enabled(src))
		return true;

	/* The same source can be visible twice (two items); hear it once. */
	if (std::find(ctx->seen->begin(), ctx->seen->end(), src) != ctx->seen->end())
		return true;
	if (ctx->out->size() >= kMaxBusSources)
		return false;

	ctx->seen->push_back(src);
	ctx->out->push_back(obs_source_get_weak_source(src));
	return true;
}

static void rebuild_snapshot(ProgramData *d)
{
	std::vector<obs_weak_source_t *> fresh;
	std::vector<obs_source_t *> seen;

	/* During a transition the destination scene is heard (cut at start). */
	if (obs_source_t *active = obs_transition_get_active_source(d->transition)) {
		if (obs_scene_t *scene = obs_group_or_scene_from_source(active)) {
			CollectCtx ctx{&fresh, &seen, 0};
			obs_scene_enum_items(scene, collect_item, &ctx);
		}
		obs_source_release(active);
	}

	std::vector<obs_weak_source_t *> old;
	{
		std::lock_guard<std::mutex> lock(d->snapshot_mutex);
		old.swap(d->snapshot);
		d->snapshot.swap(fresh);
	}
	for (obs_weak_source_t *w : old)
		obs_weak_source_release(w);
}

static void clear_snapshot(ProgramData *d)
{
	std::vector<obs_weak_source_t *> old;
	{
		std::lock_guard<std::mutex> lock(d->snapshot_mutex);
		old.swap(d->snapshot);
	}
	for (obs_weak_source_t *w : old)
		obs_weak_source_release(w);
}

/* ------------------------------------------------------------------ */
/* Source callbacks                                                   */

static const char *prog_get_name(void *)
{
	return "Vertical+ Program";
}

static void *prog_create(obs_data_t *, obs_source_t *source)
{
	auto *d = new ProgramData();
	d->self = source;
	d->cx = settings().width;
	d->cy = settings().height;

	d->transition = obs_source_create_private("fade_transition", "Vertical+ Transition", nullptr);
	if (!d->transition)
		d->transition = obs_source_create_private("cut_transition", "Vertical+ Transition", nullptr);
	if (d->transition) {
		obs_transition_set_size(d->transition, d->cx, d->cy);
		obs_transition_set_scale_type(d->transition, OBS_TRANSITION_SCALE_ASPECT);
		obs_transition_set_alignment(d->transition, OBS_ALIGN_CENTER);
	}
	return d;
}

static void prog_destroy(void *data)
{
	auto *d = static_cast<ProgramData *>(data);
	clear_snapshot(d);
	obs_source_release(d->transition);
	delete d;
}

static uint32_t prog_get_width(void *data)
{
	return static_cast<ProgramData *>(data)->cx;
}

static uint32_t prog_get_height(void *data)
{
	return static_cast<ProgramData *>(data)->cy;
}

static void prog_video_tick(void *data, float)
{
	rebuild_snapshot(static_cast<ProgramData *>(data));
}

static void prog_video_render(void *data, gs_effect_t *)
{
	auto *d = static_cast<ProgramData *>(data);
	if (d->transition)
		obs_source_video_render(d->transition);
}

static void prog_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	auto *d = static_cast<ProgramData *>(data);
	if (d->transition)
		cb(d->self, d->transition, param);
}

static inline size_t lowest_bit(uint32_t v)
{
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++)
		if (v & (1u << i))
			return i;
	return MAX_AUDIO_MIXES;
}

static bool prog_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
			      uint32_t mixers, size_t channels, size_t sample_rate)
{
	auto *d = static_cast<ProgramData *>(data);
	const size_t out_mix = (size_t)g_reserved_mix.load();
	const uint32_t out_bit = 1u << out_mix;

	if ((mixers & out_bit) == 0)
		return false; /* nobody is listening to the vertical track */

	/* Take strong references for this tick only. */
	obs_source_t *srcs[kMaxBusSources];
	size_t count_srcs = 0;
	{
		std::lock_guard<std::mutex> lock(d->snapshot_mutex);
		for (obs_weak_source_t *w : d->snapshot)
			if (obs_source_t *s = obs_weak_source_get_source(w))
				srcs[count_srcs++] = s;
	}

	uint64_t timestamp = 0;
	for (size_t i = 0; i < count_srcs; i++) {
		obs_source_t *s = srcs[i];
		if (obs_source_audio_pending(s))
			continue;
		uint64_t ts = obs_source_get_audio_timestamp(s);
		if (ts && (!timestamp || ts < timestamp))
			timestamp = ts;
	}

	struct obs_source_audio_mix child;
	for (size_t i = 0; timestamp && i < count_srcs; i++) {
		obs_source_t *s = srcs[i];
		if (obs_source_audio_pending(s))
			continue;
		const uint64_t ts = obs_source_get_audio_timestamp(s);
		if (!ts || ts < timestamp)
			continue;

		const size_t pos = (size_t)ns_to_audio_frames(sample_rate, ts - timestamp);
		if (pos >= AUDIO_OUTPUT_FRAMES)
			continue;
		const size_t count = AUDIO_OUTPUT_FRAMES - pos;

		/* Read the child from the lowest active track it is routed to.
		 * Each track buffer already has the source's volume and mute
		 * applied. The reserved track is used only as a last resort
		 * (a vertical-only source routed to nothing else). */
		const uint32_t routed = obs_source_get_audio_mixers(s) & mixers;
		size_t in_mix = lowest_bit(routed & ~out_bit);
		if (in_mix == MAX_AUDIO_MIXES)
			in_mix = lowest_bit(routed & out_bit);
		if (in_mix == MAX_AUDIO_MIXES)
			continue;

		obs_source_get_audio_mix(s, &child);
		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[out_mix].data[ch] + pos;
			const float *in = child.output[in_mix].data[ch];
			if (!out || !in)
				continue;
			for (size_t f = 0; f < count; f++)
				out[f] += in[f];
		}
	}

	for (size_t i = 0; i < count_srcs; i++)
		obs_source_release(srcs[i]);
	if (!timestamp)
		return false;

	*ts_out = timestamp;
	return true;
}

void program_register_source()
{
	obs_source_info info = {};
	info.id = VPLUS_PROGRAM_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	/* Composite sources may not carry OBS_SOURCE_AUDIO; audio_render is
	 * how composites (like scenes) provide audio. */
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_CAP_DISABLED |
			    OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = prog_get_name;
	info.create = prog_create;
	info.destroy = prog_destroy;
	info.get_width = prog_get_width;
	info.get_height = prog_get_height;
	info.video_tick = prog_video_tick;
	info.video_render = prog_video_render;
	info.enum_active_sources = prog_enum_sources;
	info.enum_all_sources = prog_enum_sources;
	info.audio_render = prog_audio_render;
	obs_register_source(&info);
}

obs_source_t *program_transition(obs_source_t *program)
{
	if (!program)
		return nullptr;
	auto *d = static_cast<ProgramData *>(obs_obj_get_data(program));
	return d ? d->transition : nullptr;
}

void program_resize(obs_source_t *program, uint32_t cx, uint32_t cy)
{
	if (!program)
		return;
	auto *d = static_cast<ProgramData *>(obs_obj_get_data(program));
	if (!d)
		return;
	d->cx = cx;
	d->cy = cy;
	if (d->transition)
		obs_transition_set_size(d->transition, cx, cy);
}

/* ------------------------------------------------------------------ */
/* Keep every non-reserved track active while vertical outputs run.   */
/* libobs only fills a source's track buffer when that track is used  */
/* by some output; vertical-only sources on e.g. track 1 would be     */
/* silent on vertical while the main stream is offline otherwise.     */

static bool g_tracks_active = false;
static int g_tracks_reserved = -1;

static void noop_audio(void *, size_t, struct audio_data *) {}

void program_activate_tracks(bool active)
{
	if (active == g_tracks_active && g_tracks_reserved == g_reserved_mix.load())
		return;

	if (g_tracks_active) {
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++)
			if ((int)i != g_tracks_reserved)
				obs_remove_raw_audio_callback(i, noop_audio, nullptr);
		g_tracks_active = false;
	}
	if (active) {
		g_tracks_reserved = g_reserved_mix.load();
		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++)
			if ((int)i != g_tracks_reserved)
				obs_add_raw_audio_callback(i, nullptr, noop_audio, nullptr);
		g_tracks_active = true;
	}
}

/* ------------------------------------------------------------------ */
/* Reserved track                                                     */

static bool strip_reserved(void *param, obs_source_t *source)
{
	auto *count = static_cast<int *>(param);
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return true;
	const uint32_t bit = 1u << g_reserved_mix.load();
	const uint32_t mixers = obs_source_get_audio_mixers(source);
	if (mixers & bit) {
		obs_source_set_audio_mixers(source, mixers & ~bit);
		(*count)++;
	}
	return true;
}

int reserved_track_enforce()
{
	int count = 0;
	obs_enum_sources(strip_reserved, &count);
	if (count)
		vlog(LOG_INFO, "removed track %d from %d source(s); it is reserved for vertical audio",
		     g_reserved_mix.load() + 1, count);
	return count;
}

static void source_created(void *, calldata_t *cd)
{
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source || obs_obj_is_private(source))
		return;
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
		return;

	/* Saved mixer settings are applied right after creation, so strip
	 * the reserved track on the next UI loop instead of now. */
	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	run_on_ui([weak]() {
		if (obs_source_t *s = obs_weak_source_get_source(weak)) {
			int n = 0;
			strip_reserved(&n, s);
			obs_source_release(s);
		}
		obs_weak_source_release(weak);
	});
}

static bool g_watching = false;

void reserved_track_watch(bool enable)
{
	g_reserved_mix = std::clamp(settings().audio_track, 1, (int)MAX_AUDIO_MIXES) - 1;

	signal_handler_t *sh = obs_get_signal_handler();
	if (enable && !g_watching) {
		signal_handler_connect(sh, "source_create", source_created, nullptr);
		g_watching = true;
	} else if (!enable && g_watching) {
		signal_handler_disconnect(sh, "source_create", source_created, nullptr);
		g_watching = false;
	}
	if (enable)
		reserved_track_enforce();

	/* Re-apply the active track set in case the reserved index moved. */
	if (g_tracks_active) {
		program_activate_tracks(false);
		program_activate_tracks(true);
	}

	for (const std::string &c : reserved_track_conflicts())
		vlog(LOG_WARNING, "%s uses track %d, which is reserved for vertical audio", c.c_str(),
		     g_reserved_mix.load() + 1);
}

std::vector<std::string> reserved_track_conflicts()
{
	std::vector<std::string> out;
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return out;

	const int track = g_reserved_mix.load() + 1;
	const char *mode = config_get_string(cfg, "Output", "Mode");
	const bool advanced = mode && strcmp(mode, "Advanced") == 0;

	if (advanced) {
		if (config_get_int(cfg, "AdvOut", "TrackIndex") == track)
			out.emplace_back("Main stream");
		const int64_t rec_tracks = config_get_int(cfg, "AdvOut", "RecTracks");
		if (rec_tracks & (1ll << (track - 1)))
			out.emplace_back("Main recording");
		if (config_get_bool(cfg, "AdvOut", "VodTrackEnabled") &&
		    config_get_int(cfg, "AdvOut", "VodTrackIndex") == track)
			out.emplace_back("Twitch VOD track");
	} else {
		/* Simple mode streams track 1; recordings can add tracks. */
		if (track == 1)
			out.emplace_back("Main stream");
		const int64_t rec_tracks = config_get_int(cfg, "SimpleOutput", "RecTracks");
		if (rec_tracks & (1ll << (track - 1)))
			out.emplace_back("Main recording");
	}
	return out;
}

} // namespace vplus
