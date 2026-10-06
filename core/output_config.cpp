// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_config.h"

#include "core/config.h"

#include <QDebug>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <charconv>
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

const struct {
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

wl_output_transform parse_transform(const QString &raw, const QString &connector) {
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

double parse_scale(const QString &raw, const QString &connector) {
    bool ok = false;
    const double scale = raw.toDouble(&ok);
    if (!ok || scale <= 0.0 || scale > 10.0) {
        qWarning() << "Biome: output" << connector << "has invalid scale" << raw
                   << "- using 1.0";
        return 1.0;
    }
    return scale;
}

QString format_mode(const OutputConfig::Mode &mode) {
    QString text = QString("%1x%2").arg(mode.width).arg(mode.height);
    if (mode.refresh_mhz > 0) {
        text += QString("@%1.%2").arg(mode.refresh_mhz / 1000).arg(mode.refresh_mhz % 1000, 3, 10, QChar('0'));
    }
    return text;
}

QString format_transform(wl_output_transform transform) {
    for (const auto &entry : kTransforms) {
        if (entry.value == transform) {
            return entry.name;
        }
    }
    return "normal";
}

// Shortest text that parses back to the same float wlroots holds, so a
// re-applied layout compares equal (QString::number's 6 digits doesn't).
QString format_scale(double scale) {
    char buf[32];
    auto result = std::to_chars(buf, buf + sizeof(buf), static_cast<float>(scale));
    return QString::fromLatin1(buf, result.ptr - buf);
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
    const BiomeConfig &settings = biome_config();
    const QStringList connectors = settings.child_groups("Outputs");
    for (const QString &connector : connectors) {
        const QString group = "Outputs/" + connector + '/';

        OutputConfig cfg;
        cfg.enabled = settings.value(group + "enabled", true).toBool();
        cfg.mode = parse_mode(settings.value(group + "mode", "preferred").toString(), connector);
        cfg.scale = parse_scale(settings.value(group + "scale", "1.0").toString(), connector);

        const bool has_x = settings.contains(group + "x");
        const bool has_y = settings.contains(group + "y");
        if (has_x && has_y) {
            cfg.position = {settings.value(group + "x").toInt(), settings.value(group + "y").toInt()};
        } else if (has_x != has_y) {
            qWarning() << "Biome: output" << connector
                       << "has only one of x/y set - ignoring, using auto-arrange";
        }

        cfg.transform = parse_transform(settings.value(group + "transform", "normal").toString(), connector);

        result.emplace(connector.toStdString(), cfg);
    }

    return result;
}

void save_output_configs(const std::vector<std::pair<std::string, OutputConfig>> &configs) {
    QSettings settings("Biome", "Biome");
    for (const auto &[connector, cfg] : configs) {
        const QString group = "Outputs/" + QString::fromStdString(connector) + '/';
        settings.setValue(group + "enabled", cfg.enabled);
        if (cfg.mode.has_value()) {
            settings.setValue(group + "mode", format_mode(*cfg.mode));
        }
        settings.setValue(group + "scale", format_scale(cfg.scale));
        if (cfg.position.has_value()) {
            settings.setValue(group + "x", cfg.position->first);
            settings.setValue(group + "y", cfg.position->second);
        }
        settings.setValue(group + "transform", format_transform(cfg.transform));
    }
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        qWarning() << "Biome: failed to save output layout to" << settings.fileName();
    }
}
