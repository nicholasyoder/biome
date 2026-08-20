// SPDX-License-Identifier: LGPL-3.0-or-later

#include "frame_widget.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QLayout>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionToolButton>

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

void DecorationButton::resizeEvent(QResizeEvent *event) {
    QToolButton::resizeEvent(event);

    // Keeps the icon filling exactly the button's QSS-driven content box
    // (padding excluded) on every resize, so a theme can resize the button
    // via plain QSS min/max-width/height without any C++ change - same
    // SE_FrameContents query as before (see decoration/frame_widget.h),
    // since QWidget::contentsRect() does NOT reflect QSS "padding".
    QStyleOptionToolButton option;
    option.initFrom(this);
    QRect box = style()->subElementRect(QStyle::SE_FrameContents, &option, this);
    setIconSize(box.size());
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

    button_minimize_ = new DecorationButton(Region::ButtonMinimize, titlebar_);
    button_maximize_ = new DecorationButton(Region::ButtonMaximize, titlebar_);
    button_close_ = new DecorationButton(Region::ButtonClose, titlebar_);


    auto *titlebar_layout = new QHBoxLayout(titlebar_);
    titlebar_layout->setContentsMargins(0, 0, 0, 0);
    titlebar_layout->addWidget(title_label_, /*stretch=*/1);
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

    // Corners take priority over the plain edges/buttons below - a WM
    // convention that gives diagonal resize a large-enough hit target near
    // the frame's corners instead of it being a single-pixel coincidence of
    // two edges (and, same as before this used real widget geometry, can
    // still win over a button that happens to overlap a corner zone).
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

    // Everywhere else, ask the real widget tree what's actually there
    // instead of re-deriving it from geometry math.
    QWidget *hit = childAt(local_x, local_y);
    if (hit == border_bottom_) {
        return Region::ResizeS;
    }
    if (hit == border_left_) {
        return Region::ResizeW;
    }
    if (hit == border_right_) {
        return Region::ResizeE;
    }
    if (hit == button_minimize_) {
        return Region::ButtonMinimize;
    }
    if (hit == button_maximize_) {
        return Region::ButtonMaximize;
    }
    if (hit == button_close_) {
        return Region::ButtonClose;
    }
    if (hit == titlebar_ || hit == title_label_) {
        return Region::Titlebar;
    }
    // content_spacer_ (the client's own surface, not part of the
    // decoration), or nothing - both mean "not our region".
    return Region::None;
}

void DecorationFrame::setFocusedState(bool focused) {
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
