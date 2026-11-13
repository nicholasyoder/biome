// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/window_menu.h"

#include "core/cursor.h"
#include "decoration/menu.h"
#include "desktop/decoration_bridge.h"
#include "desktop/workspace.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace {

using biome_decoration::MenuEntry;
using biome_decoration::MenuItemRect;

enum ItemId {
    kMaximize = 1,
    kMinimize,
    kMove,
    kResize,
    kAlwaysOnTop,
    kWorkspace,
    kClose,
    kWorkspaceBase = 100, // + workspace index
    kSnapBase = 1000,     // + SnapZone
};

struct Panel {
    wlr_scene_buffer *buffer = nullptr;
    bool open = false;
    int x = 0, y = 0, width = 0, height = 0;
    int margin = 0; // transparent border inside width/height
    std::vector<MenuEntry> entries;
    std::vector<MenuItemRect> items;
    int hovered = -1;
    double scale = 1.0;
};

struct State {
    BiomeServer *server = nullptr;
    BiomeToplevel *target = nullptr;
    Panel panels[2];
    // The press that began a click was inside the menu, so its release may activate.
    bool armed = false;
    std::set<uint32_t> swallowed_keys;
};

State g;

bool inside(const Panel &p, double lx, double ly) {
    return p.open && lx >= p.x && ly >= p.y && lx < p.x + p.width && ly < p.y + p.height;
}

const MenuItemRect *item_at(const Panel &p, double lx, double ly) {
    if (!inside(p, lx, ly)) {
        return nullptr;
    }
    for (const MenuItemRect &r : p.items) {
        if (lx >= p.x + r.x && lx < p.x + r.x + r.width && ly >= p.y + r.y && ly < p.y + r.y + r.height) {
            return &r;
        }
    }
    return nullptr;
}

const MenuEntry *entry_for(const Panel &p, int id) {
    for (const MenuEntry &e : p.entries) {
        if (!e.separator && e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

std::vector<MenuEntry> main_entries(const BiomeToplevel *t) {
    auto item = [](int id, const char *label, bool enabled = true) {
        MenuEntry e;
        e.id = id;
        e.label = label;
        e.enabled = enabled;
        return e;
    };
    MenuEntry sep;
    sep.separator = true;

    bool normal = !t->fullscreen;
    std::vector<MenuEntry> entries;
    entries.push_back(item(kMaximize, t->maximized ? "Restore" : "Maximize", normal));
    entries.push_back(item(kMinimize, "Minimize"));
    entries.push_back(item(kMove, "Move", normal));
    entries.push_back(item(kResize, "Resize", normal && !t->maximized));
    entries.push_back(sep);
    MenuEntry top = item(kAlwaysOnTop, "Always on Top");
    top.checked = t->always_on_top;
    entries.push_back(top);
    MenuEntry ws = item(kWorkspace, "Move to Workspace");
    ws.submenu = true;
    entries.push_back(ws);
    entries.push_back(sep);
    entries.push_back(item(kClose, "Close"));
    return entries;
}

std::vector<MenuEntry> snap_entries(const BiomeToplevel *t) {
    std::vector<MenuEntry> entries;
    bool normal = !t->fullscreen;
    auto add = [&](int id, const char *label, bool checked = false) {
        MenuEntry e;
        e.id = id;
        e.label = label;
        e.enabled = normal;
        e.checked = checked;
        entries.push_back(e);
    };
    auto sep = [&] {
        MenuEntry e;
        e.separator = true;
        entries.push_back(e);
    };
    auto zone = [&](SnapZone z, const char *label) {
        add(kSnapBase + static_cast<int>(z), label, t->snap_zone == z);
    };
    if (t->maximized || t->snap_zone != SnapZone::None) {
        add(kMaximize, "Restore");
    } else {
        add(kMaximize, "Maximize");
    }
    sep();
    zone(SnapZone::Left, "Left Half");
    zone(SnapZone::Right, "Right Half");
    zone(SnapZone::TopHalf, "Top Half");
    zone(SnapZone::BottomHalf, "Bottom Half");
    sep();
    zone(SnapZone::TopLeft, "Top Left");
    zone(SnapZone::TopRight, "Top Right");
    zone(SnapZone::BottomLeft, "Bottom Left");
    zone(SnapZone::BottomRight, "Bottom Right");
    return entries;
}

std::vector<MenuEntry> workspace_entries(const BiomeToplevel *t) {
    std::vector<MenuEntry> entries;
    for (int i = 0; i < t->server->workspace_count; i++) {
        MenuEntry e;
        e.id = kWorkspaceBase + i;
        e.label = "Workspace " + std::to_string(i + 1);
        e.checked = i == t->workspace;
        e.enabled = i != t->workspace;
        entries.push_back(e);
    }
    return entries;
}

// Re-renders panel idx from its entries and hover state.
void render_panel(int idx) {
    Panel &p = g.panels[idx];
    biome_decoration::RenderedMenu menu = biome_decoration::render_menu(idx, p.entries, p.hovered, p.scale);
    p.width = menu.frame.logical_width;
    p.height = menu.frame.logical_height;
    p.margin = menu.margin;
    p.items = std::move(menu.items);
    if (!set_decoration_buffer(p.buffer, std::move(menu.frame))) {
        p.open = false;
        wlr_scene_node_set_enabled(&p.buffer->node, false);
        return;
    }
    wlr_scene_node_set_position(&p.buffer->node, p.x, p.y);
    wlr_scene_node_set_enabled(&p.buffer->node, true);
    wlr_scene_node_raise_to_top(&p.buffer->node);
}

void close_submenu() {
    Panel &sub = g.panels[1];
    if (!sub.open) {
        return;
    }
    sub.open = false;
    sub.hovered = -1;
    wlr_scene_node_set_enabled(&sub.buffer->node, false);
}

// Sets *x/*y (the panel origin) so its visible frame, with its top-left at
// (vis_x, vis_y), fits in box; flips above flip_y (visible bottom) if it overflows.
void place_visible(const wlr_box &box, const Panel &p, int vis_x, int vis_y, int flip_y, int *x, int *y) {
    int vw = p.width - 2 * p.margin;
    int vh = p.height - 2 * p.margin;
    if (vis_x + vw > box.x + box.width) {
        vis_x = box.x + box.width - vw;
    }
    if (vis_y + vh > box.y + box.height) {
        vis_y = flip_y - vh;
    }
    *x = std::max(vis_x, box.x) - p.margin;
    *y = std::max(vis_y, box.y) - p.margin;
}

wlr_output *output_at(double lx, double ly) {
    return wlr_output_layout_output_at(g.server->output_layout, lx, ly);
}

void open_submenu(int parent_id) {
    Panel &main = g.panels[0];
    const MenuItemRect *rect = nullptr;
    for (const MenuItemRect &r : main.items) {
        if (r.id == parent_id) {
            rect = &r;
        }
    }
    if (rect == nullptr || g.target == nullptr) {
        return;
    }
    Panel &sub = g.panels[1];
    sub.entries = workspace_entries(g.target);
    sub.hovered = -1;
    sub.scale = main.scale;
    // Measured first (position doesn't affect size), then placed.
    biome_decoration::RenderedMenu measured = biome_decoration::render_menu(1, sub.entries, -1, sub.scale);
    sub.width = measured.frame.logical_width;
    sub.height = measured.frame.logical_height;

    sub.margin = measured.margin;

    wlr_output *output = output_at(main.x, main.y);
    wlr_box box = output_target_box(g.server, output);
    // Visible frames touch (right of the parent, left if it would overflow),
    // first items aligned.
    int first_y = measured.items.empty() ? 0 : measured.items.front().y;
    int x = main.x + main.width - main.margin - sub.margin;
    int y = main.y + rect->y - first_y;
    if (!wlr_box_empty(&box)) {
        if (x + sub.width - sub.margin > box.x + box.width) {
            x = main.x + main.margin + sub.margin - sub.width;
        }
        int ox, oy;
        place_visible(box, sub, x + sub.margin, y + sub.margin, y + sub.margin + sub.height - 2 * sub.margin, &ox, &oy);
        x = ox;
        y = oy;
    }
    sub.x = x;
    sub.y = y;
    sub.open = true;
    render_panel(1);
    // Parent keeps its highlight while the submenu is open.
    main.hovered = parent_id;
    render_panel(0);
    wlr_scene_node_raise_to_top(&sub.buffer->node);
}

void set_hover(int idx, int id) {
    Panel &p = g.panels[idx];
    if (p.hovered == id) {
        return;
    }
    p.hovered = id;
    render_panel(idx);
}

void begin_menu_grab(BiomeToplevel *t, BiomeCursorMode mode) {
    BiomeServer *server = g.server;
    wlr_box frame;
    toplevel_get_frame_box(t, &frame);
    // Warped so the grab's offset is sensible whatever the menu was opened from.
    if (mode == BiomeCursorMode::Move) {
        wlr_cursor_warp(server->cursor, nullptr, frame.x + frame.width / 2.0,
            frame.y + decoration_titlebar_height(t, t->maximized) / 2.0);
        begin_interactive(t, mode, 0, false);
    } else {
        wlr_cursor_warp(server->cursor, nullptr, frame.x + frame.width - 1.0, frame.y + frame.height - 1.0);
        begin_interactive(t, mode, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, false);
    }
    server->grab_from_menu = server->cursor_mode != BiomeCursorMode::Passthrough;
}

void activate(int id) {
    BiomeToplevel *t = g.target;
    BiomeServer *server = g.server;
    if (t == nullptr) {
        return;
    }
    if (id >= kSnapBase) {
        window_menu_close(server);
        set_toplevel_snapped(t, static_cast<SnapZone>(id - kSnapBase));
        return;
    }
    if (id >= kWorkspaceBase) {
        window_menu_close(server);
        move_toplevel_to_workspace(t, id - kWorkspaceBase);
        return;
    }
    window_menu_close(server);
    switch (id) {
    case kMaximize:
        if (t->snap_zone != SnapZone::None) {
            set_toplevel_snapped(t, SnapZone::None);
        } else {
            set_toplevel_maximized(t, !t->maximized);
        }
        break;
    case kMinimize:
        set_toplevel_minimized(t, true);
        break;
    case kMove:
        begin_menu_grab(t, BiomeCursorMode::Move);
        break;
    case kResize:
        begin_menu_grab(t, BiomeCursorMode::Resize);
        break;
    case kAlwaysOnTop:
        set_toplevel_always_on_top(t, !t->always_on_top);
        break;
    case kClose:
        close_toplevel(t);
        break;
    default:
        break;
    }
}

// Activating or descending into the item under a pointer/keyboard selection.
void activate_item(int panel_idx, int id) {
    const MenuEntry *entry = entry_for(g.panels[panel_idx], id);
    if (entry == nullptr || !entry->enabled) {
        return;
    }
    if (entry->submenu) {
        open_submenu(id);
        return;
    }
    activate(id);
}

// Next enabled item from the hovered one in direction dir (wraps).
int step_hover(const Panel &p, int dir) {
    std::vector<int> ids;
    for (const MenuItemRect &r : p.items) {
        const MenuEntry *e = entry_for(p, r.id);
        if (e != nullptr && e->enabled) {
            ids.push_back(r.id);
        }
    }
    if (ids.empty()) {
        return -1;
    }
    auto it = std::find(ids.begin(), ids.end(), p.hovered);
    int n = static_cast<int>(ids.size());
    int pos = it == ids.end() ? (dir > 0 ? 0 : n - 1) : (static_cast<int>(it - ids.begin()) + dir + n) % n;
    return ids[static_cast<size_t>(pos)];
}

bool is_modifier(xkb_keysym_t sym) {
    return sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R;
}

} // namespace

void window_menu_init(BiomeServer *server) {
    g.server = server;
    for (Panel &p : g.panels) {
        // Children of the scene root, like the switcher: not owned by any window.
        p.buffer = wlr_scene_buffer_create(&server->scene->tree, nullptr);
        wlr_scene_node_set_enabled(&p.buffer->node, false);
    }
}

bool window_menu_active() {
    return g.panels[0].open;
}

void window_menu_close(BiomeServer *server) {
    (void)server;
    close_submenu();
    Panel &main = g.panels[0];
    if (main.open) {
        main.open = false;
        main.hovered = -1;
        wlr_scene_node_set_enabled(&main.buffer->node, false);
    }
    g.target = nullptr;
    g.armed = false;
}

static void open_menu(BiomeToplevel *toplevel, int x, int y, int anchor_height, bool snap, bool armed) {
    BiomeServer *server = toplevel->server;
    if (server->session_locked || g.server == nullptr) {
        return;
    }
    window_menu_close(server);

    wlr_output *output = output_at(x, y);
    Panel &main = g.panels[0];
    g.target = toplevel;
    main.entries = snap ? snap_entries(toplevel) : main_entries(toplevel);
    main.hovered = -1;
    main.scale = output != nullptr ? output->scale : 1.0;
    biome_decoration::RenderedMenu measured = biome_decoration::render_menu(0, main.entries, -1, main.scale);
    main.width = measured.frame.logical_width;
    main.height = measured.frame.logical_height;
    main.margin = measured.margin;

    // (x, y) is where the visible frame's corner goes.
    wlr_box box = output_target_box(server, output);
    if (!wlr_box_empty(&box)) {
        place_visible(box, main, x, y, y - anchor_height, &main.x, &main.y);
    } else {
        main.x = x - main.margin;
        main.y = y - main.margin;
    }
    main.open = true;
    render_panel(0);
    if (!main.open) {
        g.target = nullptr;
        return;
    }
    // Nothing under the pointer is a client's any more.
    wlr_seat_pointer_notify_clear_focus(server->seat);
    window_menu_motion(server, server->cursor->x, server->cursor->y);
    g.armed = armed;
}

void window_menu_open(BiomeToplevel *toplevel, int x, int y, int anchor_height) {
    open_menu(toplevel, x, y, anchor_height, false, false);
}

void window_menu_open_snap(BiomeToplevel *toplevel, int x, int y, int anchor_height, bool armed) {
    open_menu(toplevel, x, y, anchor_height, true, armed);
}

void window_menu_remove_toplevel(BiomeServer *server, BiomeToplevel *toplevel) {
    if (g.target == toplevel) {
        window_menu_close(server);
    }
}

void window_menu_motion(BiomeServer *server, double lx, double ly) {
    wlr_seat_pointer_notify_clear_focus(server->seat);
    wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
    Panel &main = g.panels[0];
    Panel &sub = g.panels[1];

    if (const MenuItemRect *r = item_at(sub, lx, ly)) {
        set_hover(1, r->id);
        return;
    }
    if (inside(sub, lx, ly)) {
        set_hover(1, -1);
        return;
    }
    if (const MenuItemRect *r = item_at(main, lx, ly)) {
        const MenuEntry *entry = entry_for(main, r->id);
        if (entry != nullptr && entry->enabled && entry->submenu) {
            if (!sub.open) {
                open_submenu(r->id);
            }
            return;
        }
        close_submenu();
        set_hover(0, entry != nullptr && entry->enabled ? r->id : -1);
        return;
    }
    // Over a gap or outside: the submenu's parent keeps its highlight.
    if (inside(main, lx, ly) && !sub.open) {
        set_hover(0, -1);
    }
}

bool window_menu_button(BiomeServer *server, uint32_t time_msec, uint32_t button, bool pressed) {
    double lx = server->cursor->x, ly = server->cursor->y;
    Panel &main = g.panels[0];
    Panel &sub = g.panels[1];

    if (!pressed) {
        // The opening click's press reached the seat; its release must too.
        if (server->seat->pointer_state.button_count > 0) {
            wlr_seat_pointer_notify_button(server->seat, time_msec, button, WL_POINTER_BUTTON_STATE_RELEASED);
        }
        if (!g.armed) {
            return true;
        }
        g.armed = false;
        const MenuItemRect *r = item_at(sub, lx, ly);
        int idx = 1;
        if (r == nullptr) {
            r = item_at(main, lx, ly);
            idx = 0;
        }
        if (r != nullptr) {
            activate_item(idx, r->id);
        }
        return true;
    }

    if (!inside(main, lx, ly) && !inside(sub, lx, ly)) {
        window_menu_close(server);
        return true;
    }
    g.armed = true;
    return true;
}

bool window_menu_key(BiomeServer *server, uint32_t keycode, xkb_keysym_t sym, bool pressed) {
    if (!pressed) {
        return g.swallowed_keys.erase(keycode) > 0;
    }
    if (!window_menu_active()) {
        return false;
    }
    if (is_modifier(sym)) {
        return false;
    }
    g.swallowed_keys.insert(keycode);

    Panel &main = g.panels[0];
    Panel &sub = g.panels[1];
    int idx = sub.open ? 1 : 0;
    Panel &cur = g.panels[idx];
    switch (sym) {
    case XKB_KEY_Escape:
        if (sub.open) {
            close_submenu();
        } else {
            window_menu_close(server);
        }
        break;
    case XKB_KEY_Down:
        set_hover(idx, step_hover(cur, 1));
        break;
    case XKB_KEY_Up:
        set_hover(idx, step_hover(cur, -1));
        break;
    case XKB_KEY_Right: {
        const MenuEntry *e = entry_for(main, main.hovered);
        if (!sub.open && e != nullptr && e->submenu && e->enabled) {
            open_submenu(main.hovered);
            set_hover(1, step_hover(sub, 1));
        }
        break;
    }
    case XKB_KEY_Left:
        close_submenu();
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        if (cur.hovered >= 0) {
            const MenuEntry *e = entry_for(cur, cur.hovered);
            if (e != nullptr && e->submenu && !sub.open) {
                open_submenu(cur.hovered);
                set_hover(1, step_hover(sub, 1));
            } else {
                activate_item(idx, cur.hovered);
            }
        }
        break;
    default:
        break;
    }
    return true;
}
