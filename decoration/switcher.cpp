// SPDX-License-Identifier: LGPL-3.0-or-later

#include "switcher.h"

#include "frame_widget.h" // repolish_tree, force_activate_layouts
#include "theme.h" // apply_decoration_stylesheet

#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSizePolicy>
#include <QString>
#include <QToolButton>
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

    // Adds/removes rows to match entries.size(), sets each row's "selected"
    // dynamic property (biome-dark.qss's #biomeSwitcherRow[selected="true"]
    // rule), icon, and full, not-yet-elided text. Callers must
    // force_activate_layouts() the panel afterwards, then call elideRows(),
    // so each row's real QSS-laid-out width is known before eliding
    // against it.
    void setEntries(const std::vector<SwitcherEntry> &entries, int selected_index);

    // Elides each row's text to fit its actual laid-out width (QSS
    // padding/min-width already resolved by then) - avoids a
    // hand-duplicated pixel-padding constant to guess it upfront.
    void elideRows();

private:
    // One row: a container (#biomeSwitcherRow, carries the "selected"
    // background) holding an icon (#biomeSwitcherRowIcon, hidden when the
    // entry has none - same collapse-the-slot behavior as the titlebar
    // icon) and the title text (#biomeSwitcherRowText).
    struct Row {
        QWidget *container = nullptr;
        QToolButton *icon = nullptr;
        QLabel *text = nullptr;
    };

    QVBoxLayout *rows_layout_ = nullptr;
    std::vector<Row> rows_;
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
        Row row;
        row.container = new QWidget(this);
        row.container->setObjectName("biomeSwitcherRow");
        row.container->setAttribute(Qt::WA_StyledBackground, true);

        row.icon = new QToolButton(row.container);
        row.icon->setObjectName("biomeSwitcherRowIcon");
        row.icon->setFocusPolicy(Qt::NoFocus);
        row.icon->setAttribute(Qt::WA_StyledBackground, true);
        row.icon->hide(); // shown below only for entries that have an icon

        row.text = new QLabel(row.container);
        row.text->setObjectName("biomeSwitcherRowText");
        // Ignored on the horizontal axis so a long title's natural
        // font-metric width never enters the layout's minimum-size
        // computation - the row should elide within whatever width the
        // panel's own fixed QSS width leaves it, never grow the panel
        // (same reasoning as frame_widget.cpp's title_label_).
        row.text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

        auto *row_layout = new QHBoxLayout(row.container);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(0);
        row_layout->addWidget(row.icon, 0, Qt::AlignVCenter);
        row_layout->addWidget(row.text, /*stretch=*/1, Qt::AlignVCenter);

        rows_layout_->addWidget(row.container);
        rows_.push_back(row);
    }
    while (rows_.size() > entries.size()) {
        QWidget *container = rows_.back().container;
        rows_.pop_back();
        rows_layout_->removeWidget(container);
        // Biome never pumps a Qt event loop, so deleteLater() would just
        // leak - plain delete is safe here since nothing else references
        // these rows offscreen (deletes the icon/text children along with it).
        delete container;
    }

    full_labels_.resize(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        const Row &row = rows_[i];
        row.container->setProperty("selected", static_cast<int>(i) == selected_index);
        full_labels_[i] = entries[i].label;
        // Same untrusted-title concern as DecorationFrame::setTitle()
        // (frame_widget.cpp) - simplified() keeps an embedded newline from
        // turning one row into a multi-line label.
        row.text->setText(QString::fromUtf8(entries[i].label.c_str()).simplified());

        const IconImage &icon = entries[i].icon;
        bool has_icon = icon.size > 0 && !icon.pixels.empty();
        if (has_icon) {
            // See DecorationFrame::setIcon() (frame_widget.cpp) - QImage
            // wraps icon.pixels' own memory, fine since QPixmap::fromImage()
            // copies out of it before this loop iteration ends.
            QImage image(icon.pixels.data(), icon.size, icon.size, QImage::Format_ARGB32_Premultiplied);
            row.icon->setIcon(QIcon(QPixmap::fromImage(image)));
        }
        row.icon->setVisible(has_icon);
    }
}

void SwitcherPanel::elideRows() {
    for (size_t i = 0; i < rows_.size(); i++) {
        const Row &row = rows_[i];
        QFontMetrics metrics(row.text->font());
        QString full = QString::fromUtf8(full_labels_[i].c_str()).simplified();
        QString elided = metrics.elidedText(full, Qt::ElideRight, row.text->contentsRect().width());
        row.text->setText(elided);
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
