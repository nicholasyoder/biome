// SPDX-License-Identifier: LGPL-3.0-or-later

#include "menu.h"

#include "frame_widget.h" // repolish_tree, show_offscreen, flush_layouts

#include <QAction>
#include <QMenu>
#include <QString>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

namespace biome_decoration {

namespace {

// Transparent root owning the QMenu as a child: a styled root would paint a
// flat rect instead of honouring QSS border-radius (see switcher.cpp's
// SwitcherRoot). Qt::Widget instead of the default Popup flag, since the menu
// is never mapped as a window.
class MenuRoot : public QWidget {
public:
    MenuRoot() {
        setObjectName("biomeMenuRoot");
        setAttribute(Qt::WA_TranslucentBackground);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        menu = new QMenu(this);
        menu->setWindowFlags(Qt::Widget);
        menu->setObjectName("biomeMenu");
        layout->addWidget(menu);
        show_offscreen(this);
    }

    QMenu *menu = nullptr;
};

MenuRoot *g_roots[2] = {};

} // namespace

RenderedMenu render_menu(int panel, const std::vector<MenuEntry> &entries, int hovered_id, double scale) {
    RenderedMenu result;
    if (entries.empty() || panel < 0 || panel > 1) {
        return result;
    }
    if (g_roots[panel] == nullptr) {
        g_roots[panel] = new MenuRoot();
    }
    MenuRoot *root = g_roots[panel];
    QMenu *menu = root->menu;

    // Synchronous delete (not deleteLater), same reasoning as switcher.cpp.
    for (QAction *old : menu->actions()) {
        menu->removeAction(old);
        delete old->menu();
        delete old;
    }

    std::vector<std::pair<int, QAction *>> actions;
    QAction *hovered = nullptr;
    for (const MenuEntry &entry : entries) {
        if (entry.separator) {
            menu->addSeparator();
            continue;
        }
        // Labels are untrusted-adjacent (workspace names later); '&' would become a mnemonic.
        QString label = QString::fromUtf8(entry.label.c_str());
        label.replace('&', "&&");
        auto *action = new QAction(label, menu);
        action->setEnabled(entry.enabled);
        if (entry.checked) {
            action->setCheckable(true);
            action->setChecked(true);
        }
        if (entry.submenu) {
            action->setMenu(new QMenu(menu)); // never shown; only gives the arrow
        }
        menu->addAction(action);
        actions.emplace_back(entry.id, action);
        if (entry.id == hovered_id && entry.enabled) {
            hovered = action;
        }
    }
    menu->setActiveAction(hovered);

    repolish_tree(root);
    flush_layouts();
    menu->setFixedSize(menu->sizeHint());
    root->adjustSize();
    flush_layouts();

    for (const auto &[id, action] : actions) {
        QRect rect = menu->actionGeometry(action).translated(menu->pos());
        result.items.push_back({id, rect.x(), rect.y(), rect.width(), rect.height()});
    }
    QStyle *style = menu->style();
    result.margin = qMax(0, style->pixelMetric(QStyle::PM_MenuPanelWidth, nullptr, menu)
                            - style->pixelMetric(QStyle::PM_DefaultFrameWidth, nullptr, menu));
    result.frame = render_widget(root, scale);
    return result;
}

} // namespace biome_decoration
