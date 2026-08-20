// SPDX-License-Identifier: LGPL-3.0-or-later

#include "theme.h"

#include <QFile>
#include <QIODevice>
#include <QString>

// Q_INIT_RESOURCE must be called from outside any C++ namespace - it expands
// to a call to the qrc compiler's global ::qInitResources_theme(). Without
// this, the embedded resource never registers: Qt resources compiled into a
// *static* library get dropped by the linker unless something forces a
// reference to their translation unit.
static void ensure_decoration_resources_registered() {
    Q_INIT_RESOURCE(theme);
}

namespace biome_decoration {

namespace {

constexpr const char *kStylesheetPath = ":/biome/decoration/biome-dark.qss";

DecorationFrame *g_frame = nullptr;

} // namespace

void apply_decoration_stylesheet(QWidget *root) {
    ensure_decoration_resources_registered();

    QString stylesheet;
    QFile file(kStylesheetPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        stylesheet = QString::fromUtf8(file.readAll());
    }

    root->setStyleSheet(stylesheet);
    repolish_tree(root);
}

void load_decoration_theme() {
    g_frame = new DecorationFrame();
    apply_decoration_stylesheet(g_frame);

    // Gives the widget tree a valid initial layout before any real toplevel
    // exists - without this, borderWidth()/titlebarHeight() would read
    // content_spacer_'s pre-layout (0, 0) position if queried before the
    // first render_decoration() call, which re-runs layoutFor() with the
    // real content size on every repaint.
    g_frame->layoutFor(200, 200);
}

DecorationFrame *decoration_frame() {
    return g_frame;
}

} // namespace biome_decoration
