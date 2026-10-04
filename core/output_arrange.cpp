// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_arrange.h"

#include <algorithm>
#include <cstdlib>

namespace {

int overlap_w(const wlr_box &a, const wlr_box &b) {
    return std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x);
}

int overlap_h(const wlr_box &a, const wlr_box &b) {
    return std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y);
}

bool connected(const wlr_box &a, const wlr_box &b) {
    int ow = overlap_w(a, b);
    int oh = overlap_h(a, b);
    return (ow > 0 && oh >= 0) || (oh > 0 && ow >= 0);
}

// Shift along one axis that makes b's range touch a's (0 if they already meet).
int close_shift(int a_lo, int a_len, int b_lo, int b_len) {
    if (b_lo >= a_lo + a_len) {
        return a_lo + a_len - b_lo;
    }
    if (b_lo + b_len <= a_lo) {
        return a_lo - (b_lo + b_len);
    }
    return 0;
}

// Shift along one axis that aligns b's nearer edge with a's, giving a positive overlap.
int align_shift(int a_lo, int a_len, int b_lo, int b_len) {
    return b_lo >= a_lo ? (a_lo + a_len) - (b_lo + b_len) : a_lo - b_lo;
}

struct Candidate {
    int component;
    int dx;
    int dy;
};

// Shifts that would make `b` share an edge with `a`.
void add_candidates(const wlr_box &a, const wlr_box &b, int component, std::vector<Candidate> &out) {
    int cx = close_shift(a.x, a.width, b.x, b.width);
    int cy = close_shift(a.y, a.height, b.y, b.height);
    if (overlap_w(a, b) > 0) {
        out.push_back({component, 0, cy});
    } else if (overlap_h(a, b) > 0) {
        out.push_back({component, cx, 0});
    } else {
        out.push_back({component, cx, align_shift(a.y, a.height, b.y, b.height)});
        out.push_back({component, align_shift(a.x, a.width, b.x, b.width), cy});
    }
}

bool fits(const std::vector<wlr_box> &boxes, const std::vector<int> &components, const Candidate &c) {
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (components[i] != c.component) {
            continue;
        }
        wlr_box moved = boxes[i];
        moved.x += c.dx;
        moved.y += c.dy;
        for (size_t j = 0; j < boxes.size(); ++j) {
            if (components[j] != c.component && overlap_w(moved, boxes[j]) > 0 && overlap_h(moved, boxes[j]) > 0) {
                return false;
            }
        }
    }
    return true;
}

// Removes bands along one axis that no box covers, shifting the side away from the anchor.
// Keeps the arrangement exactly and can't create overlap.
void remove_empty_bands(std::vector<wlr_box> &boxes, size_t anchor, bool horizontal) {
    auto lo = [horizontal](const wlr_box &b) { return horizontal ? b.x : b.y; };
    auto hi = [horizontal](const wlr_box &b) { return horizontal ? b.x + b.width : b.y + b.height; };
    std::vector<wlr_box> sorted = boxes;
    std::sort(sorted.begin(), sorted.end(), [&](const wlr_box &a, const wlr_box &b) { return lo(a) < lo(b); });

    std::vector<int> shifts(boxes.size(), 0);
    int covered = hi(sorted[0]);
    for (const wlr_box &b : sorted) {
        if (lo(b) > covered) {
            int width = lo(b) - covered;
            bool anchor_after = lo(boxes[anchor]) >= lo(b);
            for (size_t i = 0; i < boxes.size(); ++i) {
                bool after = lo(boxes[i]) >= lo(b);
                if (after != anchor_after) {
                    shifts[i] += after ? -width : width;
                }
            }
        }
        covered = std::max(covered, hi(b));
    }
    for (size_t i = 0; i < boxes.size(); ++i) {
        (horizontal ? boxes[i].x : boxes[i].y) += shifts[i];
    }
}

} // namespace

std::vector<int> layout_components(const std::vector<wlr_box> &boxes) {
    std::vector<int> components(boxes.size(), -1);
    int next = 0;
    for (size_t start = 0; start < boxes.size(); ++start) {
        if (components[start] != -1) {
            continue;
        }
        components[start] = next;
        std::vector<size_t> pending = {start};
        while (!pending.empty()) {
            const wlr_box a = boxes[pending.back()];
            pending.pop_back();
            for (size_t i = 0; i < boxes.size(); ++i) {
                if (components[i] == -1 && connected(a, boxes[i])) {
                    components[i] = next;
                    pending.push_back(i);
                }
            }
        }
        ++next;
    }
    return components;
}

bool close_layout_gaps(std::vector<wlr_box> &boxes, size_t anchor) {
    if (anchor >= boxes.size()) {
        return true;
    }
    remove_empty_bands(boxes, anchor, true);
    remove_empty_bands(boxes, anchor, false);

    // What's left (e.g. corner-only contact) attaches group by group, nearest first.
    for (;;) {
        std::vector<int> components = layout_components(boxes);
        if (*std::max_element(components.begin(), components.end()) == 0) {
            return true;
        }
        int anchor_component = components[anchor];
        std::vector<Candidate> candidates;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (components[i] != anchor_component) {
                continue;
            }
            for (size_t j = 0; j < boxes.size(); ++j) {
                if (components[j] != anchor_component) {
                    add_candidates(boxes[i], boxes[j], components[j], candidates);
                }
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            return std::abs(a.dx) + std::abs(a.dy) < std::abs(b.dx) + std::abs(b.dy);
        });
        auto chosen = std::find_if(candidates.begin(), candidates.end(),
                                   [&](const Candidate &c) { return fits(boxes, components, c); });
        if (chosen == candidates.end()) {
            return false;
        }
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (components[i] == chosen->component) {
                boxes[i].x += chosen->dx;
                boxes[i].y += chosen->dy;
            }
        }
    }
}
