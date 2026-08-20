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
// How many pixels near a corner count as a diagonal resize handle rather
// than a plain edge - pure WM click-precision convention, not a rendered
// decoration element, so there's no QSS/widget equivalent to source it
// from (unlike border/titlebar/button metrics below).
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
    // Ignored (not the QLabel default of Preferred) on the horizontal axis
    // so a long title's natural font-metric width never enters the layout's
    // minimum-size computation below - the label should elide/clip within
    // whatever space the buttons/borders leave it, never grow the frame.
    title_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // Centers within the label's own (stretched-to-fill) rect - since the
    // icon sits to its left and the buttons to its right in unequal widths
    // (see titlebar_layout below), that rect isn't itself centered in the
    // full titlebar, so this reads as close to but not exactly centered;
    // accepted as fine rather than adding matched-width spacers to correct
    // it.
    title_label_->setAlignment(Qt::AlignCenter);

    icon_button_ = new QToolButton(titlebar_);
    icon_button_->setObjectName("biomeTitleIcon");
    icon_button_->setFocusPolicy(Qt::NoFocus);
    icon_button_->setAttribute(Qt::WA_StyledBackground, true);
    // Hidden until setIcon() below is given a real IconImage - a window with
    // no resolvable icon shows no gap for it at all, not an empty slot.
    icon_button_->hide();

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

    // middle_row: the left/right border strips flank content_spacer_, which
    // layoutFor() below sizes to exactly the client's content area
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
    // Resizing (and hover/press re-renders, which call this with an
    // unchanged content size every time) is the hot path for this widget -
    // skip the repolish/layout/resize dance entirely when the frame is
    // already sized for this content, rather than redoing it on every mouse
    // motion event regardless of whether anything actually changed.
    if (content_spacer_->width() == content_width && content_spacer_->height() == content_height) {
        return;
    }

    content_spacer_->setFixedSize(content_width, content_height);
    // Invalidate first: setFixedSize() normally flags stale layout caches
    // via a posted QEvent::LayoutRequest, which never arrives since Biome
    // has no running Qt event loop to pump it. Without this, minimumSizeHint()
    // below could read a stale cached size from decoration_frame()'s
    // *previous* caller - this widget is shared across every toplevel's
    // render, so "previous" often means a different window's content size.
    force_activate_layouts(this);
    // minimumSizeHint() is the frame's true required size now that it's
    // fresh. Resizing to it explicitly (vs. leaving it to the second
    // force_activate_layouts() call below) matters because QLayout::
    // activate() on a top-level widget only ever grows it, never shrinks.
    resize(minimumSizeHint());
    // Re-activate now that the frame is at its correct final size, so every
    // child is positioned/sized against that instead of the stale one above.
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
    // little/no margin and they end up sitting inside a corner or edge hit
    // zone - a click is only ever a resize when it's outside the button's
    // actual widget geometry.
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

    // No resizing at all while maximized - a maximized window must be
    // demaximized first (standard WM convention). This can't be left to
    // border_left_/border_right_/border_bottom_ collapsing to 0 size under
    // this theme's [biomeMaximized="true"] QSS (see biome-dark.qss): that's
    // this theme's choice, not a guarantee every theme makes, so it's
    // enforced explicitly here instead.
    if (!maximized) {
        // Corners take priority over the plain edges below - a WM
        // convention that gives diagonal resize a large-enough hit target
        // near the frame's corners instead of it being a single-pixel
        // coincidence of two edges.
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
        // (those only flank the middle content row - see the constructor),
        // so unlike the childAt() checks below, its top edge and sides need
        // the same geometry-based margin the corners above use rather than
        // a widget to ask.
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

        // Everywhere else, ask the real widget tree what's actually there
        // instead of re-deriving it from geometry math.
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
        // The icon isn't clickable (no context menu yet) - it's just part of
        // the draggable titlebar, same as the title text and empty titlebar
        // space, rather than a dead zone.
        return Region::Titlebar;
    }
    // content_spacer_ (the client's own surface, not part of the
    // decoration), or nothing - both mean "not our region".
    return Region::None;
}

void DecorationFrame::setFocusedState(bool focused) {
    if (property("focused").toBool() == focused) {
        return;
    }
    setProperty("focused", focused);
    // Descendant selectors keyed off #biomeFrame[focused="..."] (the title
    // label, the buttons' glyph color) need their own repolish - Qt's
    // stylesheet rule cache is per-widget and isn't invalidated just because
    // an ancestor's dynamic property changed.
    repolish_tree(this);
}

void DecorationFrame::setMaximizedState(bool maximized) {
    // Deliberately NOT "maximized" - QWidget already declares a real,
    // read-only Q_PROPERTY of exactly that name (bool maximized READ
    // isMaximized, see qwidget.h), and QObject::setProperty() silently
    // no-ops (returns false, value left untouched) when a static property
    // has no WRITE function. Verified directly: setProperty("maximized",
    // true) on a QWidget-derived instance left property("maximized")
    // reading back false, no error, no warning - every [maximized=...] QSS
    // selector would have silently never matched.
    if (property("biomeMaximized").toBool() == maximized) {
        return;
    }
    setProperty("biomeMaximized", maximized);
    // Same rationale as setFocusedState() above - descendant selectors keyed
    // off #biomeFrame[biomeMaximized="..."] need their own repolish since an
    // ancestor's dynamic property change alone doesn't invalidate a child's
    // cached stylesheet rules.
    repolish_tree(this);
    // Unlike setFocusedState()'s QSS rules (colors only), [biomeMaximized=...]
    // rules can change border strips' min-/max-width/height - real box-model
    // geometry, not just paint. polish() (inside repolish_tree() above)
    // updates each border widget's minimum/maximumSize from the new
    // stylesheet, but the *layout* that actually resizes them to match only
    // reflows via a posted QEvent::LayoutRequest - which never arrives, same
    // "no running Qt event loop" issue layoutFor() already works around (see
    // its own comment). Without this, borderWidth()/rightBorderWidth()/
    // bottomBorderHeight()/hitTest() would keep reading the *previous*
    // state's stale sizes right after this call.
    //
    // A plain force_activate_layouts() alone isn't enough here either -
    // verified directly. Shrinking a border to 0 shrinks the *frame's*
    // minimumSizeHint, but activate() on a top-level widget only ever grows
    // it to fit a bigger hint, never shrinks it to a smaller one (same
    // caveat layoutFor() documents above), so the frame was staying at its
    // previous, larger size and silently handing the freed-up space to
    // whichever sibling widget - the titlebar in practice - happened to be
    // the QLayout's only non-fixed-size item, instead of actually shrinking.
    // The explicit resize(minimumSizeHint()) below (identical to layoutFor())
    // forces that shrink before the final re-activate.
    force_activate_layouts(this);
    resize(minimumSizeHint());
    force_activate_layouts(this);
}

void DecorationFrame::setTitle(const QString &title) {
    // A client's title is untrusted text - QLabel renders an embedded '\n'
    // as a hard line break and grows the titlebar to fit, so simplified()
    // collapses any whitespace/newlines to guarantee single-line text.
    title_label_->setText(title.simplified());
}

void DecorationFrame::setIcon(const IconImage &icon) {
    bool has_icon = icon.size > 0 && !icon.pixels.empty();
    if (has_icon) {
        // QImage wraps icon.pixels' own memory (no copy) - fine here since
        // QPixmap::fromImage() below copies out of it before this function
        // returns, the only point at which that wrap needs to stay valid.
        QImage image(icon.pixels.data(), icon.size, icon.size, QImage::Format_ARGB32_Premultiplied);
        icon_button_->setIcon(QIcon(QPixmap::fromImage(image)));
    }
    if (has_icon == icon_button_->isHidden()) {
        icon_button_->setVisible(has_icon);
        // Showing/hiding a layout item is a box-model change like
        // setMaximizedState()'s border toggling above, not just paint - same
        // invalidate->resize(minimumSizeHint())->invalidate dance is needed
        // since there's no running event loop to reflow this otherwise.
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
