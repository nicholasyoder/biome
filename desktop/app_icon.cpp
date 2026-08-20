// SPDX-License-Identifier: LGPL-3.0-or-later

#include "app_icon.h"

#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <cstdlib>
#include <unordered_map>

namespace {

// A standard hicolor/breeze/etc. bucket size, comfortably covering both the
// titlebar's and the switcher row's QSS-declared slot sizes - decoration/
// widgets scale this cached bitmap down as needed.
constexpr int kIconRasterSize = 32;

// Per the XDG base directory spec: XDG_DATA_DIRS falls back to this exact
// list when unset. $HOME/.local/share/applications is always searched
// first, ahead of the system dirs, matching every desktop-entry-consuming
// tool's precedence order.
QStringList data_dirs() {
    QStringList dirs;
    dirs << QDir::homePath() + "/.local/share";
    const char *env = std::getenv("XDG_DATA_DIRS");
    QString value = (env != nullptr && env[0] != '\0')
        ? QString::fromLocal8Bit(env)
        : QStringLiteral("/usr/local/share:/usr/share");
    dirs << value.split(':', Qt::SkipEmptyParts);
    return dirs;
}

// app_id is spec-recommended to equal its .desktop file's basename, so this
// is the common case - no directory scan needed.
QString find_desktop_file_exact(const QString &id) {
    for (const QString &dir : data_dirs()) {
        QString path = dir + "/applications/" + id + ".desktop";
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return QString();
}

// Fallback for apps whose app_id/WM_CLASS doesn't match their desktop
// file's name - scans (non-recursively; a distro-vendor subdirectory nested
// under applications/ won't be found, an accepted gap in this fallback
// path) for a StartupWMClass= match instead.
QString find_desktop_file_by_wm_class(const QString &id) {
    for (const QString &dir : data_dirs()) {
        QDir applications_dir(dir + "/applications");
        if (!applications_dir.exists()) {
            continue;
        }
        const QFileInfoList entries =
            applications_dir.entryInfoList(QStringList() << "*.desktop", QDir::Files);
        for (const QFileInfo &entry : entries) {
            QSettings settings(entry.absoluteFilePath(), QSettings::IniFormat);
            if (settings.value("Desktop Entry/StartupWMClass").toString() == id) {
                return entry.absoluteFilePath();
            }
        }
    }
    return QString();
}

QIcon icon_from_desktop_file(const QString &path) {
    QSettings settings(path, QSettings::IniFormat);
    QString icon_value = settings.value("Desktop Entry/Icon").toString();
    if (icon_value.isEmpty()) {
        return QIcon();
    }
    // Icon= can be either a bare icon-theme name or an absolute image path.
    return QDir::isAbsolutePath(icon_value) ? QIcon(icon_value) : QIcon::fromTheme(icon_value);
}

biome_decoration::IconImage rasterize(const QIcon &icon) {
    biome_decoration::IconImage result;
    if (icon.isNull()) {
        return result;
    }
    QPixmap pixmap = icon.pixmap(kIconRasterSize, kIconRasterSize);
    if (pixmap.isNull()) {
        return result;
    }
    QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (image.width() != kIconRasterSize || image.height() != kIconRasterSize) {
        // Some lookups hand back a different size than requested.
        image = image.scaled(kIconRasterSize, kIconRasterSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    result.size = kIconRasterSize;
    result.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return result;
}

std::unordered_map<std::string, biome_decoration::IconImage> g_app_id_icon_cache;

// Base directories icon themes live under, per the icon theme spec's search
// order - $HOME/.icons (legacy per-user location) first, then each XDG data
// dir's icons/ subdirectory. Qt's default QIcon::themeSearchPaths() is just
// an internal resource path unless a platform-theme plugin (QT_QPA_
// PLATFORMTHEME, e.g. qt6ct) populates it - nothing sets that env var in
// Biome's bare-TTY target environment (no session to set it), so this is
// set explicitly instead of relying on it.
QStringList icon_theme_search_paths() {
    QStringList dirs;
    dirs << QDir::homePath() + "/.icons";
    for (const QString &dir : data_dirs()) {
        dirs << dir + "/icons";
    }
    return dirs;
}

// True if name is an installed, real icon theme (has an index.theme) - lets
// init_icon_theme() below fall through to the next candidate rather than
// handing QIcon::setThemeName() a name that resolves nothing.
bool icon_theme_exists(const QString &name) {
    if (name.isEmpty()) {
        return false;
    }
    for (const QString &dir : icon_theme_search_paths()) {
        if (QFileInfo::exists(dir + "/" + name + "/index.theme")) {
            return true;
        }
    }
    return false;
}

} // namespace

void init_icon_theme() {
    QIcon::setThemeSearchPaths(icon_theme_search_paths());

    // qt6ct.conf first (Forest's startforest session seeds it) to match the
    // rest of the desktop's icon theme, not just pick any installed one.
    QSettings qt6ct(QDir::homePath() + "/.config/qt6ct/qt6ct.conf", QSettings::IniFormat);
    QString theme = qt6ct.value("Appearance/icon_theme").toString();

    if (!icon_theme_exists(theme)) {
        // GTK3's settings.ini as a secondary source, no GTK dependency needed.
        QSettings gtk3(QDir::homePath() + "/.config/gtk-3.0/settings.ini", QSettings::IniFormat);
        theme = gtk3.value("Settings/gtk-icon-theme-name").toString();
    }

    if (!icon_theme_exists(theme)) {
        // hicolor is spec-mandated and always present.
        theme = QStringLiteral("hicolor");
    }

    QIcon::setThemeName(theme);
}

biome_decoration::IconImage resolve_app_id_icon(const std::string &app_id) {
    if (app_id.empty()) {
        return {};
    }
    auto cached = g_app_id_icon_cache.find(app_id);
    if (cached != g_app_id_icon_cache.end()) {
        return cached->second;
    }

    QString id = QString::fromStdString(app_id);
    QString path = find_desktop_file_exact(id);
    if (path.isEmpty()) {
        path = find_desktop_file_by_wm_class(id);
    }

    biome_decoration::IconImage result;
    if (!path.isEmpty()) {
        result = rasterize(icon_from_desktop_file(path));
    }
    g_app_id_icon_cache.emplace(app_id, result);
    return result;
}

biome_decoration::IconImage resolve_xwayland_icon(
        xcb_ewmh_connection_t *ewmh, xcb_window_t window, const std::string &wm_class) {
    if (ewmh != nullptr) {
        xcb_get_property_cookie_t cookie = xcb_ewmh_get_wm_icon(ewmh, window);
        xcb_ewmh_get_wm_icon_reply_t reply;
        if (xcb_ewmh_get_wm_icon_reply(ewmh, cookie, &reply, nullptr)) {
            // _NET_WM_ICON commonly carries several sizes - pick the one
            // closest to the canonical raster size, preferring to downscale
            // a larger one over upscaling a smaller one on a tie.
            xcb_ewmh_wm_icon_iterator_t iter = xcb_ewmh_get_wm_icon_iterator(&reply);
            uint32_t *best_data = nullptr;
            uint32_t best_width = 0;
            uint32_t best_height = 0;
            while (iter.rem > 0) {
                if (iter.width > 0 && iter.height > 0) {
                    int distance = static_cast<int>(iter.width) - kIconRasterSize;
                    int best_distance = static_cast<int>(best_width) - kIconRasterSize;
                    bool closer = best_data == nullptr
                        || std::abs(distance) < std::abs(best_distance)
                        || (std::abs(distance) == std::abs(best_distance) && distance >= 0 && best_distance < 0);
                    if (closer) {
                        best_data = iter.data;
                        best_width = iter.width;
                        best_height = iter.height;
                    }
                }
                xcb_ewmh_get_wm_icon_next(&iter);
            }

            biome_decoration::IconImage result;
            if (best_data != nullptr) {
                // xcb-ewmh decodes into host-order 0xAARRGGBB with straight
                // (not premultiplied) alpha per the EWMH spec - build as
                // plain ARGB32 and let convertToFormat() premultiply.
                QImage image(reinterpret_cast<const uchar *>(best_data),
                    static_cast<int>(best_width), static_cast<int>(best_height), QImage::Format_ARGB32);
                result = rasterize(QIcon(QPixmap::fromImage(
                    image.convertToFormat(QImage::Format_ARGB32_Premultiplied))));
            }
            xcb_ewmh_get_wm_icon_reply_wipe(&reply);
            if (result.size > 0) {
                return result;
            }
        }
    }

    // No client-supplied icon - fall back to the desktop-file lookup.
    return resolve_app_id_icon(wm_class);
}
