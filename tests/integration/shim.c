// SPDX-License-Identifier: LGPL-3.0-or-later
//
// LD_PRELOADed into biome by run.sh: adds a fake keyboard and polls
// $XDG_RUNTIME_DIR for control files (key-<code>, unplug-<output>).
#define _GNU_SOURCE
#define WLR_USE_UNSTABLE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>

#define MAX_OUTPUTS 8

static struct wlr_keyboard kb;
static const struct wlr_keyboard_impl kb_impl = {.name = "test-kb"};
static struct wl_display *display;
static struct wl_event_source *timer;
static const char *dir;

struct tracked {
    struct wlr_output *output;
    struct wl_listener destroy;
};
static struct tracked outputs[MAX_OUTPUTS];
static struct wl_listener new_output;

static void output_destroyed(struct wl_listener *l, void *data) {
    (void)data;
    struct tracked *t = wl_container_of(l, t, destroy);
    wl_list_remove(&t->destroy.link);
    t->output = NULL;
}

static void handle_new_output(struct wl_listener *l, void *data) {
    (void)l;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!outputs[i].output) {
            outputs[i].output = data;
            outputs[i].destroy.notify = output_destroyed;
            wl_signal_add(&outputs[i].output->events.destroy, &outputs[i].destroy);
            return;
        }
    }
}

static void tap(uint32_t key) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    struct wlr_keyboard_key_event ev = {
        .time_msec = (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000),
        .keycode = key,
        .update_state = true,
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
    };
    wlr_keyboard_notify_key(&kb, &ev);
    ev.state = WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(&kb, &ev);
    fprintf(stderr, "SHIM: typed key %u\n", key);
}

static int consume(const char *name) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    return unlink(path) == 0;
}

static int on_timer(void *data) {
    (void)data;
    char name[128];
    static const uint32_t keys[] = {1 /* Esc */, 30 /* A */};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        snprintf(name, sizeof name, "key-%u", keys[i]);
        if (consume(name)) {
            tap(keys[i]);
        }
    }
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!outputs[i].output) {
            continue;
        }
        snprintf(name, sizeof name, "unplug-%s", outputs[i].output->name);
        if (consume(name)) {
            fprintf(stderr, "SHIM: unplugging %s\n", outputs[i].output->name);
            wlr_output_destroy(outputs[i].output);
        }
    }
    wl_event_source_timer_update(timer, 50);
    return 0;
}

struct wl_display *wl_display_create(void) {
    struct wl_display *(*real)(void) = dlsym(RTLD_NEXT, "wl_display_create");
    display = real();
    return display;
}

bool wlr_backend_start(struct wlr_backend *backend) {
    bool (*real)(struct wlr_backend *) = dlsym(RTLD_NEXT, "wlr_backend_start");
    // The multi backend starts its children through this wrapper too.
    static int depth, done;
    if (depth > 0 || done) {
        return real(backend);
    }
    new_output.notify = handle_new_output;
    wl_signal_add(&backend->events.new_output, &new_output);
    depth++;
    bool ok = real(backend);
    depth--;
    done = 1;
    wl_list_remove(&new_output.link);
    dir = getenv("XDG_RUNTIME_DIR");
    wlr_keyboard_init(&kb, &kb_impl, "test-kb");
    wl_signal_emit(&backend->events.new_input, &kb.base);
    timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), on_timer, NULL);
    wl_event_source_timer_update(timer, 50);
    fprintf(stderr, "SHIM: ready\n");
    return ok;
}
