// SPDX-License-Identifier: LGPL-3.0-or-later

#include "switcher.h"

#include "frame_widget.h" // repolish_tree, force_activate_layouts
#include "theme.h" // apply_decoration_stylesheet

#include <QFontMetrics>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>

namespace biome_decoration {

namespace {

// The real, QSS-styled Alt-Tab panel: a QFrame (#biomeSwitcherPanel, see
// biome-dark.qss) containing one QLabel row per window (#biomeSwitcherRow),
// styled/colored entirely through the stylesheet - not a hand-painted
// overlay reconstructing theme colors via pixel sampling. Built once and
// reused/resized on every render_switcher() call, the same pattern
// frame_widget.h's DecorationFrame uses.
class SwitcherPanel : public QFrame {
    Q_OBJECT
    Q_PROPERTY(int panelPadding READ panelPadding WRITE setPanelPadding)

public:
    explicit SwitcherPanel(QWidget *parent = nullptr);

    int panelPadding() const { return panel_padding_; }
    void setPanelPadding(int value);

    // Adds/removes row QLabels to match entries.size(), sets each row's
    // "selected" dynamic property (biome-dark.qss's #biomeSwitcherRow
    // [selected="true"] rule) and full, not-yet-elided text. Callers must
    // force_activate_layouts() the panel afterwards, then call elideRows(),
    // so each row's real QSS-laid-out width is known before eliding
    // against it.
    void setEntries(const std::vector<SwitcherEntry> &entries, int selected_index);

    // Elides each row's text to fit its actual laid-out width (QSS
    // padding/min-width already resolved by then) - avoids a
    // hand-duplicated pixel-padding constant to guess it upfront.
    void elideRows();

private:
    QVBoxLayout *rows_layout_ = nullptr;
    std::vector<QLabel *> rows_;
    std::vector<std::string> full_labels_;
    int panel_padding_ = 0;
};

SwitcherPanel::SwitcherPanel(QWidget *parent) : QFrame(parent) {
    setObjectName("biomeSwitcherPanel");
    rows_layout_ = new QVBoxLayout(this);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(0);
}

void SwitcherPanel::setPanelPadding(int value) {
    panel_padding_ = value;
    rows_layout_->setContentsMargins(value, value, value, value);
}

void SwitcherPanel::setEntries(const std::vector<SwitcherEntry> &entries, int selected_index) {
    while (rows_.size() < entries.size()) {
        auto *row = new QLabel(this);
        row->setObjectName("biomeSwitcherRow");
        // Ignored on the horizontal axis so a long title's natural
        // font-metric width never enters the layout's minimum-size
        // computation - the row should elide within whatever width the
        // panel's own fixed QSS width leaves it, never grow the panel
        // (same reasoning as frame_widget.cpp's title_label_).
        row->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        rows_layout_->addWidget(row);
        rows_.push_back(row);
    }
    while (rows_.size() > entries.size()) {
        QLabel *row = rows_.back();
        rows_.pop_back();
        rows_layout_->removeWidget(row);
        // Biome never pumps a Qt event loop, so deleteLater() would just
        // leak - plain delete is safe here since nothing else references
        // these rows offscreen.
        delete row;
    }

    full_labels_.resize(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        rows_[i]->setProperty("selected", static_cast<int>(i) == selected_index);
        full_labels_[i] = entries[i].label;
        // Same untrusted-title concern as DecorationFrame::setTitle()
        // (frame_widget.cpp) - simplified() keeps an embedded newline from
        // turning one row into a multi-line label.
        rows_[i]->setText(QString::fromUtf8(entries[i].label.c_str()).simplified());
    }
}

void SwitcherPanel::elideRows() {
    for (size_t i = 0; i < rows_.size(); i++) {
        QLabel *row = rows_[i];
        QFontMetrics metrics(row->font());
        QString full = QString::fromUtf8(full_labels_[i].c_str()).simplified();
        QString elided = metrics.elidedText(full, Qt::ElideRight, row->contentsRect().width());
        row->setText(elided);
    }
}

SwitcherPanel *g_panel = nullptr;

} // namespace

RenderedFrame render_switcher(const std::vector<SwitcherEntry> &entries, int selected_index) {
    RenderedFrame frame;
    if (entries.empty()) {
        return frame;
    }

    if (g_panel == nullptr) {
        g_panel = new SwitcherPanel();
        apply_decoration_stylesheet(g_panel);
    }

    g_panel->setEntries(entries, selected_index);
    repolish_tree(g_panel); // newly-added rows above need their QSS applied too
    g_panel->resize(g_panel->minimumSizeHint());
    force_activate_layouts(g_panel);
    g_panel->elideRows();

    int width = g_panel->width();
    int height = g_panel->height();
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    g_panel->render(&image);

    frame.width = width;
    frame.height = height;
    frame.stride = image.bytesPerLine();
    frame.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return frame;
}

} // namespace biome_decoration

#include "switcher.moc"
