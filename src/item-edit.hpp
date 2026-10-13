/*
 * Vertical+ scene item helpers shared by the dock preview and tests.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "transform-math.hpp"

#include <obs.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <cmath>

namespace vplus {

/* ------------------------------------------------------------------ */
/* Scene item helpers                                                 */

/* Visual rect of an item in canvas pixels (axis-aligned bounds). */
inline xform::Rect item_rect(obs_sceneitem_t *item)
{
	matrix4 box;
	obs_sceneitem_get_box_transform(item, &box);
	float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
	const float us[4] = {0.f, 1.f, 1.f, 0.f}, vs[4] = {0.f, 0.f, 1.f, 1.f};
	for (int i = 0; i < 4; i++) {
		vec3 p;
		vec3_set(&p, us[i], vs[i], 0.f);
		vec3_transform(&p, &p, &box);
		minX = std::min(minX, p.x);
		minY = std::min(minY, p.y);
		maxX = std::max(maxX, p.x);
		maxY = std::max(maxY, p.y);
	}
	return {minX, minY, maxX - minX, maxY - minY};
}

inline bool item_contains(obs_sceneitem_t *item, float cx, float cy)
{
	matrix4 box, inv;
	obs_sceneitem_get_box_transform(item, &box);
	if (!matrix4_inv(&inv, &box))
		return false;
	vec3 p;
	vec3_set(&p, cx, cy, 0.f);
	vec3_transform(&p, &p, &inv);
	return p.x >= 0.f && p.x <= 1.f && p.y >= 0.f && p.y <= 1.f;
}

inline bool item_unrotated(obs_sceneitem_t *item)
{
	return std::fabs(std::fmod(obs_sceneitem_get_rot(item), 360.f)) < 0.01f;
}

/* Move the item so its visual rect starts at (x, y), keeping its size. */
inline void place_item_at(obs_sceneitem_t *item, float x, float y)
{
	const xform::Rect r = item_rect(item);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += x - r.x;
	pos.y += y - r.y;
	obs_sceneitem_set_pos(item, &pos);
}

inline obs_sceneitem_t *selected_item(obs_scene_t *scene)
{
	obs_sceneitem_t *found = nullptr;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			if (obs_sceneitem_selected(item))
				*static_cast<obs_sceneitem_t **>(param) = item; /* topmost wins */
			return true;
		},
		&found);
	return found;
}

inline void select_only(obs_scene_t *scene, obs_sceneitem_t *keep)
{
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			const bool want = item == static_cast<obs_sceneitem_t *>(param);
			if (obs_sceneitem_selected(item) != want)
				obs_sceneitem_select(item, want);
			return true;
		},
		keep);
}


/* Resize an item so its visual rect becomes r. info0 and r0 are the item's
 * transform and rect when the drag started. Works for scale-based and
 * bounds-based items and keeps the item's alignment and flips. */
inline void apply_resize(obs_sceneitem_t *item, const obs_transform_info &info0, const xform::Rect &r0,
			 const xform::Rect &r)
{
	obs_transform_info info = info0;
	const bool bounded = info0.bounds_type != OBS_BOUNDS_NONE;
	const bool flipX = bounded ? info0.bounds.x < 0.f : info0.scale.x < 0.f;
	const bool flipY = bounded ? info0.bounds.y < 0.f : info0.scale.y < 0.f;

	if (bounded) {
		vec2_set(&info.bounds, flipX ? -r.w : r.w, flipY ? -r.h : r.h);
	} else if (r0.w > 0.f && r0.h > 0.f) {
		info.scale.x = info0.scale.x * (r.w / r0.w);
		info.scale.y = info0.scale.y * (r.h / r0.h);
	}
	xform::anchor_for(r, info.alignment, flipX, flipY, info.pos.x, info.pos.y);
	obs_sceneitem_set_info2(item, &info);
}

} // namespace vplus
