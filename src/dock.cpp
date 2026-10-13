/*
 * Vertical+ dock: live preview, vertical scene list, item list with
 * visibility and link toggles, quick layouts, and output buttons.
 *
 * Plain Qt widgets with lambda connections (no moc needed).
 * The preview supports OBS-style select / move / resize of sources.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include "item-edit.hpp"
#include "transform-math.hpp"

#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

namespace vplus {

/* ------------------------------------------------------------------ */
/* Preview: live view of the vertical canvas with OBS-style editing.  */
/*   click: select    drag: move (snaps to canvas edges and centre)   */
/*   drag a handle: resize (corners keep shape; Shift = free stretch) */
/*   Ctrl while dragging: no snapping    arrows: nudge 1 px (Shift 10)*/
/*   double-click: Edit Transform        right-click: item menu       */

class PreviewWidget : public QWidget {
public:
	std::function<void(int64_t)> onSelect;                      /* 0 = none */
	std::function<void(const QPoint &, int64_t)> onContextMenu; /* global pos, item id */

	explicit PreviewWidget(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMouseTracking(true);
		setFocusPolicy(Qt::ClickFocus);
		setMinimumSize(90, 160);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	~PreviewWidget() override
	{
		endDrag();
		destroyDisplay();
		std::lock_guard<std::mutex> lock(sceneMutex);
		obs_weak_source_release(weakScene);
		weakScene = nullptr;
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* The scene shown in the preview, used for hit testing and outlines. */
	void setScene(obs_scene_t *scene)
	{
		endDrag();
		std::lock_guard<std::mutex> lock(sceneMutex);
		obs_weak_source_release(weakScene);
		weakScene = scene ? obs_source_get_weak_source(obs_scene_get_source(scene)) : nullptr;
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		createDisplay();
		canvas::set_preview_visible(true);
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		canvas::set_preview_visible(false);
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (display) {
			const QSize s = e->size() * devicePixelRatioF();
			obs_display_resize(display, (uint32_t)s.width(), (uint32_t)s.height());
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		float cx, cy;
		xform::Letterbox lb;
		if (!mapToCanvas(e->position(), lb, cx, cy))
			return;
		obs_scene_t *scene = strongScene();
		if (!scene)
			return;

		if (e->button() == Qt::RightButton) {
			obs_sceneitem_t *hit = hitTest(scene, cx, cy);
			if (hit) {
				select_only(scene, hit);
				notifySelect(hit);
			}
			obs_sceneitem_t *sel = selected_item(scene);
			const int64_t id = sel ? obs_sceneitem_get_id(sel) : 0;
			obs_scene_release(scene);
			if (id && onContextMenu)
				onContextMenu(e->globalPosition().toPoint(), id);
			return;
		}
		if (e->button() != Qt::LeftButton) {
			obs_scene_release(scene);
			return;
		}

		const float tol = kHandlePx * lb.unitsPerPixel();
		obs_sceneitem_t *target = nullptr;
		int handle = xform::None;

		/* A handle of the current selection takes priority. */
		if (obs_sceneitem_t *sel = selected_item(scene)) {
			handle = handleAt(sel, cx, cy, tol);
			if (handle != xform::None)
				target = sel;
		}
		if (!target) {
			target = hitTest(scene, cx, cy);
			select_only(scene, target);
			notifySelect(target);
		}

		if (target && !obs_sceneitem_locked(target)) {
			dragItem = target;
			obs_sceneitem_addref(dragItem);
			dragHandle = handle;
			dragStartX = cx;
			dragStartY = cy;
			dragRect = item_rect(target);
			obs_sceneitem_get_info2(target, &dragInfo);
		}
		obs_scene_release(scene);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		float cx, cy;
		xform::Letterbox lb;
		if (!mapToCanvas(e->position(), lb, cx, cy))
			return;

		if (!dragItem) {
			updateHoverCursor(cx, cy, lb);
			return;
		}

		const bool snap = !(e->modifiers() & Qt::ControlModifier);
		const float snapTol = kSnapPx * lb.unitsPerPixel();

		if (dragHandle == xform::None) {
			float dx = cx - dragStartX, dy = cy - dragStartY;
			if (snap) {
				xform::Rect moved = dragRect;
				moved.x += dx;
				moved.y += dy;
				float sdx, sdy;
				xform::snap_move(moved, lb.canvasW, lb.canvasH, snapTol, sdx, sdy);
				dx += sdx;
				dy += sdy;
			}
			vec2 pos;
			vec2_set(&pos, dragInfo.pos.x + dx, dragInfo.pos.y + dy);
			obs_sceneitem_set_pos(dragItem, &pos);
			return;
		}

		/* Resize. */
		if (snap) {
			const float xs[3] = {0.f, lb.canvasW * .5f, lb.canvasW};
			const float ys[3] = {0.f, lb.canvasH * .5f, lb.canvasH};
			cx = xform::snap_value(cx, xs, 3, snapTol);
			cy = xform::snap_value(cy, ys, 3, snapTol);
		}
		const bool freeAspect = e->modifiers() & Qt::ShiftModifier;
		const xform::Rect r = xform::resize(dragRect, dragHandle, cx, cy, freeAspect, 8.f);

		apply_resize(dragItem, dragInfo, dragRect, r);
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton)
			endDrag();
	}

	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		float cx, cy;
		xform::Letterbox lb;
		if (e->button() != Qt::LeftButton || !mapToCanvas(e->position(), lb, cx, cy))
			return;
		endDrag();
		if (obs_scene_t *scene = strongScene()) {
			if (obs_sceneitem_t *hit = hitTest(scene, cx, cy))
				obs_frontend_open_sceneitem_edit_transform(hit);
			obs_scene_release(scene);
		}
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		const float step = (e->modifiers() & Qt::ShiftModifier) ? 10.f : 1.f;
		float dx = 0.f, dy = 0.f;
		switch (e->key()) {
		case Qt::Key_Left:
			dx = -step;
			break;
		case Qt::Key_Right:
			dx = step;
			break;
		case Qt::Key_Up:
			dy = -step;
			break;
		case Qt::Key_Down:
			dy = step;
			break;
		case Qt::Key_Escape:
			if (obs_scene_t *scene = strongScene()) {
				select_only(scene, nullptr);
				notifySelect(nullptr);
				obs_scene_release(scene);
			}
			return;
		default:
			QWidget::keyPressEvent(e);
			return;
		}
		if (obs_scene_t *scene = strongScene()) {
			obs_sceneitem_t *sel = selected_item(scene);
			if (sel && !obs_sceneitem_locked(sel)) {
				vec2 pos;
				obs_sceneitem_get_pos(sel, &pos);
				pos.x += dx;
				pos.y += dy;
				obs_sceneitem_set_pos(sel, &pos);
			}
			obs_scene_release(scene);
		}
	}

private:
	static constexpr float kHandlePx = 7.f; /* handle grab radius, device px */
	static constexpr float kSnapPx = 8.f;   /* snap distance, device px */
	static constexpr float kHandleDrawPx = 8.f;

	obs_display_t *display = nullptr;
	std::mutex sceneMutex;
	obs_weak_source_t *weakScene = nullptr;

	obs_sceneitem_t *dragItem = nullptr; /* strong ref while dragging */
	int dragHandle = xform::None;
	float dragStartX = 0.f, dragStartY = 0.f;
	xform::Rect dragRect;
	obs_transform_info dragInfo = {};

	obs_scene_t *strongScene()
	{
		std::lock_guard<std::mutex> lock(sceneMutex);
		obs_source_t *src = obs_weak_source_get_source(weakScene);
		obs_scene_t *scene = obs_scene_from_source(src);
		if (!scene)
			obs_source_release(src);
		return scene; /* caller releases with obs_scene_release */
	}

	void endDrag()
	{
		if (dragItem) {
			obs_sceneitem_release(dragItem);
			dragItem = nullptr;
		}
		dragHandle = xform::None;
	}

	void notifySelect(obs_sceneitem_t *item)
	{
		if (onSelect)
			onSelect(item ? obs_sceneitem_get_id(item) : 0);
	}

	bool mapToCanvas(const QPointF &p, xform::Letterbox &lb, float &cx, float &cy)
	{
		obs_canvas_t *c = canvas::get();
		obs_video_info ovi = {};
		if (!c || !obs_canvas_get_video_info(c, &ovi))
			return false;
		const qreal dpr = devicePixelRatioF();
		lb = xform::Letterbox::fit((uint32_t)std::lround(width() * dpr), (uint32_t)std::lround(height() * dpr),
					   ovi.base_width, ovi.base_height);
		if (!lb.valid())
			return false;
		lb.toCanvas((float)(p.x() * dpr), (float)(p.y() * dpr), cx, cy);
		return true;
	}

	/* Topmost visible item under the point. */
	static obs_sceneitem_t *hitTest(obs_scene_t *scene, float cx, float cy)
	{
		struct Ctx {
			float x, y;
			obs_sceneitem_t *hit;
		} ctx{cx, cy, nullptr};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *c = static_cast<Ctx *>(param);
				if (obs_sceneitem_visible(item) && item_contains(item, c->x, c->y))
					c->hit = item; /* later = higher in the stack */
				return true;
			},
			&ctx);
		return ctx.hit;
	}

	static int handleAt(obs_sceneitem_t *item, float cx, float cy, float tol)
	{
		if (!item_unrotated(item))
			return xform::None; /* rotated items: move only (use Edit Transform) */
		const xform::Rect r = item_rect(item);
		for (int h = 0; h < 8; h++) {
			float fx, fy;
			xform::handle_fraction(h, fx, fy);
			const float hx = r.x + r.w * fx, hy = r.y + r.h * fy;
			if (std::fabs(cx - hx) <= tol && std::fabs(cy - hy) <= tol)
				return h;
		}
		return xform::None;
	}

	void updateHoverCursor(float cx, float cy, const xform::Letterbox &lb)
	{
		Qt::CursorShape shape = Qt::ArrowCursor;
		if (obs_scene_t *scene = strongScene()) {
			obs_sceneitem_t *sel = selected_item(scene);
			const int h = sel ? handleAt(sel, cx, cy, kHandlePx * lb.unitsPerPixel()) : xform::None;
			switch (h) {
			case xform::TopLeft:
			case xform::BottomRight:
				shape = Qt::SizeFDiagCursor;
				break;
			case xform::TopRight:
			case xform::BottomLeft:
				shape = Qt::SizeBDiagCursor;
				break;
			case xform::Left:
			case xform::Right:
				shape = Qt::SizeHorCursor;
				break;
			case xform::Top:
			case xform::Bottom:
				shape = Qt::SizeVerCursor;
				break;
			default:
				if (obs_sceneitem_t *hit = hitTest(scene, cx, cy))
					shape = obs_sceneitem_locked(hit) ? Qt::ArrowCursor : Qt::SizeAllCursor;
			}
			obs_scene_release(scene);
		}
		if (cursor().shape() != shape)
			setCursor(shape);
	}

	void createDisplay()
	{
		if (display || !windowHandle())
			return;

		gs_init_data info = {};
		const QSize s = size() * devicePixelRatioF();
		info.cx = (uint32_t)s.width();
		info.cy = (uint32_t)s.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#if defined(_WIN32)
		info.window.hwnd = reinterpret_cast<void *>(winId());
#elif defined(__APPLE__)
		info.window.view = (id)winId();
#else
		if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
			return; /* Wayland preview needs QPA native handles; see README */
		info.window.id = (uint32_t)winId();
		info.window.display = obs_get_nix_platform_display();
#endif
		display = obs_display_create(&info, 0xFF101010);
		if (display)
			obs_display_add_draw_callback(display, draw, this);
	}

	void destroyDisplay()
	{
		if (!display)
			return;
		obs_display_remove_draw_callback(display, draw, this);
		obs_display_destroy(display);
		display = nullptr;
	}

	/* ---- drawing (graphics thread) ---- */

	static void drawRectOutline(const xform::Rect &r)
	{
		gs_render_start(true);
		gs_vertex2f(r.x, r.y);
		gs_vertex2f(r.right(), r.y);
		gs_vertex2f(r.right(), r.bottom());
		gs_vertex2f(r.x, r.bottom());
		gs_vertex2f(r.x, r.y);
		gs_render_stop(GS_LINESTRIP);
	}

	static void drawFilledSquare(float cx, float cy, float size)
	{
		const float h = size * .5f;
		gs_render_start(true);
		gs_vertex2f(cx - h, cy - h);
		gs_vertex2f(cx + h, cy - h);
		gs_vertex2f(cx - h, cy + h);
		gs_vertex2f(cx + h, cy + h);
		gs_render_stop(GS_TRISTRIP);
	}

	void drawSelection(const xform::Letterbox &lb)
	{
		obs_scene_t *scene = strongScene();
		if (!scene)
			return;

		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
		gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
		vec4 red;
		vec4_set(&red, 1.f, 0.f, 0.f, 1.f);
		gs_effect_set_vec4(color, &red);

		const float handleSize = kHandleDrawPx * lb.unitsPerPixel();
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		struct Ctx {
			float handleSize;
		} ctx{handleSize};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				if (!obs_sceneitem_selected(item))
					return true;
				const float hs = static_cast<Ctx *>(param)->handleSize;
				const xform::Rect r = item_rect(item);
				drawRectOutline(r);
				if (item_unrotated(item) && !obs_sceneitem_locked(item)) {
					for (int h = 0; h < 8; h++) {
						float fx, fy;
						xform::handle_fraction(h, fx, fy);
						drawFilledSquare(r.x + r.w * fx, r.y + r.h * fy, hs);
					}
				}
				return true;
			},
			&ctx);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
		obs_scene_release(scene);
	}

	static void draw(void *data, uint32_t cx, uint32_t cy)
	{
		/* Runs on the graphics thread: take a strong reference so a
		 * scene-collection switch cannot free the canvas mid-draw. */
		obs_canvas_t *c = obs_get_canvas_by_name(VPLUS_CANVAS_NAME);
		if (!c)
			return;
		obs_video_info ovi = {};
		if (!obs_canvas_get_video_info(c, &ovi) || !ovi.base_width || !ovi.base_height) {
			obs_canvas_release(c);
			return;
		}

		const xform::Letterbox lb = xform::Letterbox::fit(cx, cy, ovi.base_width, ovi.base_height);
		if (!lb.valid()) {
			obs_canvas_release(c);
			return;
		}

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_set_viewport(lb.x, lb.y, lb.w, lb.h);
		obs_render_canvas_texture(c);
		static_cast<PreviewWidget *>(data)->drawSelection(lb);
		gs_projection_pop();
		gs_viewport_pop();
		obs_canvas_release(c);
	}
};

/* ------------------------------------------------------------------ */
/* Dock                                                               */

class VerticalDock : public QWidget {
public:
	explicit VerticalDock(QWidget *parent) : QWidget(parent)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(4, 4, 4, 4);

		auto *split = new QSplitter(Qt::Vertical, this);
		preview = new PreviewWidget(split);
		split->addWidget(preview);

		auto *lists = new QWidget(split);
		auto *ll = new QHBoxLayout(lists);
		ll->setContentsMargins(0, 0, 0, 0);

		/* Scenes column */
		auto *sceneCol = new QVBoxLayout();
		sceneCol->addWidget(new QLabel(obs_module_text("Dock.Scenes")));
		scenes = new QListWidget();
		sceneCol->addWidget(scenes);
		auto *sceneBtns = new QHBoxLayout();
		auto *addScene = new QToolButton();
		addScene->setText("+");
		auto *delScene = new QToolButton();
		delScene->setText("-");
		sceneBtns->addWidget(addScene);
		sceneBtns->addWidget(delScene);
		sceneBtns->addStretch();
		sceneCol->addLayout(sceneBtns);
		ll->addLayout(sceneCol);

		/* Items column */
		auto *itemCol = new QVBoxLayout();
		itemCol->addWidget(new QLabel(obs_module_text("Dock.Sources")));
		items = new QListWidget();
		items->setContextMenuPolicy(Qt::CustomContextMenu);
		itemCol->addWidget(items);
		auto *itemBtns = new QHBoxLayout();
		auto *addItem = new QToolButton();
		addItem->setText("+");
		addItem->setPopupMode(QToolButton::InstantPopup);
		auto *addMenu = new QMenu(addItem);
		addItem->setMenu(addMenu);
		auto *delItem = new QToolButton();
		delItem->setText("-");
		itemBtns->addWidget(addItem);
		itemBtns->addWidget(delItem);
		itemBtns->addStretch();
		itemCol->addLayout(itemBtns);
		ll->addLayout(itemCol);

		split->addWidget(lists);
		split->setStretchFactor(0, 3);
		split->setStretchFactor(1, 2);
		root->addWidget(split);

		/* Outputs */
		warning = new QLabel();
		warning->setWordWrap(true);
		warning->setStyleSheet("color: #e0a030;");
		warning->hide();
		root->addWidget(warning);

		auto *outRow = new QHBoxLayout();
		streamBtn = new QPushButton();
		recordBtn = new QPushButton();
		auto *settingsBtn = new QPushButton(obs_module_text("Dock.Settings"));
		outRow->addWidget(streamBtn);
		outRow->addWidget(recordBtn);
		outRow->addWidget(settingsBtn);
		root->addLayout(outRow);

		/* Wiring */
		connect(scenes, &QListWidget::itemClicked, this, [](QListWidgetItem *it) {
			canvas::set_scene(it->text().toStdString());
		});
		connect(scenes, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
			bool okd = false;
			QString name = QInputDialog::getText(this, obs_module_text("Dock.RenameScene"),
							     obs_module_text("Dock.Name"), QLineEdit::Normal,
							     it->text(), &okd);
			if (okd && !name.isEmpty())
				canvas::rename_scene(it->text().toUtf8().constData(), name.toUtf8().constData());
			refresh();
		});
		connect(addScene, &QToolButton::clicked, this, [this]() {
			bool okd = false;
			QString name = QInputDialog::getText(this, obs_module_text("Dock.AddScene"),
							     obs_module_text("Dock.Name"), QLineEdit::Normal,
							     QString(), &okd);
			if (okd && !name.isEmpty())
				canvas::create_scene(name.toUtf8().constData());
			refresh();
		});
		connect(delScene, &QToolButton::clicked, this, [this]() {
			if (QListWidgetItem *it = scenes->currentItem())
				canvas::remove_scene(it->text().toUtf8().constData());
			refresh();
		});

		connect(addMenu, &QMenu::aboutToShow, this, [this, addMenu]() { buildAddMenu(addMenu); });
		connect(delItem, &QToolButton::clicked, this, [this]() { removeSelectedItem(); });
		connect(items, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
			if (updating)
				return;
			withCurrentScene([&](obs_scene_t *scene) {
				links::set_visible(scene, it->text().toUtf8().constData(),
						   it->checkState() == Qt::Checked, true);
			});
		});
		connect(items, &QListWidget::customContextMenuRequested, this,
			[this](const QPoint &pos) { itemMenu(items->mapToGlobal(pos), currentItemId()); });
		connect(items, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *it, QListWidgetItem *) {
			if (updating)
				return;
			const int64_t id = it ? it->data(Qt::UserRole).toLongLong() : 0;
			withCurrentScene([id](obs_scene_t *scene) {
				select_only(scene, id ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr);
			});
		});
		preview->onSelect = [this](int64_t id) { selectRow(id); };
		preview->onContextMenu = [this](const QPoint &pos, int64_t id) { itemMenu(pos, id); };

		connect(streamBtn, &QPushButton::clicked, this, [this]() {
			if (outputs::streaming() || outputs::stream_connecting()) {
				outputs::stop_stream();
			} else if (!outputs::start_stream()) {
				showStreamError();
			}
			refreshButtons();
		});
		connect(recordBtn, &QPushButton::clicked, this, []() {
			if (outputs::recording())
				outputs::stop_record();
			else
				outputs::start_record();
		});
		connect(settingsBtn, &QPushButton::clicked, this, []() { settings_open_dialog(); });
	}

	~VerticalDock() override { disconnectScene(); }

	void refresh()
	{
		updating = true;
		const std::string current = canvas::current_scene();
		scenes->clear();
		for (const std::string &name : canvas::scene_names()) {
			auto *it = new QListWidgetItem(QString::fromStdString(name), scenes);
			if (name == current)
				scenes->setCurrentItem(it);
		}
		updating = false;
		obs_scene_t *cur = canvas::get_scene(current.c_str());
		preview->setScene(cur);
		obs_scene_release(cur);
		connectScene();
		refreshItems();
		refreshButtons();

		std::vector<std::string> conflicts = reserved_track_conflicts();
		if (conflicts.empty()) {
			warning->hide();
		} else {
			QString msg = QString(obs_module_text("Dock.TrackConflict")).arg(settings().audio_track);
			for (const std::string &c : conflicts)
				msg += "\n- " + QString::fromStdString(c);
			warning->setText(msg);
			warning->show();
		}
	}

	void refreshItems()
	{
		updating = true;
		items->clear();
		int64_t selectedId = 0;
		withCurrentScene([this, &selectedId](obs_scene_t *scene) {
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					auto *list = static_cast<QListWidget *>(param);
					obs_source_t *src = obs_sceneitem_get_source(item);
					QString label = QString::fromUtf8(obs_source_get_name(src));
					/* Top of the list = top of the stack, like OBS. */
					auto *it = new QListWidgetItem(label);
					it->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(obs_sceneitem_get_id(item)));
					it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
					it->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
					if (!links::item_link_enabled(item))
						it->setToolTip(obs_module_text("Dock.Unlinked"));
					list->insertItem(0, it);
					return true;
				},
				items);
			if (obs_sceneitem_t *sel = selected_item(scene))
				selectedId = obs_sceneitem_get_id(sel);
		});
		updating = false;
		selectRow(selectedId);
	}

	/* Explain why the vertical stream is not running, with a way to fix it. */
	void showStreamError()
	{
		const std::string err = outputs::stream_error();
		if (err.empty())
			return;
		QMessageBox box(this);
		box.setIcon(QMessageBox::Warning);
		box.setWindowTitle(obs_module_text("StreamError.Title"));
		box.setText(QString::fromUtf8(err.c_str()));
		QPushButton *open = box.addButton(obs_module_text("Dock.OpenSettings"), QMessageBox::AcceptRole);
		box.addButton(QMessageBox::Close);
		box.exec();
		if (box.clickedButton() == open)
			settings_open_dialog();
	}

	void refreshButtons()
	{
		const char *streamKey = outputs::stream_reconnecting() ? "Dock.Reconnecting"
					: outputs::streaming()          ? "Dock.StopStream"
					: outputs::stream_connecting()  ? "Dock.Connecting"
									: "Dock.StartStream";
		streamBtn->setText(obs_module_text(streamKey));
		streamBtn->setStyleSheet(outputs::streaming() && !outputs::stream_reconnecting()
						 ? "QPushButton { background-color: #8b1a1a; color: white; }"
						 : "");
		recordBtn->setText(obs_module_text(outputs::recording() ? "Dock.StopRecord" : "Dock.StartRecord"));
	}

private:
	PreviewWidget *preview = nullptr;
	QListWidget *scenes = nullptr;
	QListWidget *items = nullptr;
	QLabel *warning = nullptr;
	QPushButton *streamBtn = nullptr;
	QPushButton *recordBtn = nullptr;
	bool updating = false;
	obs_source_t *connectedScene = nullptr; /* strong ref */

	template<typename F> void withCurrentScene(F &&fn)
	{
		if (obs_scene_t *scene = canvas::get_scene(canvas::current_scene().c_str())) {
			fn(scene);
			obs_scene_release(scene);
		}
	}

	static void sceneSignal(void *data, calldata_t *)
	{
		QPointer<VerticalDock> dock = static_cast<VerticalDock *>(data);
		QMetaObject::invokeMethod(
			dock.data(),
			[dock]() {
				if (dock)
					dock->refreshItems();
			},
			Qt::QueuedConnection);
	}

	void connectScene()
	{
		disconnectScene();
		withCurrentScene([this](obs_scene_t *scene) {
			connectedScene = obs_source_get_ref(obs_scene_get_source(scene));
			signal_handler_t *sh = obs_source_get_signal_handler(connectedScene);
			for (const char *sig : {"item_add", "item_remove", "item_visible", "reorder", "refresh"})
				signal_handler_connect(sh, sig, sceneSignal, this);
		});
	}

	void disconnectScene()
	{
		if (!connectedScene)
			return;
		signal_handler_t *sh = obs_source_get_signal_handler(connectedScene);
		for (const char *sig : {"item_add", "item_remove", "item_visible", "reorder", "refresh"})
			signal_handler_disconnect(sh, sig, sceneSignal, this);
		obs_source_release(connectedScene);
		connectedScene = nullptr;
	}

	int64_t currentItemId() const
	{
		QListWidgetItem *it = items->currentItem();
		return it ? it->data(Qt::UserRole).toLongLong() : 0;
	}

	obs_sceneitem_t *selectedItem(obs_scene_t *scene)
	{
		const int64_t id = currentItemId();
		return id ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
	}

	void selectRow(int64_t id)
	{
		const bool prev = updating;
		updating = true;
		items->setCurrentItem(nullptr);
		for (int i = 0; id && i < items->count(); i++) {
			if (items->item(i)->data(Qt::UserRole).toLongLong() == id) {
				items->setCurrentItem(items->item(i));
				break;
			}
		}
		updating = prev;
	}

	void buildAddMenu(QMenu *menu)
	{
		menu->clear();
		QAction *reframe = menu->addAction(obs_module_text("Dock.AddReframe"));
		connect(reframe, &QAction::triggered, this, [this]() { addReframe(); });
		menu->addSeparator();

		/* Existing inputs and main scenes (as nested scenes). */
		struct Ctx {
			QMenu *menu;
			VerticalDock *dock;
		} ctx{menu, this};
		obs_enum_sources(
			[](void *param, obs_source_t *src) {
				auto *c = static_cast<Ctx *>(param);
				if (strcmp(obs_source_get_id(src), VPLUS_REFRAME_ID) == 0)
					return true;
				QString name = QString::fromUtf8(obs_source_get_name(src));
				QAction *a = c->menu->addAction(name);
				VerticalDock *dock = c->dock;
				QObject::connect(a, &QAction::triggered, dock, [dock, name]() { dock->addExisting(name); });
				return true;
			},
			&ctx);
		obs_frontend_source_list list = {};
		obs_frontend_get_scenes(&list);
		if (list.sources.num)
			menu->addSeparator();
		for (size_t i = 0; i < list.sources.num; i++) {
			QString name = QString::fromUtf8(obs_source_get_name(list.sources.array[i]));
			QAction *a = menu->addAction(QString(obs_module_text("Dock.MainScene")).arg(name));
			connect(a, &QAction::triggered, this, [this, name]() { addExisting(name); });
		}
		obs_frontend_source_list_free(&list);
	}

	void addExisting(const QString &name)
	{
		obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
		if (!src)
			return;
		withCurrentScene([&](obs_scene_t *scene) {
			if (obs_sceneitem_t *item = obs_scene_add(scene, src))
				applyLayout(item, "fit");
		});
		obs_source_release(src);
		refreshItems();
	}

	void addReframe()
	{
		int n = 1;
		std::string name;
		for (;; n++) {
			name = std::string(obs_module_text("Reframe.Name")) + (n > 1 ? " " + std::to_string(n) : "");
			obs_source_t *exists = obs_get_source_by_name(name.c_str());
			if (!exists)
				break;
			obs_source_release(exists);
		}
		obs_source_t *src = obs_source_create(VPLUS_REFRAME_ID, name.c_str(), nullptr, nullptr);
		if (!src)
			return;
		withCurrentScene([&](obs_scene_t *scene) {
			if (obs_sceneitem_t *item = obs_scene_add(scene, src))
				applyLayout(item, "fill");
		});
		obs_source_release(src);
		refreshItems();
	}

	void removeSelectedItem()
	{
		withCurrentScene([this](obs_scene_t *scene) {
			if (obs_sceneitem_t *item = selectedItem(scene))
				obs_sceneitem_remove(item);
		});
		refreshItems();
	}

	static void applyLayout(obs_sceneitem_t *item, const char *layout)
	{
		const float W = (float)settings().width, H = (float)settings().height;
		obs_transform_info info = {};
		obs_sceneitem_get_info2(item, &info);
		info.rot = 0.0f;
		info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
		info.bounds_alignment = OBS_ALIGN_CENTER;
		info.crop_to_bounds = false;

		if (strcmp(layout, "fill") == 0) {
			vec2_set(&info.pos, 0.0f, 0.0f);
			vec2_set(&info.bounds, W, H);
			info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
			info.crop_to_bounds = true;
		} else if (strcmp(layout, "top") == 0) {
			vec2_set(&info.pos, 0.0f, 0.0f);
			vec2_set(&info.bounds, W, H / 2.0f);
			info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
			info.crop_to_bounds = true;
		} else if (strcmp(layout, "bottom") == 0) {
			vec2_set(&info.pos, 0.0f, H / 2.0f);
			vec2_set(&info.bounds, W, H / 2.0f);
			info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
			info.crop_to_bounds = true;
		} else { /* fit */
			vec2_set(&info.pos, 0.0f, 0.0f);
			vec2_set(&info.bounds, W, H);
			info.bounds_type = OBS_BOUNDS_SCALE_INNER;
		}
		obs_sceneitem_set_info2(item, &info);
	}

	void itemMenu(const QPoint &globalPos, int64_t id)
	{
		if (!id)
			return;
		obs_scene_t *scene = canvas::get_scene(canvas::current_scene().c_str());
		if (!scene)
			return;
		obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, id);
		if (!item) {
			obs_scene_release(scene);
			return;
		}
		obs_sceneitem_addref(item);
		obs_scene_release(scene);

		QMenu menu(this);
		connect(menu.addAction(obs_module_text("Dock.EditTransform")), &QAction::triggered, this,
			[item]() { obs_frontend_open_sceneitem_edit_transform(item); });
		connect(menu.addAction(obs_module_text("Dock.CenterItem")), &QAction::triggered, this, [item]() {
			const xform::Rect r = item_rect(item);
			place_item_at(item, ((float)settings().width - r.w) * .5f, ((float)settings().height - r.h) * .5f);
		});
		connect(menu.addAction(obs_module_text("Dock.ResetTransform")), &QAction::triggered, this, [item]() {
			obs_transform_info info = {};
			vec2_set(&info.scale, 1.f, 1.f);
			info.alignment = OBS_ALIGN_LEFT | OBS_ALIGN_TOP;
			info.bounds_type = OBS_BOUNDS_NONE;
			info.bounds_alignment = OBS_ALIGN_CENTER;
			obs_sceneitem_set_info2(item, &info);
			obs_sceneitem_crop crop = {};
			obs_sceneitem_set_crop(item, &crop);
		});
		menu.addSeparator();
		struct L {
			const char *key;
			const char *layout;
		};
		for (L l : {L{"Layout.Fit", "fit"}, L{"Layout.Fill", "fill"}, L{"Layout.Top", "top"},
			    L{"Layout.Bottom", "bottom"}}) {
			const char *layout = l.layout;
			connect(menu.addAction(obs_module_text(l.key)), &QAction::triggered, this,
				[item, layout]() { applyLayout(item, layout); });
		}
		menu.addSeparator();
		QAction *link = menu.addAction(obs_module_text("Dock.LinkVisibility"));
		link->setCheckable(true);
		link->setChecked(links::item_link_enabled(item));
		connect(link, &QAction::toggled, this, [this, item](bool on) {
			links::set_item_link_enabled(item, on);
			refreshItems();
		});
		menu.addSeparator();
		connect(menu.addAction(obs_module_text("Dock.Properties")), &QAction::triggered, this,
			[item]() { obs_frontend_open_source_properties(obs_sceneitem_get_source(item)); });
		connect(menu.addAction(obs_module_text("Dock.Filters")), &QAction::triggered, this,
			[item]() { obs_frontend_open_source_filters(obs_sceneitem_get_source(item)); });

		menu.exec(globalPos);
		obs_sceneitem_release(item);
	}
};

static QPointer<VerticalDock> g_dock;

void dock_create()
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	g_dock = new VerticalDock(main);
	if (!obs_frontend_add_dock_by_id("vertical-plus-dock", obs_module_text("VerticalPlus"), g_dock.data()))
		vlog(LOG_WARNING, "could not add the Vertical+ dock");

	canvas::on_scene_changed([](const std::string &) { dock_refresh(); });
	outputs::on_event([](outputs::Event e) {
		if (!g_dock)
			return;
		g_dock->refreshButtons();
		if (e == outputs::Event::StreamFailed)
			g_dock->showStreamError();
	});
}

void dock_refresh()
{
	if (g_dock)
		g_dock->refresh();
}

} // namespace vplus
