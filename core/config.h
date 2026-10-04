// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Read-only merged view of Biome's INI config: /etc/biome/conf.d/*.conf in
// filename order, then ~/.config/Biome/Biome.conf. Later sources replace
// earlier ones per key; keys are QSettings paths ("Outputs/eDP-1/x").

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariant>

class BiomeConfig {
public:
    QVariant value(const QString &key, const QVariant &default_value = {}) const;
    bool contains(const QString &key) const;
    // Distinct first path segments below `group`, like QSettings::childGroups().
    QStringList child_groups(const QString &group) const;

private:
    friend const BiomeConfig &biome_config();
    QMap<QString, QVariant> values_;
};

// Loaded on first call; startup-only, never reloaded.
const BiomeConfig &biome_config();
