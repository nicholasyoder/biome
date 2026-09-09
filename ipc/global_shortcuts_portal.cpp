// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ipc/global_shortcuts_portal.h"

#include "core/keybindings.h"

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMetaType>
#include <QDateTime>

#include <utility>

QDBusArgument &operator<<(QDBusArgument &arg, const GlobalShortcutSpec &spec) {
    arg.beginStructure();
    arg << spec.id << spec.options;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, GlobalShortcutSpec &spec) {
    arg.beginStructure();
    arg >> spec.id >> spec.options;
    arg.endStructure();
    return arg;
}

GlobalShortcutsPortal::GlobalShortcutsPortal(QObject *parent) : QObject(parent) {
}

uint GlobalShortcutsPortal::CreateSession(const QDBusObjectPath &handle,
        const QDBusObjectPath &session_handle, const QString &app_id,
        const QVariantMap &options, QVariantMap &results) {
    (void)handle;
    (void)app_id;
    (void)options;

    const QString owner = session_handle.path();
    m_sessions.insert(owner, {});

    auto *sessionObject = new PortalSession(this, owner);
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(owner, sessionObject,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllProperties)) {
        wlr_log(WLR_ERROR, "GlobalShortcuts: failed to register Session object at %s: %s",
            qPrintable(owner), qPrintable(bus.lastError().message()));
    }
    m_sessionObjects.insert(owner, sessionObject);

    results.clear();
    return 0; // success
}

uint GlobalShortcutsPortal::BindShortcuts(const QDBusObjectPath &handle,
        const QDBusObjectPath &session_handle, const QList<GlobalShortcutSpec> &shortcuts,
        const QString &parent_window, const QVariantMap &options, QVariantMap &results) {
    (void)handle;
    (void)parent_window;
    (void)options;

    const QString owner = session_handle.path();
    if (!m_sessions.contains(owner)) {
        wlr_log(WLR_ERROR, "GlobalShortcuts: BindShortcuts for unknown session %s",
            qPrintable(owner));
        return 2; // no such session
    }

    // "An application can only attempt to bind shortcuts of a session
    // once" per the real frontend spec - not enforced here yet (this
    // prototype doesn't model session state that deeply), but rebinding
    // just re-registers on top of whatever's already there rather than
    // replacing it, which would double-fire on a second call. Left as a
    // known gap for the later step that actually wires up reloadhotkeys().
    QList<GlobalShortcutSpec> bound;
    for (const GlobalShortcutSpec &spec : shortcuts) {
        const QString preferred_trigger = spec.options.value(QStringLiteral("preferred_trigger")).toString();
        const std::optional<ParsedTrigger> parsed = parse_trigger(preferred_trigger);
        if (!parsed) {
            wlr_log(WLR_ERROR,
                "GlobalShortcuts: BindShortcuts got unparseable preferred_trigger \"%s\" for "
                "shortcut \"%s\" - skipping, no confirmation UI exists to ask for another one",
                qPrintable(preferred_trigger), qPrintable(spec.id));
            continue;
        }

        // Auto-accept with no confirmation dialog - see this file's header
        // comment. shortcut_id is captured by value since spec.id is a
        // QString living in the caller's temporary shortcuts list.
        const QString shortcut_id = spec.id;
        add_portal_keybinding(owner, *parsed,
            [this, session_handle, shortcut_id](BiomeServer *, uint32_t) {
                const qulonglong now = static_cast<qulonglong>(QDateTime::currentMSecsSinceEpoch());
                // Every bound shortcut is treated as an instant (press-only)
                // action for this prototype - fire Activated immediately
                // followed by Deactivated rather than tracking real
                // press/hold state. Press/hold semantics are deferred, per
                // the plan doc.
                emit Activated(session_handle, shortcut_id, now, {});
                emit Deactivated(session_handle, shortcut_id, now, {});
                return true;
            });

        GlobalShortcutSpec out;
        out.id = spec.id;
        out.options.insert(QStringLiteral("description"),
            spec.options.value(QStringLiteral("description")));
        // A real backend renders a localized, human-readable string here
        // (e.g. "Ctrl+Alt+Return"); echoing the raw trigger string back is a
        // stand-in good enough to confirm round-tripping for this
        // prototype.
        out.options.insert(QStringLiteral("trigger_description"), preferred_trigger);
        bound.push_back(out);
    }

    m_sessions.insert(owner, bound);
    results.clear();
    results.insert(QStringLiteral("shortcuts"), QVariant::fromValue(bound));
    return 0;
}

uint GlobalShortcutsPortal::ListShortcuts(const QDBusObjectPath &handle,
        const QDBusObjectPath &session_handle, const QVariantMap &options,
        QVariantMap &results) {
    (void)handle;
    (void)options;

    const QString owner = session_handle.path();
    const auto it = m_sessions.constFind(owner);
    if (it == m_sessions.constEnd()) {
        wlr_log(WLR_ERROR, "GlobalShortcuts: ListShortcuts for unknown session %s",
            qPrintable(owner));
        return 2;
    }

    results.clear();
    results.insert(QStringLiteral("shortcuts"), QVariant::fromValue(it.value()));
    return 0;
}

uint GlobalShortcutsPortal::ConfigureShortcuts(const QDBusObjectPath &handle,
        const QDBusObjectPath &session_handle, const QString &parent_window,
        const QVariantMap &options, QVariantMap &results) {
    (void)handle;
    (void)session_handle;
    (void)parent_window;
    (void)options;
    results.clear();
    return 2; // not supported - Biome has no shortcut-configuration UI
}

void GlobalShortcutsPortal::closeSession(const QString &owner) {
    remove_session_keybindings(owner);
    m_sessions.remove(owner);
    m_sessionObjects.remove(owner);
}

PortalSession::PortalSession(GlobalShortcutsPortal *portal, QString path)
    : QObject(portal), m_portal(portal), m_path(std::move(path)) {
}

void PortalSession::Close() {
    QDBusConnection::sessionBus().unregisterObject(m_path);
    m_portal->closeSession(m_path);
    deleteLater();
}

void global_shortcuts_portal_init(BiomeServer *server) {
    (void)server; // dispatch is the qt_glib_bridge's problem now, already live before this runs

    qDBusRegisterMetaType<GlobalShortcutSpec>();
    qDBusRegisterMetaType<QList<GlobalShortcutSpec>>();

    static GlobalShortcutsPortal *portal = new GlobalShortcutsPortal();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        wlr_log(WLR_ERROR, "Biome: no D-Bus session bus available, GlobalShortcuts portal disabled");
        return;
    }
    if (!bus.registerObject(QStringLiteral("/org/freedesktop/portal/desktop"), portal,
            QDBusConnection::ExportAllSlots | QDBusConnection::ExportAllSignals)) {
        wlr_log(WLR_ERROR, "Biome: failed to register GlobalShortcuts portal object: %s",
            qPrintable(bus.lastError().message()));
        return;
    }
    // Real backend bus-name convention (org.freedesktop.impl.portal.desktop.<name>)
    // - matters once portals.conf wiring routes xdg-desktop-portal to this
    // name in a later step; harmless to claim now even though nothing looks
    // it up yet.
    if (!bus.registerService(QStringLiteral("org.freedesktop.impl.portal.desktop.biome"))) {
        wlr_log(WLR_ERROR, "Biome: failed to register GlobalShortcuts portal bus name: %s",
            qPrintable(bus.lastError().message()));
    }
}
