// SPDX-License-Identifier: LGPL-3.0-or-later

#include "switcher_highlight.h"

#include "frame_widget.h" // show_offscreen

#include <QFrame>
#include <QVBoxLayout>
#include <QWidget>

namespace biome_decoration {

namespace {

// Transparent root owning the styled QFrame as a child, rather than
// rendering the frame itself as the top-level widget - same reasoning as
// switcher.cpp's SwitcherRoot: Qt only clips a styled widget's own
// background/border to its QSS border-radius when it's painted as a child.
class SwitcherHighlightRoot : public QWidget {
    Q_OBJECT

public:
    explicit SwitcherHighlightRoot(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("biomeSwitcherHighlightRoot");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        frame = new QFrame(this);
        frame->setObjectName("biomeSwitcherHighlight");
        layout->addWidget(frame);
        show_offscreen(this);
    }

    QFrame *frame = nullptr;
};

SwitcherHighlightRoot *g_root = nullptr;

} // namespace

RenderedFrame render_switcher_highlight(int width, int height, double scale) {
    if (width <= 0 || height <= 0) {
        return {};
    }

    if (g_root == nullptr) {
        g_root = new SwitcherHighlightRoot();
        // No per-widget setStyleSheet() needed - see switcher.cpp's
        // render_switcher() for why the app-wide stylesheet already covers
        // widgets constructed after load_decoration_theme() runs.
    }

    g_root->resize(width, height); // visible, so this relays out synchronously
    return render_widget(g_root, scale);
}

} // namespace biome_decoration

#include "switcher_highlight.moc"
