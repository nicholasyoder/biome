// SPDX-License-Identifier: LGPL-3.0-or-later

#include "switcher.h"

#include "frame_widget.h" // repolish_tree, force_activate_layouts

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

// The real, QSS-styled Alt-Tab panel: a QFrame (#biomeSwitcherPanel) holding
// a horizontal row of one icon per window (#biomeSwitcherIcon) above a
// single title label (#biomeSwitcherTitleLabel) for whichever entry is
// currently selected, styled entirely through the stylesheet. Built once and
// reused/resized on every render_switcher() call, same pattern as
// frame_widget.h's DecorationFrame.
class SwitcherPanel : public QFrame {
    Q_OBJECT

public:
    explicit SwitcherPanel(QWidget *parent = nullptr);

    // Adds/removes icon buttons to match entries.size(), sets each one's
    // "selected" dynamic property and icon, and sets the title label to the
    // selected entry's full, not-yet-elided text. Callers must
    // force_activate_layouts() the panel afterwards, then call
    // elideTitle(), so the label's real laid-out width is known before
    // eliding against it.
    void setEntries(const std::vector<SwitcherEntry> &entries, int selected_index);

    // Elides the title label to fit its actual laid-out width, avoiding a
    // hand-duplicated pixel-padding constant to guess it upfront.
    void elideTitle();

private:
    QVBoxLayout *layout_ = nullptr;
    QHBoxLayout *icons_layout_ = nullptr;
    QLabel *title_label_ = nullptr;
    std::vector<QToolButton *> icons_;
    std::string full_title_;
};

SwitcherPanel::SwitcherPanel(QWidget *parent) : QFrame(parent) {
    setObjectName("biomeSwitcherPanel");
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);

    icons_layout_ = new QHBoxLayout();
    icons_layout_->setContentsMargins(0, 0, 0, 0);
    layout_->addLayout(icons_layout_);

    title_label_ = new QLabel(this);
    title_label_->setObjectName("biomeSwitcherTitleLabel");
    title_label_->setAlignment(Qt::AlignHCenter);
    // Ignored horizontally so a long title never grows the panel - it
    // should elide within the icon row's width (same reasoning as
    // frame_widget.cpp's title_label_).
    title_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout_->addWidget(title_label_);
}

void SwitcherPanel::setEntries(const std::vector<SwitcherEntry> &entries, int selected_index) {
    while (icons_.size() < entries.size()) {
        auto *icon = new QToolButton(this);
        icon->setObjectName("biomeSwitcherIcon");
        icon->setFocusPolicy(Qt::NoFocus);
        icon->setAttribute(Qt::WA_StyledBackground, true);
        icons_layout_->addWidget(icon);
        icons_.push_back(icon);
    }
    while (icons_.size() > entries.size()) {
        QToolButton *icon = icons_.back();
        icons_.pop_back();
        icons_layout_->removeWidget(icon);
        // Biome never pumps a Qt event loop, so deleteLater() would just
        // leak - plain delete is safe since nothing else references these.
        delete icon;
    }

    for (size_t i = 0; i < entries.size(); i++) {
        QToolButton *icon = icons_[i];
        icon->setProperty("selected", static_cast<int>(i) == selected_index);

        const IconImage &icon_image = entries[i].icon;
        bool has_icon = icon_image.size > 0 && !icon_image.pixels.empty();
        if (has_icon) {
            QImage image(icon_image.pixels.data(), icon_image.size, icon_image.size,
                QImage::Format_ARGB32_Premultiplied);
            icon->setIcon(QIcon(QPixmap::fromImage(image)));
        }
        icon->setVisible(has_icon);
    }

    full_title_.clear();
    if (selected_index >= 0 && static_cast<size_t>(selected_index) < entries.size()) {
        full_title_ = entries[static_cast<size_t>(selected_index)].label;
    }
    // Same untrusted-title concern as DecorationFrame::setTitle() -
    // simplified() keeps an embedded newline from making a multi-line label.
    title_label_->setText(QString::fromUtf8(full_title_.c_str()).simplified());
}

void SwitcherPanel::elideTitle() {
    QFontMetrics metrics(title_label_->font());
    QString full = QString::fromUtf8(full_title_.c_str()).simplified();
    QString elided = metrics.elidedText(full, Qt::ElideRight, title_label_->contentsRect().width());
    title_label_->setText(elided);
}

// Transparent root that owns SwitcherPanel as a child, rather than rendering
// the panel itself as the top-level widget. Qt only clips a styled widget's
// own background/border to its QSS border-radius when it's painted as a
// child - a styled widget rendered as the root of a QWidget::render() call
// instead paints a flat rect and relies on masking the *native* window's
// shape to get the rounded look on a real on-screen window, which never
// happens here since nothing in Biome's decoration pipeline is ever shown.
// Same reasoning as frame_widget.h's DecorationFrame staying transparent and
// leaving its own corners to child widgets (biomeTitlebar/biomeBorderBottom).
class SwitcherRoot : public QWidget {
    Q_OBJECT

public:
    explicit SwitcherRoot(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("biomeSwitcherRoot");
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        panel = new SwitcherPanel(this);
        layout->addWidget(panel);
    }

    SwitcherPanel *panel = nullptr;
};

SwitcherRoot *g_root = nullptr;

} // namespace

RenderedFrame render_switcher(const std::vector<SwitcherEntry> &entries, int selected_index) {
    RenderedFrame frame;
    if (entries.empty()) {
        return frame;
    }

    if (g_root == nullptr) {
        g_root = new SwitcherRoot();
        // No per-widget setStyleSheet() needed - decoration/theme.cpp's
        // load_decoration_theme() already applied the theme application-wide
        // via qApp->setStyleSheet(), which cascades to widgets constructed
        // afterwards same as one set directly on them. The repolish_tree()
        // call just below (needed unconditionally on every render anyway,
        // for newly-added rows) covers this first-construction case too.
    }

    g_root->panel->setEntries(entries, selected_index);
    repolish_tree(g_root); // newly-added rows above need their QSS applied too
    g_root->resize(g_root->minimumSizeHint());
    force_activate_layouts(g_root);
    g_root->panel->elideTitle();

    int width = g_root->width();
    int height = g_root->height();
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

#include "switcher.moc"
