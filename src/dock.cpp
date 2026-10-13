/*
 * Vertical+ dock: live preview, vertical scene list, item list with
 * visibility and link toggles, quick layouts, and output buttons.
 *
 * Plain Qt widgets with lambda connections (no moc needed).
 * Drag-to-transform inside the preview is milestone 4; for now items are
 * placed with the layout presets in the item context menu.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "vplus.hpp"

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <cstring>

#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
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
/* Preview                                                            */

class PreviewWidget : public QWidget {
public:
	explicit PreviewWidget(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(90, 160);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	~PreviewWidget() override { destroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

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

private:
	obs_display_t *display = nullptr;

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

	static void draw(void *, uint32_t cx, uint32_t cy)
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

		/* Letterbox the canvas into the widget. */
		const float sx = (float)cx / (float)ovi.base_width;
		const float sy = (float)cy / (float)ovi.base_height;
		const float scale = sx < sy ? sx : sy;
		const int w = (int)(ovi.base_width * scale), h = (int)(ovi.base_height * scale);
		const int x = ((int)cx - w) / 2, y = ((int)cy - h) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)ovi.base_width, 0.0f, (float)ovi.base_height, -100.0f, 100.0f);
		gs_set_viewport(x, y, w, h);
		obs_render_canvas_texture(c);
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
			[this](const QPoint &pos) { itemMenu(items->mapToGlobal(pos)); });

		connect(streamBtn, &QPushButton::clicked, this, []() {
			if (outputs::streaming())
				outputs::stop_stream();
			else
				outputs::start_stream();
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
		withCurrentScene([this](obs_scene_t *scene) {
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					auto *list = static_cast<QListWidget *>(param);
					obs_source_t *src = obs_sceneitem_get_source(item);
					QString label = QString::fromUtf8(obs_source_get_name(src));
					/* Top of the list = top of the stack, like OBS. */
					auto *it = new QListWidgetItem(label);
					it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
					it->setCheckState(obs_sceneitem_visible(item) ? Qt::Checked : Qt::Unchecked);
					if (!links::item_link_enabled(item))
						it->setToolTip(obs_module_text("Dock.Unlinked"));
					list->insertItem(0, it);
					return true;
				},
				items);
		});
		updating = false;
	}

	void refreshButtons()
	{
		streamBtn->setText(obs_module_text(outputs::streaming() ? "Dock.StopStream" : "Dock.StartStream"));
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

	obs_sceneitem_t *selectedItem(obs_scene_t *scene)
	{
		QListWidgetItem *it = items->currentItem();
		if (!it)
			return nullptr;
		return obs_scene_find_source(scene, it->text().toUtf8().constData());
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

	void itemMenu(const QPoint &globalPos)
	{
		obs_scene_t *scene = canvas::get_scene(canvas::current_scene().c_str());
		if (!scene)
			return;
		obs_sceneitem_t *item = selectedItem(scene);
		if (!item) {
			obs_scene_release(scene);
			return;
		}
		obs_sceneitem_addref(item);
		obs_scene_release(scene);

		QMenu menu(this);
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
	outputs::on_event([](outputs::Event) {
		if (g_dock)
			g_dock->refreshButtons();
	});
}

void dock_refresh()
{
	if (g_dock)
		g_dock->refresh();
}

} // namespace vplus
