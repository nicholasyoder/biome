// SPDX-License-Identifier: LGPL-3.0-or-later
//
// org.freedesktop.impl.portal.GlobalShortcuts - the desktop-specific
// *backend* interface xdg-desktop-portal brokers to on behalf of an app
// calling the generic org.freedesktop.portal.GlobalShortcuts frontend (the
// same split KWin/Mutter implement for GNOME/KDE). Implementing the backend
// rather than the frontend directly is what satisfies docs/plan.md's
// Decoupling goal here: any shell going through the standard broker can use
// this, not just Forest specifically. See docs/phase4-plan.md's Workstream
// C for the full research behind this choice.
//
// Scope for this first prototype step (Biome-side only, no forest/ changes,
// no portals.conf/xdg-desktop-portal system wiring yet - see the plan doc):
// CreateSession/BindShortcuts/ListShortcuts, auto-accepting every requested
// shortcut with no confirmation dialog (matches Biome's fixed-policy
// identity - there's no shortcut-picker UI and none is planned).
// ConfigureShortcuts replies "not supported". Trigger parsing and the
// actual key-press matching/dispatch live in core/keybindings.h, shared
// with Biome's own built-in compositor keybindings - this class only
// forwards BindShortcuts requests into that registry and re-emits
// Activated/Deactivated as real D-Bus signals when a registered trigger
// fires.
//
// Session teardown: CreateSession() also exports a real per-session
// org.freedesktop.impl.portal.Session object (PortalSession below) at the
// session_handle path, since a live busctl monitor capture during
// Workstream C step 2 (docs/phase4-session-log.md, 2026-08-26) confirmed
// the real xdg-desktop-portal daemon actually calls
// org.freedesktop.DBus.Properties.GetAll and
// org.freedesktop.impl.portal.Session.Close() against that exact path -
// without an object there, both got UnknownObject errors back.

#pragma once

#include "core/server.h"

#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

// One entry of the portal's `a(sa{sv})` shortcuts array: a caller-chosen id
// plus a vardict of well-known keys (description/preferred_trigger on the
// way in, description/trigger_description on the way back out).
struct GlobalShortcutSpec {
    QString id;
    QVariantMap options;
};
Q_DECLARE_METATYPE(GlobalShortcutSpec)

QDBusArgument &operator<<(QDBusArgument &arg, const GlobalShortcutSpec &spec);
const QDBusArgument &operator>>(const QDBusArgument &arg, GlobalShortcutSpec &spec);

class GlobalShortcutsPortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.GlobalShortcuts")

public:
    explicit GlobalShortcutsPortal(QObject *parent = nullptr);

public slots:
    // Real impl.portal.* backend methods reply synchronously with
    // (response, results) - unlike the frontend interfaces, which return a
    // request object path and reply later via a Request::Response signal
    // (that async dance is the portal daemon's own job, not the backend's).
    // QtDBus maps a slot's return value to the first D-Bus out-argument and
    // any trailing non-const-reference parameters to the rest - see
    // https://doc.qt.io/qt-6/qtdbus-index.html.
    uint CreateSession(const QDBusObjectPath &handle, const QDBusObjectPath &session_handle,
        const QString &app_id, const QVariantMap &options, QVariantMap &results);

    uint BindShortcuts(const QDBusObjectPath &handle, const QDBusObjectPath &session_handle,
        const QList<GlobalShortcutSpec> &shortcuts, const QString &parent_window,
        const QVariantMap &options, QVariantMap &results);

    uint ListShortcuts(const QDBusObjectPath &handle, const QDBusObjectPath &session_handle,
        const QVariantMap &options, QVariantMap &results);

    uint ConfigureShortcuts(const QDBusObjectPath &handle, const QDBusObjectPath &session_handle,
        const QString &parent_window, const QVariantMap &options, QVariantMap &results);

signals:
    void Activated(const QDBusObjectPath &session_handle, const QString &shortcut_id,
        qulonglong timestamp, const QVariantMap &options);
    void Deactivated(const QDBusObjectPath &session_handle, const QString &shortcut_id,
        qulonglong timestamp, const QVariantMap &options);
    void ShortcutsChanged(const QDBusObjectPath &session_handle,
        const QList<GlobalShortcutSpec> &shortcuts);

private:
    // Called by PortalSession::Close() once it has unregistered itself
    // from the bus - drops the session's portal keybindings and its
    // bookkeeping entries. Not a D-Bus slot itself, just a plain call from
    // PortalSession back to its owning portal.
    void closeSession(const QString &owner);
    friend class PortalSession;

    // session_handle path -> the bound shortcuts array last returned for
    // it, for ListShortcuts to hand back.
    QHash<QString, QList<GlobalShortcutSpec>> m_sessions;
    // session_handle path -> the exported Session object owning that path,
    // so CreateSession/closeSession can register/tear it down.
    QHash<QString, class PortalSession *> m_sessionObjects;
};

// org.freedesktop.impl.portal.Session - the per-session object the real
// portal daemon expects at the session_handle path (see this file's header
// comment). One instance per session: created and registered on the bus in
// GlobalShortcutsPortal::CreateSession(), unregistered and deleted once
// Close() fires.
class PortalSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Session")
    Q_PROPERTY(uint version READ version)

public:
    PortalSession(GlobalShortcutsPortal *portal, QString path);

    uint version() const { return 1; }

public slots:
    void Close();

private:
    GlobalShortcutsPortal *m_portal;
    QString m_path;
};

// Registers the GlobalShortcutSpec D-Bus marshalling, constructs a
// process-lifetime GlobalShortcutsPortal (intentionally never freed - same
// convention as Biome's other global singletons, e.g. the wlroots globals
// created in main.cpp), and registers it on the session bus.
void global_shortcuts_portal_init(BiomeServer *server);
