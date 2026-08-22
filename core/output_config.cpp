// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_config.h"

#include <QDebug>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <cmath>

namespace {

std::optional<OutputConfig::Mode> parse_mode(const QString &raw, const QString &connector) {
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("preferred", Qt::CaseInsensitive) == 0) {
        return std::nullopt;
    }

    const int x_pos = trimmed.indexOf('x', 0, Qt::CaseInsensitive);
    if (x_pos <= 0) {
        qWarning() << "Biome: output" << connector << "has malformed mode" << raw
                   << "- expected WIDTHxHEIGHT[@REFRESH] or \"preferred\", using preferred";
        return std::nullopt;
    }

    QString rest = trimmed.mid(x_pos + 1);
    int refresh_mhz = 0;
    const int at_pos = rest.indexOf('@');
    if (at_pos >= 0) {
        bool ok = false;
        const double refresh_hz = rest.mid(at_pos + 1).toDouble(&ok);
        if (ok && refresh_hz > 0) {
            refresh_mhz = static_cast<int>(std::llround(refresh_hz * 1000.0));
        } else {
            qWarning() << "Biome: output" << connector << "has malformed refresh in mode" << raw
                       << "- ignoring refresh, matching on resolution only";
        }
        rest = rest.left(at_pos);
    }

    bool width_ok = false, height_ok = false;
    const int width = trimmed.left(x_pos).toInt(&width_ok);
    const int height = rest.toInt(&height_ok);
    if (!width_ok || !height_ok || width <= 0 || height <= 0) {
        qWarning() << "Biome: output" << connector << "has malformed mode" << raw
                   << "- using preferred";
        return std::nullopt;
    }

    return OutputConfig::Mode{width, height, refresh_mhz};
}

wl_output_transform parse_transform(const QString &raw, const QString &connector) {
    static const struct {
        const char *name;
        wl_output_transform value;
    } kTransforms[] = {
        {"normal", WL_OUTPUT_TRANSFORM_NORMAL},
        {"90", WL_OUTPUT_TRANSFORM_90},
        {"180", WL_OUTPUT_TRANSFORM_180},
        {"270", WL_OUTPUT_TRANSFORM_270},
        {"flipped", WL_OUTPUT_TRANSFORM_FLIPPED},
        {"flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90},
        {"flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180},
        {"flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270},
    };
    const QString normalized = raw.trimmed().toLower();
    for (const auto &entry : kTransforms) {
        if (normalized == entry.name) {
            return entry.value;
        }
    }
    qWarning() << "Biome: output" << connector << "has unknown transform" << raw
               << "- using normal";
    return WL_OUTPUT_TRANSFORM_NORMAL;
}

} // namespace

std::unordered_map<std::string, OutputConfig> load_output_configs() {
    std::unordered_map<std::string, OutputConfig> result;

    // Connector names become QSettings group names (Qt writes nested groups
    // as backslash-escaped keys within the parent's own [Outputs] section -
    // e.g. "eDP-1\enabled=true" - not as separate [Outputs/eDP-1] headers).
    // Holds for every backend Biome supports today: DRM connectors are
    // "<type>-<index>" (eDP-1, HDMI-A-1, DP-1, ...) per the kernel's
    // connector-type table, and the nested dev backends use "WL-<n>"/
    // "X11-<n>".
    QSettings settings("Forest", "Biome");
    settings.beginGroup("Outputs");
    const QStringList connectors = settings.childGroups();
    for (const QString &connector : connectors) {
        settings.beginGroup(connector);

        OutputConfig cfg;
        cfg.enabled = settings.value("enabled", true).toBool();
        cfg.mode = parse_mode(settings.value("mode", "preferred").toString(), connector);
        cfg.scale = settings.value("scale", 1.0).toDouble();

        const bool has_x = settings.contains("x");
        const bool has_y = settings.contains("y");
        if (has_x && has_y) {
            cfg.position = {settings.value("x").toInt(), settings.value("y").toInt()};
        } else if (has_x != has_y) {
            qWarning() << "Biome: output" << connector
                       << "has only one of x/y set - ignoring, using auto-arrange";
        }

        cfg.transform = parse_transform(settings.value("transform", "normal").toString(), connector);

        settings.endGroup(); // connector
        result.emplace(connector.toStdString(), cfg);
    }
    settings.endGroup(); // Outputs

    return result;
}
