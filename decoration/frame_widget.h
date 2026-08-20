// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The real, QSS-styled Qt widget tree behind a decoration frame: a
// DecorationFrame (border/radius/background) containing a titlebar with a
// title label and three DecorationButtons (minimize/maximize/close).
// decoration/theme.cpp builds one persistent instance at startup;
// decoration/renderer.cpp reuses that same instance for every render by
// resizing it and toggling state, then calling QWidget::render() into an
// offscreen QImage. core/main.cpp also reuses it directly for hit-testing
// and geometry (DecorationFrame::hitTest()/borderWidth()/titlebarHeight())
// instead of a separate, hand-duplicated geometry model.

#pragma once

#include "layout.h" // Region
#include "renderer.h" // IconImage

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

// Forces root's QLayout (and every descendant's) to recompute geometry
// immediately, top-down. A QLayout normally reflows via a posted
// QEvent::LayoutRequest and only activate()s once until something
// re-invalidates it - both assume a running Qt event loop delivering events
// and re-triggering invalidation on every resize. Biome never runs one
// (everything offscreen, driven synchronously - see frame_widget.cpp/
// switcher.cpp), so a widget tree's layout would otherwise just keep stale
// (default 100x30, top-left) geometry forever after the first call.
// invalidate() + activate(), unconditionally, top-down on every layout in
// the tree, sidesteps both assumptions - parent geometry (and so each child
// widget's own rect) is always current before that child's own nested
// layout, if any, recomputes against it. Shared by DecorationFrame::
// layoutFor() and switcher.cpp's SwitcherPanel, which both resize() an
// offscreen top-level widget and need its whole subtree to reflect that
// immediately.
void force_activate_layouts(QWidget *root);

// One left/right/bottom border strip - a plain styled widget (background,
// border, radius, all real QSS box-model properties) rather than a single
// CSS border spanning the whole frame, so each edge can be styled richly on
// its own, and independently sized (decoration/theme/biome-dark.qss's
// min-width/min-height per object name) - there's no assumption left/right/
// bottom share one thickness. A dedicated (near-empty) subclass only so a
// single QSS type selector (biome_decoration--DecorationBorder) can style
// all three at once, the same way DecorationButton does for the three
// buttons below.
class DecorationBorder : public QWidget {
    Q_OBJECT

public:
    DecorationBorder(const QString &object_name, QWidget *parent = nullptr);
};

// Paints its QSS background/border/radius/hover/pressed state, and its icon
// (the minimize/maximize/close glyph), entirely via the base QToolButton
// implementation - the icon itself comes from QSS qproperty-icon
// (biome-dark.qss selects a different image per button, and a dimmer variant
// when the frame is unfocused, the same way it already re-selects title
// color/border color), so there is no C++ glyph-drawing code to keep in
// sync with the theme. Its own size and spacing from its siblings come
// straight from QSS box-model properties (padding, margin) via the QLayout
// it sits in - the one thing that isn't automatic is iconSize, which
// DecorationFrame::layoutFor() keeps matched to each icon's own native
// resolution (see the .cpp) before every layout pass.
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
    // laid-out position - it sits directly against border_left_/titlebar_ in
    // the widget tree (see the constructor), so its (x, y) *is* the
    // border-width/titlebar-height offset, however that ends up being
    // computed (explicit QSS min-width/min-height, or just the natural size
    // of the label/buttons/border strips) - no need to separately track which
    // widget's minimum-size property happens to hold each number. Safe to
    // call any time after load_decoration_theme()'s initial layoutFor() call,
    // since neither coordinate depends on the content_width/content_height
    // passed to layoutFor() (only content_spacer_'s *size* does). core/
    // main.cpp uses these for its content-tree scene-node offset and window
    // move/resize/maximize math, in place of the old separately-duplicated
    // kBorderWidth/kTitlebarHeight globals.
    int borderWidth() const { return content_spacer_->x(); }
    int titlebarHeight() const { return content_spacer_->y(); }
    // Same idea as borderWidth()/titlebarHeight() above, but for the right/
    // bottom edges - border_left_/border_right_/border_bottom_ are
    // independently QSS-sized (biome-dark.qss's min-width/min-height per
    // object name), so these can't just be assumed equal to borderWidth()/
    // titlebarHeight(). Derived from the frame's own total size minus
    // content_spacer_'s box rather than querying border_right_/border_bottom_
    // directly, so it stays correct regardless of layout spacing/margins.
    int rightBorderWidth() const { return width() - content_spacer_->x() - content_spacer_->width(); }
    int bottomBorderHeight() const { return height() - content_spacer_->y() - content_spacer_->height(); }

    // Resizes the frame to fit the given client content size and resizes
    // content_spacer_ to match - the QLayout tree built in the constructor
    // (see .cpp) does the rest: the titlebar/label/buttons/borders all
    // reposition themselves around it.
    void layoutFor(int content_width, int content_height);

    // Resolves which decoration region (if any) is under local_x/local_y
    // (relative to the frame's own top-left, i.e. the container origin) for
    // a client of the given content size - replaces the old hand-coded
    // layout.cpp hit_test() with a real query against this same widget
    // tree's actual laid-out geometry (calls layoutFor() internally, so it
    // mutates this persistent instance the same way rendering already
    // does).
    // maximized selects which QSS [biomeMaximized=...] state to hit-test against
    // (see setMaximizedState()) - a theme that zeroes a border's width/
    // height under [biomeMaximized="true"] naturally stops matching that edge
    // here too, since childAt() won't find a zero-size widget.
    Region hitTest(int local_x, int local_y, int content_width, int content_height, bool maximized);

    void setFocusedState(bool focused);
    // Drives the #biomeFrame[biomeMaximized=...] QSS state - lets a theme style a
    // maximized window differently (e.g. no corner radius, thinner/no side
    // borders) via decoration/theme/biome-dark.qss alone, no C++ change
    // needed per theme. Also read back by borderWidth()/titlebarHeight()/
    // rightBorderWidth()/bottomBorderHeight() above, so callers must set
    // this before querying those if they care which state's metrics they get
    // - see desktop/decoration_bridge.cpp's accessor wrappers.
    void setMaximizedState(bool maximized);
    void setTitle(const QString &title);
    // Sets (or, for an empty IconImage, hides) the titlebar icon slot -
    // icon_button_ is a QToolButton rather than a QLabel specifically so its
    // rendered size stays QSS-controllable via qproperty-iconSize, the same
    // mechanism the min/max/close buttons already use for their (static,
    // theme-supplied) glyphs.
    void setIcon(const IconImage &icon);

    // Drives real QSS :hover/:pressed pseudo-states on whichever button (if
    // any) matches - region is one of ButtonMinimize/ButtonMaximize/
    // ButtonClose/None.
    void setHoveredRegion(Region region);
    void setPressedRegion(Region region);

private:
    QWidget *titlebar_ = nullptr;
    QLabel *title_label_ = nullptr;
    // A window's own icon (see IconImage) - not a DecorationButton (that
    // class is specifically for the min/max/close glyphs, which are static
    // theme assets driven by a Region), just a plain QToolButton so QSS's
    // qproperty-iconSize still applies to it.
    QToolButton *icon_button_ = nullptr;
    DecorationButton *button_minimize_ = nullptr;
    DecorationButton *button_maximize_ = nullptr;
    DecorationButton *button_close_ = nullptr;
    DecorationBorder *border_left_ = nullptr;
    DecorationBorder *border_right_ = nullptr;
    DecorationBorder *border_bottom_ = nullptr;
    // Empty, never-painted placeholder sized to exactly the client's content
    // area (see layoutFor()) - the middle row's QHBoxLayout sizes
    // border_left_/border_right_ around it, the same way it would around
    // any other fixed-size widget, so the borders never need manual
    // geometry math.
    QWidget *content_spacer_ = nullptr;
};

} // namespace biome_decoration
