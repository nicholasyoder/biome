// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/fade_config.h"

#include "core/config.h"

#include <QString>
#include <QStringList>
#include <QVariant>

namespace {

// Takes the raw QVariant, not `.toString()` - QSettings' IniFormat
// auto-detects an unescaped comma in a value as its own QStringList
// encoding on read-back, and QVariant::toString() on a QStringList with
// more than one element returns an empty string rather than rejoining it.
// A single-namespace value (no comma) round-trips fine as a plain QString
// either way, which is what let this go unnoticed until a second
// namespace was added to the same key. toStringList() normalizes both
// cases: one entry for a plain string, N entries for an auto-split list.
std::unordered_set<std::string> parse_namespace_list(const QVariant &raw) {
    std::unordered_set<std::string> result;
    for (const QString &ns : raw.toStringList()) {
        const QString trimmed = ns.trimmed();
        if (!trimmed.isEmpty()) {
            result.insert(trimmed.toStdString());
        }
    }
    return result;
}

} // namespace

FadeConfig load_fade_config() {
    const BiomeConfig &settings = biome_config();
    FadeConfig config;
    config.fading_namespaces = parse_namespace_list(settings.value("LayerShell/fadingNamespaces"));
    config.scanout_fading_namespaces = parse_namespace_list(settings.value("LayerShell/scanoutFadingNamespaces"));
    return config;
}
