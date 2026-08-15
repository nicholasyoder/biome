// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The real, QSS-styled Qt widget tree behind a decoration frame: a
// DecorationFrame (border/radius/background) containing a titlebar with a
// title label and three DecorationButtons (minimize/maximize/close).
// decoration/theme.cpp builds one persistent instance at startup and reads
// its qproperty-* values (set by decoration/theme/biome-dark.qss) back into
// layout.h's runtime geometry globals; decoration/renderer.cpp reuses that
// same instance for every render by resizing it and toggling state, then
// calling QWidget::render() into an offscreen QImage.

#pragma once

#include "layout.h"

#include <QColor>
#include <QLabel>
#include <QString>
#include <QToolButton>
#include <QWidget>

namespace biome_decoration {

// Forces every widget in root's subtree (root included) to reapply its QSS
// rules, including qproperty-* value application - needed for widgets that
// are never QWidget::show()n (Biome renders offscreen), since Qt normally
// only auto-polishes a widget on its first show.
void repolish_tree(QWidget *root);

// One left/right/bottom border strip - a plain styled widget (background,
// border, radius, all real QSS box-model properties) rather than a single
// CSS border spanning the whole frame, so each edge can be styled richly on
// its own. A dedicated (near-empty) subclass only so a single QSS type
// selector (biome_decoration--DecorationBorder) can style all three at
// once, the same way DecorationButton does for the three buttons below.
class DecorationBorder : public QWidget {
    Q_OBJECT

public:
    DecorationBorder(const QString &object_name, QWidget *parent = nullptr);
};

// Paints its QSS background/border/radius/hover/pressed state via the base
// QToolButton implementation, then draws the minimize/maximize/close glyph
// on top - Forest's QSS has no icon set for these, so the glyph stays
// hand-drawn (matching decoration/renderer.cpp's previous approach).
class DecorationButton : public QToolButton {
    Q_OBJECT
    Q_PROPERTY(int buttonSize READ buttonSize WRITE setButtonSize)
    Q_PROPERTY(int buttonSpacing READ buttonSpacing WRITE setButtonSpacing)
    Q_PROPERTY(int buttonMarginRight READ buttonMarginRight WRITE setButtonMarginRight)
    Q_PROPERTY(QColor glyphColor READ glyphColor WRITE setGlyphColor)

public:
    DecorationButton(Region region, QWidget *parent = nullptr);

    Region region() const { return region_; }

    int buttonSize() const { return button_size_; }
    void setButtonSize(int value) { button_size_ = value; }
    int buttonSpacing() const { return button_spacing_; }
    void setButtonSpacing(int value) { button_spacing_ = value; }
    int buttonMarginRight() const { return button_margin_right_; }
    void setButtonMarginRight(int value) { button_margin_right_ = value; }
    QColor glyphColor() const { return glyph_color_; }
    void setGlyphColor(const QColor &value) { glyph_color_ = value; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Region region_;
    int button_size_ = kButtonSize;
    int button_spacing_ = kButtonSpacing;
    int button_margin_right_ = kButtonMarginRight;
    QColor glyph_color_ = Qt::white;
};

class DecorationFrame : public QFrame {
    Q_OBJECT
    Q_PROPERTY(int borderWidth READ borderWidth WRITE setBorderWidth)
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius)
    Q_PROPERTY(int titlebarHeight READ titlebarHeight WRITE setTitlebarHeight)

public:
    explicit DecorationFrame(QWidget *parent = nullptr);

    int borderWidth() const { return border_width_; }
    void setBorderWidth(int value) { border_width_ = value; }
    int cornerRadius() const { return corner_radius_; }
    void setCornerRadius(int value) { corner_radius_ = value; }
    int titlebarHeight() const { return titlebar_height_; }
    void setTitlebarHeight(int value) { titlebar_height_ = value; }

    // Reads this instance's own qproperty-driven values back into layout.h's
    // runtime globals (kBorderWidth, kTitlebarHeight, kButtonSize, ...) -
    // called once by theme.cpp after the stylesheet is applied and the
    // widget is polished.
    void applyMetricsToLayout() const;

    // Resizes the frame and repositions the titlebar/label/buttons for the
    // given client content size, using layout.h's shared geometry math.
    void layoutFor(int content_width, int content_height);

    void setFocusedState(bool focused);
    void setTitle(const QString &title);

    // Drives real QSS :hover/:pressed pseudo-states on whichever button (if
    // any) matches - region is one of ButtonMinimize/ButtonMaximize/
    // ButtonClose/None.
    void setHoveredRegion(Region region);
    void setPressedRegion(Region region);

    // Accessors used by theme.cpp to sample colors back out of the real
    // QSS-styled widgets for decoration/switcher.cpp's separate hand-painted
    // Alt-Tab panel, rather than duplicating literal color values in C++.
    QWidget *titlebarWidget() const { return titlebar_; }
    QColor titleColor() const { return title_label_->palette().color(title_label_->foregroundRole()); }

private:
    QWidget *titlebar_ = nullptr;
    QLabel *title_label_ = nullptr;
    DecorationButton *button_minimize_ = nullptr;
    DecorationButton *button_maximize_ = nullptr;
    DecorationButton *button_close_ = nullptr;
    DecorationBorder *border_left_ = nullptr;
    DecorationBorder *border_right_ = nullptr;
    DecorationBorder *border_bottom_ = nullptr;
    int border_width_ = kBorderWidth;
    int corner_radius_ = 0;
    int titlebar_height_ = kTitlebarHeight;
};

} // namespace biome_decoration
