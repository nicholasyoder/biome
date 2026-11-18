// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "layout.h" // Region

#include <QImage>
#include <QRect>

#include <cstdint>
#include <vector>

class QWidget;

namespace biome_decoration {

class DecorationFrame;

// Rendered widget pixels: premultiplied ARGB32 at logical size * scale. Null
// image for an empty source.
struct RenderedFrame {
    QImage image;
    int logical_width = 0; // the scene buffer's dest size
    int logical_height = 0;
};

// Renders source (logical, whole widget if null) into a buffer of scale times
// as many pixels. Shared by every decoration/ renderer.
RenderedFrame render_widget(QWidget *widget, double scale, const QRect &source = {});

// A resolved window icon - square ARGB32-premultiplied pixel data at
// whatever fixed size desktop/app_icon.h rasterizes to. size == 0 (pixels
// empty) means no icon was found; icon_from_image() substitutes a generic one.
struct IconImage {
    int size = 0;
    std::vector<uint8_t> pixels;
};

// The frame is drawn as four buffers around the client's surface, so a
// titlebar-only change (title, hover) re-renders just the titlebar.
enum class Strip { Top, Left, Right, Bottom };
constexpr int kStripCount = 4;

// Applies a window's state to its own DecorationFrame
// (create_decoration_frame(), decoration/theme.h) and lays it out for a client
// of content_width x content_height. hovered/pressed_region select which
// button (if any) gets a live :hover/:pressed state. An empty icon shows a
// generic one. False if the content size is non-positive.
bool update_decoration(DecorationFrame *widget, int content_width, int content_height,
    bool focused, bool urgent, bool maximized, const char *title, const IconImage &icon,
    Region hovered_region, Region pressed_region);

// Renders one strip of the frame as last laid out by update_decoration(); its
// position within the frame is DecorationFrame::stripRect(). scale is the
// target output's.
RenderedFrame render_decoration_strip(DecorationFrame *widget, Strip strip, double scale);

} // namespace biome_decoration
