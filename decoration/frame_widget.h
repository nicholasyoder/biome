// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The real, QSS-styled Qt widget tree behind a decoration frame: a
// DecorationFrame (border/radius/background) containing a titlebar with a
// title label and three DecorationButtons (minimize/maximize/close).
// decoration/theme.cpp builds one persistent instance at startup;
// decoration/renderer.cpp reuses it for every render by resizing it and
// toggling state, then calling QWidget::render() into an offscreen QImage.
// core/main.cpp also reuses it for hit-testing and geometry
// (hitTest()/borderWidth()/titlebarHeight()) instead of a separate model.

#pragma once

#include "layout.h" // Region
#include "renderer.h" // IconImage

#include <QLabel>
#include <QString>
#include <QToolButton>
#include <QWidget>

namespace biome_decoration {

// Forces every widget in root's subtree (root included) to reapply its QSS
// rules, including qproperty-* values - needed since Biome never
// QWidget::show()s these widgets (everything renders offscreen) and Qt
// normally only auto-polishes a widget on its first show.
void repolish_tree(QWidget *root);

// Forces root's QLayout (and every descendant's) to recompute geometry
// immediately, top-down. A QLayout normally reflows via a posted
// QEvent::LayoutRequest, which needs a running Qt event loop to deliver -
// Biome has none (everything offscreen, driven synchronously), so a widget
// tree's layout would otherwise keep stale (default 100x30) geometry
// forever. Shared by DecorationFrame::layoutFor() and switcher.cpp's
// SwitcherPanel, which both resize() an offscreen top-level widget and need
// its subtree to reflect that immediately.
void force_activate_layouts(QWidget *root);

// One left/right/bottom border strip - a plain styled widget rather than a
// single CSS border spanning the whole frame, so each edge can be styled and
// sized independently (decoration/theme/biome-dark.qss's min-width/
// min-height per object name). A near-empty subclass only so one QSS type
// selector can style all three at once, same as DecorationButton below.
class DecorationBorder : public QWidget {
    Q_OBJECT

public:
    DecorationBorder(const QString &object_name, QWidget *parent = nullptr);
};

// Paints its QSS background/border/radius/hover/pressed state and its icon
// (minimize/maximize/close glyph) entirely via the base QToolButton -  the
// icon itself comes from QSS qproperty-icon (biome-dark.qss selects per
// button, and a dimmer variant when unfocused), so there's no C++
// glyph-drawing to keep in sync with the theme.
class DecorationButton : public QToolButton {
    Q_OBJECT

public:
    DecorationButton(Region region, QWidget *parent = nullptr);

    Region region() const { return region_; }

private:
    Region region_;
};

class DecorationFrame : public QFrame {
    Q_OBJECT

public:
    explicit DecorationFrame(QWidget *parent = nullptr);

    // Content-size-independent metrics, read live off content_spacer_'s own
    // laid-out position - it sits directly against border_left_/titlebar_,
    // so its (x, y) *is* the border-width/titlebar-height offset, however
    // that ends up computed. Safe to call any time after
    // load_decoration_theme()'s initial layoutFor() call. Used by core/
    // main.cpp for the content-tree scene-node offset and window
    // move/resize/maximize math.
    int borderWidth() const { return content_spacer_->x(); }
    int titlebarHeight() const { return content_spacer_->y(); }
    // Same idea, but for the right/bottom edges - border strips are
    // independently QSS-sized, so these can't be assumed equal to
    // borderWidth()/titlebarHeight(). Derived from the frame's total size
    // minus content_spacer_'s box, so it stays correct regardless of layout
    // spacing/margins.
    int rightBorderWidth() const { return width() - content_spacer_->x() - content_spacer_->width(); }
    int bottomBorderHeight() const { return height() - content_spacer_->y() - content_spacer_->height(); }

    // Resizes the frame to fit the given client content size; the QLayout
    // tree built in the constructor repositions everything else around it.
    void layoutFor(int content_width, int content_height);

    // Resolves which decoration region (if any) is under local_x/local_y
    // (relative to the frame's own top-left) for a client of the given
    // content size - a real query against the widget tree's laid-out
    // geometry (calls layoutFor() internally). maximized selects which QSS
    // [biomeMaximized=...] state to hit-test against - a theme that zeroes a
    // border's size under that state naturally stops matching that edge
    // here too, since childAt() won't find a zero-size widget.
    Region hitTest(int local_x, int local_y, int content_width, int content_height, bool maximized);

    void setFocusedState(bool focused);
    // Drives the #biomeFrame[biomeMaximized=...] QSS state - lets a theme
    // style a maximized window differently (e.g. no corner radius) via QSS
    // alone. Also read back by borderWidth()/titlebarHeight()/
    // rightBorderWidth()/bottomBorderHeight() above, so callers must set
    // this before querying those.
    void setMaximizedState(bool maximized);
    void setTitle(const QString &title);
    // Sets (or, for an empty IconImage, hides) the titlebar icon slot -
    // icon_button_ is a QToolButton rather than a QLabel so its rendered
    // size stays QSS-controllable via qproperty-iconSize.
    void setIcon(const IconImage &icon);

    // Drives real QSS :hover/:pressed pseudo-states on whichever button (if
    // any) matches - region is one of ButtonMinimize/ButtonMaximize/
    // ButtonClose/None.
    void setHoveredRegion(Region region);
    void setPressedRegion(Region region);

private:
    QWidget *titlebar_ = nullptr;
    QLabel *title_label_ = nullptr;
    // A window's own icon - not a DecorationButton (that's for the static
    // min/max/close glyphs), just a plain QToolButton so qproperty-iconSize
    // still applies to it.
    QToolButton *icon_button_ = nullptr;
    DecorationButton *button_minimize_ = nullptr;
    DecorationButton *button_maximize_ = nullptr;
    DecorationButton *button_close_ = nullptr;
    DecorationBorder *border_left_ = nullptr;
    DecorationBorder *border_right_ = nullptr;
    DecorationBorder *border_bottom_ = nullptr;
    // Empty, never-painted placeholder sized to exactly the client's content
    // area - the middle row's QHBoxLayout sizes border_left_/border_right_
    // around it like any other fixed-size widget.
    QWidget *content_spacer_ = nullptr;
};

} // namespace biome_decoration
