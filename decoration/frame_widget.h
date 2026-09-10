// SPDX-License-Identifier: LGPL-3.0-or-later
//
// The real, QSS-styled Qt widget tree behind a decoration frame: a
// DecorationFrame (border/radius/background) containing a titlebar with a
// title label and three DecorationButtons (minimize/maximize/close).
// biome_decoration::create_decoration_frame() (decoration/theme.h) builds
// one instance per toplevel (BiomeToplevel::decoration_frame, desktop/
// toplevel.h) - decoration/renderer.cpp renders it for every repaint by
// resizing it and toggling state, then calling QWidget::render() into an
// offscreen QImage; core/cursor.cpp reuses the same instance for
// hit-testing and geometry (hitTest()/borderWidth()/titlebarHeight())
// instead of a separate model.

#pragma once

#include "layout.h" // Region
#include "renderer.h" // IconImage

#include <QIcon>
#include <QLabel>
#include <QString>
#include <QToolButton>
#include <QWidget>

namespace biome_decoration {

// Generic icon-theme fallback (hicolor's spec-mandated
// application-x-executable) for a window whose own icon couldn't be
// resolved - shown instead of leaving a gap. Shared by DecorationFrame's
// titlebar icon and switcher.cpp's per-entry icons.
QIcon fallback_icon();

// Forces every widget in root's subtree (root included) to reapply its QSS
// rules, including qproperty-* values - needed since Biome never
// QWidget::show()s these widgets (everything renders offscreen) and Qt
// normally only auto-polishes a widget on its first show.
void repolish_tree(QWidget *root);

// Forces root's QLayout (and every descendant's) to recompute geometry
// immediately, in two passes: bottom-up invalidate, so an ancestor's own
// activate() never sizes itself against a descendant widget's stale cached
// minimumSizeHint()/sizeHint() (invalidate() alone is enough to freshen
// that cache - it does no geometry math, so it's safe to do before the
// ancestor even knows its own final size); then top-down activate, so
// every descendant reflows against its real, final geometry after root's
// own activate() potentially resized it - QLayout::activate() no-ops once
// its "activated" flag is set, so without this second pass a resized
// child's own contents would stay positioned for its old size.
// A QLayout normally reflows via a posted QEvent::LayoutRequest, delivered
// whenever Qt's event dispatcher next runs - core/qt_glib_bridge.cpp makes
// that genuinely live (fd-driven off Biome's own wl_display loop, not a
// poll), but decoration rendering still runs synchronously inside a
// wlroots callback and needs correct geometry for that same call, not
// "whenever Qt next dispatches" - hence forcing this explicitly rather
// than trusting the event to arrive in time. Shared by
// DecorationFrame::layoutFor() and switcher.cpp's SwitcherPanel, which both
// resize() an offscreen top-level widget and need its subtree to reflect
// that immediately.
void force_activate_layouts(QWidget *root);

// force_activate_layouts(root) (to freshen minimumSizeHint() against
// current content), then resize root down to that hint (QLayout::activate()
// on a top-level widget only ever grows it, never shrinks), then
// force_activate_layouts(root) again so every descendant's geometry
// reflects that final, possibly-shrunk size. The common "measure, then
// commit" pattern behind DecorationFrame::layoutFor()/setMaximizedState()/
// setIcon() and switcher.cpp's render_switcher().
void relayout_and_shrink_to_fit(QWidget *root);

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
    // Sets the titlebar icon slot - an empty IconImage gets fallback_icon()
    // rather than leaving a gap. icon_button_ is a QToolButton rather than a
    // QLabel so its rendered size stays QSS-controllable via
    // qproperty-iconSize.
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
