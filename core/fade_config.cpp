// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/fade_config.h"

#include <QSettings>
#include <QString>
#include <QStringList>

namespace {

std::unordered_set<std::string> parse_namespace_list(const QString &raw) {
    std::unordered_set<std::string> result;
    for (const QString &ns : raw.split(',', Qt::SkipEmptyParts)) {
        const QString trimmed = ns.trimmed();
        if (!trimmed.isEmpty()) {
            result.insert(trimmed.toStdString());
        }
    }
    return result;
}

} // namespace

FadeConfig load_fade_config() {
    QSettings settings("Forest", "Biome");
    settings.beginGroup("LayerShell");
    FadeConfig config;
    config.fading_namespaces = parse_namespace_list(settings.value("fadingNamespaces").toString());
    config.scanout_fading_namespaces = parse_namespace_list(settings.value("scanoutFadingNamespaces").toString());
    settings.endGroup();
    return config;
}
