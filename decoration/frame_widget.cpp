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

void show_offscreen(QWidget *root) {
    // One shared 1x1 host means one tiny backing store - a shown top-level
    // per tree would allocate (and repaint) a window-sized one for nothing.
    static QWidget *host = [] {
        auto *widget = new QWidget();
        widget->setAttribute(Qt::WA_DontShowOnScreen);
        widget->resize(1, 1);
        widget->show();
        return widget;
    }();
    root->setParent(host);
    root->move(1, 1); // outside the host, so never painted into its backing store
    root->show();
}

namespace {
struct LayoutRequestCounter : QObject {
    int count = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::LayoutRequest) {
            count++;
        }
        return false;
    }
};
} // namespace

void flush_layouts() {
    // Each pass only delivers already-posted events, and a child's relayout
    // posts its parent's request - repeat until the tree settles, as the event loop would.
    LayoutRequestCounter counter;
    QCoreApplication::instance()->installEventFilter(&counter);
    do {
        counter.count = 0;
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    } while (counter.count > 0);
    QCoreApplication::instance()->removeEventFilter(&counter);
}

QIcon fallback_icon() {
    static const QIcon icon = QIcon::fromTheme("application-x-executable");
    return icon;
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
    icon_button_->hide(); // shown by setIcon() once it has a real or fallback icon to set

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
    // Frame tracks its contents' size hint, shrinking as well as growing.
    main_layout->setSizeConstraint(QLayout::SetFixedSize);

    show_offscreen(this);
}

void DecorationFrame::layoutFor(int content_width, int content_height) {
    // Hot path (every resize and hover/press re-render calls this, often
    // with an unchanged size) - skip the repolish/layout/resize dance when
    // the frame is already sized for this content.
    if (content_spacer_->width() == content_width && content_spacer_->height() == content_height) {
        return;
    }

    content_spacer_->setFixedSize(content_width, content_height);
    flush_layouts();
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
    // Unlike setFocusedState(), these rules can change border sizes, not just paint.
    repolish_tree(this);
    flush_layouts();
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
    } else {
        icon_button_->setIcon(fallback_icon());
    }
    if (icon_button_->isHidden()) {
        icon_button_->setVisible(true); // only on the first call; never hidden again
        flush_layouts();
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
