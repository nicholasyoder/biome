// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/touch_store.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace {

QString store_path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + "/biome/touch.conf";
}

// QSettings would treat '/' in a device name as a group separator.
QString group_for(const char *device_name) {
    return QString::fromUtf8(device_name).replace('/', '_');
}

} // namespace

TouchRemembered touch_store_load(const char *device_name) {
    QSettings settings(store_path(), QSettings::IniFormat);
    settings.beginGroup(group_for(device_name));
    return {
        settings.value("connector").toString().toStdString(),
        settings.value("make").toString().toStdString(),
        settings.value("model").toString().toStdString(),
        settings.value("serial").toString().toStdString(),
    };
}

void touch_store_save(const char *device_name, const TouchRemembered &answer) {
    QDir().mkpath(QFileInfo(store_path()).path());
    QSettings settings(store_path(), QSettings::IniFormat);
    settings.beginGroup(group_for(device_name));
    settings.setValue("connector", QString::fromStdString(answer.connector));
    settings.setValue("make", QString::fromStdString(answer.make));
    settings.setValue("model", QString::fromStdString(answer.model));
    settings.setValue("serial", QString::fromStdString(answer.serial));
}
