// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/layers.h"

void scene_layers_init(BiomeServer *server) {
    server->layers.background = wlr_scene_tree_create(&server->scene->tree);
    server->layers.bottom = wlr_scene_tree_create(&server->scene->tree);
    server->layers.toplevels = wlr_scene_tree_create(&server->scene->tree);
    server->layers.above = wlr_scene_tree_create(&server->scene->tree);
    server->layers.top = wlr_scene_tree_create(&server->scene->tree);
    server->layers.overlay = wlr_scene_tree_create(&server->scene->tree);
    server->layers.fullscreen = wlr_scene_tree_create(&server->scene->tree);
    server->layers.osd = wlr_scene_tree_create(&server->scene->tree);
    server->layers.session_lock = wlr_scene_tree_create(&server->scene->tree);
    // Starts disabled; its per-output content exists regardless of lock state.
    wlr_scene_node_set_enabled(&server->layers.session_lock->node, false);
}
