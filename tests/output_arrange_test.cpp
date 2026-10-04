// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_arrange.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void expect(bool condition, const char *what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

wlr_box box(int x, int y, int width = 1920, int height = 1080) {
    return wlr_box{x, y, width, height};
}

bool at(const wlr_box &b, int x, int y) {
    return b.x == x && b.y == y;
}

void test_components() {
    expect(layout_components({box(0, 0), box(1920, 0)}) == std::vector<int>({0, 0}), "edge contact connects");
    expect(layout_components({box(0, 0), box(1920, 1080)}) == std::vector<int>({0, 1}), "corner contact doesn't");
    expect(layout_components({box(0, 0), box(100, 100)}) == std::vector<int>({0, 0}), "overlap connects");
    expect(layout_components({box(0, 0), box(3840, 0), box(1920, 0)}) == std::vector<int>({0, 0, 0}),
           "bridged by a later box");
}

void test_middle_unplug_row() {
    std::vector<wlr_box> boxes = {box(0, 0), box(3840, 0)};
    expect(close_layout_gaps(boxes, 0), "row: ok");
    expect(at(boxes[0], 0, 0) && at(boxes[1], 1920, 0), "row: right output slides left");
}

void test_middle_unplug_column() {
    std::vector<wlr_box> boxes = {box(0, 0), box(0, 2160)};
    expect(close_layout_gaps(boxes, 0), "column: ok");
    expect(at(boxes[1], 0, 1080), "column: lower output slides up");
}

void test_anchor_stays() {
    std::vector<wlr_box> boxes = {box(0, 0), box(3840, 0)};
    expect(close_layout_gaps(boxes, 1), "anchor: ok");
    expect(at(boxes[0], 1920, 0) && at(boxes[1], 3840, 0), "anchor: non-anchor moves");
}

void test_l_shape() {
    std::vector<wlr_box> boxes = {box(0, 0), box(0, 1080), box(3840, 1080)};
    expect(close_layout_gaps(boxes, 0), "L: ok");
    expect(at(boxes[2], 1920, 1080), "L: attaches to the nearest box");
}

void test_diagonal() {
    std::vector<wlr_box> boxes = {box(0, 0), box(2500, 1500)};
    expect(close_layout_gaps(boxes, 0), "diagonal: ok");
    expect(at(boxes[1], 1920, 0), "diagonal: closes the smaller gap, aligns the other edge");
}

void test_corner_contact() {
    std::vector<wlr_box> boxes = {box(0, 0), box(1920, 1080)};
    expect(close_layout_gaps(boxes, 0), "corner: ok");
    expect(at(boxes[1], 1920, 0), "corner: becomes edge-adjacent");
}

void test_mirrored_group_kept() {
    std::vector<wlr_box> boxes = {box(0, 0), box(3840, 0), box(3840, 0)};
    expect(close_layout_gaps(boxes, 0), "mirror: ok");
    expect(at(boxes[1], 1920, 0) && at(boxes[2], 1920, 0), "mirror: group moves together");
}

void test_stale_scale() {
    // Left output went to scale 1.5 (1920x1080 -> 1280x720) with x/y unchanged.
    std::vector<wlr_box> boxes = {box(0, 0, 1280, 720), box(1920, 0)};
    expect(close_layout_gaps(boxes, 0), "scale: ok");
    expect(at(boxes[1], 1280, 0), "scale: gap closed");
}

void test_arrangement_kept() {
    // Right of A, and C directly below B, both with gaps.
    std::vector<wlr_box> boxes = {box(0, 0, 1280, 720), box(5000, 0, 1280, 720), box(5000, 3000, 1280, 720)};
    expect(close_layout_gaps(boxes, 0), "arrangement: ok");
    expect(at(boxes[1], 1280, 0) && at(boxes[2], 1280, 720), "arrangement: C stays below B");
}

void test_no_new_overlap() {
    // C is closest to A, but straight up would land on B's spot.
    std::vector<wlr_box> boxes = {box(0, 0), box(1920, 0), box(1000, 2000)};
    expect(close_layout_gaps(boxes, 0), "overlap: ok");
    for (size_t i = 0; i < boxes.size(); ++i) {
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            const wlr_box &a = boxes[i], &b = boxes[j];
            bool overlap = a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
            expect(!overlap, "overlap: none introduced");
        }
    }
    expect(layout_components(boxes) == std::vector<int>({0, 0, 0}), "overlap: connected");
}

} // namespace

int main() {
    test_components();
    test_middle_unplug_row();
    test_middle_unplug_column();
    test_anchor_stays();
    test_l_shape();
    test_diagonal();
    test_corner_contact();
    test_mirrored_group_kept();
    test_stale_scale();
    test_arrangement_kept();
    test_no_new_overlap();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
