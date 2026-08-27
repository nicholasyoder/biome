// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/keybindings.h"

#include "desktop/decoration_bridge.h"
#include "desktop/toplevel.h"
#include "desktop/workspace.h"

#include <QStringList>

#include <algorithm>
#include <string>
#include <vector>

namespace {

// CapsLock (WLR_MODIFIER_CAPS) and the two rarely-used extra modifiers
// (MOD3/MOD5) are never part of the shortcuts-spec grammar and are masked
// out unconditionally - a shortcut shouldn't stop working just because
// CapsLock happens to be on. WLR_MODIFIER_MOD2 (the spec's "NUM") parses
// successfully in parse_trigger() but is masked out here too, deliberately:
// nothing needs a NumLock-conditioned shortcut yet, and honoring it by
// default would mean every existing binding (built-in or portal) silently
// stops matching whenever NumLock is toggled on - a worse default than
// just not supporting NUM-sensitive triggers yet. Revisit if something
// actually needs one.
constexpr uint32_t kMatchableModifierMask =
    WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO;

bool trigger_matches(const ParsedTrigger &trigger, xkb_keysym_t sym, uint32_t modifiers) {
    return trigger.keysym == sym && trigger.modmask == (modifiers & kMatchableModifierMask);
}

struct PortalBinding {
    QString owner;
    BiomeKeybinding binding;
};

std::vector<PortalBinding> &portal_bindings() {
    static std::vector<PortalBinding> bindings;
    return bindings;
}

// Alt-Tab/Alt-Shift-Tab: cycle through windows in MRU order, no live
// preview (matches xfwm4's cycle_preview=false default). Moved verbatim
// from the old handle_keybinding()'s Tab case. Deliberately not one of
// builtin_keybindings()'s table entries: it's stateful (a live MRU
// switcher/preview spanning an entire Alt-hold, not a one-shot action -
// see BiomeServer::switcher_active/switcher_preview_index), and xkb remaps
// the *keysym itself* to ISO_Left_Tab when Shift is held, which doesn't fit
// the table's exact (modmask, keysym) match without a special-cased
// equivalence - not worth forcing into the generic shape for the one
// binding that's already the most complex piece of this file.
bool handle_switcher_key(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers) {
    if (!(modifiers & WLR_MODIFIER_ALT)) {
        return false;
    }
    if (sym != XKB_KEY_Tab && sym != XKB_KEY_ISO_Left_Tab) {
        return false;
    }
    if (wl_list_empty(&server->toplevels)) {
        return true;
    }
    bool reverse = (modifiers & WLR_MODIFIER_SHIFT) || sym == XKB_KEY_ISO_Left_Tab;

    // The switcher always shows a static snapshot of MRU order taken at the
    // start of this Alt-hold, in both modes - server->toplevels itself
    // isn't re-read again until the hold ends, so the on-screen list
    // doesn't reshuffle underfoot as you cycle. Only switcher_preview_index
    // moves, wrapping over the snapshot.
    if (!server->switcher_active) {
        server->switcher_order.clear();
        BiomeToplevel *pos;
        wl_list_for_each(pos, &server->toplevels, link) {
            server->switcher_order.push_back(pos);
        }
        server->switcher_preview_index = 0;
    }
    int count = static_cast<int>(server->switcher_order.size());
    server->switcher_preview_index =
        (server->switcher_preview_index + (reverse ? -1 : 1) + count) % count;

    if constexpr (!kSwitcherSwitchOnRelease) {
        BiomeToplevel *target =
            server->switcher_order[static_cast<size_t>(server->switcher_preview_index)];
        if (target->minimized) {
            set_toplevel_minimized(target, false);
        }
        focus_toplevel(target, toplevel_surface(target));
    }
    server->switcher_active = true;
    update_switcher_overlay(server);
    return true;
}

// Compiled-in default keybindings, each trigger run through parse_trigger()
// itself rather than hand-built from WLR_MODIFIER_* constants - this
// exercises the same parser a portal-registered shortcut will use, and
// keeps built-ins and portal shortcuts genuinely the same mechanism instead
// of two independent ones running side by side.
const std::vector<BiomeKeybinding> &builtin_keybindings() {
    static const std::vector<BiomeKeybinding> table = [] {
        std::vector<BiomeKeybinding> t;
        auto add = [&](const std::string &trigger, std::function<bool(BiomeServer *, uint32_t)> invoke) {
            std::optional<ParsedTrigger> parsed = parse_trigger(QString::fromStdString(trigger));
            if (!parsed) {
                wlr_log(WLR_ERROR, "Biome: built-in keybinding trigger \"%s\" failed to parse",
                    trigger.c_str());
                return;
            }
            t.push_back(BiomeKeybinding{*parsed, std::move(invoke)});
        };

        add("ALT+Escape", [](BiomeServer *server, uint32_t) {
            wl_display_terminate(server->display);
            return true;
        });

        // Ctrl-Alt-Left/Right: switch workspace. Ctrl-Alt-Shift-Left/Right:
        // same, but bring the focused window along too.
        for (int dir = -1; dir <= 1; dir += 2) {
            const char *plain = dir < 0 ? "CTRL+ALT+Left" : "CTRL+ALT+Right";
            const char *with_shift = dir < 0 ? "CTRL+ALT+SHIFT+Left" : "CTRL+ALT+SHIFT+Right";

            add(plain, [dir](BiomeServer *server, uint32_t) {
                switch_workspace(server, server->active_workspace + dir);
                return true;
            });
            add(with_shift, [dir](BiomeServer *server, uint32_t) {
                int target = server->active_workspace + dir;
                if (!wl_list_empty(&server->toplevels)) {
                    BiomeToplevel *focused = wl_container_of(server->toplevels.next, focused, link);
                    move_toplevel_to_workspace(focused, target);
                }
                switch_workspace(server, target);
                return true;
            });
        }

        // Ctrl-Alt-1..4: jump directly to a workspace.
        for (int i = 0; i < 4; i++) {
            add("CTRL+ALT+" + std::to_string(i + 1), [i](BiomeServer *server, uint32_t) {
                switch_workspace(server, i);
                return true;
            });
        }

        return t;
    }();
    return table;
}

} // namespace

std::optional<ParsedTrigger> parse_trigger(const QString &trigger) {
    const QStringList parts = trigger.split('+', Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return std::nullopt;
    }

    uint32_t modmask = 0;
    for (qsizetype i = 0; i < parts.size() - 1; i++) {
        const QString &mod = parts[i];
        if (mod == "CTRL") {
            modmask |= WLR_MODIFIER_CTRL;
        } else if (mod == "ALT") {
            modmask |= WLR_MODIFIER_ALT;
        } else if (mod == "SHIFT") {
            modmask |= WLR_MODIFIER_SHIFT;
        } else if (mod == "LOGO") {
            modmask |= WLR_MODIFIER_LOGO;
        } else if (mod == "NUM") {
            modmask |= WLR_MODIFIER_MOD2;
        } else {
            return std::nullopt;
        }
    }

    const QByteArray key_name = parts.last().toUtf8();
    xkb_keysym_t keysym = xkb_keysym_from_name(key_name.constData(), XKB_KEYSYM_NO_FLAGS);
    if (keysym == XKB_KEY_NoSymbol) {
        return std::nullopt;
    }

    return ParsedTrigger{modmask, keysym};
}

bool handle_key_press(BiomeServer *server, xkb_keysym_t sym, uint32_t modifiers) {
    // Ctrl-Alt-F1..F12: hand VT switching back to the session/kernel. Taking
    // over a KMS/DRM session puts the console in graphics mode, so the
    // compositor has to notice these and call wlr_session_change_vt() itself.
    // No-op (but still swallowed) on nested backends, which have no session.
    if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (server->session != nullptr) {
            unsigned vt = static_cast<unsigned>(sym - XKB_KEY_XF86Switch_VT_1 + 1);
            wlr_session_change_vt(server->session, vt);
        }
        return true;
    }

    // No keybinding of any kind - built-in, switcher, or portal-registered -
    // should fire while the session is locked; returning false lets the raw
    // key event fall through to whatever currently has keyboard focus (the
    // lock surface), which needs real keystrokes for its own password-entry
    // UI.
    if (server->session_locked) {
        return false;
    }

    if (handle_switcher_key(server, sym, modifiers)) {
        return true;
    }

    // Built-ins always win on an exact-trigger collision with a portal-
    // registered shortcut - a deliberate, conservative default. Whether a
    // portal-bound shortcut should ever be allowed to shadow a built-in is
    // a real open question (ties back into docs/plan.md's "fixed-policy"
    // stance) - left for a later step once there's an actual Forest-side
    // config entry that wants to do this, rather than guessed at now.
    for (const BiomeKeybinding &entry : builtin_keybindings()) {
        if (trigger_matches(entry.trigger, sym, modifiers)) {
            return entry.invoke(server, modifiers);
        }
    }

    for (const PortalBinding &entry : portal_bindings()) {
        if (trigger_matches(entry.binding.trigger, sym, modifiers)) {
            return entry.binding.invoke(server, modifiers);
        }
    }

    return false;
}

void add_portal_keybinding(const QString &owner, ParsedTrigger trigger,
        std::function<bool(BiomeServer *, uint32_t)> invoke) {
    portal_bindings().push_back(PortalBinding{owner, BiomeKeybinding{trigger, std::move(invoke)}});
}

void remove_session_keybindings(const QString &owner) {
    std::vector<PortalBinding> &bindings = portal_bindings();
    bindings.erase(
        std::remove_if(bindings.begin(), bindings.end(),
            [&owner](const PortalBinding &entry) { return entry.owner == owner; }),
        bindings.end());
}
