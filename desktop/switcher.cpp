// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/switcher.h"

#include "decoration/switcher.h"
#include "decoration/switcher_highlight.h"
#include "desktop/decoration_bridge.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

biome_decoration::SwitcherEntry switcher_entry_for(BiomeToplevel *pos) {
    const char *title = pos->type == BiomeToplevelType::Xdg
        ? pos->xdg_toplevel->title
        : pos->xwayland_surface->title;
    const char *app_id = pos->type == BiomeToplevelType::Xdg
        ? pos->xdg_toplevel->app_id
        : pos->xwayland_surface->class_;
    std::string label;
    if (title != nullptr && title[0] != '\0') {
        label = title;
    } else if (app_id != nullptr && app_id[0] != '\0') {
        label = app_id;
    } else {
        label = "(untitled)";
    }
    return {label, pos->icon, pos->urgent};
}

void hide_overlay(BiomeServer *server) {
    wlr_scene_node_set_enabled(&server->switcher.panel->node, false);
    wlr_scene_node_set_enabled(&server->switcher.highlight->node, false);
}

// Outlines the selected window's frame without raising or focusing it. Hidden
// if the selection is minimized.
void update_highlight(BiomeServer *server, BiomeToplevel *selected) {
    wlr_scene_buffer *highlight = server->switcher.highlight;
    bool visible = selected->placed && selected->scene_tree->node.enabled;
    if (!visible) {
        wlr_scene_node_set_enabled(&highlight->node, false);
        return;
    }

    wlr_box box;
    toplevel_get_frame_box(selected, &box);
    wlr_buffer *buffer = create_decoration_buffer(
        biome_decoration::render_switcher_highlight(box.width, box.height));
    if (buffer == nullptr) {
        wlr_scene_node_set_enabled(&highlight->node, false);
        return;
    }
    wlr_scene_buffer_set_buffer(highlight, buffer);
    wlr_buffer_drop(buffer);
    wlr_scene_node_set_position(&highlight->node, box.x, box.y);
    wlr_scene_node_set_enabled(&highlight->node, true);
    wlr_scene_node_raise_to_top(&highlight->node);
}

void update_overlay(BiomeServer *server) {
    BiomeSwitcher &switcher = server->switcher;
    if (!switcher.active) {
        hide_overlay(server);
        return;
    }

    std::vector<biome_decoration::SwitcherEntry> entries;
    for (BiomeToplevel *pos : switcher.order) {
        entries.push_back(switcher_entry_for(pos));
    }
    biome_decoration::RenderedFrame frame = biome_decoration::render_switcher(entries, switcher.index);
    int width = frame.width;
    int height = frame.height;
    wlr_buffer *buffer = create_decoration_buffer(std::move(frame));
    if (buffer == nullptr) {
        hide_overlay(server);
        return;
    }
    wlr_scene_buffer_set_buffer(switcher.panel, buffer);
    wlr_buffer_drop(buffer);

    // Center on the output under the cursor; the layout box's center often
    // lands on a seam between outputs.
    wlr_output *wlr_output = wlr_output_layout_output_at(
        server->output_layout, server->cursor->x, server->cursor->y);
    wlr_box target = output_target_box(server, wlr_output);
    if (wlr_box_empty(&target)) {
        hide_overlay(server);
        return;
    }
    wlr_scene_node_set_position(&switcher.panel->node,
        target.x + (target.width - width) / 2,
        target.y + (target.height - height) / 2);
    wlr_scene_node_set_enabled(&switcher.panel->node, true);

    // Panel raised last so a highlight box never occludes it.
    update_highlight(server, switcher.order[static_cast<size_t>(switcher.index)]);
    wlr_scene_node_raise_to_top(&switcher.panel->node);
}

void close_switcher(BiomeServer *server) {
    server->switcher.active = false;
    server->switcher.order.clear();
    server->switcher.index = 0;
    update_overlay(server);
}

} // namespace

void switcher_init(BiomeServer *server) {
    // Children of the scene root: not owned by any one window.
    server->switcher.panel = wlr_scene_buffer_create(&server->scene->tree, nullptr);
    server->switcher.highlight = wlr_scene_buffer_create(&server->scene->tree, nullptr);
    hide_overlay(server);
}

void switcher_cycle(BiomeServer *server, bool reverse) {
    BiomeSwitcher &switcher = server->switcher;
    // Snapshot once per hold so the list doesn't reshuffle while cycling.
    if (!switcher.active) {
        switcher.order.clear();
        BiomeToplevel *pos;
        wl_list_for_each(pos, &server->toplevels, link) {
            if (pos->workspace == server->active_workspace) {
                switcher.order.push_back(pos);
            }
        }
        if (switcher.order.empty()) {
            return;
        }
        switcher.index = 0;
        switcher.active = true;
    }
    int count = static_cast<int>(switcher.order.size());
    switcher.index = (switcher.index + (reverse ? -1 : 1) + count) % count;
    update_overlay(server);
}

void switcher_commit(BiomeServer *server) {
    BiomeSwitcher &switcher = server->switcher;
    if (!switcher.active) {
        return;
    }
    BiomeToplevel *target = switcher.order[static_cast<size_t>(switcher.index)];
    if (target->minimized) {
        set_toplevel_minimized(target, false);
    }
    focus_toplevel(target);
    close_switcher(server);
}

void switcher_cancel(BiomeServer *server) {
    if (server->switcher.active) {
        close_switcher(server);
    }
}

void switcher_remove_toplevel(BiomeServer *server, BiomeToplevel *toplevel) {
    BiomeSwitcher &switcher = server->switcher;
    auto it = std::find(switcher.order.begin(), switcher.order.end(), toplevel);
    if (it == switcher.order.end()) {
        return;
    }
    auto erased_index = it - switcher.order.begin();
    switcher.order.erase(it);

    if (switcher.order.empty()) {
        close_switcher(server);
        return;
    }
    if (erased_index < switcher.index) {
        switcher.index--;
    }
    int count = static_cast<int>(switcher.order.size());
    switcher.index = ((switcher.index % count) + count) % count;
    update_overlay(server);
}
