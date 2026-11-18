// SPDX-License-Identifier: LGPL-3.0-or-later
//
// Test client for run.sh. Prints progress lines to stdout for the driver.
//   win <title> [remap|ssd]  map an xdg toplevel (unmap and remap it, or ask for
//                         server-side decorations)
//   panel                 exclusive-zone layer surface on every output
//   lock                  lock; a lock surface per output; logs enter/key
//   power on|off          set every output's DPMS mode, then exit
//   workspaces <id>       list ext-workspace handles, activate <id>, report the result
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-output-power-management-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define MAX_OUTPUTS 8

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct ext_session_lock_manager_v1 *lock_manager;
static struct zwlr_output_power_manager_v1 *power_manager;
static struct wl_seat *seat;
static struct ext_workspace_manager_v1 *workspace_manager;
static struct zxdg_decoration_manager_v1 *decoration_manager;
static struct ext_session_lock_v1 *lock;
static const char *mode;
static int started;
static struct wl_surface *window_surface;

struct output {
    uint32_t global;
    struct wl_output *wl;
    char name[32];
    struct wl_surface *surface;
};
static struct output outputs[MAX_OUTPUTS];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static struct wl_buffer *make_buffer(int w, int h, uint32_t color) {
    if (w <= 0) w = 64;
    if (h <= 0) h = 64;
    int fd = memfd_create("buf", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)w * h * 4) < 0) {
        exit(2);
    }
    uint32_t *px = mmap(NULL, (size_t)w * h * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; i++) px[i] = color;
    munmap(px, (size_t)w * h * 4);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, w * h * 4);
    struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return b;
}

static const char *surface_name(struct wl_surface *s) {
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (outputs[i].surface && outputs[i].surface == s) return outputs[i].name;
    }
    if (s && s == window_surface) return "window";
    return s ? "other" : "null";
}

// ---- keyboard ----
static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz) {
    (void)d; (void)k; (void)f; (void)sz;
    close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s, struct wl_array *keys) {
    (void)d; (void)k; (void)serial; (void)keys;
    say("enter %s", surface_name(s));
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
    (void)d; (void)k; (void)serial;
    say("leave %s", surface_name(s));
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key, uint32_t state) {
    (void)d; (void)k; (void)serial; (void)t;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) say("key %u", key);
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) {
    (void)d; (void)k; (void)s; (void)a; (void)b; (void)c; (void)g;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t delay) {
    (void)d; (void)k; (void)r; (void)delay;
}
static const struct wl_keyboard_listener kb_listener = {kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    static int have_kb;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !have_kb) {
        have_kb = 1;
        wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kb_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

// ---- outputs ----
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
                         const char *make, const char *model, int32_t t) {
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)make; (void)model; (void)t;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d; (void)o; (void)f; (void)w; (void)h; (void)r;
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) { (void)d; (void)o; (void)f; }
static void out_name(void *d, struct wl_output *o, const char *name) {
    (void)o;
    struct output *out = d;
    snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_desc(void *d, struct wl_output *o, const char *desc) { (void)d; (void)o; (void)desc; }
static const struct wl_output_listener output_listener = {out_geometry, out_mode, out_done, out_scale, out_name,
                                                          out_desc};

// ---- lock ----
static void lock_surface_configure(void *d, struct ext_session_lock_surface_v1 *ls, uint32_t serial, uint32_t w,
                                   uint32_t h) {
    struct wl_surface *s = d;
    ext_session_lock_surface_v1_ack_configure(ls, serial);
    wl_surface_attach(s, make_buffer((int)w, (int)h, 0xff2040a0), 0, 0);
    wl_surface_damage(s, 0, 0, (int)w, (int)h);
    wl_surface_commit(s);
}
static const struct ext_session_lock_surface_v1_listener lock_surface_listener = {lock_surface_configure};

static void lock_locked(void *d, struct ext_session_lock_v1 *l) {
    (void)d; (void)l;
    say("locked");
}
static void lock_finished(void *d, struct ext_session_lock_v1 *l) {
    (void)d; (void)l;
    say("finished");
}
static const struct ext_session_lock_v1_listener lock_listener = {lock_locked, lock_finished};

// ---- layer surface ----
static void layer_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h) {
    struct wl_surface *s = d;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    wl_surface_attach(s, make_buffer((int)w, (int)h, 0xff808080), 0, 0);
    wl_surface_commit(s);
    say("ready");
}
static void layer_closed(void *d, struct zwlr_layer_surface_v1 *l) {
    (void)d;
    zwlr_layer_surface_v1_destroy(l);
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};

static void setup_output(struct output *o) {
    if (!strcmp(mode, "lock")) {
        o->surface = wl_compositor_create_surface(compositor);
        struct ext_session_lock_surface_v1 *ls = ext_session_lock_v1_get_lock_surface(lock, o->surface, o->wl);
        ext_session_lock_surface_v1_add_listener(ls, &lock_surface_listener, o->surface);
    } else if (!strcmp(mode, "panel")) {
        o->surface = wl_compositor_create_surface(compositor);
        struct zwlr_layer_surface_v1 *l =
            zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surface, o->wl, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "panel");
        zwlr_layer_surface_v1_set_anchor(l, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_size(l, 0, 30);
        zwlr_layer_surface_v1_set_exclusive_zone(l, 30);
        zwlr_layer_surface_v1_add_listener(l, &layer_listener, o->surface);
        wl_surface_commit(o->surface);
    }
}

// ---- ext-workspace ----
struct workspace {
    struct ext_workspace_handle_v1 *handle;
    char id[16], name[16];
    uint32_t state;
};
static struct workspace workspaces[16];
static int nworkspaces, workspaces_done;

static void ws_id(void *d, struct ext_workspace_handle_v1 *h, const char *id) {
    (void)h;
    snprintf(((struct workspace *)d)->id, 16, "%s", id);
}
static void ws_name(void *d, struct ext_workspace_handle_v1 *h, const char *name) {
    (void)h;
    snprintf(((struct workspace *)d)->name, 16, "%s", name);
}
static void ws_coordinates(void *d, struct ext_workspace_handle_v1 *h, struct wl_array *c) { (void)d; (void)h; (void)c; }
static void ws_state(void *d, struct ext_workspace_handle_v1 *h, uint32_t state) {
    (void)h;
    ((struct workspace *)d)->state = state;
}
static void ws_caps(void *d, struct ext_workspace_handle_v1 *h, uint32_t caps) { (void)d; (void)h; (void)caps; }
static void ws_removed(void *d, struct ext_workspace_handle_v1 *h) { (void)d; (void)h; }
static const struct ext_workspace_handle_v1_listener ws_listener = {ws_id, ws_name, ws_coordinates, ws_state, ws_caps,
                                                                    ws_removed};

static void wm_group(void *d, struct ext_workspace_manager_v1 *m, struct ext_workspace_group_handle_v1 *g) {
    (void)d; (void)m; (void)g;
}
static void wm_workspace(void *d, struct ext_workspace_manager_v1 *m, struct ext_workspace_handle_v1 *h) {
    (void)d; (void)m;
    if (nworkspaces < 16) {
        workspaces[nworkspaces].handle = h;
        ext_workspace_handle_v1_add_listener(h, &ws_listener, &workspaces[nworkspaces++]);
    }
}
static void wm_done(void *d, struct ext_workspace_manager_v1 *m) {
    (void)d; (void)m;
    workspaces_done++;
}
static void wm_finished(void *d, struct ext_workspace_manager_v1 *m) { (void)d; (void)m; }
static const struct ext_workspace_manager_v1_listener workspace_manager_listener = {wm_group, wm_workspace, wm_done,
                                                                                    wm_finished};

// ---- registry ----
static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (!strcmp(iface, ext_session_lock_manager_v1_interface.name)) {
        lock_manager = wl_registry_bind(r, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwlr_output_power_manager_v1_interface.name)) {
        power_manager = wl_registry_bind(r, name, &zwlr_output_power_manager_v1_interface, 1);
    } else if (!strcmp(iface, ext_workspace_manager_v1_interface.name)) {
        workspace_manager = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
        ext_workspace_manager_v1_add_listener(workspace_manager, &workspace_manager_listener, NULL);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        decoration_manager = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            struct output *o = &outputs[i];
            if (o->wl) continue;
            o->global = name;
            o->wl = wl_registry_bind(r, name, &wl_output_interface, v < 4 ? v : 4);
            wl_output_add_listener(o->wl, &output_listener, o);
            if (started) {
                wl_display_roundtrip(display);
                setup_output(o);
            }
            break;
        }
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
    (void)d; (void)r;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (outputs[i].wl && outputs[i].global == name) {
            say("output-removed %s", outputs[i].name);
        }
    }
}
static const struct wl_registry_listener registry_listener = {reg_global, reg_remove};

// ---- xdg toplevel ----
static void wm_ping(void *d, struct xdg_wm_base *w, uint32_t serial) {
    (void)d;
    xdg_wm_base_pong(w, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static int configured;
static void xdg_configure(void *d, struct xdg_surface *s, uint32_t serial) {
    (void)d;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}
static const struct xdg_surface_listener xdg_surface_listener = {xdg_configure};
static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {
    (void)d; (void)t; (void)w; (void)h; (void)st;
}
static void tl_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = {tl_configure, tl_close};

static void wait_configure(struct wl_surface *s) {
    configured = 0;
    wl_surface_commit(s);
    while (!configured) {
        if (wl_display_dispatch(display) < 0) exit(1);
    }
}

static void deco_configure(void *d, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode) {
    (void)d; (void)deco;
    say("decoration %s", mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE ? "server-side" : "client-side");
}
static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {deco_configure};

static void run_window(const char *title, int remap, int ssd) {
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    struct wl_surface *s = wl_compositor_create_surface(compositor);
    window_surface = s;
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, s);
    xdg_surface_add_listener(xs, &xdg_surface_listener, NULL);
    struct xdg_toplevel *t = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(t, &toplevel_listener, NULL);
    xdg_toplevel_set_title(t, title);
    if (ssd) {
        struct zxdg_toplevel_decoration_v1 *deco = zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_manager, t);
        zxdg_toplevel_decoration_v1_add_listener(deco, &deco_listener, NULL);
        zxdg_toplevel_decoration_v1_set_mode(deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wait_configure(s);
    wl_surface_attach(s, make_buffer(400, 300, 0xff40a040), 0, 0);
    wl_surface_commit(s);
    wl_display_roundtrip(display);
    if (remap) {
        // Unmap via a null buffer; the next commit is an initial commit again.
        wl_surface_attach(s, NULL, 0, 0);
        wl_surface_commit(s);
        wl_display_roundtrip(display);
        xdg_toplevel_set_maximized(t);
        wait_configure(s);
        wl_surface_attach(s, make_buffer(400, 300, 0xff40a040), 0, 0);
        wl_surface_commit(s);
        wl_display_roundtrip(display);
    }
    say("ready");
}

static void print_workspaces(void) {
    for (int i = 0; i < nworkspaces; i++) {
        say("workspace id=%s name=%s%s", workspaces[i].id, workspaces[i].name,
            (workspaces[i].state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE) ? " active" : "");
    }
}

static int run_workspaces(const char *activate_id) {
    while (!workspaces_done) {
        if (wl_display_dispatch(display) < 0) return 1;
    }
    print_workspaces();
    for (int i = 0; i < nworkspaces; i++) {
        if (!strcmp(workspaces[i].id, activate_id)) ext_workspace_handle_v1_activate(workspaces[i].handle);
    }
    ext_workspace_manager_v1_commit(workspace_manager);
    int seen = workspaces_done;
    while (workspaces_done == seen) {
        if (wl_display_dispatch(display) < 0) return 1;
    }
    say("after-activate");
    print_workspaces();
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s win|panel|lock|power ...\n", argv[0]);
        return 2;
    }
    mode = argv[1];
    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "biome-test-client: no display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!strcmp(mode, "workspaces")) {
        return run_workspaces(argc > 2 ? argv[2] : "0");
    }
    if (!strcmp(mode, "power")) {
        int on = argc > 2 && !strcmp(argv[2], "on");
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            if (!outputs[i].wl) continue;
            struct zwlr_output_power_v1 *p = zwlr_output_power_manager_v1_get_output_power(power_manager, outputs[i].wl);
            zwlr_output_power_v1_set_mode(p, on ? ZWLR_OUTPUT_POWER_V1_MODE_ON : ZWLR_OUTPUT_POWER_V1_MODE_OFF);
        }
        wl_display_roundtrip(display);
        return 0;
    }
    if (!strcmp(mode, "win")) {
        run_window(argc > 2 ? argv[2] : "test-win", argc > 3 && !strcmp(argv[3], "remap"),
                   argc > 3 && !strcmp(argv[3], "ssd"));
    } else if (!strcmp(mode, "lock")) {
        lock = ext_session_lock_manager_v1_lock(lock_manager);
        ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
    }
    started = 1;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (outputs[i].wl) setup_output(&outputs[i]);
    }
    while (wl_display_dispatch(display) != -1) {
    }
    say("disconnected");
    return 0;
}
