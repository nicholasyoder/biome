// SPDX-License-Identifier: LGPL-3.0-or-later

#include "frame_widget.h"

#include <QCoreApplication>
#include <QEnterEvent>
#include <QEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
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
    // No WA_TranslucentBackground: on an offscreen-rendered (never shown)
    // widget it suppresses ALL painting, not just compositing - verified
    // directly, an otherwise-identical widget painted nothing at all with
    // it set, vs. correctly painting only its non-transparent regions
    // without it. WA_StyledBackground + this frame's own "background:
    // transparent" QSS declaration already gets the same per-region
    // transparency onto the pre-cleared QImage renderer.cpp renders into.
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);
    setProperty("focused", true);

    titlebar_ = new QWidget(this);
    titlebar_->setObjectName("biomeTitlebar");
    titlebar_->setAttribute(Qt::WA_StyledBackground, true);

    title_label_ = new QLabel(titlebar_);
    title_label_->setObjectName("biomeTitle");
    title_label_->setAttribute(Qt::WA_StyledBackground, true);

    button_minimize_ = new DecorationButton(Region::ButtonMinimize, titlebar_);
    button_maximize_ = new DecorationButton(Region::ButtonMaximize, titlebar_);
    button_close_ = new DecorationButton(Region::ButtonClose, titlebar_);

    border_left_ = new DecorationBorder("biomeBorderLeft", this);
    border_right_ = new DecorationBorder("biomeBorderRight", this);
    border_bottom_ = new DecorationBorder("biomeBorderBottom", this);
}

void DecorationFrame::applyMetricsToLayout() const {
    kBorderWidth = border_width_;
    kTitlebarHeight = titlebar_height_;
    kButtonSize = button_minimize_->buttonSize();
    kButtonSpacing = button_minimize_->buttonSpacing();
    kButtonMarginRight = button_minimize_->buttonMarginRight();
}

void DecorationFrame::layoutFor(int content_width, int content_height) {
    int width = container_width(content_width);
    int height = container_height(content_height);
    resize(width, height);

    titlebar_->setGeometry(0, 0, width, kTitlebarHeight);

    TitleRect tr = title_rect(content_width);
    title_label_->setGeometry(tr.x, tr.y, tr.width, tr.height);

    ButtonRects buttons = button_rects(content_width);
    button_minimize_->setGeometry(buttons.minimize_x, buttons.y, kButtonSize, kButtonSize);
    button_maximize_->setGeometry(buttons.maximize_x, buttons.y, kButtonSize, kButtonSize);
    button_close_->setGeometry(buttons.close_x, buttons.y, kButtonSize, kButtonSize);

    BorderRects borders = border_rects(content_width, content_height);
    border_left_->setGeometry(borders.left.x, borders.left.y, borders.left.width, borders.left.height);
    border_right_->setGeometry(borders.right.x, borders.right.y, borders.right.width, borders.right.height);
    border_bottom_->setGeometry(borders.bottom.x, borders.bottom.y, borders.bottom.width, borders.bottom.height);
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
