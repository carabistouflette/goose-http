/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#ifndef GH_MAX_CLIENTS
#define GH_MAX_CLIENTS 32
#endif
#ifndef GH_MAX_GENERATION
#define GH_MAX_GENERATION 0x7FFFFFFFFFFFLL
#endif
#if GH_MAX_CLIENTS < 1 || GH_MAX_CLIENTS > 32767
#error "GH_MAX_CLIENTS must fit the client handle index"
#endif

typedef enum { CLIENT_FREE, CLIENT_ACTIVE, CLIENT_BUSY, CLIENT_CLOSING } client_state;
typedef struct {
    uint64_t generation;
    client_state state;
    char *host;
    int64_t port;
    CURL *curl;
} client_slot;

/* The lock protects registry state only. A busy lease exclusively owns the
 * slot's host and easy handle throughout request setup, I/O and cleanup. */
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static client_slot clients[GH_MAX_CLIENTS];

static client_slot *find_client(int64_t id) {
    if (id <= 0) return NULL;
    uint64_t token = (uint64_t)id;
    uint64_t low = token & UINT64_C(0xffff);
    if (!(low & UINT64_C(0x8000))) return NULL;
    uint64_t index = low & UINT64_C(0x7fff);
    uint64_t generation = token >> 16;
    if (!index || index > GH_MAX_CLIENTS || !generation) return NULL;
    client_slot *slot = &clients[index - 1];
    return slot->state == CLIENT_ACTIVE && slot->generation == generation ? slot : NULL;
}

int64_t gh_client_open(gh_input origin, gh_metadata meta) {
    if (meta.len != 1 || !meta.data || !gh_valid_slice(origin.data, origin.len, 8192) ||
        gh_overlap(meta.data, sizeof(int64_t), origin.data, origin.len)) return GH_INVALID;
    meta.data[0] = 0;
    if (gh_ensure_initialized() != 0) return GH_SETUP;
    char *host = NULL;
    int64_t port = 0;
    int result = gh_parse_client_origin(origin, &host, &port);
    if (result != GH_OK) return result;

    int64_t id = 0;
    pthread_mutex_lock(&clients_lock);
    for (size_t i = 0; i < GH_MAX_CLIENTS; ++i) {
        client_slot *slot = &clients[i];
        if (!slot->generation) slot->generation = 1;
        if (slot->state != CLIENT_FREE || slot->generation >= (uint64_t)GH_MAX_GENERATION) continue;
        slot->host = host;
        slot->port = port;
        slot->curl = NULL;
        slot->state = CLIENT_ACTIVE;
        id = (int64_t)((slot->generation << 16) | UINT64_C(0x8000) | (i + 1));
        break;
    }
    pthread_mutex_unlock(&clients_lock);
    if (!id) { curl_free(host); return GH_SETUP; }
    meta.data[0] = id;
    return GH_OK;
}

int64_t gh_client_request(int64_t id, gh_input url, int64_t method, gh_input packed,
                          gh_input upload, int64_t timeout_ms, int64_t allow_http,
                          gh_output body, gh_output headers, gh_metadata meta) {
    /* Validate writable regions before publishing even an invalid-handle result.
     * The engine repeats these checks for its independent one-shot entrypoint. */
    if (meta.len != 4 || !meta.data || !gh_valid_slice(url.data, url.len, 8192) ||
        !gh_valid_slice(packed.data, packed.len, 65536) ||
        !gh_valid_slice(upload.data, upload.len, 1048576) ||
        !gh_valid_slice(body.data, body.len, 16777216) ||
        !gh_valid_slice(headers.data, headers.len, 262144)) return GH_INVALID;
    const void *ptrs[] = {url.data, packed.data, upload.data, body.data, headers.data, meta.data};
    int64_t lens[] = {url.len, packed.len, upload.len, body.len, headers.len, 32};
    for (size_t i = 3; i < 6; ++i)
        for (size_t j = 0; j < i; ++j)
            if (gh_overlap(ptrs[i], lens[i], ptrs[j], lens[j])) return GH_INVALID;
    memset(meta.data, 0, 32);
    if (method != 0 || upload.len != 0) return GH_INVALID;

    pthread_mutex_lock(&clients_lock);
    client_slot *slot = find_client(id);
    if (slot) slot->state = CLIENT_BUSY;
    pthread_mutex_unlock(&clients_lock);
    if (!slot) return GH_INVALID;

    int64_t result = gh_request_engine(&slot->curl, slot->host, slot->port, url,
        method, packed, upload, timeout_ms, allow_http, body, headers, meta);
    /* Also evict failures returned before the engine reached its transfer
     * cleanup path. Invalid input leaves a previously good connection intact. */
    if (result != GH_OK && result != GH_INVALID && slot->curl) {
        curl_easy_cleanup(slot->curl);
        slot->curl = NULL;
    }
    pthread_mutex_lock(&clients_lock);
    slot->state = CLIENT_ACTIVE;
    pthread_mutex_unlock(&clients_lock);
    return result;
}

int64_t gh_client_close(int64_t id) {
    if (!id) return GH_OK;
    pthread_mutex_lock(&clients_lock);
    client_slot *slot = find_client(id);
    if (slot) slot->state = CLIENT_CLOSING;
    pthread_mutex_unlock(&clients_lock);
    if (!slot) return GH_INVALID;

    if (slot->curl) curl_easy_cleanup(slot->curl);
    curl_free(slot->host);
    slot->curl = NULL;
    slot->host = NULL;
    slot->port = 0;
    pthread_mutex_lock(&clients_lock);
    if (slot->generation < (uint64_t)GH_MAX_GENERATION) ++slot->generation;
    slot->state = CLIENT_FREE;
    pthread_mutex_unlock(&clients_lock);
    return GH_OK;
}
