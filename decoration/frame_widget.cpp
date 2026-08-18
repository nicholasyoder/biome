// SPDX-License-Identifier: LGPL-3.0-or-later

#include "frame_widget.h"

#include <QBoxLayout>
#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
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

namespace {

// A QLayout normally reflows via a posted QEvent::LayoutRequest, and only
// activate()s once until something re-invalidates it - both of which
// assume a running Qt event loop delivering events and re-triggering
// invalidation on every resize. Biome never runs one (see theme.cpp/
// renderer.cpp: everything is driven synchronously through direct calls),
// so titlebar_'s nested layout in particular would just keep stale
// (default 100x30, top-left) geometry forever after the first call.
// invalidate() + activate(), unconditionally, top-down on every layout in
// the tree, sidesteps both assumptions - parent geometry (and so each
// child widget's own rect) is always current before that child's own
// nested layout, if any, recomputes against it.
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

} // namespace

namespace {
// How many pixels near a corner count as a diagonal resize handle rather
// than a plain edge - pure WM click-precision convention, not a rendered
// decoration element, so there's no QSS/widget equivalent to source it
// from (unlike border/titlebar/button metrics below).
constexpr int kResizeCornerSize = 12;
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

void DecorationButton::paintEvent(QPaintEvent *event) {
    QToolButton::paintEvent(event);

    // The button's QSS background/border/radius/hover/pressed state is
    // already painted above - Forest's QSS has no icon set for these, so
    // the minimize/maximize/close glyph itself stays hand-drawn on top.
    //
    // The glyph box comes from SE_FrameContents, not a hardcoded margin:
    // QWidget::contentsRect()/contentsMargins() do NOT reflect QSS
    // "padding" (verified directly - contentsMargins() stayed 0,0,0,0 with
    // a 20px padding rule applied), since Qt's stylesheet engine only
    // consults padding through QStyle's own subElementRect/sizeFromContents
    // queries, not the generic QWidget margin API. SE_FrameContents is the
    // query that does honor it (also verified directly: with a 20px
    // padding + 1px border on a 60x60 widget it returned the expected
    // 21,21,18,18).
    QStyleOptionToolButton option;
    option.initFrom(this);
    QRect box = style()->subElementRect(QStyle::SE_FrameContents, &option, this);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(glyph_color_, 1.4));
    painter.setBrush(Qt::NoBrush);

    switch (region_) {
    case Region::ButtonMinimize:
        painter.drawLine(QPointF(box.left(), box.bottom()), QPointF(box.right(), box.bottom()));
        break;
    case Region::ButtonMaximize:
        painter.drawRect(box);
        break;
    case Region::ButtonClose:
        painter.drawLine(box.topLeft(), box.bottomRight());
        painter.drawLine(box.topRight(), box.bottomLeft());
        break;
    default:
        break;
    }
}

DecorationBorder::DecorationBorder(const QString &object_name, QWidget *parent) : QWidget(parent) {
    setObjectName(object_name);
    setAttribute(Qt::WA_StyledBackground, true);
}

DecorationFrame::DecorationFrame(QWidget *parent) : QFrame(parent) {
    setObjectName("biomeFrame");
    setProperty("focused", true);

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
    // minimumSizeHint() (-> main_layout's totalMinimumSize()) is the frame's
    // true required size: border/titlebar/border-bottom's own QSS min-sizes
    // plus content_spacer_'s just-set fixed size, with title_label_ excluded
    // via its Ignored size policy above. Resizing to it explicitly - rather
    // than leaving this top-level widget to force_activate_layouts()'s
    // activate() alone - matters because QLayout::activate() on a top-level
    // widget only ever grows it (resize(minimumSize().expandedTo(size())):
    // an expandedTo() can't shrink), so relying on activate() alone would
    // silently freeze the frame at its largest-ever size and never shrink
    // back down on a smaller content_width/content_height.
    resize(minimumSizeHint());
    force_activate_layouts(this);
}

Region DecorationFrame::hitTest(int local_x, int local_y, int content_width, int content_height) {
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

void DecorationFrame::setTitle(const QString &title) {
    title_label_->setText(title);
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
