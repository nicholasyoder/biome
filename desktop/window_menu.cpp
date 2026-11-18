// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/window_menu.h"

#include "core/cursor.h"
#include "decoration/menu.h"
#include "desktop/decoration_bridge.h"
#include "desktop/qt_buffer.h"
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


} // namespace

struct BiomeWindowMenu {
    BiomeServer *server = nullptr;
    BiomeToplevel *target = nullptr;
    Panel panels[2];
    // The press that began a click was inside the menu, so its release may activate.
    bool armed = false;
    std::set<uint32_t> swallowed_keys;
};


namespace {

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
    auto add = [&](int id, const char *label, bool checked = false, MenuEntry::Zone zone = {0, 0, 1, 1}) {
        MenuEntry e;
        e.id = id;
        e.label = label;
        e.enabled = normal;
        e.checked = checked;
        e.zone = zone;
        entries.push_back(e);
    };
    auto sep = [&] {
        MenuEntry e;
        e.separator = true;
        entries.push_back(e);
    };
    auto zone = [&](SnapZone z, const char *label, MenuEntry::Zone rect) {
        add(kSnapBase + static_cast<int>(z), label, t->snap_zone == z, rect);
    };
    if (t->maximized || t->snap_zone != SnapZone::None) {
        add(kMaximize, "Restore");
    } else {
        add(kMaximize, "Maximize");
    }
    sep();
    zone(SnapZone::Left, "Left Half", {0, 0, .5, 1});
    zone(SnapZone::Right, "Right Half", {.5, 0, .5, 1});
    zone(SnapZone::TopHalf, "Top Half", {0, 0, 1, .5});
    zone(SnapZone::BottomHalf, "Bottom Half", {0, .5, 1, .5});
    sep();
    zone(SnapZone::TopLeft, "Top Left", {0, 0, .5, .5});
    zone(SnapZone::TopRight, "Top Right", {.5, 0, .5, .5});
    zone(SnapZone::BottomLeft, "Bottom Left", {0, .5, .5, .5});
    zone(SnapZone::BottomRight, "Bottom Right", {.5, .5, .5, .5});
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
void render_panel(BiomeWindowMenu &g, int idx) {
    Panel &p = g.panels[idx];
    biome_decoration::RenderedMenu menu = biome_decoration::render_menu(idx, p.entries, p.hovered, p.scale);
    p.width = menu.frame.logical_width;
    p.height = menu.frame.logical_height;
    p.margin = menu.margin;
    p.items = std::move(menu.items);
    if (!osd_node_show(p.buffer, std::move(menu.frame), p.x, p.y)) {
        p.open = false;
    }
}

void close_submenu(BiomeWindowMenu &g) {
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

wlr_output *output_at(BiomeWindowMenu &g, double lx, double ly) {
    return wlr_output_layout_output_at(g.server->output_layout, lx, ly);
}

void open_submenu(BiomeWindowMenu &g, int parent_id) {
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

    wlr_output *output = output_at(g, main.x, main.y);
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
    render_panel(g, 1);
    // Parent keeps its highlight while the submenu is open.
    main.hovered = parent_id;
    render_panel(g, 0);
    wlr_scene_node_raise_to_top(&sub.buffer->node);
}

void set_hover(BiomeWindowMenu &g, int idx, int id) {
    Panel &p = g.panels[idx];
    if (p.hovered == id) {
        return;
    }
    p.hovered = id;
    render_panel(g, idx);
}

void begin_menu_grab(BiomeWindowMenu &g, BiomeToplevel *t, BiomeCursorMode mode) {
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
    server->grab.from_menu = server->grab.mode != BiomeCursorMode::Passthrough;
}

void activate(BiomeWindowMenu &g, int id) {
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
        begin_menu_grab(g, t, BiomeCursorMode::Move);
        break;
    case kResize:
        begin_menu_grab(g, t, BiomeCursorMode::Resize);
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
void activate_item(BiomeWindowMenu &g, int panel_idx, int id) {
    const MenuEntry *entry = entry_for(g.panels[panel_idx], id);
    if (entry == nullptr || !entry->enabled) {
        return;
    }
    if (entry->submenu) {
        open_submenu(g, id);
        return;
    }
    activate(g, id);
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
    server->window_menu = new BiomeWindowMenu;
    BiomeWindowMenu &g = *server->window_menu;
    g.server = server;
    for (Panel &p : g.panels) {
        p.buffer = wlr_scene_buffer_create(server->layers.osd, nullptr);
        wlr_scene_node_set_enabled(&p.buffer->node, false);
    }
}

void window_menu_finish(BiomeServer *server) {
    delete server->window_menu;
    server->window_menu = nullptr;
}

bool window_menu_active(BiomeServer *server) {
    return server->window_menu->panels[0].open;
}

void window_menu_close(BiomeServer *server) {
    BiomeWindowMenu &g = *server->window_menu;
    close_submenu(g);
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
    if (server->session_locked || !toplevel->placed) {
        return;
    }
    window_menu_close(server);
    BiomeWindowMenu &g = *server->window_menu;

    wlr_output *output = output_at(g, x, y);
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
    render_panel(g, 0);
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
    BiomeWindowMenu &g = *server->window_menu;
    if (g.target == toplevel) {
        window_menu_close(server);
    }
}

void window_menu_motion(BiomeServer *server, double lx, double ly) {
    BiomeWindowMenu &g = *server->window_menu;
    wlr_seat_pointer_notify_clear_focus(server->seat);
    cursor_set_xcursor(server, "default");
    Panel &main = g.panels[0];
    Panel &sub = g.panels[1];

    if (const MenuItemRect *r = item_at(sub, lx, ly)) {
        set_hover(g, 1, r->id);
        return;
    }
    if (inside(sub, lx, ly)) {
        set_hover(g, 1, -1);
        return;
    }
    if (const MenuItemRect *r = item_at(main, lx, ly)) {
        const MenuEntry *entry = entry_for(main, r->id);
        if (entry != nullptr && entry->enabled && entry->submenu) {
            if (!sub.open) {
                open_submenu(g, r->id);
            }
            return;
        }
        close_submenu(g);
        set_hover(g, 0, entry != nullptr && entry->enabled ? r->id : -1);
        return;
    }
    // Over a gap or outside: the submenu's parent keeps its highlight.
    if (inside(main, lx, ly) && !sub.open) {
        set_hover(g, 0, -1);
    }
}

bool window_menu_button(BiomeServer *server, uint32_t time_msec, uint32_t button, bool pressed) {
    BiomeWindowMenu &g = *server->window_menu;
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
            activate_item(g, idx, r->id);
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
    BiomeWindowMenu &g = *server->window_menu;
    if (!pressed) {
        return g.swallowed_keys.erase(keycode) > 0;
    }
    if (!window_menu_active(server)) {
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
            close_submenu(g);
        } else {
            window_menu_close(server);
        }
        break;
    case XKB_KEY_Down:
        set_hover(g, idx, step_hover(cur, 1));
        break;
    case XKB_KEY_Up:
        set_hover(g, idx, step_hover(cur, -1));
        break;
    case XKB_KEY_Right: {
        const MenuEntry *e = entry_for(main, main.hovered);
        if (!sub.open && e != nullptr && e->submenu && e->enabled) {
            open_submenu(g, main.hovered);
            set_hover(g, 1, step_hover(sub, 1));
        }
        break;
    }
    case XKB_KEY_Left:
        close_submenu(g);
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
    case XKB_KEY_space:
        if (cur.hovered >= 0) {
            const MenuEntry *e = entry_for(cur, cur.hovered);
            if (e != nullptr && e->submenu && !sub.open) {
                open_submenu(g, cur.hovered);
                set_hover(g, 1, step_hover(sub, 1));
            } else {
                activate_item(g, idx, cur.hovered);
            }
        }
        break;
    default:
        break;
    }
    return true;
}
