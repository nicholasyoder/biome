// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/config.h"

#include <QDir>
#include <QSet>
#include <QSettings>

namespace {

constexpr const char *kSystemDropInDir = "/etc/biome/conf.d";

void merge_from(QSettings &settings, QMap<QString, QVariant> &values) {
    for (const QString &key : settings.allKeys()) {
        values.insert(key, settings.value(key));
    }
}

} // namespace

QVariant BiomeConfig::value(const QString &key, const QVariant &default_value) const {
    return values_.value(key, default_value);
}

bool BiomeConfig::contains(const QString &key) const {
    return values_.contains(key);
}

QStringList BiomeConfig::child_groups(const QString &group) const {
    const QString prefix = group + '/';
    QStringList groups;
    QSet<QString> seen;
    for (auto it = values_.lowerBound(prefix); it != values_.end() && it.key().startsWith(prefix); ++it) {
        const qsizetype slash = it.key().indexOf('/', prefix.size());
        if (slash < 0) {
            continue;
        }
        const QString child = it.key().mid(prefix.size(), slash - prefix.size());
        if (!seen.contains(child)) {
            seen.insert(child);
            groups.append(child);
        }
    }
    return groups;
}

const BiomeConfig &biome_config() {
    static const BiomeConfig config = [] {
        BiomeConfig c;
        const QDir drop_ins(kSystemDropInDir);
        for (const QString &name : drop_ins.entryList({"*.conf"}, QDir::Files, QDir::Name)) {
            QSettings settings(drop_ins.filePath(name), QSettings::IniFormat);
            merge_from(settings, c.values_);
        }
        QSettings user("Biome", "Biome");
        // Otherwise /etc/xdg/Biome/Biome.conf would be a second system location.
        user.setFallbacksEnabled(false);
        merge_from(user, c.values_);
        return c;
    }();
    return config;
}
