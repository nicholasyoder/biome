// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/touch_identify.h"

#include "core/touch_map.h"
#include "desktop/qt_buffer.h"
#include "decoration/touch_identify.h"

void touch_identify_show(BiomeServer *server, const std::vector<wlr_output *> &outputs, const char *device_name) {
    touch_identify_hide(server);
    const int count = std::min(static_cast<int>(outputs.size()), kTouchIdentifySlots);
    for (int i = 0; i < count; i++) {
        wlr_box box;
        wlr_output_layout_get_box(server->output_layout, outputs[i], &box);
        if (wlr_box_empty(&box)) {
            continue;
        }
        wlr_scene_tree *tree = wlr_scene_tree_create(server->layers.osd);
        wlr_scene_node_set_position(&tree->node, box.x, box.y);
        const float scrim[4] = {0, 0, 0, 0.55f};
        wlr_scene_rect_create(tree, box.width, box.height, scrim);

        const double scale = outputs[i]->scale;
        biome_decoration::RenderedFrame prompt = biome_decoration::render_touch_prompt(device_name, scale);
        const int px = (box.width - prompt.logical_width) / 2, py = (box.height - prompt.logical_height) / 2;
        osd_node_show(wlr_scene_buffer_create(tree, nullptr), std::move(prompt), px, py);

        double sx, sy;
        touch_identify_slot(i, &sx, &sy);
        biome_decoration::RenderedFrame target = biome_decoration::render_touch_target(scale);
        const int tx = static_cast<int>(sx * box.width) - target.logical_width / 2;
        const int ty = static_cast<int>(sy * box.height) - target.logical_height / 2;
        osd_node_show(wlr_scene_buffer_create(tree, nullptr), std::move(target), tx, ty);

        server->touch.identify_trees.push_back(tree);
    }
}

void touch_identify_hide(BiomeServer *server) {
    for (wlr_scene_tree *tree : server->touch.identify_trees) {
        wlr_scene_node_destroy(&tree->node);
    }
    server->touch.identify_trees.clear();
}
