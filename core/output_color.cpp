// SPDX-License-Identifier: LGPL-3.0-or-later

#include "core/output_color.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

extern "C" {
#include <wlr/backend/drm.h> // no C++ linkage guards of its own
}

#include <cstring>

namespace {

// Looks up a connector enum property by name; returns false if absent.
bool find_enum_prop(int fd, uint32_t connector_id, const char *name, const char *value_name,
                    uint32_t *prop_id, uint64_t *value) {
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (props == nullptr) {
        return false;
    }
    bool found = false;
    for (uint32_t i = 0; i < props->count_props && !found; i++) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop == nullptr) {
            continue;
        }
        if (std::strcmp(prop->name, name) == 0) {
            for (int e = 0; e < prop->count_enums; e++) {
                if (std::strcmp(prop->enums[e].name, value_name) == 0) {
                    *prop_id = prop->prop_id;
                    *value = prop->enums[e].value;
                    found = true;
                    break;
                }
            }
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return found;
}

} // namespace

void output_set_full_range(BiomeServer *server, wlr_output *output) {
    if (server->session == nullptr || !wlr_output_is_drm(output)) {
        return;
    }
    uint32_t connector_id = wlr_drm_connector_get_id(output);

    // wlroots doesn't expose its master fd; the session's device fds are it.
    wlr_device *dev;
    wl_list_for_each(dev, &server->session->devices, link) {
        if (!drmIsMaster(dev->fd)) {
            continue;
        }
        uint32_t prop_id;
        uint64_t value;
        if (!find_enum_prop(dev->fd, connector_id, "Broadcast RGB", "Full", &prop_id, &value)) {
            continue;
        }
        if (drmModeObjectSetProperty(dev->fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, prop_id, value) != 0) {
            wlr_log_errno(WLR_ERROR, "output %s: failed to set Broadcast RGB=Full", output->name);
        } else {
            wlr_log(WLR_INFO, "output %s: Broadcast RGB set to Full", output->name);
        }
        return;
    }
    wlr_log(WLR_DEBUG, "output %s: no Broadcast RGB property, leaving range as-is", output->name);
}
