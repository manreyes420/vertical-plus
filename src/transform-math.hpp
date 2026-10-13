/*
 * Vertical+ preview editing: pure geometry for moving and resizing scene
 * items in the dock preview. No OBS calls here, so it can be unit tested.
 * All values are canvas pixels.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vplus::xform {

struct Rect {
	float x = 0, y = 0, w = 0, h = 0;
	float right() const { return x + w; }
	float bottom() const { return y + h; }
};

/* Handles clockwise from top-left. */
enum Handle : int {
	None = -1,
	TopLeft = 0,
	Top = 1,
	TopRight = 2,
	Right = 3,
	BottomRight = 4,
	Bottom = 5,
	BottomLeft = 6,
	Left = 7,
};

/* Where each handle sits on the rect, as a fraction of width/height. */
inline void handle_fraction(int h, float &fx, float &fy)
{
	static const float kx[8] = {0.f, .5f, 1.f, 1.f, 1.f, .5f, 0.f, 0.f};
	static const float ky[8] = {0.f, 0.f, 0.f, .5f, 1.f, 1.f, 1.f, .5f};
	fx = (h >= 0 && h < 8) ? kx[h] : .5f;
	fy = (h >= 0 && h < 8) ? ky[h] : .5f;
}

inline bool moves_left(int h)
{
	return h == TopLeft || h == BottomLeft || h == Left;
}
inline bool moves_right(int h)
{
	return h == TopRight || h == BottomRight || h == Right;
}
inline bool moves_top(int h)
{
	return h == TopLeft || h == Top || h == TopRight;
}
inline bool moves_bottom(int h)
{
	return h == BottomLeft || h == Bottom || h == BottomRight;
}
inline bool is_corner(int h)
{
	return h == TopLeft || h == TopRight || h == BottomRight || h == BottomLeft;
}

/* Snap v to the nearest of the given lines when within threshold. */
inline float snap_value(float v, const float *lines, int count, float threshold, bool *snapped = nullptr)
{
	float best = v, bestDist = threshold;
	bool hit = false;
	for (int i = 0; i < count; i++) {
		const float d = std::fabs(v - lines[i]);
		if (d <= bestDist) {
			bestDist = d;
			best = lines[i];
			hit = true;
		}
	}
	if (snapped)
		*snapped = hit;
	return best;
}

/*
 * Resize r0 by dragging handle h to (px, py).
 * Corners keep the aspect ratio unless freeAspect; edges stretch one axis.
 * The opposite edge or corner stays fixed. Sizes never go below minSize.
 */
inline Rect resize(const Rect &r0, int h, float px, float py, bool freeAspect, float minSize)
{
	float x1 = r0.x, y1 = r0.y, x2 = r0.right(), y2 = r0.bottom();

	if (moves_left(h))
		x1 = std::min(px, x2 - minSize);
	if (moves_right(h))
		x2 = std::max(px, x1 + minSize);
	if (moves_top(h))
		y1 = std::min(py, y2 - minSize);
	if (moves_bottom(h))
		y2 = std::max(py, y1 + minSize);

	Rect r{x1, y1, x2 - x1, y2 - y1};

	if (is_corner(h) && !freeAspect && r0.w > 0.f && r0.h > 0.f) {
		/* Follow whichever axis the pointer moved further along. */
		const float f = std::max(r.w / r0.w, r.h / r0.h);
		const float nw = std::max(r0.w * f, minSize);
		const float nh = std::max(r0.h * f, minSize);
		if (moves_left(h))
			r.x = x2 - nw;
		r.w = nw;
		if (moves_top(h))
			r.y = y2 - nh;
		r.h = nh;
	}
	return r;
}

/* OBS alignment flags (obs-defs.h). */
constexpr uint32_t kAlignLeft = 1u << 0, kAlignRight = 1u << 1, kAlignTop = 1u << 2, kAlignBottom = 1u << 3;

/*
 * OBS positions a scene item by its alignment point on its bounding box.
 * Given the visual rect we want, return that point. A negative scale flips
 * the box, which mirrors which edge the alignment point is on.
 */
inline void anchor_for(const Rect &r, uint32_t align, bool flipX, bool flipY, float &ax, float &ay)
{
	uint32_t a = align;
	if (flipX && (a & (kAlignLeft | kAlignRight)))
		a ^= (kAlignLeft | kAlignRight);
	if (flipY && (a & (kAlignTop | kAlignBottom)))
		a ^= (kAlignTop | kAlignBottom);

	ax = (a & kAlignLeft) ? r.x : (a & kAlignRight) ? r.right() : r.x + r.w * .5f;
	ay = (a & kAlignTop) ? r.y : (a & kAlignBottom) ? r.bottom() : r.y + r.h * .5f;
}

/*
 * Snap a moved rect so one of its edges or its centre lands on the canvas
 * edges or centre lines. Returns the adjustment to add to the move delta.
 */
inline void snap_move(const Rect &r, float canvasW, float canvasH, float threshold, float &dx, float &dy)
{
	const float xs[3] = {0.f, canvasW * .5f, canvasW};
	const float ys[3] = {0.f, canvasH * .5f, canvasH};
	const float candX[3] = {r.x, r.x + r.w * .5f, r.right()};
	const float candY[3] = {r.y, r.y + r.h * .5f, r.bottom()};

	dx = 0.f;
	dy = 0.f;
	float bestX = threshold + 1.f, bestY = threshold + 1.f;
	for (float c : candX)
		for (float l : xs)
			if (std::fabs(l - c) <= threshold && std::fabs(l - c) < bestX) {
				bestX = std::fabs(l - c);
				dx = l - c;
			}
	for (float c : candY)
		for (float l : ys)
			if (std::fabs(l - c) <= threshold && std::fabs(l - c) < bestY) {
				bestY = std::fabs(l - c);
				dy = l - c;
			}
}

/* Letterboxed placement of a canvas inside a widget, in device pixels.
 * The preview draw and the mouse mapping must agree exactly. */
struct Letterbox {
	int x = 0, y = 0, w = 0, h = 0;
	float canvasW = 0.f, canvasH = 0.f;

	static Letterbox fit(uint32_t widgetW, uint32_t widgetH, uint32_t cw, uint32_t ch)
	{
		Letterbox lb;
		lb.canvasW = (float)cw;
		lb.canvasH = (float)ch;
		if (!cw || !ch || !widgetW || !widgetH)
			return lb;
		const float sx = (float)widgetW / (float)cw, sy = (float)widgetH / (float)ch;
		const float s = sx < sy ? sx : sy;
		lb.w = std::min((int)widgetW, (int)std::lround((double)cw * s));
		lb.h = std::min((int)widgetH, (int)std::lround((double)ch * s));
		lb.x = ((int)widgetW - lb.w) / 2;
		lb.y = ((int)widgetH - lb.h) / 2;
		return lb;
	}

	bool valid() const { return w > 0 && h > 0; }
	/* Canvas pixels per device pixel. */
	float unitsPerPixel() const { return valid() ? canvasW / (float)w : 1.f; }

	void toCanvas(float dx, float dy, float &cx, float &cy) const
	{
		cx = valid() ? (dx - (float)x) * canvasW / (float)w : 0.f;
		cy = valid() ? (dy - (float)y) * canvasH / (float)h : 0.f;
	}
};

} // namespace vplus::xform
