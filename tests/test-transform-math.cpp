/*
 * Unit tests for the preview move/resize geometry.
 *   g++ -std=c++17 -I ../src test-transform-math.cpp -o test-transform-math && ./test-transform-math
 */
#include "transform-math.hpp"

#include <cmath>
#include <cstdio>

using namespace vplus::xform;

static int g_fail = 0;

static void near(const char *what, float got, float want)
{
	const bool ok = std::fabs(got - want) < 0.01f;
	if (!ok) {
		printf("FAIL %-52s got %.3f want %.3f\n", what, got, want);
		g_fail++;
	}
}

static void rect(const char *what, const Rect &r, float x, float y, float w, float h)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%s.x", what);
	near(buf, r.x, x);
	snprintf(buf, sizeof(buf), "%s.y", what);
	near(buf, r.y, y);
	snprintf(buf, sizeof(buf), "%s.w", what);
	near(buf, r.w, w);
	snprintf(buf, sizeof(buf), "%s.h", what);
	near(buf, r.h, h);
}

int main()
{
	const Rect cam{100, 200, 400, 225}; /* 16:9 webcam */

	/* Corner keeps aspect: drag bottom-right out by 200 in x. */
	rect("BR aspect", resize(cam, BottomRight, 700, 425, false, 8), 100, 200, 600, 337.5f);
	/* Pointer moved further in y: y wins. */
	rect("BR aspect y", resize(cam, BottomRight, 520, 650, false, 8), 100, 200, 800, 450);
	/* Shift = free stretch. */
	rect("BR free", resize(cam, BottomRight, 700, 500, true, 8), 100, 200, 600, 300);
	/* Top-left corner keeps the bottom-right fixed. */
	rect("TL aspect", resize(cam, TopLeft, 0, 200, false, 8), 0, 143.75f, 500, 281.25f);
	/* Edges stretch one axis only. */
	rect("Right edge", resize(cam, Right, 600, 9999, false, 8), 100, 200, 500, 225);
	rect("Top edge", resize(cam, Top, -50, 100, false, 8), 100, 100, 400, 325);
	/* Can't invert past the minimum size. */
	rect("Left past right", resize(cam, Left, 900, 0, false, 8), 492, 200, 8, 225);

	/* Anchor (OBS pos) for each alignment. */
	float ax, ay;
	anchor_for(cam, kAlignLeft | kAlignTop, false, false, ax, ay);
	near("anchor TL x", ax, 100), near("anchor TL y", ay, 200);
	anchor_for(cam, 0, false, false, ax, ay);
	near("anchor center x", ax, 300), near("anchor center y", ay, 312.5f);
	anchor_for(cam, kAlignRight | kAlignBottom, false, false, ax, ay);
	near("anchor BR x", ax, 500), near("anchor BR y", ay, 425);
	/* Flipped horizontally: a left-aligned item's pos is the visual right edge. */
	anchor_for(cam, kAlignLeft | kAlignTop, true, false, ax, ay);
	near("anchor flipX x", ax, 500), near("anchor flipX y", ay, 200);

	/* Snapping to canvas edges and centre (1080x1920). */
	float dx, dy;
	snap_move(Rect{6, 900, 400, 225}, 1080, 1920, 10, dx, dy);
	near("snap left edge dx", dx, -6), near("snap no y", dy, 0);
	snap_move(Rect{343, 1000, 400, 225}, 1080, 1920, 10, dx, dy);
	near("snap centre dx", dx, -3);
	snap_move(Rect{100, 1690, 400, 225}, 1080, 1920, 10, dx, dy);
	near("snap bottom dy", dy, 5);
	snap_move(Rect{100, 500, 400, 225}, 1080, 1920, 10, dx, dy);
	near("no snap dx", dx, 0), near("no snap dy", dy, 0);

	/* Letterbox: 1080x1920 canvas in a 1920x500 widget (the dock in the screenshot). */
	Letterbox lb = Letterbox::fit(1920, 500, 1080, 1920);
	near("lb w", (float)lb.w, 281), near("lb h", (float)lb.h, 500), near("lb x", (float)lb.x, 819);
	float cx, cy;
	lb.toCanvas(819 + 281 / 2.f, 250, cx, cy);
	near("lb centre x", cx, 540), near("lb centre y", cy, 960);
	lb.toCanvas(819, 0, cx, cy);
	near("lb origin x", cx, 0), near("lb origin y", cy, 0);

	printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "ALL PASSED", g_fail, g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
