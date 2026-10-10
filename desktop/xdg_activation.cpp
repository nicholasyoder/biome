// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/xdg_activation.h"

#include "desktop/toplevel.h"

namespace {

struct BiomeActivationToken {
    // The token's seat serial belongs to the client that got the last press.
    bool fresh_at_issue = false;
    uint64_t press_recipient_changes = 0;
    wl_listener destroy = {};
};

void handle_token_destroy(wl_listener *listener, void *data) {
    (void)data;
    BiomeActivationToken *state = wl_container_of(listener, state, destroy);
    wl_list_remove(&state->destroy.link);
    delete state;
}

void handle_new_token(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, xdg_activation_new_token);
    auto *token = static_cast<wlr_xdg_activation_token_v1 *>(data);

    auto *state = new BiomeActivationToken();
    state->press_recipient_changes = server->press_recipient_changes;
    // wlroots already checked the serial was sent to the requester; serials
    // are per-client, so this asks "is the requester the last press's recipient".
    if (token->seat != nullptr && server->last_press_recipient != nullptr) {
        wlr_seat_client *seat_client = wlr_seat_client_for_wl_client(token->seat, server->last_press_recipient);
        state->fresh_at_issue =
            seat_client != nullptr && wlr_seat_client_validate_event_serial(seat_client, token->serial);
    }
    state->destroy.notify = handle_token_destroy;
    wl_signal_add(&token->events.destroy, &state->destroy);
    token->data = state;
}

void handle_request_activate(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, xdg_activation_request_activate);
    auto *event = static_cast<wlr_xdg_activation_v1_request_activate_event *>(data);

    // Unmapped: map-time focus already covers new windows.
    BiomeToplevel *toplevel = toplevel_from_xdg(wlr_xdg_toplevel_try_from_wlr_surface(event->surface));
    if (toplevel == nullptr || !event->surface->mapped) {
        return;
    }

    auto *state = static_cast<BiomeActivationToken *>(event->token->data);
    bool honored = state != nullptr && state->fresh_at_issue &&
        state->press_recipient_changes == server->press_recipient_changes && !server->session_locked;
    if (honored) {
        activate_toplevel(toplevel);
    } else {
        set_toplevel_urgent(toplevel, true);
    }
}

} // namespace

void xdg_activation_init(BiomeServer *server) {
    server->xdg_activation = wlr_xdg_activation_v1_create(server->display);
    server->xdg_activation_new_token.notify = handle_new_token;
    wl_signal_add(&server->xdg_activation->events.new_token, &server->xdg_activation_new_token);
    server->xdg_activation_request_activate.notify = handle_request_activate;
    wl_signal_add(&server->xdg_activation->events.request_activate, &server->xdg_activation_request_activate);
}

void xdg_activation_note_press(BiomeServer *server, wl_client *recipient) {
    if (recipient != server->last_press_recipient || recipient == nullptr) {
        server->press_recipient_changes++;
        server->last_press_recipient = recipient;
    }
}
