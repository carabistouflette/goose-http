/* SPDX-License-Identifier: Apache-2.0 */
/* Real easy handles with a deterministic, socket-free transfer boundary. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "client_failures:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };
enum { COMPLETE, PARTIAL, INTERIM_PARTIAL, NO_STATUS, BODY_OVERFLOW, HEADER_OVERFLOW };
static int backend, fail_easy, fail_url, fail_url_set, fail_url_get, fail_append;
static int fail_option, fail_malloc, url_sets, url_gets, appends, options, allocations;
static int initialized, cleaned, resets, performed, scenario;
static CURL *live;
static const void *url_pointer, *header_pointer;
static curl_write_callback body_callback, header_callback;
static void *body_data, *header_data;
static CURLcode transfer_code, info_code;
static long response_code;
static size_t body_bytes, header_bytes;
static unsigned char payload[128];

void *__real_malloc(size_t);
CURL *__real_curl_easy_init(void);
void __real_curl_easy_cleanup(CURL *);
void __real_curl_easy_reset(CURL *);
CURLU *__real_curl_url(void);
CURLUcode __real_curl_url_set(CURLU *, CURLUPart, const char *, unsigned int);
CURLUcode __real_curl_url_get(const CURLU *, CURLUPart, char **, unsigned int);
struct curl_slist *__real_curl_slist_append(struct curl_slist *, const char *);
void __real_curl_slist_free_all(struct curl_slist *);
CURLcode __real_curl_easy_setopt(CURL *, CURLoption, ...);

void *__wrap_malloc(size_t size) {
    if (!backend && ++allocations == fail_malloc) return NULL;
    return __real_malloc(size);
}
CURL *__wrap_curl_easy_init(void) {
    if (fail_easy) return NULL;
    CHECK(!live);
    ++backend; live = __real_curl_easy_init(); --backend;
    CHECK(live); ++initialized;
    return live;
}
static void detached(void) {
    CHECK(!url_pointer && !header_pointer && !body_callback && !header_callback);
    CHECK(!body_data && !header_data);
}
void __wrap_curl_easy_cleanup(CURL *handle) {
    CHECK(handle == live); detached();
    ++backend; __real_curl_easy_cleanup(handle); --backend;
    live = NULL; ++cleaned;
}
void __wrap_curl_easy_reset(CURL *handle) {
    CHECK(handle == live);
    ++backend; __real_curl_easy_reset(handle); --backend;
    url_pointer = header_pointer = NULL;
    body_callback = header_callback = NULL;
    body_data = header_data = NULL;
    ++resets;
}
CURLU *__wrap_curl_url(void) {
    if (fail_url) return NULL;
    ++backend; CURLU *result = __real_curl_url(); --backend;
    return result;
}
CURLUcode __wrap_curl_url_set(CURLU *handle, CURLUPart part, const char *value, unsigned int flags) {
    if (++url_sets == fail_url_set) return CURLUE_OUT_OF_MEMORY;
    ++backend; CURLUcode result = __real_curl_url_set(handle, part, value, flags); --backend;
    return result;
}
CURLUcode __wrap_curl_url_get(const CURLU *handle, CURLUPart part, char **value, unsigned int flags) {
    if (++url_gets == fail_url_get) return CURLUE_OUT_OF_MEMORY;
    ++backend; CURLUcode result = __real_curl_url_get(handle, part, value, flags); --backend;
    return result;
}
struct curl_slist *__wrap_curl_slist_append(struct curl_slist *list, const char *line) {
    if (++appends == fail_append) return NULL;
    ++backend; struct curl_slist *result = __real_curl_slist_append(list, line); --backend;
    return result;
}
void __wrap_curl_slist_free_all(struct curl_slist *list) {
    /* The reset must precede freeing the request's borrowed list. */
    detached();
    ++backend; __real_curl_slist_free_all(list); --backend;
}
CURLcode __wrap_curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    CHECK(handle == live);
    va_list args; va_start(args, option);
    int fail = ++options == fail_option;
    CURLcode result;
    ++backend;
    if (option < CURLOPTTYPE_OBJECTPOINT) {
        long value = va_arg(args, long);
        if (option == CURLOPT_HTTPAUTH || option == CURLOPT_PROXYAUTH) CHECK(value == CURLAUTH_NONE);
        if (option == CURLOPT_HTTP_VERSION) CHECK(value == CURL_HTTP_VERSION_1_1);
        if (option == CURLOPT_MAXCONNECTS) CHECK(value == 1);
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_FUNCTIONPOINT) {
        void *value = va_arg(args, void *);
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
        if (result == CURLE_OK) {
            if (option == CURLOPT_URL) url_pointer = value;
            if (option == CURLOPT_HTTPHEADER) header_pointer = value;
            if (option == CURLOPT_WRITEDATA) body_data = value;
            if (option == CURLOPT_HEADERDATA) header_data = value;
        }
    } else if (option < CURLOPTTYPE_OFF_T) {
        CHECK(option == CURLOPT_WRITEFUNCTION || option == CURLOPT_HEADERFUNCTION);
        curl_write_callback value = va_arg(args, curl_write_callback);
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
        if (result == CURLE_OK) {
            if (option == CURLOPT_WRITEFUNCTION) body_callback = value;
            else header_callback = value;
        }
    } else {
        CHECK(option < CURLOPTTYPE_BLOB);
        curl_off_t value = va_arg(args, curl_off_t);
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
    }
    --backend; va_end(args);
    return result;
}
CURLcode __wrap_curl_easy_getinfo(CURL *handle, CURLINFO info, ...) {
    CHECK(handle == live && info == CURLINFO_RESPONSE_CODE);
    va_list args; va_start(args, info);
    long *status = va_arg(args, long *);
    if (info_code == CURLE_OK) *status = response_code;
    va_end(args);
    return info_code;
}
static int emit(const char *text, size_t length) {
    return header_callback((char *)text, 1, length, header_data) == length;
}
static int headers(void) {
    static const char status[] = "HTTP/1.1 200 OK\r\n", blank[] = "\r\n";
    if (!header_bytes) return 1;
    if (scenario == INTERIM_PARTIAL &&
        (!emit("HTTP/1.1 100 Continue\r\n", 23) || !emit(blank, 2))) return 0;
    if (!emit(status, sizeof status - 1)) return 0;
    if (scenario == PARTIAL || scenario == INTERIM_PARTIAL) return 1;
    CHECK(header_bytes >= 19 && header_bytes <= sizeof payload);
    if (header_bytes != 19) {
        char field[128]; size_t length = header_bytes - 19;
        CHECK(length >= 5);
        memcpy(field, "X:", 2); memset(field + 2, 'x', length - 4);
        memcpy(field + length - 2, "\r\n", 2);
        if (!emit(field, length)) return 0;
    }
    return emit(blank, 2);
}
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    CHECK(handle == live && url_pointer && header_pointer);
    CHECK(body_callback && header_callback && body_data && header_data);
    ++performed;
    CHECK(body_callback(NULL, 0, SIZE_MAX, body_data) == 0);
    CHECK(body_callback(NULL, SIZE_MAX, 0, body_data) == 0);
    CHECK(header_callback(NULL, 0, SIZE_MAX, header_data) == 0);
    if (scenario == BODY_OVERFLOW || scenario == HEADER_OVERFLOW) {
        curl_write_callback callback = scenario == BODY_OVERFLOW ? body_callback : header_callback;
        void *opaque = scenario == BODY_OVERFLOW ? body_data : header_data;
        CHECK(callback((char *)payload, SIZE_MAX, 2, opaque) == 0);
        return CURLE_WRITE_ERROR;
    }
    if (scenario != NO_STATUS && !headers()) return CURLE_WRITE_ERROR;
    CHECK(body_bytes <= sizeof payload);
    if (body_callback((char *)payload, 1, body_bytes, body_data) != body_bytes)
        return CURLE_WRITE_ERROR;
    return transfer_code;
}

static gh_input input(const char *text) { return (gh_input){(const uint8_t *)text, (int64_t)strlen(text)}; }
static void prepare(void) {
    fail_easy = fail_url = fail_url_set = fail_url_get = fail_append = fail_option = fail_malloc = 0;
    url_sets = url_gets = appends = options = allocations = performed = 0;
    scenario = COMPLETE; transfer_code = info_code = CURLE_OK;
    response_code = 200; body_bytes = 3; header_bytes = 19;
    detached();
}
static int64_t open_client(void) {
    int64_t id = 123;
    CHECK(gh_client_open(input("https://example.invalid"), (gh_metadata){&id, 1}) == OK);
    CHECK(id > 0); return id;
}
static int64_t request_fields(int64_t id, gh_input url, int64_t method, gh_input fields,
                              size_t body_limit, size_t header_limit, int64_t meta[4]) {
    unsigned char body[130], head[130];
    CHECK(body_limit <= 128 && header_limit <= 128);
    memset(body, 0xa5, sizeof body); memset(head, 0xa5, sizeof head);
    for (size_t i = 0; i < 4; ++i) meta[i] = -1;
    int64_t result = gh_client_request(id, url, method, fields, (gh_input){NULL, 0},
        1234, 0, (gh_output){body + 1, (int64_t)body_limit},
        (gh_output){head + 1, (int64_t)header_limit}, (gh_metadata){meta, 4});
    detached();
    CHECK(body[0] == 0xa5 && head[0] == 0xa5);
    for (size_t i = body_limit + 1; i < sizeof body; ++i) CHECK(body[i] == 0xa5);
    for (size_t i = header_limit + 1; i < sizeof head; ++i) CHECK(head[i] == 0xa5);
    if (result == OK) {
        CHECK(meta[0] == CURLE_OK && meta[1] == response_code);
        CHECK(meta[2] == (int64_t)body_bytes && meta[3] == (int64_t)header_bytes);
        CHECK(!memcmp(body + 1, payload, body_bytes));
    } else CHECK(meta[2] == 0 && meta[3] == 0);
    return result;
}
static int64_t request(int64_t id, size_t body_limit, size_t header_limit, int64_t meta[4]) {
    static const char fields[] = "X-Test: a\0X-Empty:\0";
    return request_fields(id, input("https://example.invalid/test"), 0,
        (gh_input){(const uint8_t *)fields, sizeof fields - 1}, body_limit, header_limit, meta);
}
static void recover(int64_t id) {
    int64_t meta[4]; int before = initialized;
    prepare(); CHECK(request(id, 64, 64, meta) == OK);
    CHECK(initialized == before + 1 && performed == 1 && live);
}
static void failed_cached(int64_t id, int want, CURLcode code) {
    int64_t meta[4]; int before = cleaned, before_init = initialized;
    CHECK(live && request(id, 64, 64, meta) == want);
    CHECK(meta[0] == code && cleaned == before + 1 && initialized == before_init && !live);
    CHECK(meta[1] == (performed && info_code == CURLE_OK ? response_code : 0));
    if (want == SETUP) CHECK(!performed);
    recover(id);
}
int main(void) {
    int64_t meta[4], id;
    memset(payload, 'p', sizeof payload);
    prepare(); id = open_client();
    CHECK(!live); recover(id);
    int total_options = options, total_appends = appends, total_sets = url_sets, total_gets = url_gets;
    CHECK(total_options >= 20 && total_appends == 3 && total_sets > 0 && total_gets >= 3);
    int before_init = initialized, before_reset = resets, before_clean = cleaned;
    prepare(); CHECK(request(id, 64, 64, meta) == OK);
    CHECK(initialized == before_init && resets == before_reset + 1 && cleaned == before_clean);
    /* Every SET failure must reset partially attached pointers and evict cache. */
    for (int i = 1; i <= total_options; ++i) {
        prepare(); fail_option = i;
        failed_cached(id, SETUP, CURLE_UNKNOWN_OPTION);
    }
    for (int i = 1; i <= total_appends; ++i) {
        prepare(); fail_append = i; failed_cached(id, SETUP, CURLE_OK);
    }
    prepare(); fail_malloc = 1; failed_cached(id, SETUP, CURLE_OK);
    prepare(); fail_url = 1; failed_cached(id, SETUP, CURLE_OK);
    for (int i = 1; i <= total_sets; ++i) {
        prepare(); fail_url_set = i; failed_cached(id, SETUP, CURLE_OK);
    }
    for (int i = 1; i <= total_gets; ++i) {
        prepare(); fail_url_get = i; failed_cached(id, SETUP, CURLE_OK);
    }
    /* Invalid requests retain the successfully reset idle easy handle. */
    static const char *const invalid_urls[] = {"https://elsewhere.invalid/test", "http://example.invalid/test", "https://example.invalid:444/test"};
    for (size_t i = 0; i < sizeof invalid_urls / sizeof *invalid_urls; ++i) {
        prepare(); before_init = initialized; before_clean = cleaned; before_reset = resets;
        CHECK(request_fields(id, input(invalid_urls[i]), 0, (gh_input){NULL, 0}, 64, 64, meta) == INVALID);
        CHECK(!performed && live && cleaned == before_clean && resets == before_reset);
        prepare(); CHECK(request(id, 64, 64, meta) == OK); CHECK(initialized == before_init);
    }
    prepare(); CHECK(request_fields(id, input("https://example.invalid/test"), 1,
        (gh_input){NULL, 0}, 64, 64, meta) == INVALID); CHECK(!performed && live);
    prepare(); CHECK(request_fields(id, input("https://example.invalid/test"), 0,
        input("Host: forbidden"), 64, 64, meta) == INVALID); CHECK(!performed && live);
    prepare(); transfer_code = CURLE_OPERATION_TIMEDOUT;
    failed_cached(id, TIMEOUT, CURLE_OPERATION_TIMEDOUT);
    prepare(); transfer_code = CURLE_RECV_ERROR; failed_cached(id, TRANSPORT, CURLE_RECV_ERROR);
    prepare(); info_code = CURLE_BAD_FUNCTION_ARGUMENT; failed_cached(id, TRANSPORT, CURLE_BAD_FUNCTION_ARGUMENT);
    prepare(); transfer_code = CURLE_OPERATION_TIMEDOUT; info_code = CURLE_BAD_FUNCTION_ARGUMENT;
    failed_cached(id, TIMEOUT, CURLE_OPERATION_TIMEDOUT);
    prepare(); scenario = PARTIAL; failed_cached(id, TRANSPORT, CURLE_PARTIAL_FILE);
    prepare(); scenario = INTERIM_PARTIAL; failed_cached(id, TRANSPORT, CURLE_PARTIAL_FILE);
    prepare(); scenario = NO_STATUS; failed_cached(id, TRANSPORT, CURLE_PARTIAL_FILE);
    prepare(); body_bytes = 65; failed_cached(id, BODY, CURLE_WRITE_ERROR);
    prepare(); header_bytes = 65; failed_cached(id, HEADERS, CURLE_WRITE_ERROR);
    prepare(); scenario = BODY_OVERFLOW; failed_cached(id, BODY, CURLE_WRITE_ERROR);
    prepare(); scenario = HEADER_OVERFLOW; failed_cached(id, HEADERS, CURLE_WRITE_ERROR);
    prepare(); body_bytes = 64; header_bytes = 64; response_code = 503;
    CHECK(request(id, 64, 64, meta) == OK); CHECK(meta[1] == 503);
    prepare(); body_bytes = 0; CHECK(request(id, 0, 19, meta) == OK);
    prepare(); CHECK(request(id, 64, 0, meta) == HEADERS); CHECK(!live);
    prepare(); fail_easy = 1; CHECK(request(id, 64, 64, meta) == SETUP);
    CHECK(!performed && !live && meta[0] == CURLE_OK); recover(id);
    CHECK(gh_client_close(id) == OK); CHECK(!live && initialized == cleaned);
    CHECK(gh_client_close(id) == INVALID);
    /* Opening has the same allocation-failure classification, without a handle. */
    for (int fault = 0; fault < 5; ++fault) {
        prepare();
        if (fault == 0) fail_url = 1;
        else if (fault == 1) fail_url_set = 1;
        else fail_url_get = fault - 1;
        id = -1;
        CHECK(gh_client_open(input("https://example.invalid"), (gh_metadata){&id, 1}) == SETUP);
        CHECK(id == 0 && !live && initialized == cleaned);
        prepare(); id = open_client(); CHECK(gh_client_close(id) == OK);
    }
    puts("reusable client failure tests passed");
    return 0;
}
