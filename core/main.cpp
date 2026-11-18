// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Entry point: argument parsing, the offscreen Qt application decoration/
// needs, and wiring up each module's _init(server) in dependency order.
// Everything past setup is generic wlroots server lifecycle - no compositor
// policy lives in this file.

#include "wlroots.hpp"

#include "core/cursor.h"
#include "core/input.h"
#include "core/keybindings.h"
#include "core/output.h"
#include "core/gamma.h"
#include "core/output_power.h"
#include "core/pointer_constraints.h"
#include "core/qt_glib_bridge.h"
#include "core/server.h"
#include "decoration/theme.h"
#include "desktop/app_icon.h"
#include "desktop/decoration_bridge.h"
#include "desktop/ext_workspace.h"
#include "desktop/foreign_toplevel.h"
#include "desktop/idle.h"
#include "desktop/layer_shell.h"
#include "desktop/session_lock.h"
#include "desktop/switcher.h"
#include "desktop/window_menu.h"
#include "desktop/xdg_activation.h"
#include "desktop/xdg_shell.h"
#include "desktop/xwayland_shell.h"
#include "core/config.h"
#include "ipc/cursor_theme_watcher.h"
#include "ipc/theme_watcher.h"
#include "ipc/global_shortcuts_portal.h"
#include "ipc/workspace_bridge.h"

#include <QApplication>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <getopt.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

pid_t startup_pid = -1;
int startup_status = 0;

// greetd only starts the session once its greeter command exits, so Biome
// exits with the -s command (as cage does).
int handle_startup_exit(int, uint32_t, void *data) {
    auto *display = static_cast<wl_display *>(data);
    if (waitpid(startup_pid, &startup_status, 0) == startup_pid) {
        if (WIFEXITED(startup_status)) {
            wlr_log(WLR_INFO, "Startup command exited with status %d", WEXITSTATUS(startup_status));
        } else if (WIFSIGNALED(startup_status)) {
            wlr_log(WLR_INFO, "Startup command killed by signal %d", WTERMSIG(startup_status));
        }
    }
    startup_pid = -1;
    wl_display_terminate(display);
    return 0;
}

int signal_pipe[2] = {-1, -1};

void on_signal(int) {
    int saved = errno;
    char b = 0;
    (void)!write(signal_pipe[1], &b, 1);
    errno = saved;
}

// A pipe rather than wl_event_loop_add_signal: Qt's threads already exist with
// these signals unblocked, so a signalfd would miss them.
int handle_signal_pipe(int fd, uint32_t, void *data) {
    char b;
    (void)!read(fd, &b, 1);
    wlr_log(WLR_INFO, "Termination signal received");
    wl_display_terminate(static_cast<wl_display *>(data));
    return 0;
}

// wlroots asserts nothing still listens on an object's signals when it is
// destroyed. Modules finish in reverse init order, except that the backend
// (whose output_destroy() still needs the scene, cursor and renderer) goes
// before the rest, once nothing listens for new devices.
void teardown(BiomeServer &server) {
    wl_display_destroy_clients(server.display);
    xwayland_finish(&server);
    input_finish(&server);
    output_manager_finish(&server);
    wlr_backend_destroy(server.backend);
    wlr_scene_node_destroy(&server.scene->tree.node);
    cursor_finish(&server);
    wlr_allocator_destroy(server.allocator);
    wlr_renderer_destroy(server.renderer);
    gamma_finish(&server);
    output_power_finish(&server);
    idle_finish(&server);
    pointer_constraints_finish(&server);
    keybindings_finish(&server);
    snap_finish(&server);
    decoration_finish(&server);
    xdg_shell_finish(&server);
    window_menu_finish(&server);
    xdg_activation_finish(&server);
    ext_workspace_finish(&server);
    foreign_toplevel_finish(&server);
    layer_shell_finish(&server);
    session_lock_finish(&server);
    wl_display_destroy(server.display);
}

} // namespace

int main(int argc, char *argv[]) {
#ifdef BIOME_DEV_BUILD
    wlr_log_init(WLR_DEBUG, nullptr);
#else
    // BIOME_DEBUG=1 for bug reports.
    const char *debug_env = getenv("BIOME_DEBUG");
    wlr_log_init(debug_env && strcmp(debug_env, "1") == 0 ? WLR_DEBUG : WLR_INFO, nullptr);
#endif
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
    biome_decoration::load_decoration_theme(
        biome_config().value("Theme/dir").toString(), biome_config().value("Theme/name").toString());
    init_icon_theme();
    server.display = wl_display_create();
    // Bridges Qt's GLib-backed dispatcher into wl_event_loop so Qt/QtDBus
    // dispatch fd-driven off Biome's own loop. Called this early so it's live
    // for the entire rest of startup.
    qt_glib_bridge_init(&server);
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

    if (!wlr_renderer_init_wl_shm(server.renderer, server.display)) {
        wlr_log(WLR_ERROR, "failed to create wl_shm");
        return 1;
    }
    // Same check wlr_renderer_init_wl_display uses; created here instead so
    // the scene can get it for per-surface scanout feedback.
    if (wlr_renderer_get_texture_formats(server.renderer, WLR_BUFFER_CAP_DMABUF) != nullptr &&
        wlr_renderer_get_drm_fd(server.renderer) >= 0) {
        server.linux_dmabuf =
            wlr_linux_dmabuf_v1_create_with_renderer(server.display, 4, server.renderer);
        if (server.linux_dmabuf == nullptr) {
            wlr_log(WLR_ERROR, "failed to create linux-dmabuf-v1");
            return 1;
        }
    }

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
    // Unfocused clipboard access for clipboard managers; see
    // architecture-notes.md "Data control".
    wlr_data_control_manager_v1_create(server.display);

    // fractional_scale_manager + viewporter: without these, clients only see
    // the legacy integer wl_output.scale (wlroots advertises ceil() of the
    // real per-output scale over that protocol - see output.c's send_scale())
    // instead of the exact per-output float scale, so any output configured
    // with a non-integer scale (e.g. 1.5) makes fractional-scale-aware
    // clients (Qt included) mis-size their surfaces relative to that output's
    // real logical resolution. wlr_scene already drives both of these
    // automatically per-surface (types/scene/surface.c calls
    // wlr_fractional_scale_v1_notify_scale() using the exact output scale
    // whenever a surface's current output set changes) - creating the
    // globals here is the only wiring needed.
    wlr_fractional_scale_manager_v1_create(server.display, 1);
    wlr_viewporter_create(server.display);
    // Lets clients paint solid fills (dim overlays) without a full-size shm upload.
    wlr_single_pixel_buffer_manager_v1_create(server.display);
    // Hint-only globals; wlr_scene applies alpha-modifier per surface itself.
    wlr_content_type_manager_v1_create(server.display, 1);
    wlr_alpha_modifier_v1_create(server.display);

    // Order: output_manager_init creates the scene and layers everyone uses;
    // input_init creates the seat that pointer_constraints and xwayland need.
    output_manager_init(&server);
    session_lock_init(&server);
    layer_shell_init(&server);
    foreign_toplevel_init(&server);
    ext_workspace_init(&server);
    xdg_activation_init(&server);
    switcher_init(&server);
    window_menu_init(&server);
    xdg_shell_init(&server);
    cursor_init(&server);
    keybindings_init(&server);
    input_init(&server);
    pointer_constraints_init(&server);
    idle_init(&server);
    output_power_init(&server);
    gamma_init(&server);
    xwayland_init(&server, compositor);
    global_shortcuts_portal_init(&server);
    workspace_bridge_init(&server);
    cursor_theme_watcher_init(&server);
    theme_watcher_init(&server);

    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        teardown(server);
        return 1;
    }

    if (!wlr_backend_start(server.backend)) {
        teardown(server);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket, true);
    // pidfd rather than a SIGCHLD signalfd: Qt's threads already exist with
    // SIGCHLD unblocked, and it can't steal wlroots' Xwayland waitpid.
    int startup_pidfd = -1;
    wl_event_source *startup_source = nullptr;
    if (startup_cmd) {
        startup_pid = fork();
        if (startup_pid == 0) {
            execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)nullptr);
            _exit(127);
        }
        if (startup_pid < 0) {
            wlr_log_errno(WLR_ERROR, "failed to fork startup command");
        } else if ((startup_pidfd = static_cast<int>(syscall(SYS_pidfd_open, startup_pid, 0))) < 0) {
            wlr_log_errno(WLR_ERROR, "pidfd_open failed; Biome won't exit with the startup command");
        } else {
            startup_source = wl_event_loop_add_fd(wl_display_get_event_loop(server.display),
                                                  startup_pidfd, WL_EVENT_READABLE,
                                                  handle_startup_exit, server.display);
        }
    }
    wl_event_source *signal_source = nullptr;
    if (pipe2(signal_pipe, O_CLOEXEC | O_NONBLOCK) == 0) {
        signal_source = wl_event_loop_add_fd(wl_display_get_event_loop(server.display),
                                             signal_pipe[0], WL_EVENT_READABLE,
                                             handle_signal_pipe, server.display);
        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
    } else {
        wlr_log_errno(WLR_ERROR, "pipe2 failed; SIGTERM won't run teardown");
    }
    wlr_log(WLR_INFO, "Running Biome on WAYLAND_DISPLAY=%s", socket);
    wl_display_run(server.display);

    if (signal_source) {
        wl_event_source_remove(signal_source);
    }
    if (startup_source) {
        wl_event_source_remove(startup_source);
    }
    if (startup_pid > 0) { // exiting on a signal, not the child's exit
        kill(startup_pid, SIGTERM);
        waitpid(startup_pid, nullptr, 0);
    }
    if (startup_pidfd >= 0) {
        close(startup_pidfd);
    }

    teardown(server);
    if (WIFEXITED(startup_status)) {
        return WEXITSTATUS(startup_status);
    }
    return WIFSIGNALED(startup_status) ? 128 + WTERMSIG(startup_status) : 0;
}
