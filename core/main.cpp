// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Entry point: argument parsing, the offscreen Qt application decoration/
// needs, and wiring up each module's _init(server) in dependency order (see
// docs/plan.md's "Split core/main.cpp into per-concern modules" writeup for
// why the code is split the way it is). Everything past setup is generic
// wlroots server lifecycle - no compositor policy lives in this file.

#include "wlroots.hpp"

#include "core/cursor.h"
#include "core/input.h"
#include "core/output.h"
#include "core/server.h"
#include "decoration/theme.h"
#include "desktop/app_icon.h"
#include "desktop/decoration_bridge.h"
#include "desktop/xdg_shell.h"
#include "desktop/xwayland_shell.h"

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
    // rendering is driven synchronously from this event loop, not a Qt one.
    // Uses its own fixed argc/argv rather than Biome's real ones, since
    // Biome's "-s"/"-h" flags are unrelated to Qt's own CLI arguments.
    // Passed as a "-platform" argument rather than a QT_QPA_PLATFORM
    // qputenv(): qputenv is a real setenv() on Biome's own process, which
    // every client Biome starts (and everything *those* spawn, e.g. a
    // terminal's shell and whatever the user runs from it) inherits -
    // forcing every real Qt app in the session onto the offscreen platform
    // too, silently breaking all of them. The "-platform" argument only
    // selects the platform for this one QApplication instance.
    static int qt_argc = 3;
    static char qt_arg0[] = "biome";
    static char qt_arg1[] = "-platform";
    static char qt_arg2[] = "offscreen";
    static char *qt_argv[] = {qt_arg0, qt_arg1, qt_arg2, nullptr};
    QApplication qt_app(qt_argc, qt_argv);

    BiomeServer server;
    biome_decoration::load_decoration_theme();
    init_icon_theme();
    // The Wayland display is managed by libwayland. It handles accepting
    // clients from the Unix socket, managing Wayland globals, and so on.
    server.display = wl_display_create();
    // The backend is a wlroots feature which abstracts the underlying input
    // and output hardware. The autocreate option will choose the most
    // suitable backend based on the current environment, such as opening an
    // X11 window if an X11 server is running.
    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), &server.session);
    if (server.backend == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_backend");
        return 1;
    }

    // Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The
    // user can also specify a renderer using the WLR_RENDERER env var. The
    // renderer is responsible for defining the various pixel formats it
    // supports for shared memory, this configures that for clients.
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (server.renderer == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_renderer");
        return 1;
    }

    wlr_renderer_init_wl_display(server.renderer, server.display);

    // Autocreates an allocator for us. The allocator is the bridge between
    // the renderer and the backend. It handles the buffer creation,
    // allowing wlroots to render onto the screen.
    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (server.allocator == nullptr) {
        wlr_log(WLR_ERROR, "failed to create wlr_allocator");
        return 1;
    }

    // This creates some hands-off wlroots interfaces. The compositor is
    // necessary for clients to allocate surfaces, the subcompositor allows
    // to assign the role of subsurfaces to surfaces and the data device
    // manager handles the clipboard. Each of these wlroots interfaces has
    // room for you to dig your fingers in and play with their behavior if
    // you want. Note that the clients cannot set the selection directly
    // without compositor approval, see the handling of the
    // request_set_selection event below.
    wlr_compositor *compositor = wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    output_manager_init(&server);
    decoration_bridge_init(&server);
    xdg_shell_init(&server);
    cursor_init(&server);
    input_init(&server);
    xwayland_init(&server, compositor);

    // Add a Unix socket to the Wayland display.
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_backend_destroy(server.backend);
        return 1;
    }

    // Start the backend. This will enumerate outputs and inputs, become the
    // DRM master, etc.
    if (!wlr_backend_start(server.backend)) {
        wlr_backend_destroy(server.backend);
        wl_display_destroy(server.display);
        return 1;
    }

    // Set the WAYLAND_DISPLAY environment variable to our socket and run
    // the startup command if requested.
    setenv("WAYLAND_DISPLAY", socket, true);
    if (startup_cmd) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)nullptr);
        }
    }
    // Run the Wayland event loop. This does not return until you exit the
    // compositor. Starting the backend rigged up all of the necessary event
    // loop configuration to listen to libinput events, DRM events, generate
    // frame events at the refresh rate, and so on.
    wlr_log(WLR_INFO, "Running Biome on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.display);

    // Once wl_display_run returns, we destroy all clients then shut down
    // the server.
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
