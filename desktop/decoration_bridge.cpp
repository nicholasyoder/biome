// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/decoration_bridge.h"

#include "core/cursor.h"
#include "decoration/renderer.h"
#include "decoration/switcher.h"
#include "decoration/theme.h"

#include <drm_fourcc.h>
#include <string>
#include <utility>
#include <vector>

void decoration_bridge_init(BiomeServer *server) {
    // Alt-Tab switcher overlay - created once, hidden until update_switcher_
    // overlay has something to show. A direct child of the scene root (not
    // any toplevel's) since it isn't owned by a specific window.
    server->switcher_buffer = wlr_scene_buffer_create(&server->scene->tree, nullptr);
    wlr_scene_node_set_enabled(&server->switcher_buffer->node, false);
}

int decoration_border_width(bool maximized) {
    biome_decoration::decoration_frame()->setMaximizedState(maximized);
    return biome_decoration::decoration_frame()->borderWidth();
}

int decoration_titlebar_height(bool maximized) {
    biome_decoration::decoration_frame()->setMaximizedState(maximized);
    return biome_decoration::decoration_frame()->titlebarHeight();
}

int decoration_border_right_width(bool maximized) {
    biome_decoration::decoration_frame()->setMaximizedState(maximized);
    return biome_decoration::decoration_frame()->rightBorderWidth();
}

int decoration_border_bottom_height(bool maximized) {
    biome_decoration::decoration_frame()->setMaximizedState(maximized);
    return biome_decoration::decoration_frame()->bottomBorderHeight();
}

// --- Decoration buffer: wraps a decoration/renderer.h RenderedFrame in a
// minimal software wlr_buffer, so it can be handed to
// wlr_scene_buffer_set_buffer like any other buffer-backed scene node. ---

struct BiomeDecorationBuffer {
    wlr_buffer base = {};
    std::vector<uint8_t> pixels;
    int stride = 0;
};

static void decoration_buffer_destroy(wlr_buffer *buf) {
    BiomeDecorationBuffer *buffer = wl_container_of(buf, buffer, base);
    delete buffer;
}

static bool decoration_buffer_begin_data_ptr_access(wlr_buffer *buf,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    BiomeDecorationBuffer *buffer = wl_container_of(buf, buffer, base);
    *data = buffer->pixels.data();
    *format = DRM_FORMAT_ARGB8888;
    *stride = static_cast<size_t>(buffer->stride);
    return true;
}

static void decoration_buffer_end_data_ptr_access(wlr_buffer *buf) {
    (void)buf;
}

// wlr_buffer_impl is a plain field-order aggregate (destroy, get_dmabuf,
// get_shm, begin_data_ptr_access, end_data_ptr_access) - see
// wlr/interfaces/wlr_buffer.h. This buffer is CPU-only pixel data, so
// get_dmabuf/get_shm stay null.
static const wlr_buffer_impl kDecorationBufferImpl = {
    decoration_buffer_destroy,
    nullptr,
    nullptr,
    decoration_buffer_begin_data_ptr_access,
    decoration_buffer_end_data_ptr_access,
};

// Takes ownership of frame's pixels and wraps them in a new wlr_buffer
// (refcount 1, per wlr_buffer_init's contract - the caller must drop it
// once done, e.g. right after handing it to wlr_scene_buffer_set_buffer).
// Returns nullptr for an empty frame (zero content size).
static wlr_buffer *create_decoration_buffer(biome_decoration::RenderedFrame &&frame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return nullptr;
    }
    auto *buffer = new BiomeDecorationBuffer();
    buffer->stride = frame.stride;
    buffer->pixels = std::move(frame.pixels);
    wlr_buffer_init(&buffer->base, &kDecorationBufferImpl, frame.width, frame.height);
    return &buffer->base;
}

void create_toplevel_decoration(BiomeToplevel *toplevel) {
    toplevel->decoration_buffer = wlr_scene_buffer_create(toplevel->scene_tree, nullptr);
    wlr_scene_node_set_position(&toplevel->decoration_buffer->node, 0, 0);
}

void render_toplevel_decoration(BiomeToplevel *toplevel) {
    if (toplevel == nullptr || toplevel->decoration_buffer == nullptr || !toplevel->placed) {
        return;
    }
    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;

    const char *title = toplevel->type == BiomeToplevelType::Xdg
        ? toplevel->xdg_toplevel->title
        : toplevel->xwayland_surface->title;

    biome_decoration::RenderedFrame frame = biome_decoration::render_decoration(width, height,
        toplevel->focused, toplevel->maximized, title, toplevel->icon,
        toplevel->hovered_region, toplevel->pressed_region);
    wlr_buffer *buffer = create_decoration_buffer(std::move(frame));
    if (buffer == nullptr) {
        return;
    }
    wlr_scene_buffer_set_buffer(toplevel->decoration_buffer, buffer);
    wlr_buffer_drop(buffer);

    // Re-syncs content_tree to this same render's border/titlebar metrics,
    // rather than trusting the one-time snapshot taken at creation - those
    // metrics come from shared, mutable global state (decoration_frame()),
    // not something guaranteed stable between then and now.
    if (toplevel->content_tree != nullptr) {
        wlr_scene_node_set_position(&toplevel->content_tree->node,
            decoration_border_width(toplevel->maximized), decoration_titlebar_height(toplevel->maximized));
    }
}

void update_switcher_overlay(BiomeServer *server) {
    if (!server->switcher_active || wl_list_length(&server->toplevels) < 2) {
        if (server->switcher_buffer != nullptr) {
            wlr_scene_node_set_enabled(&server->switcher_buffer->node, false);
        }
        return;
    }

    std::vector<biome_decoration::SwitcherEntry> entries;
    BiomeToplevel *pos;
    wl_list_for_each(pos, &server->toplevels, link) {
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
        entries.push_back({label, pos->icon});
    }

    // server->toplevels is MRU-ordered (focus_toplevel keeps the head as
    // the most-recently-focused), so the currently-focused window - the one
    // Tab just landed on - is always entry 0.
    biome_decoration::RenderedFrame frame = biome_decoration::render_switcher(entries, 0);
    wlr_buffer *buffer = create_decoration_buffer(std::move(frame));
    if (buffer == nullptr) {
        wlr_scene_node_set_enabled(&server->switcher_buffer->node, false);
        return;
    }
    wlr_scene_buffer_set_buffer(server->switcher_buffer, buffer);
    wlr_buffer_drop(buffer);

    wlr_box layout_box;
    wlr_output_layout_get_box(server->output_layout, nullptr, &layout_box);
    wlr_scene_node_set_position(&server->switcher_buffer->node,
        layout_box.x + (layout_box.width - frame.width) / 2,
        layout_box.y + (layout_box.height - frame.height) / 2);
    wlr_scene_node_set_enabled(&server->switcher_buffer->node, true);
    wlr_scene_node_raise_to_top(&server->switcher_buffer->node);
}

BiomeToplevel *decoration_toplevel_at(
        BiomeServer *server, double lx, double ly, biome_decoration::Region *out_region) {
    double sx, sy;
    wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, &sx, &sy);
    if (node == nullptr || node->type != WLR_SCENE_NODE_BUFFER) {
        return nullptr;
    }
    wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    if (wlr_scene_surface_try_from_buffer(scene_buffer) != nullptr) {
        // A real client surface, not our decoration.
        return nullptr;
    }

    wlr_scene_tree *tree = node->parent;
    while (tree != nullptr && tree->node.data == nullptr) {
        tree = tree->node.parent;
    }
    BiomeToplevel *toplevel = tree ? static_cast<BiomeToplevel *>(tree->node.data) : nullptr;
    if (toplevel == nullptr) {
        return nullptr;
    }

    wlr_box geo;
    toplevel_get_geometry(toplevel, &geo);
    int width = geo.width > 0 ? geo.width : 0;
    int height = geo.height > 0 ? geo.height : 0;
    biome_decoration::Region region = biome_decoration::decoration_frame()->hitTest(
        static_cast<int>(sx), static_cast<int>(sy), width, height, toplevel->maximized);
    if (region == biome_decoration::Region::None) {
        return nullptr;
    }
    *out_region = region;
    return toplevel;
}

static bool is_button_region(biome_decoration::Region region) {
    using biome_decoration::Region;
    return region == Region::ButtonMinimize || region == Region::ButtonMaximize
        || region == Region::ButtonClose;
}

void update_decoration_hover(BiomeServer *server, BiomeToplevel *toplevel,
        biome_decoration::Region region) {
    BiomeToplevel *new_hovered = is_button_region(region) ? toplevel : nullptr;
    biome_decoration::Region new_region =
        new_hovered != nullptr ? region : biome_decoration::Region::None;

    BiomeToplevel *old_hovered = server->hovered_decoration_toplevel;
    if (old_hovered == new_hovered && (old_hovered == nullptr || old_hovered->hovered_region == new_region)) {
        return;
    }

    if (old_hovered != nullptr && old_hovered != new_hovered) {
        old_hovered->hovered_region = biome_decoration::Region::None;
        render_toplevel_decoration(old_hovered);
    }
    if (new_hovered != nullptr) {
        new_hovered->hovered_region = new_region;
        render_toplevel_decoration(new_hovered);
    }
    server->hovered_decoration_toplevel = new_hovered;
}

void set_decoration_pressed(BiomeServer *server, BiomeToplevel *toplevel,
        biome_decoration::Region region) {
    BiomeToplevel *new_pressed = is_button_region(region) ? toplevel : nullptr;
    biome_decoration::Region new_region =
        new_pressed != nullptr ? region : biome_decoration::Region::None;

    BiomeToplevel *old_pressed = server->pressed_decoration_toplevel;
    if (old_pressed == new_pressed && (old_pressed == nullptr || old_pressed->pressed_region == new_region)) {
        return;
    }

    if (old_pressed != nullptr && old_pressed != new_pressed) {
        old_pressed->pressed_region = biome_decoration::Region::None;
        render_toplevel_decoration(old_pressed);
    }
    if (new_pressed != nullptr) {
        new_pressed->pressed_region = new_region;
        render_toplevel_decoration(new_pressed);
    }
    server->pressed_decoration_toplevel = new_pressed;
}

void clear_decoration_tracking(BiomeServer *server, BiomeToplevel *toplevel) {
    if (server->hovered_decoration_toplevel == toplevel) {
        server->hovered_decoration_toplevel = nullptr;
    }
    if (server->pressed_decoration_toplevel == toplevel) {
        server->pressed_decoration_toplevel = nullptr;
    }
}

const char *resize_cursor_name(biome_decoration::Region region) {
    using biome_decoration::Region;
    switch (region) {
    case Region::ResizeN: return "n-resize";
    case Region::ResizeS: return "s-resize";
    case Region::ResizeE: return "e-resize";
    case Region::ResizeW: return "w-resize";
    case Region::ResizeNE: return "ne-resize";
    case Region::ResizeNW: return "nw-resize";
    case Region::ResizeSE: return "se-resize";
    case Region::ResizeSW: return "sw-resize";
    default: return "default";
    }
}

void handle_decoration_click(BiomeToplevel *toplevel, biome_decoration::Region region) {
    using biome_decoration::Region;
    switch (region) {
    case Region::Titlebar:
        // Grabbing the titlebar of a maximized window un-maximizes it
        // first, then starts the move as normal - standard WM convention.
        if (toplevel->maximized) {
            set_toplevel_maximized(toplevel, false);
        }
        begin_interactive(toplevel, BiomeCursorMode::Move, 0, false);
        break;
    case Region::ResizeN:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_TOP, false);
        break;
    case Region::ResizeS:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_BOTTOM, false);
        break;
    case Region::ResizeE:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_RIGHT, false);
        break;
    case Region::ResizeW:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_LEFT, false);
        break;
    case Region::ResizeNE:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_TOP | WLR_EDGE_RIGHT, false);
        break;
    case Region::ResizeNW:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_TOP | WLR_EDGE_LEFT, false);
        break;
    case Region::ResizeSE:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, false);
        break;
    case Region::ResizeSW:
        begin_interactive(toplevel, BiomeCursorMode::Resize, WLR_EDGE_BOTTOM | WLR_EDGE_LEFT, false);
        break;
    case Region::ButtonMaximize:
        set_toplevel_maximized(toplevel, !toplevel->maximized);
        break;
    case Region::ButtonClose:
        close_toplevel(toplevel);
        break;
    case Region::ButtonMinimize:
        set_toplevel_minimized(toplevel, true);
        break;
    case Region::None:
        break;
    }
}
