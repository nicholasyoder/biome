// SPDX-License-Identifier: LGPL-3.0-or-later

#include "desktop/ext_workspace.h"

#include "desktop/workspace.h"

#include "ext-workspace-v1-protocol.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// Matches the opaque forward declaration of BiomeServer::ext_workspace in
// core/server.h - defined at file scope (not inside the anonymous
// namespace below) so it's the same type as that field's pointee.
struct BiomeExtWorkspace {
    BiomeServer *server = nullptr;
    wl_global *global = nullptr;
    wl_list clients = {};
};

namespace {

constexpr uint32_t kExtWorkspaceVersion = 1;

struct BiomeExtWorkspaceClient;

// Per ext_workspace_handle_v1 resource - just enough to route a request
// back to the owning client's bookkeeping and know which index it is.
struct BiomeExtWorkspaceHandle {
    BiomeExtWorkspaceClient *client = nullptr;
    int index = 0;
};

// One of these per client that has bound ext_workspace_manager_v1. Freed
// via reference counting (live_resources) from each of its own resources'
// destroy callbacks rather than from a single wl_client destroy listener -
// wl_client_destroy() fires that signal *before* tearing down the client's
// own resources (manager/group/workspace handles), so a listener that
// deletes this struct runs too early and left every resource destructor
// use-after-freeing it on a disconnecting client (e.g. one killed by
// SIGINT, which tears everything down in one wl_client_destroy() call
// instead of one resource at a time). libwayland also doesn't guarantee an
// order among the resources themselves, hence the refcount rather than
// picking one resource to own the delete.
struct BiomeExtWorkspaceClient {
    BiomeServer *server = nullptr;
    wl_resource *manager_resource = nullptr;
    wl_resource *group_resource = nullptr;
    // Index-aligned with [0, server->workspace_count); entries are
    // null'd by workspace_handle_resource_destroy() if the client destroys
    // one early (via its own `destroy` request) instead of disconnecting
    // outright, so sync_active() never touches a freed resource.
    std::vector<wl_resource *> workspace_resources;
    // Set by the `activate` request, applied and cleared on the manager's
    // `commit` request - see ext-workspace-v1's commit semantics in this
    // file's header comment. -1 means nothing pending.
    int pending_activate = -1;
    wl_list link = {};
    // manager_resource + group_resource + one per workspace_resources entry -
    // see release_client_resource().
    int live_resources = 0;
};

// Called from each of a client's own resource destroy callbacks
// (manager/group/workspace handle). Only the last one to fire actually
// frees `client` and unlinks it from ext_workspace->clients - see
// BiomeExtWorkspaceClient's doc comment for why no single resource can own
// this unconditionally.
void release_client_resource(BiomeExtWorkspaceClient *client) {
    if (--client->live_resources == 0) {
        wl_list_remove(&client->link);
        delete client;
    }
}

void resource_handle_destroy(wl_client *client, wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

// --- ext_workspace_handle_v1 -------------------------------------------

void workspace_handle_resource_destroy(wl_resource *resource) {
    auto *handle = static_cast<BiomeExtWorkspaceHandle *>(wl_resource_get_user_data(resource));
    BiomeExtWorkspaceClient *client = handle->client;
    client->workspace_resources[handle->index] = nullptr;
    delete handle;
    release_client_resource(client);
}

void workspace_handle_activate(wl_client *client, wl_resource *resource) {
    (void)client;
    auto *handle = static_cast<BiomeExtWorkspaceHandle *>(wl_resource_get_user_data(resource));
    handle->client->pending_activate = handle->index;
}

// deactivate/assign/remove are all unsupported (no corresponding
// capability bit is ever advertised - see this file's header comment) -
// per spec, "The compositor will ignore requests it doesn't support", so a
// well-behaved client won't call these, and a misbehaving one is simply a
// no-op rather than an error.
void workspace_handle_deactivate(wl_client *, wl_resource *) {}
void workspace_handle_assign(wl_client *, wl_resource *, wl_resource *) {}
void workspace_handle_remove(wl_client *, wl_resource *) {}

const struct ext_workspace_handle_v1_interface workspace_handle_impl = {
    .destroy = resource_handle_destroy,
    .activate = workspace_handle_activate,
    .deactivate = workspace_handle_deactivate,
    .assign = workspace_handle_assign,
    .remove = workspace_handle_remove,
};

// --- ext_workspace_group_handle_v1 --------------------------------------

void group_handle_resource_destroy(wl_resource *resource) {
    auto *client = static_cast<BiomeExtWorkspaceClient *>(wl_resource_get_user_data(resource));
    client->group_resource = nullptr;
    release_client_resource(client);
}

// Unsupported - no create_workspace capability is ever advertised (Biome's
// workspace set is fixed, nothing dynamically creates one).
void group_handle_create_workspace(wl_client *, wl_resource *, const char *) {}

const struct ext_workspace_group_handle_v1_interface group_handle_impl = {
    .create_workspace = group_handle_create_workspace,
    .destroy = resource_handle_destroy,
};

// --- ext_workspace_manager_v1 -------------------------------------------

void manager_handle_commit(wl_client *client, wl_resource *resource) {
    (void)client;
    auto *c = static_cast<BiomeExtWorkspaceClient *>(wl_resource_get_user_data(resource));
    if (c->pending_activate >= 0) {
        int index = c->pending_activate;
        c->pending_activate = -1;
        switch_workspace(c->server, index);
    }
}

// Biome never creates new workspaces/groups after the initial burst, so
// there is truthfully nothing left to announce - immediately finishing is
// a spec-compliant simplification of "the compositor may emit further
// workspace events, until the finished event is emitted". Nulls out
// manager_resource before destroying it so a sync_active() racing with
// this (e.g. triggered by another client's activate) doesn't send `done`
// to a freed resource.
void manager_handle_stop(wl_client *client, wl_resource *resource) {
    (void)client;
    auto *c = static_cast<BiomeExtWorkspaceClient *>(wl_resource_get_user_data(resource));
    c->manager_resource = nullptr;
    ext_workspace_manager_v1_send_finished(resource);
    // Triggers manager_handle_resource_destroy below (wl_resource_destroy()
    // always runs the resource's destroy callback, explicit call or not),
    // which is what actually releases this client's share of live_resources.
    wl_resource_destroy(resource);
}

const struct ext_workspace_manager_v1_interface manager_impl = {
    .commit = manager_handle_commit,
    .stop = manager_handle_stop,
};

// manager_resource's own share of live_resources - see
// BiomeExtWorkspaceClient's doc comment. Reached either via manager_handle_stop
// above or, on a disconnecting client, directly from wl_client_destroy().
void manager_handle_resource_destroy(wl_resource *resource) {
    auto *client = static_cast<BiomeExtWorkspaceClient *>(wl_resource_get_user_data(resource));
    client->manager_resource = nullptr;
    release_client_resource(client);
}

void manager_bind(wl_client *wl_client_ptr, void *data, uint32_t version, uint32_t id) {
    auto *ext_workspace = static_cast<BiomeExtWorkspace *>(data);
    BiomeServer *server = ext_workspace->server;

    auto *c = new BiomeExtWorkspaceClient();
    c->server = server;
    c->workspace_resources.resize(server->workspace_count, nullptr);
    // manager + group + one per workspace handle - see release_client_resource().
    c->live_resources = 2 + server->workspace_count;

    c->manager_resource = wl_resource_create(wl_client_ptr, &ext_workspace_manager_v1_interface, version, id);
    if (c->manager_resource == nullptr) {
        wl_client_post_no_memory(wl_client_ptr);
        delete c;
        return;
    }
    wl_resource_set_implementation(c->manager_resource, &manager_impl, c, manager_handle_resource_destroy);

    c->group_resource = wl_resource_create(wl_client_ptr, &ext_workspace_group_handle_v1_interface, version, 0);
    wl_resource_set_implementation(c->group_resource, &group_handle_impl, c, group_handle_resource_destroy);
    ext_workspace_manager_v1_send_workspace_group(c->manager_resource, c->group_resource);
    // No output_enter sent and no create_workspace capability - see this
    // file's header comment (Biome's workspace model is global, not
    // per-output, and nothing dynamically creates a workspace).
    ext_workspace_group_handle_v1_send_capabilities(c->group_resource, 0);

    for (int index = 0; index < server->workspace_count; index++) {
        wl_resource *handle_resource =
            wl_resource_create(wl_client_ptr, &ext_workspace_handle_v1_interface, version, 0);
        auto *handle = new BiomeExtWorkspaceHandle();
        handle->client = c;
        handle->index = index;
        wl_resource_set_implementation(
            handle_resource, &workspace_handle_impl, handle, workspace_handle_resource_destroy);
        c->workspace_resources[index] = handle_resource;

        ext_workspace_manager_v1_send_workspace(c->manager_resource, handle_resource);
        ext_workspace_group_handle_v1_send_workspace_enter(c->group_resource, handle_resource);

        const std::string name = std::to_string(index + 1);
        ext_workspace_handle_v1_send_name(handle_resource, name.c_str());
        uint32_t state = index == server->active_workspace ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0;
        ext_workspace_handle_v1_send_state(handle_resource, state);
        ext_workspace_handle_v1_send_capabilities(
            handle_resource, EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
    }

    ext_workspace_manager_v1_send_done(c->manager_resource);

    wl_list_insert(&ext_workspace->clients, &c->link);
}

} // namespace

void ext_workspace_init(BiomeServer *server) {
    auto *ext_workspace = new BiomeExtWorkspace();
    ext_workspace->server = server;
    wl_list_init(&ext_workspace->clients);
    ext_workspace->global = wl_global_create(
        server->display, &ext_workspace_manager_v1_interface, kExtWorkspaceVersion, ext_workspace, manager_bind);
    server->ext_workspace = ext_workspace;
}

void ext_workspace_sync_active(BiomeServer *server) {
    if (server->ext_workspace == nullptr) {
        return;
    }
    BiomeExtWorkspaceClient *c;
    wl_list_for_each(c, &server->ext_workspace->clients, link) {
        if (c->manager_resource == nullptr) {
            // stop was already called on this client - see
            // manager_handle_stop's comment.
            continue;
        }
        for (int index = 0; index < static_cast<int>(c->workspace_resources.size()); index++) {
            wl_resource *resource = c->workspace_resources[index];
            if (resource == nullptr) {
                continue;
            }
            uint32_t state = index == server->active_workspace ? EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE : 0;
            ext_workspace_handle_v1_send_state(resource, state);
        }
        ext_workspace_manager_v1_send_done(c->manager_resource);
    }
}
