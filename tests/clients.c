/* SPDX-License-Identifier: Apache-2.0 */
/* Real loopback TLS; CA injection exists only in this linked test wrapper. */
#define _POSIX_C_SOURCE 200809L
#include "../native/http.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "clients:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };
static const char *origin, *ca;
static int trusted, hold_enabled, blocked, release_hold, performed, preflight;
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;

CURLcode __real_curl_easy_perform(CURL *easy);
CURLcode __wrap_curl_easy_perform(CURL *easy) {
    char *url = NULL;
    CHECK(curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url);
    if (preflight) {
        CHECK(!strcmp(url, "https://localhost/preflight") ||
              !strcmp(url, "https://localhost:443/preflight"));
        ++preflight;
        return CURLE_COULDNT_CONNECT; /* Port matching test; never opens a socket. */
    }
    /* Trust is restricted to the ephemeral localhost authority supplied by the
     * fixture. No other hostname or port can receive the test CA override. */
    size_t n = strlen(origin);
    CHECK(!strncmp(url, origin, n) && (url[n] == '/' || url[n] == 0));
    pthread_mutex_lock(&sync_lock);
    ++performed;
    if (hold_enabled && !strcmp(url + n, "/hold")) {
        blocked = 1;
        pthread_cond_broadcast(&sync_cond);
        while (!release_hold) pthread_cond_wait(&sync_cond, &sync_lock);
    }
    pthread_mutex_unlock(&sync_lock);
    if (trusted) {
        CURLcode code = curl_easy_setopt(easy, CURLOPT_CAINFO, ca);
        if (code != CURLE_OK) return code;
    }
    return __real_curl_easy_perform(easy);
}

static gh_input input(const char *s) { return (gh_input){(const uint8_t *)s, (int64_t)strlen(s)}; }
static gh_input none(void) { return (gh_input){NULL, 0}; }
static int64_t open_client(const char *s) {
    int64_t id = 0;
    CHECK(gh_client_open(input(s), (gh_metadata){&id, 1}) == OK && id > 0);
    CHECK(((uint64_t)id & 0x8000) != 0);
    return id;
}
static int64_t request_url(int64_t id, const char *url, int64_t method, gh_input fields,
    gh_input upload, int64_t timeout, size_t body_limit, size_t header_limit,
    unsigned char *body, unsigned char *head, int64_t meta[4]) {
    memset(meta, 0x5a, 32);
    return gh_client_request(id, input(url), method, fields, upload, timeout, 0,
        (gh_output){body, (int64_t)body_limit}, (gh_output){head, (int64_t)header_limit},
        (gh_metadata){meta, 4});
}
static int64_t call(int64_t id, const char *path, int64_t timeout, size_t body_limit,
                    size_t header_limit, unsigned char *body, unsigned char *head, int64_t meta[4]) {
    char url[512];
    CHECK(snprintf(url, sizeof url, "%s%s", origin, path) < (int)sizeof url);
    return request_url(id, url, 0, none(), none(), timeout, body_limit, header_limit, body, head, meta);
}
static long connection(const unsigned char *head, int64_t length) {
    CHECK(length > 0 && length < 4096);
    char copy[4096]; memcpy(copy, head, (size_t)length); copy[length] = 0;
    char *id = strstr(copy, "X-Connection-Id: ");
    CHECK(id);
    long result = strtol(id + strlen("X-Connection-Id: "), NULL, 10);
    CHECK(result > 0);
    return result;
}
static void empty(const int64_t meta[4]) { CHECK(meta[2] == 0 && meta[3] == 0); }
static void *held_request(void *argument) {
    int64_t id = *(int64_t *)argument, meta[4];
    unsigned char body[128], head[4096];
    CHECK(call(id, "/hold", 3000, sizeof body, sizeof head, body, head, meta) == OK);
    return NULL;
}

int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    origin = argv[1]; ca = argv[2];
    CHECK(!strncmp(origin, "https://localhost:", 18));
    unsigned char body[4096], head[4096]; int64_t meta[4];
    int64_t id = open_client(origin);
    if (argc == 4) {
        CHECK(!strcmp(argv[3], "--wrong-hostname"));
        trusted = 1;
        CHECK(call(id, "/forbidden", 3000, sizeof body, sizeof head, body, head, meta) == TRANSPORT);
        CHECK(meta[0] == CURLE_PEER_FAILED_VERIFICATION); empty(meta);
        CHECK(gh_client_close(id) == OK);
        puts("{\"hostname_verification\":\"passed\"}");
        return 0;
    }
    CHECK(call(id, "/untrusted", 3000, sizeof body, sizeof head, body, head, meta) == TRANSPORT);
    CHECK(meta[0] == CURLE_PEER_FAILED_VERIFICATION); empty(meta);
    trusted = 1;
    CHECK(call(id, "/id", 3000, sizeof body, sizeof head, body, head, meta) == OK);
    long first = connection(head, meta[3]);
    CHECK(meta[1] == 200);
    char url[512]; CHECK(snprintf(url, sizeof url, "%s/auth", origin) < (int)sizeof url);
    static const char fields[] = "Authorization: Bearer private\0X-Custom: once\0";
    CHECK(request_url(id, url, 0, (gh_input){(const uint8_t *)fields, sizeof fields - 1}, none(),
        3000, sizeof body, sizeof head, body, head, meta) == OK);
    CHECK(connection(head, meta[3]) == first);
    static const char *reused[] = {"/clean", "/status", "/redirect", "/id"};
    for (size_t i = 0; i < sizeof reused / sizeof *reused; ++i) {
        CHECK(call(id, reused[i], 3000, sizeof body, sizeof head, body, head, meta) == OK);
        CHECK(connection(head, meta[3]) == first);
        CHECK(meta[1] == (i == 1 ? 404 : i == 2 ? 302 : 200));
    }
    int before = performed;
    const char *suffix = origin + strlen("https://localhost");
    char wrong[512];
    CHECK(snprintf(wrong, sizeof wrong, "http://localhost%s/forbidden", suffix) < (int)sizeof wrong);
    CHECK(request_url(id, wrong, 0, none(), none(), 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(snprintf(wrong, sizeof wrong, "https://127.0.0.1%s/forbidden", suffix) < (int)sizeof wrong);
    CHECK(request_url(id, wrong, 0, none(), none(), 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(request_url(id, "https://localhost:1/forbidden", 0, none(), none(), 100,
        sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(request_url(id, "https://[::1%25lo]:443/forbidden", 0, none(), none(), 100,
        sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(snprintf(url, sizeof url, "%s/forbidden", origin) < (int)sizeof url);
    CHECK(request_url(id, url, 1, none(), none(), 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(request_url(id, url, 0, none(), input("upload"), 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(gh_client_request(id, input(url), 0, none(), none(), 100, 0,
        (gh_output){body, sizeof body}, (gh_output){head, sizeof head}, (gh_metadata){meta, 3}) == INVALID);
    CHECK(gh_client_request(id, input(url), 0, none(), none(), 100, 0,
        (gh_output){(uint8_t *)meta, 1}, (gh_output){head, sizeof head}, (gh_metadata){meta, 4}) == INVALID);
    CHECK(gh_client_request(id, input(url), 0, none(), none(), 100, 0,
        (gh_output){NULL, 1}, (gh_output){head, sizeof head}, (gh_metadata){meta, 4}) == INVALID);
    CHECK(performed == before);
    CHECK(call(id, "/after-invalid", 3000, sizeof body, sizeof head, body, head, meta) == OK);
    CHECK(connection(head, meta[3]) == first);

    long previous = first;
    static const struct { const char *path; int result; size_t body, headers; int timeout; } failures[] = {
        {"/big", BODY, 16, 4096, 3000}, {"/headers", HEADERS, 4096, 64, 3000},
        {"/slow", TIMEOUT, 4096, 4096, 30}, {"/partial", TRANSPORT, 4096, 4096, 3000}
    };
    for (size_t i = 0; i < sizeof failures / sizeof *failures; ++i) {
        CHECK(call(id, failures[i].path, failures[i].timeout, failures[i].body,
            failures[i].headers, body, head, meta) == failures[i].result);
        empty(meta);
        CHECK(call(id, "/recovered", 3000, sizeof body, sizeof head, body, head, meta) == OK);
        long current = connection(head, meta[3]); CHECK(current != previous); previous = current;
    }

    /* A wrapper barrier deterministically holds the lease before network I/O. */
    hold_enabled = 1;
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, held_request, &id));
    struct timespec deadline; CHECK(!clock_gettime(CLOCK_REALTIME, &deadline)); deadline.tv_sec += 5;
    pthread_mutex_lock(&sync_lock);
    while (!blocked) CHECK(!pthread_cond_timedwait(&sync_cond, &sync_lock, &deadline));
    pthread_mutex_unlock(&sync_lock);
    CHECK(gh_client_close(id) == INVALID);
    CHECK(call(id, "/forbidden", 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    int64_t second = open_client(origin);
    CHECK(call(second, "/parallel", 3000, sizeof body, sizeof head, body, head, meta) == OK);
    CHECK(gh_client_close(second) == OK);
    pthread_mutex_lock(&sync_lock); release_hold = 1; pthread_cond_broadcast(&sync_cond); pthread_mutex_unlock(&sync_lock);
    CHECK(!pthread_join(thread, NULL));
    CHECK(call(id, "/after-busy", 3000, sizeof body, sizeof head, body, head, meta) == OK);

    int64_t copied = id;
    CHECK(gh_client_close(id) == OK && gh_client_close(copied) == INVALID);
    CHECK(call(copied, "/forbidden", 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    id = open_client(origin); CHECK(id != copied);
    CHECK(gh_client_close(copied) == INVALID && gh_client_close(id) == OK);
    CHECK(gh_client_close(0) == OK && gh_client_close(-1) == INVALID);
    int64_t ids[GH_MAX_CLIENTS];
    for (size_t i = 0; i < GH_MAX_CLIENTS; ++i) ids[i] = open_client(origin);
    CHECK(gh_client_open(input(origin), (gh_metadata){meta, 1}) == SETUP && meta[0] == 0);
    for (size_t i = 0; i < GH_MAX_CLIENTS; ++i) CHECK(gh_client_close(ids[i]) == OK);
    static const char *bad[] = {"http://localhost", "https://localhost/path", "https://localhost/?q=1",
        "https://localhost/#fragment", "https://user@localhost", "https://[::1%25lo]"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i)
        CHECK(gh_client_open(input(bad[i]), (gh_metadata){meta, 1}) == INVALID && meta[0] == 0);
    CHECK(gh_client_open(input(origin), (gh_metadata){meta, 0}) == INVALID);
    CHECK(gh_client_open(input(origin), (gh_metadata){NULL, 1}) == INVALID);
    union { int64_t words[64]; char bytes[512]; } alias;
    strcpy(alias.bytes, origin);
    CHECK(gh_client_open(input(alias.bytes), (gh_metadata){alias.words, 1}) == INVALID);
    CHECK(!strcmp(alias.bytes, origin));
    /* Stream/client tokens are deliberately in separate low-bit namespaces. */
    id = open_client(origin);
    int64_t stream_meta[3], close_meta[2];
    CHECK(gh_stream_open(input(url), none(), 100, 100, 4096, 4096, 4096, 0,
        (gh_metadata){stream_meta, 3}) == OK);
    CHECK(gh_client_close(stream_meta[0]) == INVALID);
    CHECK(call(stream_meta[0], "/forbidden", 100, sizeof body, sizeof head, body, head, meta) == INVALID);
    CHECK(gh_stream_close(id, (gh_metadata){close_meta, 2}) == INVALID);
    CHECK(gh_stream_close(stream_meta[0], (gh_metadata){close_meta, 2}) == OK);
    CHECK(gh_client_close(id) == OK);
    preflight = 1;
    id = open_client("https://localhost:443/");
    CHECK(request_url(id, "https://localhost/preflight", 0, none(), none(), 100,
        sizeof body, sizeof head, body, head, meta) == TRANSPORT);
    CHECK(gh_client_close(id) == OK);
    id = open_client("https://localhost");
    CHECK(request_url(id, "https://localhost:443/preflight", 0, none(), none(), 100,
        sizeof body, sizeof head, body, head, meta) == TRANSPORT);
    CHECK(gh_client_close(id) == OK && preflight == 3);
    preflight = 0;
    id = open_client("https://[::1]");
    before = performed;
    CHECK(request_url(id, "https://[::1%25lo]/forbidden", 0, none(), none(), 100,
        sizeof body, sizeof head, body, head, meta) == INVALID && performed == before);
    CHECK(gh_client_close(id) == OK);
    printf("{\"native_clients\":\"passed\",\"performed\":%d,\"initial_connection\":%ld}\n", performed, first);
    return 0;
}
