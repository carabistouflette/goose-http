/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <time.h>

#ifndef GH_MAX_STREAMS
#define GH_MAX_STREAMS 32
#endif
#ifndef GH_MAX_GENERATION
#define GH_MAX_GENERATION 0x7FFFFFFFFFFFLL
#endif

typedef enum {
    GH_SLOT_FREE = 0,
    GH_SLOT_RESERVED = 1,
    GH_SLOT_ACTIVE = 2,
    GH_SLOT_CLOSING = 3
} gh_slot_state;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t count;
} gh_ring;

static bool gh_ring_init(gh_ring *r, size_t capacity) {
    r->data = malloc(capacity);
    if (!r->data) {
        r->capacity = 0;
        r->head = 0;
        r->count = 0;
        return false;
    }
    r->capacity = capacity;
    r->head = 0;
    r->count = 0;
    return true;
}

static void gh_ring_free(gh_ring *r) {
    free(r->data);
    r->data = NULL;
    r->capacity = 0;
    r->head = 0;
    r->count = 0;
}

static size_t gh_ring_space(const gh_ring *r) {
    return r->capacity - r->count;
}

static size_t gh_ring_write(gh_ring *r, const uint8_t *src, size_t n) {
    if (n > r->capacity - r->count) return 0;
    size_t tail = (r->head + r->count) % r->capacity;
    size_t first = r->capacity - tail;
    if (n <= first) {
        memcpy(r->data + tail, src, n);
    } else {
        memcpy(r->data + tail, src, first);
        memcpy(r->data, src + first, n - first);
    }
    r->count += n;
    return n;
}

static size_t gh_ring_read(gh_ring *r, uint8_t *dst, size_t n) {
    if (n > r->count) n = r->count;
    if (!n) return 0;
    size_t first = r->capacity - r->head;
    if (n <= first) {
        memcpy(dst, r->data + r->head, n);
    } else {
        memcpy(dst, r->data + r->head, first);
        memcpy(dst + first, r->data, n - first);
    }
    r->head = (r->head + n) % r->capacity;
    r->count -= n;
    return n;
}

static int64_t gh_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (ts.tv_nsec / 1000000);
}

typedef struct {
    pthread_mutex_t lock;
    uint64_t generation;
    gh_slot_state state;

    CURL *curl;
    CURLM *multi;
    struct curl_slist *headers_list;

    int64_t timeout_ms;
    int64_t connect_timeout_ms;
    int64_t chunk_limit;
    int64_t body_limit;
    int64_t header_limit;

    int64_t start_time_ms;
    int64_t total_bytes;
    int64_t status_code;
    CURLcode last_curl_code;

    uint8_t *header_buf;
    size_t header_used;
    bool header_started;
    bool header_complete;
    bool header_exceeded;

    gh_ring ring;
    bool body_exceeded;
    bool is_paused;

    bool transfer_done;
    bool transfer_error;
    int64_t error_code;
    bool cancelled;
} gh_stream_slot;

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static gh_stream_slot registry[GH_MAX_STREAMS];
static pthread_once_t registry_once = PTHREAD_ONCE_INIT;

static void init_registry(void) {
    for (size_t i = 0; i < GH_MAX_STREAMS; ++i) {
        pthread_mutex_init(&registry[i].lock, NULL);
        registry[i].generation = 1;
        registry[i].state = GH_SLOT_FREE;
    }
}

static void bump_generation(gh_stream_slot *slot) {
    if (slot->generation < (uint64_t)GH_MAX_GENERATION) {
        slot->generation++;
    }
}

static int64_t make_handle_id(size_t slot_idx, uint64_t generation) {
    uint64_t hid = (generation << 16) | ((uint64_t)(slot_idx + 1) & 0xFFFF);
    return (int64_t)hid;
}

static bool parse_handle_id(int64_t handle_id, size_t *slot_idx, uint64_t *generation) {
    if (handle_id <= 0) return false;
    uint64_t hid = (uint64_t)handle_id;
    uint64_t idx_plus_one = hid & 0xFFFF;
    if (!idx_plus_one || idx_plus_one > GH_MAX_STREAMS) return false;
    *slot_idx = (size_t)(idx_plus_one - 1);
    *generation = hid >> 16;
    return true;
}

static size_t stream_header_callback(char *data, size_t size, size_t count, void *opaque) {
    gh_stream_slot *slot = opaque;
    if (size && count > SIZE_MAX / size) { slot->header_exceeded = true; return 0; }
    size_t n = size * count;
    if (n > (size_t)slot->header_limit - slot->header_used) {
        slot->header_exceeded = true;
        return 0;
    }
    if (n) {
        memcpy(slot->header_buf + slot->header_used, data, n);
        slot->header_used += n;
    }
    if (n >= 5 && gh_equal_name((const uint8_t *)data, 5, "http/")) {
        slot->header_started = true;
        slot->header_complete = false;
    } else if (slot->header_started &&
               ((n == 2 && data[0] == '\r' && data[1] == '\n') || (n == 1 && data[0] == '\n'))) {
        slot->header_complete = true;
        long status = 0;
        if (curl_easy_getinfo(slot->curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK) {
            slot->status_code = status;
        }
    }
    return n;
}

static size_t stream_write_callback(char *data, size_t size, size_t count, void *opaque) {
    gh_stream_slot *slot = opaque;
    if (size && count > SIZE_MAX / size) { slot->body_exceeded = true; return 0; }
    size_t n = size * count;

    if ((int64_t)n > slot->body_limit - (slot->total_bytes + (int64_t)slot->ring.count)) {
        slot->body_exceeded = true;
        return 0;
    }

    size_t space = gh_ring_space(&slot->ring);
    if (n > space) {
        if (slot->ring.count > 0) {
            slot->is_paused = true;
            return CURL_WRITEFUNC_PAUSE;
        }
        slot->body_exceeded = true;
        return 0;
    }

    gh_ring_write(&slot->ring, (const uint8_t *)data, n);
    return n;
}

int64_t gh_stream_open(gh_input url, gh_input packed, int64_t timeout_ms,
                       int64_t connect_timeout_ms, int64_t chunk_limit,
                       int64_t body_limit, int64_t header_limit,
                       int64_t allow_http, gh_metadata meta) {
    if (meta.len != 3 || !meta.data) return GH_INVALID;

    if (!gh_valid_slice(url.data, url.len, 8192) ||
        !gh_valid_slice(packed.data, packed.len, 65536) ||
        url.len == 0 ||
        timeout_ms < 1 || timeout_ms > 86400000 ||
        connect_timeout_ms < 1 || connect_timeout_ms > timeout_ms ||
        chunk_limit < 1 || chunk_limit > 1048576 ||
        body_limit < 0 ||
        header_limit < 0 || header_limit > 262144 ||
        allow_http < 0 || allow_http > 1) return GH_INVALID;

    if (gh_overlap(meta.data, meta.len * sizeof(int64_t), url.data, url.len) ||
        gh_overlap(meta.data, meta.len * sizeof(int64_t), packed.data, packed.len)) return GH_INVALID;

    memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));

    for (int64_t i = 0; i < url.len; ++i)
        if (url.data[i] <= 32 || url.data[i] >= 127 || url.data[i] == '#') return GH_INVALID;

    bool https = url.len >= 8 && !memcmp(url.data, "https://", 8);
    bool http = url.len >= 7 && !memcmp(url.data, "http://", 7);
    if (!https && !(http && allow_http)) return GH_INVALID;

    int64_t authority = https ? 8 : 7;
    if (url.len <= authority || url.data[authority] == '/' || url.data[authority] == '?') return GH_INVALID;

    char address[8193];
    memcpy(address, url.data, (size_t)url.len);
    address[url.len] = 0;

    size_t pos = 0, count = 0, wire_bytes = 0;
    while (pos < (size_t)packed.len) {
        const uint8_t *end = memchr(packed.data + pos, 0, (size_t)packed.len - pos);
        if (!end) return GH_INVALID;
        size_t n = (size_t)(end - packed.data - pos);
        if (++count > 64 || !gh_valid_header(packed.data + pos, n)) return GH_INVALID;
        wire_bytes += n + 2;
        if (wire_bytes > 65536) return GH_INVALID;
        pos += n + 1;
    }

    if (gh_ensure_initialized() != 0) return GH_SETUP;
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    if (version->version_num < 0x075500 || !(version->features & CURL_VERSION_SSL) ||
        !(version->features & CURL_VERSION_ASYNCHDNS) || !(version->features & CURL_VERSION_THREADSAFE))
        return GH_SETUP;

    CURLU *parsed = curl_url();
    if (!parsed) return GH_SETUP;
    CURLUcode uc = curl_url_set(parsed, CURLUPART_URL, address, CURLU_DISALLOW_USER);
    char *host = NULL;
    if (uc == CURLUE_OK) uc = curl_url_get(parsed, CURLUPART_HOST, &host, 0);
    bool valid = uc == CURLUE_OK && host && *host;
    curl_free(host);
    curl_url_cleanup(parsed);
    if (uc == CURLUE_OUT_OF_MEMORY) return GH_SETUP;
    if (!valid) return GH_INVALID;

    pthread_once(&registry_once, init_registry);

    pthread_mutex_lock(&registry_lock);
    int slot_idx = -1;
    for (size_t i = 0; i < GH_MAX_STREAMS; ++i) {
        if (registry[i].state == GH_SLOT_FREE && registry[i].generation < (uint64_t)GH_MAX_GENERATION) {
            slot_idx = (int)i;
            registry[i].state = GH_SLOT_RESERVED;
            break;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    if (slot_idx < 0) return GH_SETUP;

    gh_stream_slot *slot = &registry[slot_idx];
    pthread_mutex_lock(&slot->lock);

    slot->curl = curl_easy_init();
    slot->multi = curl_multi_init();
    if (!slot->curl || !slot->multi) {
        if (slot->curl) { curl_easy_cleanup(slot->curl); slot->curl = NULL; }
        if (slot->multi) { curl_multi_cleanup(slot->multi); slot->multi = NULL; }
        pthread_mutex_lock(&registry_lock);
        bump_generation(slot);
        slot->state = GH_SLOT_FREE;
        pthread_mutex_unlock(&registry_lock);
        pthread_mutex_unlock(&slot->lock);
        return GH_SETUP;
    }

    size_t ring_cap = (size_t)chunk_limit * 2;
    if (ring_cap < 131072) ring_cap = 131072;
    if (!gh_ring_init(&slot->ring, ring_cap)) {
        curl_easy_cleanup(slot->curl); slot->curl = NULL;
        curl_multi_cleanup(slot->multi); slot->multi = NULL;
        pthread_mutex_lock(&registry_lock);
        bump_generation(slot);
        slot->state = GH_SLOT_FREE;
        pthread_mutex_unlock(&registry_lock);
        pthread_mutex_unlock(&slot->lock);
        return GH_SETUP;
    }

    slot->header_buf = malloc(header_limit > 0 ? (size_t)header_limit : 1);
    if (!slot->header_buf) {
        gh_ring_free(&slot->ring);
        curl_easy_cleanup(slot->curl); slot->curl = NULL;
        curl_multi_cleanup(slot->multi); slot->multi = NULL;
        pthread_mutex_lock(&registry_lock);
        bump_generation(slot);
        slot->state = GH_SLOT_FREE;
        pthread_mutex_unlock(&registry_lock);
        pthread_mutex_unlock(&slot->lock);
        return GH_SETUP;
    }

    slot->headers_list = NULL;
    struct curl_slist *tail = NULL;
    for (pos = 0; pos < (size_t)packed.len;) {
        const char *line = (const char *)packed.data + pos;
        size_t length = strlen(line);
        const char *colon = strchr(line, ':');
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t') ++value;
        const char *wire = line;
        char *empty_line = NULL;
        if (!*value) {
            size_t name_length = (size_t)(colon - line);
            empty_line = malloc(name_length + 2);
            if (!empty_line) goto fail_setup;
            memcpy(empty_line, line, name_length);
            empty_line[name_length] = ';'; empty_line[name_length + 1] = 0;
            wire = empty_line;
        }
        struct curl_slist *next = curl_slist_append(tail, wire);
        free(empty_line);
        if (!next) goto fail_setup;
        if (!slot->headers_list) slot->headers_list = next;
        tail = tail ? tail->next : next;
        pos += length + 1;
    }

    struct curl_slist *next = curl_slist_append(tail, "Expect:");
    if (!next) goto fail_setup;
    if (!slot->headers_list) slot->headers_list = next;
    tail = tail ? tail->next : next;

    next = curl_slist_append(tail, "Accept-Encoding: identity");
    if (!next) goto fail_setup;
    if (!slot->headers_list) slot->headers_list = next;

    CURL *curl = slot->curl;
#define S_SET(opt, val) if (curl_easy_setopt(curl, opt, val) != CURLE_OK) goto fail_setup;
    S_SET(CURLOPT_URL, address);
    S_SET(CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    S_SET(CURLOPT_PROTOCOLS_STR, allow_http ? "http,https" : "https");
    S_SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    S_SET(CURLOPT_FOLLOWLOCATION, 0L);
    S_SET(CURLOPT_PROXY, "");
    S_SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    S_SET(CURLOPT_SSL_VERIFYPEER, 1L);
    S_SET(CURLOPT_SSL_VERIFYHOST, 2L);
    S_SET(CURLOPT_NOSIGNAL, 1L);
    S_SET(CURLOPT_HTTP_CONTENT_DECODING, 0L);
    S_SET(CURLOPT_CONNECTTIMEOUT_MS, (long)connect_timeout_ms);
    S_SET(CURLOPT_HTTPHEADER, slot->headers_list);
    S_SET(CURLOPT_WRITEFUNCTION, stream_write_callback);
    S_SET(CURLOPT_WRITEDATA, slot);
    S_SET(CURLOPT_HEADERFUNCTION, stream_header_callback);
    S_SET(CURLOPT_HEADERDATA, slot);
#undef S_SET

    if (curl_multi_add_handle(slot->multi, slot->curl) != CURLM_OK) goto fail_setup;

    slot->timeout_ms = timeout_ms;
    slot->connect_timeout_ms = connect_timeout_ms;
    slot->chunk_limit = chunk_limit;
    slot->body_limit = body_limit;
    slot->header_limit = header_limit;
    slot->start_time_ms = gh_now_ms();
    slot->total_bytes = 0;
    slot->status_code = 0;
    slot->last_curl_code = CURLE_OK;
    slot->header_used = 0;
    slot->header_started = false;
    slot->header_complete = false;
    slot->header_exceeded = false;
    slot->body_exceeded = false;
    slot->is_paused = false;
    slot->transfer_done = false;
    slot->transfer_error = false;
    slot->error_code = GH_OK;
    slot->cancelled = false;

    pthread_mutex_lock(&registry_lock);
    slot->state = GH_SLOT_ACTIVE;
    int64_t handle_id = make_handle_id((size_t)slot_idx, slot->generation);
    meta.data[0] = handle_id;
    meta.data[1] = 0;
    meta.data[2] = 0;
    pthread_mutex_unlock(&registry_lock);

    pthread_mutex_unlock(&slot->lock);
    return GH_OK;

fail_setup:
    curl_slist_free_all(slot->headers_list); slot->headers_list = NULL;
    free(slot->header_buf); slot->header_buf = NULL;
    gh_ring_free(&slot->ring);
    if (slot->curl) { curl_easy_cleanup(slot->curl); slot->curl = NULL; }
    if (slot->multi) { curl_multi_cleanup(slot->multi); slot->multi = NULL; }
    pthread_mutex_lock(&registry_lock);
    bump_generation(slot);
    slot->state = GH_SLOT_FREE;
    pthread_mutex_unlock(&registry_lock);
    pthread_mutex_unlock(&slot->lock);
    return GH_SETUP;
}

static void drain_messages(gh_stream_slot *slot) {
    CURLMsg *msg;
    int msgs_left = 0;
    while ((msg = curl_multi_info_read(slot->multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            slot->last_curl_code = msg->data.result;
            if (msg->data.result != CURLE_OK) {
                slot->transfer_error = true;
                if (slot->body_exceeded) {
                    slot->error_code = GH_BODY;
                } else if (slot->header_exceeded) {
                    slot->error_code = GH_HEADERS;
                } else if (msg->data.result == CURLE_OPERATION_TIMEDOUT) {
                    slot->error_code = GH_TIMEOUT;
                } else {
                    slot->error_code = GH_TRANSPORT;
                }
            } else if (!slot->header_complete) {
                slot->transfer_error = true;
                slot->error_code = GH_TRANSPORT;
                slot->last_curl_code = CURLE_PARTIAL_FILE;
            } else {
                slot->transfer_done = true;
            }
        }
    }
}

int64_t gh_stream_step(int64_t handle_id, int64_t step_timeout_ms, gh_metadata meta) {
    if (meta.len != 7 || !meta.data || step_timeout_ms < 0) return 3;

    size_t slot_idx = 0;
    uint64_t generation = 0;
    if (!parse_handle_id(handle_id, &slot_idx, &generation)) {
        memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));
        meta.data[0] = 3;
        meta.data[1] = GH_INVALID;
        return 3;
    }

    pthread_once(&registry_once, init_registry);

    pthread_mutex_lock(&registry_lock);
    gh_stream_slot *slot = &registry[slot_idx];
    if (slot->state != GH_SLOT_ACTIVE || slot->generation != generation) {
        pthread_mutex_unlock(&registry_lock);
        memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));
        meta.data[0] = 3;
        meta.data[1] = GH_INVALID;
        return 3;
    }
    pthread_mutex_lock(&slot->lock);
    pthread_mutex_unlock(&registry_lock);

    memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));

    if (slot->cancelled) {
        meta.data[0] = 4;
        meta.data[1] = GH_OK;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 4;
    }

    if (slot->ring.count > 0) {
        meta.data[0] = 0;
        meta.data[1] = GH_OK;
        meta.data[2] = (slot->ring.count < (size_t)slot->chunk_limit) ? (int64_t)slot->ring.count : slot->chunk_limit;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 0;
    }

    if (slot->transfer_done) {
        meta.data[0] = 2;
        meta.data[1] = GH_OK;
        meta.data[2] = 0;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 2;
    }

    if (slot->transfer_error) {
        meta.data[0] = 3;
        meta.data[1] = slot->error_code;
        meta.data[2] = 0;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 3;
    }

    int64_t now = gh_now_ms();
    int64_t elapsed = now - slot->start_time_ms;
    if (elapsed >= slot->timeout_ms) {
        slot->transfer_error = true;
        slot->error_code = GH_TIMEOUT;
        slot->last_curl_code = CURLE_OPERATION_TIMEDOUT;
        meta.data[0] = 3;
        meta.data[1] = GH_TIMEOUT;
        meta.data[2] = 0;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)CURLE_OPERATION_TIMEDOUT;
        pthread_mutex_unlock(&slot->lock);
        return 3;
    }

    if (slot->is_paused && gh_ring_space(&slot->ring) >= 16384) {
        slot->is_paused = false;
        CURLcode pause_code = curl_easy_pause(slot->curl, CURLPAUSE_CONT);
        if (pause_code != CURLE_OK) {
            slot->transfer_error = true;
            slot->error_code = GH_TRANSPORT;
            slot->last_curl_code = pause_code;
        }
    }

    int64_t total_rem = slot->timeout_ms - elapsed;
    int64_t wait_ms = step_timeout_ms;
    if (wait_ms > total_rem) wait_ms = total_rem;
    if (wait_ms < 0) wait_ms = 0;
    int64_t step_deadline = gh_now_ms() + wait_ms;

    int still_running = 0;
    while (1) {
        CURLMcode mcode = curl_multi_perform(slot->multi, &still_running);
        if (mcode != CURLM_OK) {
            slot->transfer_error = true;
            slot->error_code = GH_TRANSPORT;
            slot->last_curl_code = CURLE_FAILED_INIT;
        }
        drain_messages(slot);

        if (slot->ring.count > 0 || slot->transfer_done || slot->transfer_error) break;
        if (still_running == 0) break;

        int64_t rem = step_deadline - gh_now_ms();
        if (rem <= 0) break;

        int numfds = 0;
        CURLMcode pcode = curl_multi_poll(slot->multi, NULL, 0, (int)rem, &numfds);
        if (pcode != CURLM_OK) {
            slot->transfer_error = true;
            slot->error_code = GH_TRANSPORT;
            slot->last_curl_code = CURLE_FAILED_INIT;
            break;
        }
    }

    if (slot->ring.count > 0) {
        meta.data[0] = 0;
        meta.data[1] = GH_OK;
        meta.data[2] = (slot->ring.count < (size_t)slot->chunk_limit) ? (int64_t)slot->ring.count : slot->chunk_limit;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 0;
    }

    if (slot->transfer_error) {
        meta.data[0] = 3;
        meta.data[1] = slot->error_code;
        meta.data[2] = 0;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 3;
    }

    if (slot->transfer_done) {
        meta.data[0] = 2;
        meta.data[1] = GH_OK;
        meta.data[2] = 0;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)slot->last_curl_code;
        pthread_mutex_unlock(&slot->lock);
        return 2;
    }

    now = gh_now_ms();
    if (now - slot->start_time_ms >= slot->timeout_ms) {
        slot->transfer_error = true;
        slot->error_code = GH_TIMEOUT;
        slot->last_curl_code = CURLE_OPERATION_TIMEDOUT;
        meta.data[0] = 3;
        meta.data[1] = GH_TIMEOUT;
        meta.data[2] = 0;
        meta.data[3] = (int64_t)slot->header_used;
        meta.data[4] = slot->status_code;
        meta.data[5] = slot->total_bytes;
        meta.data[6] = (int64_t)CURLE_OPERATION_TIMEDOUT;
        pthread_mutex_unlock(&slot->lock);
        return 3;
    }

    meta.data[0] = 1;
    meta.data[1] = GH_OK;
    meta.data[2] = 0;
    meta.data[3] = (int64_t)slot->header_used;
    meta.data[4] = slot->status_code;
    meta.data[5] = slot->total_bytes;
    meta.data[6] = 0;
    pthread_mutex_unlock(&slot->lock);
    return 1;
}

int64_t gh_stream_read_data(int64_t handle_id, gh_output chunk, gh_output headers, gh_metadata meta) {
    if (meta.len != 2 || !meta.data) return GH_INVALID;
    if (!gh_valid_slice(chunk.data, chunk.len, 1048576) ||
        !gh_valid_slice(headers.data, headers.len, 262144)) return GH_INVALID;
    if (gh_overlap(chunk.data, chunk.len, headers.data, headers.len) ||
        gh_overlap(chunk.data, chunk.len, meta.data, meta.len * sizeof(int64_t)) ||
        gh_overlap(headers.data, headers.len, meta.data, meta.len * sizeof(int64_t))) return GH_INVALID;

    memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));

    size_t slot_idx = 0;
    uint64_t generation = 0;
    if (!parse_handle_id(handle_id, &slot_idx, &generation)) return GH_INVALID;

    pthread_once(&registry_once, init_registry);

    pthread_mutex_lock(&registry_lock);
    gh_stream_slot *slot = &registry[slot_idx];
    if (slot->state != GH_SLOT_ACTIVE || slot->generation != generation) {
        pthread_mutex_unlock(&registry_lock);
        return GH_INVALID;
    }
    pthread_mutex_lock(&slot->lock);
    pthread_mutex_unlock(&registry_lock);

    size_t read_bytes = gh_ring_read(&slot->ring, chunk.data, (size_t)chunk.len);
    slot->total_bytes += (int64_t)read_bytes;
    meta.data[0] = (int64_t)read_bytes;

    size_t hdr_copy = (size_t)headers.len;
    if (hdr_copy > slot->header_used) hdr_copy = slot->header_used;
    if (hdr_copy > 0) {
        memcpy(headers.data, slot->header_buf, hdr_copy);
    }
    meta.data[1] = (int64_t)hdr_copy;

    if (slot->is_paused && gh_ring_space(&slot->ring) >= 16384) {
        slot->is_paused = false;
        CURLcode pause_code = curl_easy_pause(slot->curl, CURLPAUSE_CONT);
        if (pause_code != CURLE_OK) {
            slot->transfer_error = true;
            slot->error_code = GH_TRANSPORT;
            slot->last_curl_code = pause_code;
        }
    }

    pthread_mutex_unlock(&slot->lock);
    return (read_bytes == (size_t)chunk.len) ? GH_OK : GH_INVALID;
}

int64_t gh_stream_read_headers(int64_t handle_id, gh_output headers, gh_metadata meta) {
    if (meta.len != 2 || !meta.data) return GH_INVALID;
    if (!gh_valid_slice(headers.data, headers.len, 262144)) return GH_INVALID;
    if (gh_overlap(headers.data, headers.len, meta.data, meta.len * sizeof(int64_t))) return GH_INVALID;

    memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));

    size_t slot_idx = 0;
    uint64_t generation = 0;
    if (!parse_handle_id(handle_id, &slot_idx, &generation)) return GH_INVALID;

    pthread_once(&registry_once, init_registry);

    pthread_mutex_lock(&registry_lock);
    gh_stream_slot *slot = &registry[slot_idx];
    if (slot->state != GH_SLOT_ACTIVE || slot->generation != generation) {
        pthread_mutex_unlock(&registry_lock);
        return GH_INVALID;
    }
    pthread_mutex_lock(&slot->lock);
    pthread_mutex_unlock(&registry_lock);

    size_t hdr_copy = (size_t)headers.len;
    if (hdr_copy > slot->header_used) hdr_copy = slot->header_used;
    if (hdr_copy > 0) {
        memcpy(headers.data, slot->header_buf, hdr_copy);
    }
    meta.data[0] = (int64_t)hdr_copy;

    pthread_mutex_unlock(&slot->lock);
    return GH_OK;
}

int64_t gh_stream_close(int64_t handle_id, gh_metadata meta) {
    if (meta.len != 2 || !meta.data) return GH_INVALID;
    memset(meta.data, 0, (size_t)meta.len * sizeof(int64_t));
    if (handle_id == 0) return GH_OK;

    size_t slot_idx = 0;
    uint64_t generation = 0;
    if (!parse_handle_id(handle_id, &slot_idx, &generation)) return GH_INVALID;

    pthread_once(&registry_once, init_registry);

    pthread_mutex_lock(&registry_lock);
    gh_stream_slot *slot = &registry[slot_idx];
    if (slot->state != GH_SLOT_ACTIVE || slot->generation != generation) {
        pthread_mutex_unlock(&registry_lock);
        return GH_INVALID;
    }
    slot->state = GH_SLOT_CLOSING;
    pthread_mutex_unlock(&registry_lock);

    pthread_mutex_lock(&slot->lock);

    meta.data[0] = (int64_t)slot->last_curl_code;
    meta.data[1] = slot->status_code;

    CURLMcode rem_code = CURLM_OK;
    if (slot->multi && slot->curl) {
        rem_code = curl_multi_remove_handle(slot->multi, slot->curl);
    }
    if (slot->curl) {
        curl_easy_cleanup(slot->curl);
        slot->curl = NULL;
    }
    CURLMcode multi_clean = CURLM_OK;
    if (slot->multi) {
        multi_clean = curl_multi_cleanup(slot->multi);
        slot->multi = NULL;
    }
    if (slot->headers_list) {
        curl_slist_free_all(slot->headers_list);
        slot->headers_list = NULL;
    }
    if (slot->header_buf) {
        free(slot->header_buf);
        slot->header_buf = NULL;
    }
    gh_ring_free(&slot->ring);

    int64_t result = GH_OK;
    if (rem_code != CURLM_OK || multi_clean != CURLM_OK) {
        result = GH_TRANSPORT;
        meta.data[0] = (rem_code != CURLM_OK) ? (int64_t)rem_code : (int64_t)multi_clean;
    }

    pthread_mutex_lock(&registry_lock);
    bump_generation(slot);
    slot->state = GH_SLOT_FREE;
    pthread_mutex_unlock(&registry_lock);

    pthread_mutex_unlock(&slot->lock);
    return result;
}
