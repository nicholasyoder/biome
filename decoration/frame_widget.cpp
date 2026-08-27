// SPDX-License-Identifier: LGPL-3.0-or-later

#include "frame_widget.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QImage>
#include <QLayout>
#include <QPixmap>
#include <QSizePolicy>
#include <QStyle>

namespace biome_decoration {

void repolish_tree(QWidget *root) {
    root->style()->unpolish(root);
    root->style()->polish(root);
    for (QWidget *child : root->findChildren<QWidget *>()) {
        child->style()->unpolish(child);
        child->style()->polish(child);
    }
}

void force_activate_layouts(QWidget *root) {
    if (QLayout *layout = root->layout()) {
        layout->invalidate();
        layout->activate();
    }
    for (QObject *child : root->children()) {
        if (auto *child_widget = qobject_cast<QWidget *>(child)) {
            force_activate_layouts(child_widget);
        }
    }
}

namespace {
// Pixels near a corner that count as a diagonal resize handle rather than a
// plain edge - a click-precision convention, not a themed value, so it has
// no QSS equivalent.
constexpr int kResizeCornerSize = 8;
} // namespace

DecorationButton::DecorationButton(Region region, QWidget *parent)
        : QToolButton(parent), region_(region) {
    switch (region_) {
    case Region::ButtonMinimize: setObjectName("biomeButtonMinimize"); break;
    case Region::ButtonMaximize: setObjectName("biomeButtonMaximize"); break;
    case Region::ButtonClose: setObjectName("biomeButtonClose"); break;
    default: break;
    }
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_StyledBackground, true);
}

DecorationBorder::DecorationBorder(const QString &object_name, QWidget *parent) : QWidget(parent) {
    setObjectName(object_name);
    setAttribute(Qt::WA_StyledBackground, true);
}

DecorationFrame::DecorationFrame(QWidget *parent) : QFrame(parent) {
    setObjectName("biomeFrame");
    setProperty("focused", true);
    setProperty("biomeMaximized", false);

    titlebar_ = new QWidget(this);
    titlebar_->setObjectName("biomeTitlebar");

    title_label_ = new QLabel(titlebar_);
    title_label_->setObjectName("biomeTitle");
    // Ignored on the horizontal axis so a long title never grows the frame -
    // it should elide/clip within whatever space the buttons/borders leave.
    title_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Centered within its own rect, which isn't itself centered in the full
    // titlebar (icon/buttons flank it at unequal widths) - close enough,
    // not worth matched-width spacers to fix.
    title_label_->setAlignment(Qt::AlignCenter);

    icon_button_ = new QToolButton(titlebar_);
    icon_button_->setObjectName("biomeTitleIcon");
    icon_button_->setFocusPolicy(Qt::NoFocus);
    icon_button_->setAttribute(Qt::WA_StyledBackground, true);
    icon_button_->hide(); // no gap shown until setIcon() gives it a real icon

    button_minimize_ = new DecorationButton(Region::ButtonMinimize, titlebar_);
    button_maximize_ = new DecorationButton(Region::ButtonMaximize, titlebar_);
    button_close_ = new DecorationButton(Region::ButtonClose, titlebar_);

    auto *titlebar_layout = new QHBoxLayout(titlebar_);
    titlebar_layout->setContentsMargins(0, 0, 0, 0);
    titlebar_layout->setSpacing(0);
    titlebar_layout->addWidget(icon_button_, 0, Qt::AlignVCenter);
    titlebar_layout->addWidget(title_label_, /*stretch=*/1, Qt::AlignVCenter);
    titlebar_layout->addWidget(button_minimize_, 0, Qt::AlignVCenter);
    titlebar_layout->addWidget(button_maximize_, 0, Qt::AlignVCenter);
    titlebar_layout->addWidget(button_close_, 0, Qt::AlignVCenter);

    border_left_ = new DecorationBorder("biomeBorderLeft", this);
    border_right_ = new DecorationBorder("biomeBorderRight", this);
    border_bottom_ = new DecorationBorder("biomeBorderBottom", this);

    content_spacer_ = new QWidget(this);
    content_spacer_->setObjectName("biomeContent");

    // middle_row: left/right border strips flank content_spacer_, sized by
    // layoutFor() below to exactly the client's content area.
    auto *middle_row = new QHBoxLayout();
    middle_row->setContentsMargins(0, 0, 0, 0);
    middle_row->setSpacing(0);
    middle_row->addWidget(border_left_);
    middle_row->addWidget(content_spacer_);
    middle_row->addWidget(border_right_);

    auto *main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);
    main_layout->addWidget(titlebar_);
    main_layout->addLayout(middle_row);
    main_layout->addWidget(border_bottom_);
}

void DecorationFrame::layoutFor(int content_width, int content_height) {
    // Hot path (every resize and hover/press re-render calls this, often
    // with an unchanged size) - skip the repolish/layout/resize dance when
    // the frame is already sized for this content.
    if (content_spacer_->width() == content_width && content_spacer_->height() == content_height) {
        return;
    }

    content_spacer_->setFixedSize(content_width, content_height);
    // setFixedSize() normally invalidates the layout via a posted
    // QEvent::LayoutRequest, delivered whenever the Qt event loop next runs -
    // this code can't assume that happens promptly (or between this call and
    // the next), so force it synchronously instead. See force_activate_layouts()'s
    // own doc comment (frame_widget.h) for why that's still true even though
    // ipc/global_shortcuts_portal.cpp now pumps Qt's event loop periodically
    // for D-Bus - otherwise minimumSizeHint() below could read a stale size
    // left by this shared widget's previous render.
    force_activate_layouts(this);
    // QLayout::activate() on a top-level widget only ever grows it, never
    // shrinks - resize to the true minimum explicitly before the final
    // re-activate positions every child against it.
    resize(minimumSizeHint());
    force_activate_layouts(this);
}

Region DecorationFrame::hitTest(
        int local_x, int local_y, int content_width, int content_height, bool maximized) {
    setMaximizedState(maximized);
    layoutFor(content_width, content_height);

    int w = width();
    int h = height();
    if (local_x < 0 || local_y < 0 || local_x >= w || local_y >= h) {
        return Region::None;
    }

    // Buttons win over every resize check below, even if a theme gives them
    // little/no margin and they sit inside a corner or edge hit zone.
    QWidget *hit = childAt(local_x, local_y);
    if (hit == button_minimize_) {
        return Region::ButtonMinimize;
    }
    if (hit == button_maximize_) {
        return Region::ButtonMaximize;
    }
    if (hit == button_close_) {
        return Region::ButtonClose;
    }

    // No resizing while maximized (standard WM convention) - enforced
    // explicitly rather than relying on this theme's [biomeMaximized="true"]
    // QSS collapsing the border strips to 0 size, since that's this theme's
    // choice, not a guarantee every theme makes.
    if (!maximized) {
        // Corners take priority over the plain edges below, giving diagonal
        // resize a real hit target near each corner.
        bool near_left = local_x < kResizeCornerSize;
        bool near_right = local_x >= w - kResizeCornerSize;
        bool near_top = local_y < kResizeCornerSize;
        bool near_bottom = local_y >= h - kResizeCornerSize;
        if (near_top && near_left) {
            return Region::ResizeNW;
        }
        if (near_top && near_right) {
            return Region::ResizeNE;
        }
        if (near_bottom && near_left) {
            return Region::ResizeSW;
        }
        if (near_bottom && near_right) {
            return Region::ResizeSE;
        }

        // The titlebar row has no border_left_/border_right_ of its own
        // (those only flank the middle content row), so its edges need the
        // same geometry-based margin the corners above use.
        if (local_y < titlebarHeight()) {
            if (near_top) {
                return Region::ResizeN;
            }
            if (near_left) {
                return Region::ResizeW;
            }
            if (near_right) {
                return Region::ResizeE;
            }
        }

        // Everywhere else, ask the widget tree instead of geometry math.
        if (hit == border_bottom_) {
            return Region::ResizeS;
        }
        if (hit == border_left_) {
            return Region::ResizeW;
        }
        if (hit == border_right_) {
            return Region::ResizeE;
        }
    }

    if (hit == titlebar_ || hit == title_label_ || hit == icon_button_) {
        // The icon isn't clickable yet (no context menu) - it's just part of
        // the draggable titlebar, like the title text.
        return Region::Titlebar;
    }
    return Region::None; // content_spacer_ (the client's surface), or nothing
}

void DecorationFrame::setFocusedState(bool focused) {
    if (property("focused").toBool() == focused) {
        return;
    }
    setProperty("focused", focused);
    // Descendant selectors keyed off #biomeFrame[focused="..."] need their
    // own repolish - Qt's per-widget stylesheet cache isn't invalidated just
    // because an ancestor's dynamic property changed.
    repolish_tree(this);
}

void DecorationFrame::setMaximizedState(bool maximized) {
    // Named "biomeMaximized", not "maximized": QWidget already declares a
    // read-only Q_PROPERTY called "maximized" (bool maximized READ
    // isMaximized), and setProperty() silently no-ops on a static property
    // with no WRITE function - every [maximized=...] QSS selector would
    // never have matched.
    if (property("biomeMaximized").toBool() == maximized) {
        return;
    }
    setProperty("biomeMaximized", maximized);
    repolish_tree(this); // same rationale as setFocusedState() above
    // Unlike setFocusedState()'s color-only rules, [biomeMaximized=...] rules
    // can change border strips' min-/max-width/height - real geometry, not
    // just paint. repolish_tree() updates each border's minimum/maximumSize,
    // but the layout that actually resizes them to match only reflows via a
    // posted QEvent::LayoutRequest, which this code can't rely on arriving
    // promptly (see layoutFor()'s own comment on why). A plain
    // force_activate_layouts() alone isn't enough either: activate() on a
    // top-level widget only ever grows it, so shrinking a border would just
    // hand the freed space to the titlebar
    // instead of shrinking the frame. The explicit resize(minimumSizeHint())
    // below forces that shrink before the final re-activate.
    force_activate_layouts(this);
    resize(minimumSizeHint());
    force_activate_layouts(this);
}

void DecorationFrame::setTitle(const QString &title) {
    // A client's title is untrusted text - simplified() collapses any
    // embedded newlines, which QLabel would otherwise render as a hard line
    // break, growing the titlebar to fit.
    title_label_->setText(title.simplified());
}

void DecorationFrame::setIcon(const IconImage &icon) {
    bool has_icon = icon.size > 0 && !icon.pixels.empty();
    if (has_icon) {
        // QImage wraps icon.pixels' own memory (no copy) - fine since
        // QPixmap::fromImage() below copies out of it before this returns.
        QImage image(icon.pixels.data(), icon.size, icon.size, QImage::Format_ARGB32_Premultiplied);
        icon_button_->setIcon(QIcon(QPixmap::fromImage(image)));
    }
    if (has_icon == icon_button_->isHidden()) {
        icon_button_->setVisible(has_icon);
        // Same invalidate/resize/invalidate dance as setMaximizedState()'s
        // border toggling - showing/hiding a layout item is a box-model
        // change, not just paint.
        force_activate_layouts(this);
        resize(minimumSizeHint());
        force_activate_layouts(this);
    }
}

void DecorationFrame::setHoveredRegion(Region region) {
    for (DecorationButton *btn : {button_minimize_, button_maximize_, button_close_}) {
        bool should_hover = (btn->region() == region);
        if (should_hover == btn->underMouse()) {
            continue;
        }
        if (should_hover) {
            QEnterEvent enter_event{QPointF(), QPointF(), QPointF()};
            QCoreApplication::sendEvent(btn, &enter_event);
        } else {
            QEvent leave_event(QEvent::Leave);
            QCoreApplication::sendEvent(btn, &leave_event);
        }
    }
}

void DecorationFrame::setPressedRegion(Region region) {
    for (DecorationButton *btn : {button_minimize_, button_maximize_, button_close_}) {
        btn->setDown(btn->region() == region);
    }
}

} // namespace biome_decoration
