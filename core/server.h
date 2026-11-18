// SPDX-License-Identifier: LGPL-3.0-or-later
//
// BiomeServer: shared compositor world state, plus one embedded struct per
// subsystem holding that module's private globals, listeners and flags. Each
// such module has an X_init() and an X_finish() (main.cpp's teardown).

#pragma once

#include "core/output_config.h"
#include "core/fade_config.h"
#include "wlroots.hpp"

#include <cstddef>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

struct BiomeServer;
struct BiomeToplevel;

// The BiomeServer owning `listener`, the `member` of sub-struct type `Sub`.
#define biome_server_from(listener, Sub, member) \
    (reinterpret_cast<Sub *>(reinterpret_cast<char *>(listener) - offsetof(Sub, member))->server)

enum class BiomeCursorMode {
    Passthrough,
    Move,
    Resize,
};

// Window snap targets. Top is "maximize" (drag to the top edge); the *Half
// zones are menu-only.
enum class SnapZone {
    None,
    Left,
    Right,
    TopHalf,
    BottomHalf,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Top,
};

// Alt-Tab (desktop/switcher.cpp). order is the MRU snapshot frozen at the
// first Tab of an Alt-hold.
struct BiomeSwitcher {
    bool active = false;
    std::vector<BiomeToplevel *> order;
    int index = 0;
    wlr_scene_buffer *panel = nullptr;
    wlr_scene_buffer *highlight = nullptr;
};

// desktop/xdg_shell.cpp: xdg-shell, xdg-decoration and KDE server-decoration.
struct BiomeXdgShell {
    BiomeServer *server = nullptr;
    wlr_xdg_shell *shell = nullptr;
    wl_listener new_toplevel = {};
    wl_listener new_popup = {};
    wlr_xdg_decoration_manager_v1 *decoration_manager = nullptr;
    wl_listener new_toplevel_decoration = {};
    // GTK3 only speaks this older protocol.
    wlr_server_decoration_manager *kde_decoration_manager = nullptr;
    wl_listener new_kde_decoration = {};
    // KDE decorations created before their surface has a toplevel role.
    wl_list pending_kde_decorations = {};
};

// desktop/xwayland_shell.cpp.
struct BiomeXwayland {
    BiomeServer *server = nullptr;
    wlr_xwayland *wlr = nullptr;
    wl_listener new_surface = {};
    wl_listener ready = {};
    // For _NET_WM_ICON (desktop/app_icon.h); valid once ewmh_ready.
    xcb_ewmh_connection_t ewmh = {};
    bool ewmh_ready = false;
};

// desktop/layer_shell.cpp (+ layer_shell_fade.cpp).
struct BiomeLayerShell {
    BiomeServer *server = nullptr;
    wlr_layer_shell_v1 *shell = nullptr;
    wl_listener new_surface = {};
    wl_list surfaces = {}; // every BiomeLayerSurface, all outputs
    FadeConfig fade_config;
    // Fading-out content that outlived its BiomeLayerSurface, per fade mechanism.
    wl_list fading_out = {};
    wl_list scanout_fading = {};
};

// desktop/foreign_toplevel.cpp: wlr-foreign-toplevel-management, ext-foreign-
// toplevel-list (for its stable identifier) and per-window capture sources.
struct BiomeForeignToplevels {
    BiomeServer *server = nullptr;
    wlr_foreign_toplevel_manager_v1 *manager = nullptr;
    wlr_ext_foreign_toplevel_list_v1 *ext_list = nullptr;
    wl_listener capture_request = {};
};

// core/cursor.cpp: pointer event listeners and per-pointer bookkeeping.
struct BiomePointer {
    BiomeServer *server = nullptr;
    wl_listener motion = {};
    wl_listener motion_absolute = {};
    wl_listener button = {};
    wl_listener axis = {};
    wl_listener frame = {};
    wlr_pointer_gestures_v1 *gestures = nullptr;
    wl_listener swipe_begin = {};
    wl_listener swipe_update = {};
    wl_listener swipe_end = {};
    wl_listener pinch_begin = {};
    wl_listener pinch_update = {};
    wl_listener pinch_end = {};
    wl_listener hold_begin = {};
    wl_listener hold_end = {};
    wlr_cursor_shape_manager_v1 *shape_manager = nullptr;
    wl_listener request_set_shape = {};
    // Last named cursor set on wlr_cursor; empty while a client surface is shown.
    char xcursor_name[64] = {};
    // Non-null only during a client drag that has an icon.
    wlr_scene_tree *drag_icon = nullptr;
    wl_listener drag_icon_destroy = {};
    // Layout origin of the pointer-focused surface, so a button-held drag keeps
    // targeting it past its edge.
    double focus_origin_x = 0, focus_origin_y = 0;
    // Titlebar double-click detection; the toplevel is compare-only.
    uint32_t last_click_time = 0;
    BiomeToplevel *last_click_toplevel = nullptr;
};

// Interactive move/resize (core/cursor.cpp, desktop/toplevel.cpp).
struct BiomeGrab {
    BiomeCursorMode mode = BiomeCursorMode::Passthrough;
    BiomeToplevel *toplevel = nullptr;
    double x = 0, y = 0;
    wlr_box geobox = {};
    uint32_t resize_edges = 0;
    // Started from the window menu: no button is held, so the next press ends it.
    bool from_menu = false;
    bool swallow_release = false;
};

// Drag-to-edge snapping (desktop/toplevel.cpp). A zone activates after the
// pointer dwells in it; preview_zone None = hidden.
struct BiomeSnap {
    wlr_scene_buffer *preview = nullptr;
    SnapZone preview_zone = SnapZone::None;
    wlr_output *output = nullptr; // output of the active/pending zone
    wl_event_source *dwell_timer = nullptr;
    SnapZone pending = SnapZone::None;
    double anchor_x = 0, anchor_y = 0;
    double pending_x = 0, pending_y = 0;
};

// desktop/decoration_bridge.cpp: toplevels whose decoration button is
// hovered/pressed (dereferenced; cleared by clear_decoration_tracking).
struct BiomeDecorationState {
    BiomeToplevel *hovered = nullptr;
    BiomeToplevel *pressed = nullptr;
    wl_event_source *hold_timer = nullptr; // maximize press-and-hold
};

// core/pointer_constraints.cpp: relative-pointer + pointer-constraints.
struct BiomeConstraints {
    BiomeServer *server = nullptr;
    wlr_relative_pointer_manager_v1 *relative_manager = nullptr;
    wlr_pointer_constraints_v1 *manager = nullptr;
    wl_listener new_constraint = {};
    wl_listener focus_change = {};
    wlr_pointer_constraint_v1 *active = nullptr;
};

// core/input.cpp: device hotplug and seat requests.
struct BiomeInput {
    BiomeServer *server = nullptr;
    wl_listener new_input = {};
    wl_listener request_cursor = {};
    wl_listener request_set_selection = {};
    wl_listener request_set_primary_selection = {};
    wl_listener request_start_drag = {};
    wl_listener start_drag = {};
    wl_list keyboards = {};
};

// core/output.cpp: output hotplug and layout changes.
struct BiomeOutputEvents {
    BiomeServer *server = nullptr;
    wl_listener new_output = {};
    wl_listener layout_change = {};
};

// core/output_management.cpp: wlr-output-management.
struct BiomeOutputManagement {
    BiomeServer *server = nullptr;
    wlr_output_manager_v1 *manager = nullptr;
    wl_listener apply = {};
    wl_listener test = {};
    wl_event_source *publish_idle = nullptr;
    // Bumped per successful apply; an older OutputConfig::position_generation is stale.
    unsigned layout_generation = 0;
};

// core/output_power.cpp: wlr-output-power-management.
struct BiomeOutputPower {
    BiomeServer *server = nullptr;
    wlr_output_power_manager_v1 *manager = nullptr;
    wl_listener set_mode = {};
    // Connectors destroyed while powered off; an HPD bounce comes back off.
    std::vector<std::string> unplugged_while_off;
};

// core/gamma.cpp: wlr-gamma-control.
struct BiomeGamma {
    BiomeServer *server = nullptr;
    wlr_gamma_control_manager_v1 *manager = nullptr;
    wl_listener set_gamma = {};
};

// desktop/session_lock.cpp: ext-session-lock-v1.
struct BiomeSessionLock {
    BiomeServer *server = nullptr;
    wlr_session_lock_manager_v1 *manager = nullptr;
    wl_listener new_lock = {};
    // The current lock object; null after the client unlocks or dies (the
    // session stays locked then, see BiomeServer::session_locked).
    wlr_session_lock_v1 *active = nullptr;
    bool locked_sent = false; // wlr_session_lock_v1::locked_sent is private in 0.20
    wl_listener new_surface = {};
    wl_listener unlock = {};
    wl_listener destroy = {};
};

// desktop/xdg_activation.cpp. A token goes stale once a press reaches another
// client (press_recipient_changes); last_press_recipient is compare-only.
struct BiomeActivation {
    BiomeServer *server = nullptr;
    wlr_xdg_activation_v1 *manager = nullptr;
    wl_listener new_token = {};
    wl_listener request_activate = {};
    uint64_t press_recipient_changes = 0;
    wl_client *last_press_recipient = nullptr;
    // Honored activation for a popup not mapped yet; xdg_popup_map focuses it.
    wlr_surface *unmapped_popup = nullptr;
};

// desktop/idle.cpp: ext-idle-notify + idle-inhibit.
struct BiomeIdle {
    BiomeServer *server = nullptr;
    wlr_idle_notifier_v1 *notifier = nullptr;
    wlr_idle_inhibit_manager_v1 *inhibit_manager = nullptr;
    wl_listener new_inhibitor = {};
    bool inhibited = false; // last value sent to the notifier
};

// desktop/ext_workspace.cpp: handles[i] is workspace i.
struct BiomeExtWorkspace {
    BiomeServer *server = nullptr;
    wlr_ext_workspace_manager_v1 *manager = nullptr;
    wlr_ext_workspace_group_handle_v1 *group = nullptr;
    wl_listener commit = {};
    std::vector<wlr_ext_workspace_handle_v1 *> handles;
};

#define BIOME_LISTENER_STRUCT(T) \
    static_assert(std::is_standard_layout_v<T>, #T " holds listeners, so must be standard-layout")
BIOME_LISTENER_STRUCT(BiomeXdgShell);
BIOME_LISTENER_STRUCT(BiomeXwayland);
BIOME_LISTENER_STRUCT(BiomeLayerShell);
BIOME_LISTENER_STRUCT(BiomeForeignToplevels);
BIOME_LISTENER_STRUCT(BiomePointer);
BIOME_LISTENER_STRUCT(BiomeConstraints);
BIOME_LISTENER_STRUCT(BiomeInput);
BIOME_LISTENER_STRUCT(BiomeOutputEvents);
BIOME_LISTENER_STRUCT(BiomeOutputManagement);
BIOME_LISTENER_STRUCT(BiomeOutputPower);
BIOME_LISTENER_STRUCT(BiomeGamma);
BIOME_LISTENER_STRUCT(BiomeSessionLock);
BIOME_LISTENER_STRUCT(BiomeActivation);
BIOME_LISTENER_STRUCT(BiomeIdle);
BIOME_LISTENER_STRUCT(BiomeExtWorkspace);
#undef BIOME_LISTENER_STRUCT

struct BiomeServer {
    wl_display *display = nullptr;
    wlr_backend *backend = nullptr;
    wlr_session *session = nullptr; // null unless on a real DRM session
    wlr_renderer *renderer = nullptr;
    wlr_allocator *allocator = nullptr;
    wlr_linux_dmabuf_v1 *linux_dmabuf = nullptr; // null if the renderer can't import dmabufs
    wlr_scene *scene = nullptr;
    wlr_scene_output_layout *scene_layout = nullptr;

    // Fixed scene-layer stack (core/layers.cpp), bottom to top; this order is
    // the global stacking policy. osd holds compositor UI (switcher, menu,
    // drag icon); only session_lock is above it.
    struct {
        wlr_scene_tree *background = nullptr;
        wlr_scene_tree *bottom = nullptr;
        wlr_scene_tree *toplevels = nullptr;
        wlr_scene_tree *above = nullptr; // always-on-top windows
        wlr_scene_tree *top = nullptr;
        wlr_scene_tree *overlay = nullptr;
        wlr_scene_tree *fullscreen = nullptr;
        wlr_scene_tree *osd = nullptr;
        wlr_scene_tree *session_lock = nullptr;
    } layers;

    wl_list toplevels = {};
    wl_list unmanaged = {}; // BiomeUnmanaged (override-redirect X11)
    // The one toplevel with xdg "activated"; survives focus moving to a popup.
    BiomeToplevel *activated_toplevel = nullptr;
    int active_workspace = 0;
    int workspace_count = 4;
    // Set by ipc/workspace_bridge.cpp (null without a session bus). Call after
    // server->toplevels reflects the change: it snapshots the list.
    void (*window_workspaces_changed)(BiomeServer *server) = nullptr;

    wlr_cursor *cursor = nullptr;
    wlr_xcursor_manager *cursor_mgr = nullptr;
    wlr_seat *seat = nullptr;

    wlr_output_layout *output_layout = nullptr;
    wl_list outputs = {};
    // Biome.conf at startup, then each live apply, so a reconnecting connector
    // gets its live state back.
    std::unordered_map<std::string, OutputConfig> output_configs;

    // From lock acceptance until a real unlock; survives the lock client dying.
    bool session_locked = false;

    BiomeXdgShell xdg;
    BiomeXwayland xwayland;
    BiomeLayerShell layer;
    BiomeForeignToplevels ftl;
    BiomePointer pointer;
    BiomeGrab grab;
    BiomeSnap snap;
    BiomeDecorationState decor;
    BiomeConstraints constraints;
    BiomeInput input;
    BiomeOutputEvents output_events;
    BiomeOutputManagement output_mgmt;
    BiomeOutputPower output_power;
    BiomeGamma gamma;
    BiomeSessionLock lock;
    BiomeActivation activation;
    BiomeIdle idle;
    BiomeExtWorkspace ext_workspace;
    BiomeSwitcher switcher;
    struct BiomeKeys *keys = nullptr; // core/keybindings.cpp
    struct BiomeWindowMenu *window_menu = nullptr; // desktop/window_menu.cpp
};

struct BiomeOutput {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_output *wlr = nullptr;
    wl_listener frame = {};
    wl_listener request_state = {};
    wl_listener destroy = {};

    // Logically off (config or runtime change): not in output_layout. DPMS never re-lights it.
    bool disabled = false;
    // DPMS off (core/output_power.cpp): wlr disabled but still in the layout.
    bool powered_off = false;
    // One-shot retry of a failed power commit; target is !powered_off.
    wl_event_source *power_retry_timer = nullptr;
    // Last layout position; kept while disabled so it can be reported/restored.
    int layout_x = 0;
    int layout_y = 0;

    // Session lock (desktop/session_lock.cpp), created for every output so one
    // hotplugged mid-lock is blanked from its first frame. lock_rect is the
    // opaque backstop under the client's lock surface.
    wlr_scene_tree *lock_tree = nullptr;
    wlr_scene_rect *lock_rect = nullptr;
    wlr_session_lock_surface_v1 *lock_surface = nullptr; // reconfigured on resize
    bool pending_lock_frame = false; // no frame committed since the lock began

    // Per-output children of the layer-shell layers, at the output's layout position.
    wlr_scene_tree *layer_background = nullptr;
    wlr_scene_tree *layer_bottom = nullptr;
    wlr_scene_tree *layer_top = nullptr;
    wlr_scene_tree *layer_overlay = nullptr;
    // Output-local box minus exclusive zones (desktop/layer_shell.cpp).
    wlr_box usable_area = {};
    // Invisible frames left before pending layer-surface fade-ins start, all
    // at once. Absorbs the slow first flip after a scanout fade's new buffers.
    int fade_warmup_ticks = 0;
};

struct BiomeKeyboard {
    wl_list link = {};
    BiomeServer *server = nullptr;
    wlr_keyboard *wlr = nullptr;

    wl_listener modifiers = {};
    wl_listener key = {};
    wl_listener destroy = {};
};
