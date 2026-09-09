// SPDX-License-Identifier: LGPL-3.0-or-later
//
// See qt_glib_bridge.h for what this is and why. Implementation follows
// docs/qt-event-loop-integration-research.md's "Core algorithm" section -
// see that doc for the reasoning behind each piece, especially the
// resync-every-cycle in rearm() (skipping it is exactly how a D-Bus reply
// can silently never dispatch) and the "acquire, never release" choice below.

#include "core/qt_glib_bridge.h"

#include "core/server.h"
#include "wlroots.hpp"

#include <glib.h>

#include <ctime>
#include <unordered_map>
#include <vector>

namespace {

GMainContext *g_context = nullptr;
gint g_priority = 0;
std::vector<GPollFD> g_poll_fds;
guint g_n_fds = 0;
std::unordered_map<int, wl_event_source *> g_fd_sources;
wl_event_source *g_timeout_source = nullptr;
wl_event_loop *g_loop = nullptr;

// Diagnostics - same cadence/level as the polling pump this replaces, and
// the practical safety net for the livelock risk noted in the research doc:
// a spike in rearm cycles with no matching wall-clock time passing would
// show up here immediately, rather than manifesting only as "compositor
// seems frozen".
uint64_t g_rearm_cycles_since_log = 0;
uint64_t g_pump_calls_since_log = 0;
uint64_t g_pump_dispatched_since_log = 0;
timespec g_last_log{};

// Livelock guard: if some GLib/Qt source's prepare() reports "ready right
// now" on every single cycle (not expected in normal Qt/QtDBus usage - see
// docs/qt-event-loop-integration-research.md's "Livelock" risk), rearm()'s
// inner loop would otherwise spin forever without returning control to
// wl_event_loop_dispatch(). That doesn't just waste CPU: while stuck inside
// this callback, wl_display_run()'s own outer `while (display->run)` loop
// never gets back around to recheck the terminate flag either, so a fatal
// backend condition (nested-backend host window closing, a DRM/libinput
// failure) that calls wl_display_terminate() from a *different* fd's
// callback in the same dispatch batch goes unnoticed and Biome hangs
// instead of exiting. Capping iterations and deferring the rest via an idle
// source (rather than looping unboundedly) keeps control flowing back
// through wl_event_loop_dispatch() regularly so termination and every other
// subsystem still get a turn.
constexpr int kRearmLivelockGuard = 256;

void rearm();
void pump();

uint32_t mask_from_glib_events(gushort events) {
    uint32_t mask = 0;
    if (events & G_IO_IN) {
        mask |= WL_EVENT_READABLE;
    }
    if (events & G_IO_OUT) {
        mask |= WL_EVENT_WRITABLE;
    }
    // HANGUP/ERROR aren't requested explicitly - poll()/epoll report them in
    // revents regardless of the requested mask, and g_poll() in pump() below
    // picks them up the same way, so there's nothing to opt into here.
    return mask;
}

void maybe_log() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - g_last_log.tv_sec < 10) {
        return;
    }
    wlr_log(WLR_DEBUG,
        "qt-glib-bridge: %llu rearm cycles, %llu pump() calls (%llu with dispatch), "
        "%zu fd sources, over last ~10s",
        static_cast<unsigned long long>(g_rearm_cycles_since_log),
        static_cast<unsigned long long>(g_pump_calls_since_log),
        static_cast<unsigned long long>(g_pump_dispatched_since_log), g_fd_sources.size());
    g_rearm_cycles_since_log = 0;
    g_pump_calls_since_log = 0;
    g_pump_dispatched_since_log = 0;
    g_last_log = now;
}

int on_fd_ready(int fd, uint32_t mask, void *data) {
    (void)fd;
    (void)mask;
    (void)data;
    pump();
    rearm();
    return 0;
}

int on_timeout(void *data) {
    (void)data;
    pump();
    rearm();
    return 0;
}

// Diffs g_fd_sources against the fd set from the most recent query() and
// adds/updates/removes wl_event_loop fd sources to match.
void sync_fd_sources() {
    std::unordered_map<int, uint32_t> new_fds;
    new_fds.reserve(g_n_fds);
    for (guint i = 0; i < g_n_fds; i++) {
        new_fds[g_poll_fds[i].fd] = mask_from_glib_events(g_poll_fds[i].events);
    }

    for (auto &[fd, mask] : new_fds) {
        auto it = g_fd_sources.find(fd);
        if (it == g_fd_sources.end()) {
            g_fd_sources[fd] = wl_event_loop_add_fd(g_loop, fd, mask, on_fd_ready, nullptr);
        } else {
            wl_event_source_fd_update(it->second, mask);
        }
    }

    for (auto it = g_fd_sources.begin(); it != g_fd_sources.end();) {
        if (new_fds.find(it->first) == new_fds.end()) {
            wl_event_source_remove(it->second);
            it = g_fd_sources.erase(it);
        } else {
            ++it;
        }
    }
}

void arm_timeout(gint timeout_ms) {
    // 0 disarms (standard libwayland contract, same as an itimerspec of
    // zero) - used whenever query() reports timeout_ms == -1 ("block
    // indefinitely," i.e. nothing time-based pending, only fd-driven).
    wl_event_source_timer_update(g_timeout_source, timeout_ms < 0 ? 0 : timeout_ms);
}

void pump() {
    g_pump_calls_since_log++;
    g_poll(g_poll_fds.data(), g_n_fds, 0);
    if (g_main_context_check(g_context, g_priority, g_poll_fds.data(), static_cast<gint>(g_n_fds))) {
        g_pump_dispatched_since_log++;
        g_main_context_dispatch(g_context);
    }
}

void continue_rearm_via_idle(void *data) {
    (void)data;
    rearm();
}

void rearm() {
    for (int iterations = 0;; iterations++) {
        g_rearm_cycles_since_log++;
        maybe_log();

        if (iterations >= kRearmLivelockGuard) {
            wlr_log(WLR_ERROR,
                "qt-glib-bridge: rearm() hit its %d-iteration livelock guard - a GLib/Qt "
                "source's prepare() is reporting ready every cycle; deferring the rest via "
                "an idle source instead of spinning (see docs/qt-event-loop-integration-"
                "research.md's Livelock risk)",
                kRearmLivelockGuard);
            wl_event_loop_add_idle(g_loop, continue_rearm_via_idle, nullptr);
            return;
        }

        gboolean immediate = g_main_context_prepare(g_context, &g_priority);

        gint timeout_ms = 0;
        for (;;) {
            gint n = g_main_context_query(
                g_context, g_priority, &timeout_ms, g_poll_fds.data(), static_cast<gint>(g_poll_fds.size()));
            if (n <= static_cast<gint>(g_poll_fds.size())) {
                g_n_fds = static_cast<guint>(n);
                break;
            }
            g_poll_fds.resize(n);
        }

        sync_fd_sources();
        arm_timeout(timeout_ms);

        if (!immediate) {
            return; // nothing ready now; wait for a registered fd/timer to fire
        }
        // Something's ready right now - dispatch it, then loop back to
        // re-prepare, since dispatch can change what's pending next.
        pump();
    }
}

} // namespace

void qt_glib_bridge_init(BiomeServer *server) {
    g_context = g_main_context_default();
    g_loop = wl_display_get_event_loop(server->display);
    g_timeout_source = wl_event_loop_add_timer(g_loop, on_timeout, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &g_last_log);

    if (!g_main_context_acquire(g_context)) {
        // Single-threaded process, called once at startup before anything
        // else touches g_main_context_default() - this should never fail.
        // If it somehow does, log loudly and bail rather than silently
        // running degraded; there's no fallback path.
        wlr_log(WLR_ERROR,
            "qt_glib_bridge: failed to acquire GMainContext - Qt/D-Bus dispatch will not work");
        return;
    }

    rearm();
}
