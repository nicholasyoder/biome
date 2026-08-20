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

// Icon themes provide this size cleanly (a standard hicolor/breeze/etc.
// bucket), and it comfortably covers both the titlebar's and the switcher
// row's QSS-declared slot sizes - decoration/ widgets scale this cached
// bitmap down as needed, the same way the SVG button glyphs are
// scale-independent of their rendered size.
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
    // The desktop-entry spec allows Icon= to be either a bare icon-theme
    // name or an absolute path to an image file - both are valid.
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
        // Some icon-theme/fallback lookups can hand back a different size
        // than requested - scale explicitly rather than caching a size
        // decoration/'s IconImage::size field wouldn't actually match.
        image = image.scaled(kIconRasterSize, kIconRasterSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    result.size = kIconRasterSize;
    result.pixels.assign(image.constBits(), image.constBits() + static_cast<size_t>(image.sizeInBytes()));
    return result;
}

std::unordered_map<std::string, biome_decoration::IconImage> g_app_id_icon_cache;

// Base directories icon themes live under, per the icon theme spec's search
// order - $HOME/.icons (legacy per-user location) first, then each XDG data
// dir's icons/ subdirectory. Qt's own default QIcon::themeSearchPaths() is
// just an internal Qt resource path (":/icons") unless a *platform-theme*
// plugin (QT_QPA_PLATFORMTHEME, e.g. qt6ct) populates it with the real ones
// below - confirmed empirically: with that env var present, QIcon::fromTheme()
// already works with no explicit setThemeSearchPaths() call at all,
// regardless of Biome forcing the "offscreen" *windowing* platform: the
// theme plugin and the windowing platform load independently. But nothing
// sets QT_QPA_PLATFORMTHEME in Biome's actual bare-TTY target environment
// (no session to set it), so init_icon_theme() sets this explicitly rather
// than relying on it.
//
// Once Phase 4 (Forest integration) exists, a forest-session-style launcher
// that execs Biome could export QT_QPA_PLATFORMTHEME first, making this
// specific call redundant for that launch path alone - worth re-checking
// then, not assuming. It wouldn't become removable outright, though: Biome
// running standalone (no Forest, no session) is a first-class case by this
// project's own design (see docs/plan.md's Phase 3/4 ordering, and the
// self-contained embedded theme replacing the old Forest-QSS-reading
// module), and that case would still hit this exact gap.
QStringList icon_theme_search_paths() {
    QStringList dirs;
    dirs << QDir::homePath() + "/.icons";
    for (const QString &dir : data_dirs()) {
        dirs << dir + "/icons";
    }
    return dirs;
}

// True if name is an installed, real icon theme (has an index.theme) - a
// value read out of a config file is just a string someone typed, not a
// guarantee it's still installed, so this is what lets init_icon_theme()
// below actually fall through to the next candidate rather than handing
// QIcon::setThemeName() a name that resolves nothing.
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

    // qt6ct.conf first: it's the one config file that's actually live on a
    // deployment using qt6ct as its platform-theme plugin (as Forest's
    // startforest session script seeds it, via Forest's own
    // usr/share/forest/qtct.conf default) - matching "the same icon theme
    // as everything else in the desktop" is the explicit goal here, not
    // just picking any installed theme.
    QSettings qt6ct(QDir::homePath() + "/.config/qt6ct/qt6ct.conf", QSettings::IniFormat);
    QString theme = qt6ct.value("Appearance/icon_theme").toString();

    if (!icon_theme_exists(theme)) {
        // GTK3's settings.ini as a secondary source - same simple key=value
        // format as the .desktop parsing above, no GTK dependency needed.
        QSettings gtk3(QDir::homePath() + "/.config/gtk-3.0/settings.ini", QSettings::IniFormat);
        theme = gtk3.value("Settings/gtk-icon-theme-name").toString();
    }

    if (!icon_theme_exists(theme)) {
        // hicolor is spec-mandated and always present on any system with
        // icon-theme support at all - covers icons an app ships directly
        // under its own name (many do), even if broader
        // semantic/generic-named icons won't resolve.
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
            // Pick the icon closest to the canonical raster size (preferring
            // to downscale a larger one over upscaling a smaller one on a
            // tie) rather than just taking whichever the client listed
            // first - _NET_WM_ICON commonly carries several sizes.
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
                // xcb-ewmh decodes _NET_WM_ICON's CARDINAL array into
                // host-order 0xAARRGGBB values already - straight (not
                // premultiplied) alpha per the EWMH spec, so build as plain
                // ARGB32 first and let convertToFormat() do the
                // premultiplication correctly rather than assuming the
                // source is already premultiplied.
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

    // No client-supplied icon (or ewmh init failed) - fall back to the same
    // desktop-file lookup xdg-shell clients use, keyed on WM_CLASS.
    return resolve_app_id_icon(wm_class);
}
