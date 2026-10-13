/*
 * Linked visibility: when a source is shown or hidden on one canvas, every
 * linked copy of the same source on the other canvas follows.
 *
 * Every visibility change in OBS (click, hotkey, websocket, Stream Deck,
 * Advanced Scene Switcher) goes through obs_sceneitem_set_visible, which
 * fires the scene's "item_visible" signal, so one handler covers them all.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#include <vector>

namespace vplus::links {

static const char *kLinkKey = "vplus_link";

struct Connection {
	obs_source_t *scene_source; /* strong ref */
};

static std::vector<Connection> g_conns;
static thread_local bool g_guard = false;

bool item_link_enabled(obs_sceneitem_t *item)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	bool enabled = true;
	if (priv) {
		if (obs_data_has_user_value(priv, kLinkKey))
			enabled = obs_data_get_bool(priv, kLinkKey);
		obs_data_release(priv);
	}
	return enabled;
}

void set_item_link_enabled(obs_sceneitem_t *item, bool enabled)
{
	obs_data_t *priv = obs_sceneitem_get_private_settings(item);
	if (!priv)
		return;
	if (enabled)
		obs_data_unset_user_value(priv, kLinkKey);
	else
		obs_data_set_bool(priv, kLinkKey, false);
	obs_data_release(priv);
}

static bool is_vertical_scene(obs_scene_t *scene)
{
	obs_canvas_t *c = obs_source_get_canvas(obs_scene_get_source(scene));
	const bool vertical = c && c == canvas::get();
	obs_canvas_release(c);
	return vertical;
}

/* Scenes on the other canvas that should receive the change. */
static std::vector<obs_source_t *> target_scenes(bool from_vertical)
{
	std::vector<obs_source_t *> out;
	const bool all = settings().link_all_scenes;

	if (from_vertical) {
		if (all) {
			obs_frontend_source_list list = {};
			obs_frontend_get_scenes(&list);
			for (size_t i = 0; i < list.sources.num; i++)
				out.push_back(obs_source_get_ref(list.sources.array[i]));
			obs_frontend_source_list_free(&list);
		} else if (obs_source_t *cur = obs_frontend_get_current_scene()) {
			out.push_back(cur);
		}
	} else {
		if (all) {
			for (const std::string &name : canvas::scene_names())
				if (obs_scene_t *s = canvas::get_scene(name.c_str())) {
					out.push_back(obs_source_get_ref(obs_scene_get_source(s)));
					obs_scene_release(s);
				}
		} else if (obs_scene_t *s = canvas::get_scene(canvas::current_scene().c_str())) {
			out.push_back(obs_source_get_ref(obs_scene_get_source(s)));
			obs_scene_release(s);
		}
	}
	return out;
}

/* Is this scene the current program scene of its canvas? */
static bool is_current(obs_scene_t *scene, bool vertical)
{
	obs_source_t *src = obs_scene_get_source(scene);
	if (vertical)
		return canvas::current_scene() == obs_source_get_name(src);
	obs_source_t *cur = obs_frontend_get_current_scene();
	const bool match = cur == src;
	obs_source_release(cur);
	return match;
}

struct MirrorCtx {
	obs_source_t *source;
	bool visible;
	int changed;
};

static bool mirror_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<MirrorCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src == ctx->source) {
		if (obs_sceneitem_visible(item) != ctx->visible && item_link_enabled(item)) {
			obs_sceneitem_set_visible(item, ctx->visible);
			ctx->changed++;
		}
	} else if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, mirror_item, param);
	}
	return true;
}

static void mirror(obs_weak_source_t *weak_scene, obs_weak_source_t *weak_src, bool visible)
{
	obs_source_t *scene_src = obs_weak_source_get_source(weak_scene);
	obs_source_t *src = obs_weak_source_get_source(weak_src);
	obs_scene_t *scene = obs_scene_from_source(scene_src);

	if (scene && src) {
		const bool from_vertical = is_vertical_scene(scene);
		if (settings().link_all_scenes || is_current(scene, from_vertical)) {
			MirrorCtx ctx{src, visible, 0};
			g_guard = true;
			for (obs_source_t *t : target_scenes(from_vertical)) {
				if (obs_scene_t *ts = obs_scene_from_source(t))
					obs_scene_enum_items(ts, mirror_item, &ctx);
				obs_source_release(t);
			}
			g_guard = false;
			if (ctx.changed)
				vlog(LOG_DEBUG, "linked visibility: '%s' %s on %d item(s)", obs_source_get_name(src),
				     visible ? "shown" : "hidden", ctx.changed);
		}
	}
	obs_source_release(scene_src);
	obs_source_release(src);
}

static void on_item_visible(void *, calldata_t *cd)
{
	if (g_guard || !settings().link_visibility)
		return;

	obs_scene_t *scene = static_cast<obs_scene_t *>(calldata_ptr(cd, "scene"));
	obs_sceneitem_t *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	const bool visible = calldata_bool(cd, "visible");
	if (!scene || !item || !item_link_enabled(item))
		return;

	/* The signal can fire on any thread while scene locks are held, so
	 * do the mirroring on the UI thread with weak references. */
	obs_weak_source_t *ws = obs_source_get_weak_source(obs_scene_get_source(scene));
	obs_weak_source_t *wi = obs_source_get_weak_source(obs_sceneitem_get_source(item));
	run_on_ui([ws, wi, visible]() {
		mirror(ws, wi, visible);
		obs_weak_source_release(ws);
		obs_weak_source_release(wi);
	});
}

static void connect_scene(obs_source_t *scene_source)
{
	signal_handler_t *sh = obs_source_get_signal_handler(scene_source);
	signal_handler_connect(sh, "item_visible", on_item_visible, nullptr);
	g_conns.push_back({obs_source_get_ref(scene_source)});
}

static void disconnect_all()
{
	for (Connection &c : g_conns) {
		signal_handler_disconnect(obs_source_get_signal_handler(c.scene_source), "item_visible",
					  on_item_visible, nullptr);
		obs_source_release(c.scene_source);
	}
	g_conns.clear();
}

void init()
{
	refresh();
}

void shutdown()
{
	disconnect_all();
}

void refresh()
{
	disconnect_all();
	if (!settings().link_visibility)
		return;

	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++)
		connect_scene(list.sources.array[i]);
	obs_frontend_source_list_free(&list);

	for (const std::string &name : canvas::scene_names())
		if (obs_scene_t *s = canvas::get_scene(name.c_str())) {
			connect_scene(obs_scene_get_source(s));
			obs_scene_release(s);
		}
}

bool set_visible(obs_scene_t *scene, const char *source_name, bool visible, bool link)
{
	if (!scene || !source_name)
		return false;
	obs_sceneitem_t *item = obs_scene_find_source_recursive(scene, source_name);
	if (!item)
		return false;
	const bool prev = g_guard;
	g_guard = !link; /* suppress mirroring when not linked */
	obs_sceneitem_set_visible(item, visible);
	g_guard = prev;
	return true;
}

} // namespace vplus::links
