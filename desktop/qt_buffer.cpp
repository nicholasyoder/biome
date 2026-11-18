// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/qt_buffer.h"

#include <drm_fourcc.h>
#include <utility>

namespace {

// Wraps the rendered QImage itself; no copy.
struct QtBuffer {
    wlr_buffer base = {};
    QImage image;
};

void qt_buffer_destroy(wlr_buffer *buf) {
    QtBuffer *buffer = wl_container_of(buf, buffer, base);
    delete buffer;
}

bool qt_buffer_begin_data_ptr_access(wlr_buffer *buf,
        uint32_t flags, void **data, uint32_t *format, size_t *stride) {
    (void)flags;
    QtBuffer *buffer = wl_container_of(buf, buffer, base);
    // constBits(): bits() could detach a shared image.
    *data = const_cast<uchar *>(buffer->image.constBits());
    *format = DRM_FORMAT_ARGB8888;
    *stride = static_cast<size_t>(buffer->image.bytesPerLine());
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
    if (frame.image.isNull()) {
        return false;
    }
    // Same pixels (a cached render) as already shown: skip the texture re-upload.
    wlr_buffer *current = scene_buffer->buffer;
    if (current != nullptr && current->impl == &kQtBufferImpl) {
        QtBuffer *shown = wl_container_of(current, shown, base);
        if (shown->image.cacheKey() == frame.image.cacheKey()) {
            wlr_scene_buffer_set_dest_size(scene_buffer, frame.logical_width, frame.logical_height);
            return true;
        }
    }
    auto *buffer = new QtBuffer();
    const int width = frame.image.width(), height = frame.image.height();
    buffer->image = std::move(frame.image);
    wlr_buffer_init(&buffer->base, &kQtBufferImpl, width, height);
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
