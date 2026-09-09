// SPDX-License-Identifier: LGPL-3.0-or-later

#include "switcher_highlight.h"

#include "frame_widget.h" // repolish_tree, force_activate_layouts

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
    }

    QFrame *frame = nullptr;
};

SwitcherHighlightRoot *g_root = nullptr;

} // namespace

RenderedFrame render_switcher_highlight(int width, int height) {
    RenderedFrame frame;
    if (width <= 0 || height <= 0) {
        return frame;
    }

    if (g_root == nullptr) {
        g_root = new SwitcherHighlightRoot();
        // No per-widget setStyleSheet() needed - see switcher.cpp's
        // render_switcher() for why the app-wide stylesheet already covers
        // widgets constructed after load_decoration_theme() runs.
    }

    repolish_tree(g_root);
    g_root->resize(width, height);
    force_activate_layouts(g_root);

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    g_root->render(&image);

    frame.width = width;
    frame.height = height;
    frame.stride = image.bytesPerLine();
    frame.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return frame;
}

} // namespace biome_decoration

#include "switcher_highlight.moc"
