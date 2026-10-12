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

void attach_state(BiomeServer *server, wlr_xdg_activation_token_v1 *token, bool fresh_at_issue) {
    auto *state = new BiomeActivationToken();
    state->fresh_at_issue = fresh_at_issue;
    state->press_recipient_changes = server->press_recipient_changes;
    state->destroy.notify = handle_token_destroy;
    wl_signal_add(&token->events.destroy, &state->destroy);
    token->data = state;
}

void handle_new_token(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, xdg_activation_new_token);
    auto *token = static_cast<wlr_xdg_activation_token_v1 *>(data);

    // wlroots already checked the serial was sent to the requester; serials
    // are per-client, so this asks "is the requester the last press's recipient".
    bool fresh = false;
    if (token->seat != nullptr && server->last_press_recipient != nullptr) {
        wlr_seat_client *seat_client = wlr_seat_client_for_wl_client(token->seat, server->last_press_recipient);
        fresh = seat_client != nullptr && wlr_seat_client_validate_event_serial(seat_client, token->serial);
    }
    attach_state(server, token, fresh);
}

void handle_request_activate(wl_listener *listener, void *data) {
    BiomeServer *server = wl_container_of(listener, server, xdg_activation_request_activate);
    auto *event = static_cast<wlr_xdg_activation_v1_request_activate_event *>(data);

    auto *state = static_cast<BiomeActivationToken *>(event->token->data);
    bool honored = state != nullptr && state->fresh_at_issue &&
        state->press_recipient_changes == server->press_recipient_changes && !server->session_locked;

    // A popup can't be raised or marked urgent; an honored one just gets focus,
    // deferred to map if needed (xdg_popup_map).
    if (wlr_xdg_popup_try_from_wlr_surface(event->surface) != nullptr) {
        if (!honored) {
            return;
        }
        if (event->surface->mapped) {
            grant_keyboard_focus_to_non_toplevel(server, event->surface);
        } else {
            server->activated_unmapped_popup = event->surface;
        }
        return;
    }

    // Unmapped: map-time focus already covers new windows.
    BiomeToplevel *toplevel = toplevel_from_xdg(wlr_xdg_toplevel_try_from_wlr_surface(event->surface));
    if (toplevel == nullptr || !event->surface->mapped) {
        return;
    }

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

std::string xdg_activation_issue_token(BiomeServer *server) {
    wlr_xdg_activation_token_v1 *token = wlr_xdg_activation_token_v1_create(server->xdg_activation);
    if (token == nullptr) {
        return {};
    }
    attach_state(server, token, true);
    return wlr_xdg_activation_token_v1_get_name(token);
}

void xdg_activation_note_press(BiomeServer *server, wl_client *recipient) {
    if (recipient != server->last_press_recipient || recipient == nullptr) {
        server->press_recipient_changes++;
        server->last_press_recipient = recipient;
    }
}
