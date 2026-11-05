/* SPDX-License-Identifier: Apache-2.0 */
/* Fixed valid-workload microbenchmark, not a replacement for gh_request validation.
 * Trust injection is confined to this executable and the exact fixture URL. */
#define _POSIX_C_SOURCE 200809L
#include "../native/http.h"
#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "reuse:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { HEADER_CAP = 1024, BODY_CAP = 4096, TIMEOUT_MS = 2000 };
typedef struct { unsigned char *data; size_t cap, used; } buffer;
typedef struct {
    int64_t elapsed_ns;
    long connects, status, version, verified;
    curl_off_t connect_us, tls_us, total_us;
} observation;
static const char *fixture_url, *fixture_ca;
static bool native_mode, measure;
static observation observed;
int gh_ensure_initialized(void);
CURLcode __real_curl_easy_perform(CURL *handle);

static int64_t now_ns(void) {
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
}

/* Only production gh_request needs this test-link trust injection. Direct
 * adapters configure the identical CA explicitly, before calling perform. */
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    char *url = NULL;
    CHECK(curl_easy_getinfo(handle, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK);
    CHECK(url && !strcmp(url, fixture_url));
    if (native_mode)
        CHECK(curl_easy_setopt(handle, CURLOPT_CAINFO, fixture_ca) == CURLE_OK);
    CURLcode code = __real_curl_easy_perform(handle);
    CHECK(curl_easy_getinfo(handle, CURLINFO_NUM_CONNECTS, &observed.connects) == CURLE_OK);
    CHECK(curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &observed.status) == CURLE_OK);
    CHECK(curl_easy_getinfo(handle, CURLINFO_HTTP_VERSION, &observed.version) == CURLE_OK);
    CHECK(curl_easy_getinfo(handle, CURLINFO_SSL_VERIFYRESULT, &observed.verified) == CURLE_OK);
    if (measure) {
        CHECK(curl_easy_getinfo(handle, CURLINFO_CONNECT_TIME_T, &observed.connect_us) == CURLE_OK);
        CHECK(curl_easy_getinfo(handle, CURLINFO_APPCONNECT_TIME_T, &observed.tls_us) == CURLE_OK);
        CHECK(curl_easy_getinfo(handle, CURLINFO_TOTAL_TIME_T, &observed.total_us) == CURLE_OK);
    }
    return code;
}

static size_t receive(char *data, size_t size, size_t count, void *opaque) {
    buffer *b = opaque;
    if (size && count > SIZE_MAX / size) return 0;
    size_t n = size * count;
    if (n > b->cap - b->used) return 0;
    if (n) memcpy(b->data + b->used, data, n);
    b->used += n;
    return n;
}

static void configure(CURL *curl, struct curl_slist *headers, buffer *body, buffer *head) {
#define SET(option, value) CHECK(curl_easy_setopt(curl, option, value) == CURLE_OK)
    SET(CURLOPT_URL, fixture_url);
    SET(CURLOPT_PROTOCOLS_STR, "https");
    SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    SET(CURLOPT_FOLLOWLOCATION, 0L);
    SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NETRC, (long)CURL_NETRC_IGNORED);
    SET(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L);
    SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_CAINFO, fixture_ca);
    SET(CURLOPT_NOSIGNAL, 1L);
    SET(CURLOPT_TIMEOUT_MS, (long)TIMEOUT_MS);
    SET(CURLOPT_HTTP_CONTENT_DECODING, 0L);
    SET(CURLOPT_HTTPHEADER, headers);
    SET(CURLOPT_HTTPGET, 1L);
    SET(CURLOPT_WRITEFUNCTION, receive);
    SET(CURLOPT_WRITEDATA, body);
    SET(CURLOPT_HEADERFUNCTION, receive);
    SET(CURLOPT_HEADERDATA, head);
#undef SET
}

static long number(const char *text, long maximum) {
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    CHECK(!errno && end != text && !*end && value >= 0 && value <= maximum);
    return value;
}

static uint64_t checksum(const unsigned char *data, size_t n) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < n; ++i) { hash ^= data[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}

static void json_string(const char *s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c < 32 || c > 126) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    CHECK(argc == 8);
    native_mode = !strcmp(argv[1], "native_cold");
    bool reuse = !strcmp(argv[1], "direct_reuse");
    CHECK(native_mode || reuse || !strcmp(argv[1], "direct_cold"));
    fixture_url = argv[2]; fixture_ca = argv[3];
    long requests = number(argv[4], 10000), bytes = number(argv[5], BODY_CAP);
    CHECK(requests >= 1 && (bytes == 0 || bytes == BODY_CAP));
    measure = number(argv[6], 1) != 0;
    long port = number(argv[7], 65535);
    CHECK(port > 0);
    char expected_url[128];
    CHECK(snprintf(expected_url, sizeof expected_url, "https://localhost:%ld/payload/%ld", port, bytes) > 0);
    CHECK(!strcmp(fixture_url, expected_url));
    CHECK(gh_ensure_initialized() == 0);
    observation *samples = calloc((size_t)requests, sizeof *samples);
    CHECK(samples);
    unsigned char body_bytes[BODY_CAP + 1], header_bytes[HEADER_CAP + 1], expected[BODY_CAP];
    for (long i = 0; i < bytes; ++i) expected[i] = (unsigned char)((i * 131 + 17) % 256);
    uint64_t expected_sum = checksum(expected, (size_t)bytes);
    char expected_headers[HEADER_CAP];
    int header_len = snprintf(expected_headers, sizeof expected_headers,
        "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nContent-Type: application/octet-stream\r\nX-Reuse-Fixture: 1\r\n\r\n", bytes);
    CHECK(header_len > 0 && header_len < HEADER_CAP);
    CURL *retained = NULL;
    int64_t batch_start = measure ? now_ns() : 0;
    for (long i = 0; i < requests; ++i) {
        memset(body_bytes, 0xa5, sizeof body_bytes);
        memset(header_bytes, 0xa5, sizeof header_bytes);
        memset(&observed, 0, sizeof observed);
        buffer body = {body_bytes, (size_t)bytes, 0};
        buffer head = {header_bytes, HEADER_CAP, 0};
        int64_t start = measure ? now_ns() : 0;
        if (native_mode) {
            static const unsigned char packed[] = "Accept-Encoding: identity\0";
            int64_t meta[4] = {0};
            CHECK(gh_request((gh_input){(const uint8_t *)fixture_url, (int64_t)strlen(fixture_url)},
                0, (gh_input){packed, sizeof packed - 1}, (gh_input){NULL, 0}, TIMEOUT_MS, 0,
                (gh_output){body_bytes, bytes}, (gh_output){header_bytes, HEADER_CAP},
                (gh_metadata){meta, 4}) == 0);
            CHECK(meta[0] == CURLE_OK && meta[1] == 200 && meta[2] == bytes && meta[3] == header_len);
            body.used = (size_t)meta[2]; head.used = (size_t)meta[3];
        } else {
            CURL *curl;
            if (reuse && retained) { curl = retained; curl_easy_reset(curl); }
            else { curl = curl_easy_init(); CHECK(curl); if (reuse) retained = curl; }
            struct curl_slist *headers = curl_slist_append(NULL, "Accept-Encoding: identity");
            CHECK(headers);
            struct curl_slist *next = curl_slist_append(headers, "Expect:");
            CHECK(next); headers = next;
            configure(curl, headers, &body, &head);
            CHECK(curl_easy_perform(curl) == CURLE_OK);
            /* Remove callback and header pointers before their storage expires;
             * easy_reset preserves the connection cache, not request options. */
            CHECK(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL) == CURLE_OK);
            CHECK(curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL) == CURLE_OK);
            CHECK(curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL) == CURLE_OK);
            curl_slist_free_all(headers);
            if (!reuse) curl_easy_cleanup(curl);
        }
        observed.elapsed_ns = measure ? now_ns() - start : 0;
        CHECK(observed.connects == ((!reuse || i == 0) ? 1 : 0));
        CHECK(observed.status == 200 && observed.version == CURL_HTTP_VERSION_1_1 && observed.verified == 0);
        CHECK(body.used == (size_t)bytes && head.used == (size_t)header_len);
        CHECK(!memcmp(body_bytes, expected, (size_t)bytes));
        CHECK(checksum(body_bytes, body.used) == expected_sum);
        CHECK(!memcmp(header_bytes, expected_headers, head.used));
        CHECK(body_bytes[bytes] == 0xa5 && header_bytes[HEADER_CAP] == 0xa5);
        samples[i] = observed;
    }
    if (retained) curl_easy_cleanup(retained);
    int64_t batch_ns = measure ? now_ns() - batch_start : 0;
    const curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    printf("{\"mode\":"); json_string(argv[1]);
    printf(",\"requests\":%ld,\"body_bytes\":%ld,\"header_bytes\":%d,\"checksum_fnv1a64\":\"%016" PRIx64 "\",\"curl_version\":", requests, bytes, header_len, expected_sum);
    json_string(version->version); printf(",\"tls_backend\":"); json_string(version->ssl_version ? version->ssl_version : "none");
    if (measure) printf(",\"elapsed_monotonic_ns\":%" PRId64, batch_ns);
    printf(",\"transfers\":[");
    for (long i = 0; i < requests; ++i) {
        observation *s = &samples[i];
        printf("%s{\"new_connections\":%ld,\"http_status\":%ld,\"http_version\":\"1.1\",\"ssl_verify_result\":%ld", i ? "," : "", s->connects, s->status, s->verified);
        if (measure) printf(",\"elapsed_monotonic_ns\":%" PRId64 ",\"connect_us\":%" CURL_FORMAT_CURL_OFF_T ",\"appconnect_us\":%" CURL_FORMAT_CURL_OFF_T ",\"curl_total_us\":%" CURL_FORMAT_CURL_OFF_T,
            s->elapsed_ns, s->connect_us, s->tls_us, s->total_us);
        putchar('}');
    }
    puts("]}");
    free(samples);
    curl_global_cleanup();
    return 0;
}
