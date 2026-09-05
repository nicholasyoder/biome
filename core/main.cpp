// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Entry point: argument parsing, the offscreen Qt application decoration/
// needs, and wiring up each module's _init(server) in dependency order.
// Everything past setup is generic wlroots server lifecycle - no compositor
// policy lives in this file.

#include "wlroots.hpp"

#include "core/cursor.h"
#include "core/input.h"
#include "core/output.h"
#include "core/server.h"
#include "decoration/theme.h"
#include "desktop/app_icon.h"
#include "desktop/decoration_bridge.h"
#include "desktop/ext_workspace.h"
#include "desktop/foreign_toplevel.h"
#include "desktop/layer_shell.h"
#include "desktop/session_lock.h"
#include "desktop/xdg_shell.h"
#include "desktop/xwayland_shell.h"
#include "ipc/global_shortcuts_portal.h"
#include "ipc/workspace_bridge.h"

#include <QApplication>

#include <cstdio>
#include <cstdlib>
#include <getopt.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, nullptr);
    char *startup_cmd = nullptr;

    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            startup_cmd = optarg;
            break;
        default:
            printf("Usage: %s [-s startup command]\n", argv[0]);
            return 0;
        }
    }
    if (optind < argc) {
        printf("Usage: %s [-s startup command]\n", argv[0]);
        return 0;
    }

    // Offscreen QApplication for the decoration/ Qt renderer - no real
    // display needed, and Biome never calls exec(): QPainter/QImage
    // rendering is driven synchronously from this event loop instead. Its
    // own fixed argc/argv keep Biome's "-s"/"-h" flags separate from Qt's.
    // The offscreen platform is passed as a "-platform" argument rather than
    // via qputenv(QT_QPA_PLATFORM): qputenv is a real setenv() on Biome's own
    // process, which every client Biome spawns (and everything those spawn)
    // would inherit, forcing every real Qt app in the session onto the
    // offscreen platform too. "-platform" only affects this QApplication.
    static int qt_argc = 3;
    static char qt_arg0[] = "biome";
    static char qt_arg1[] = "-platform";
    static char qt_arg2[] = "offscreen";
    static char *qt_argv[] = {qt_arg0, qt_arg1, qt_arg2, nullptr};
    QApplication qt_app(qt_argc, qt_argv);

    BiomeServer server;
    biome_decoration::load_decoration_theme();
    init_icon_theme();
    server.display = wl_display_create();
    // Autocreate picks the most suitable backend for the environment (e.g.
    // an X11 window if an X11 server is running).
    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), &server.session);
    if (server.backend == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return 1;
    }

    // Pixman, GLES2, or Vulkan, per WLR_RENDERER or autodetection.
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return 1;
    }

    wlr_renderer_init_wl_display(server.renderer, server.display);

    // The bridge between renderer and backend, handling buffer creation.
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return 1;
    }

    // compositor: lets clients allocate surfaces. subcompositor: assigns the
    // subsurface role. data_device_manager: clipboard + drag-and-drop (see
    // seat_request_set_selection/seat_request_start_drag in
    // core/input.cpp). primary_selection_manager: select-to-copy /
    // middle-click-paste (see seat_request_set_primary_selection).
    wlr_compositor *compositor = wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);
    wlr_primary_selection_v1_device_manager_create(server.display);

    output_manager_init(&server);
    session_lock_init(&server);
    layer_shell_init(&server);
    foreign_toplevel_init(&server);
    ext_workspace_init(&server);
    decoration_bridge_init(&server);
    xdg_shell_init(&server);
    cursor_init(&server);
    input_init(&server);
    xwayland_init(&server, compositor);
    global_shortcuts_portal_init(&server);
    workspace_bridge_init(&server);

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    if (startup_cmd) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)nullptr);
        }
    }
    wlr_log(WLR_INFO, "Running Biome on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.display);

    wl_display_destroy_clients(server.display);
    if (server.ewmh_ready) {
        xcb_ewmh_connection_wipe(&server.ewmh);
    }
    if (server.xwayland) {
        wlr_xwayland_destroy(server.xwayland);
    }
    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    wlr_backend_destroy(server.backend);
    wl_display_destroy(server.display);
    return 0;
}
