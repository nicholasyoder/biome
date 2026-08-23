// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/layers.h"

void scene_layers_init(BiomeServer *server) {
    server->layers.background = wlr_scene_tree_create(&server->scene->tree);
    server->layers.bottom = wlr_scene_tree_create(&server->scene->tree);
    server->layers.toplevels = wlr_scene_tree_create(&server->scene->tree);
    server->layers.top = wlr_scene_tree_create(&server->scene->tree);
    server->layers.overlay = wlr_scene_tree_create(&server->scene->tree);
    server->layers.session_lock = wlr_scene_tree_create(&server->scene->tree);
    // Starts disabled - only enabled for the duration of a lock (mirrors
    // the old BiomeServer::lock_tree's own initial-disabled convention).
    // Its content (BiomeOutput::lock_tree, one per output) is created
    // unconditionally by server_new_output() regardless of lock state, so
    // this is what actually keeps it invisible/unhittable outside a lock -
    // being structurally topmost only matters for what happens once it's
    // enabled.
    wlr_scene_node_set_enabled(&server->layers.session_lock->node, false);
}
