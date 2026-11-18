// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/qt_buffer.h"

#include <drm_fourcc.h>
#include <utility>

namespace {

struct QtBuffer {
    wlr_buffer base = {};
    std::vector<uint8_t> pixels;
    int stride = 0;
};

void qt_buffer_destroy(wlr_buffer *buf) {
    QtBuffer *buffer = wl_container_of(buf, buffer, base);
    delete buffer;
}

bool qt_buffer_begin_data_ptr_access(wlr_buffer *buf,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    QtBuffer *buffer = wl_container_of(buf, buffer, base);
    *data = buffer->pixels.data();
    *format = DRM_FORMAT_ARGB8888;
    *stride = static_cast<size_t>(buffer->stride);
    return true;
}

void qt_buffer_end_data_ptr_access(wlr_buffer *buf) {
    (void)buf;
}

// CPU-only pixel data: no dmabuf/shm export.
const wlr_buffer_impl kQtBufferImpl = {
    qt_buffer_destroy,
    nullptr,
    nullptr,
    qt_buffer_begin_data_ptr_access,
    qt_buffer_end_data_ptr_access,
};

} // namespace

bool set_scene_frame(wlr_scene_buffer *scene_buffer, biome_decoration::RenderedFrame &&frame) {
    if (frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    auto *buffer = new QtBuffer();
    buffer->stride = frame.stride;
    buffer->pixels = std::move(frame.pixels);
    wlr_buffer_init(&buffer->base, &kQtBufferImpl, frame.width, frame.height);
    // Dest size first, so the buffer is never briefly laid out at its pixel size.
    wlr_scene_buffer_set_dest_size(scene_buffer, frame.logical_width, frame.logical_height);
    wlr_scene_buffer_set_buffer(scene_buffer, &buffer->base);
    wlr_buffer_drop(&buffer->base);
    return true;
}

bool osd_node_show(wlr_scene_buffer *node, biome_decoration::RenderedFrame &&frame, int x, int y) {
    if (!set_scene_frame(node, std::move(frame))) {
        wlr_scene_node_set_enabled(&node->node, false);
        return false;
    }
    wlr_scene_node_set_position(&node->node, x, y);
    wlr_scene_node_set_enabled(&node->node, true);
    wlr_scene_node_raise_to_top(&node->node);
    return true;
}
