/* SPDX-License-Identifier: Apache-2.0 */
/* Deterministic faults at the private transport boundary; never opens a socket. */
#include "../native/http.h"
#include <curl/curl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failures:%d: %s\n", __LINE__, #x); exit(1); } } while (0)
enum { OK, INVALID, SETUP, TRANSPORT, TIMEOUT, BODY, HEADERS };
enum { VERIFY_PEER = 1, VERIFY_HOST = 2, NO_PROXY = 4, NO_REDIRECT = 8,
       NO_SIGNAL = 16, PROTOCOLS = 32, REDIRECT_PROTOCOLS = 64, TIMEOUT_SET = 128 };
static int fail_easy, fail_url, fail_append, append_count, fail_option, option_count;
static int performed, security_options, allow_http, fail_url_set, fail_url_get;
static CURLcode transfer_result;
static curl_write_callback body_callback, header_callback;
static void *body_data, *header_data;
static size_t send_body, send_header;
static unsigned char payload[256];

CURL *__real_curl_easy_init(void);
CURLU *__real_curl_url(void);
CURLUcode __real_curl_url_set(CURLU *, CURLUPart, const char *, unsigned int);
CURLUcode __real_curl_url_get(const CURLU *, CURLUPart, char **, unsigned int);
struct curl_slist *__real_curl_slist_append(struct curl_slist *, const char *);
CURLcode __real_curl_easy_setopt(CURL *, CURLoption, ...);
CURL *__wrap_curl_easy_init(void) { return fail_easy ? NULL : __real_curl_easy_init(); }
CURLU *__wrap_curl_url(void) { return fail_url ? NULL : __real_curl_url(); }
CURLUcode __wrap_curl_url_set(CURLU *handle, CURLUPart part, const char *value, unsigned int flags) {
    return fail_url_set ? CURLUE_OUT_OF_MEMORY : __real_curl_url_set(handle, part, value, flags);
}
CURLUcode __wrap_curl_url_get(const CURLU *handle, CURLUPart part, char **value, unsigned int flags) {
    return fail_url_get ? CURLUE_OUT_OF_MEMORY : __real_curl_url_get(handle, part, value, flags);
}
struct curl_slist *__wrap_curl_slist_append(struct curl_slist *list, const char *line) {
    ++append_count;
    return append_count == fail_append ? NULL : __real_curl_slist_append(list, line);
}
CURLcode __wrap_curl_easy_setopt(CURL *handle, CURLoption option, ...) {
    va_list args;
    va_start(args, option);
    int fail = ++option_count == fail_option;
    CURLcode result;
    if (option < CURLOPTTYPE_OBJECTPOINT) {
        long value = va_arg(args, long);
        switch (option) {
        case CURLOPT_SSL_VERIFYPEER: CHECK(value == 1); security_options |= VERIFY_PEER; break;
        case CURLOPT_SSL_VERIFYHOST: CHECK(value == 2); security_options |= VERIFY_HOST; break;
        case CURLOPT_FOLLOWLOCATION: CHECK(value == 0); security_options |= NO_REDIRECT; break;
        case CURLOPT_NOSIGNAL: CHECK(value == 1); security_options |= NO_SIGNAL; break;
        case CURLOPT_TIMEOUT_MS: CHECK(value == 1234); security_options |= TIMEOUT_SET; break;
        default: break;
        }
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_FUNCTIONPOINT) {
        /* All object arguments used by the adapter are object pointers. */
        void *value = va_arg(args, void *);
        if (option == CURLOPT_PROXY) { CHECK(value && !strcmp(value, "")); security_options |= NO_PROXY; }
        if (option == CURLOPT_PROTOCOLS_STR) {
            CHECK(value && !strcmp(value, allow_http ? "http,https" : "https")); security_options |= PROTOCOLS;
        }
        if (option == CURLOPT_REDIR_PROTOCOLS_STR) {
            CHECK(value && !strcmp(value, "https")); security_options |= REDIRECT_PROTOCOLS;
        }
        if (option == CURLOPT_WRITEDATA) body_data = value;
        if (option == CURLOPT_HEADERDATA) header_data = value;
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
    } else if (option < CURLOPTTYPE_OFF_T) {
        CHECK(option == CURLOPT_WRITEFUNCTION || option == CURLOPT_HEADERFUNCTION);
        curl_write_callback value = va_arg(args, curl_write_callback);
        if (option == CURLOPT_WRITEFUNCTION) body_callback = value; else header_callback = value;
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
    } else {
        CHECK(option < CURLOPTTYPE_BLOB);
        curl_off_t value = va_arg(args, curl_off_t);
        result = fail ? CURLE_UNKNOWN_OPTION : __real_curl_easy_setopt(handle, option, value);
    }
    va_end(args);
    return result;
}
CURLcode __wrap_curl_easy_perform(CURL *handle) {
    (void)handle;
    ++performed;
    CHECK(security_options == 255);
    CHECK(body_callback && header_callback && body_data && header_data);
    if (send_header && header_callback((char *)payload, 1, send_header, header_data) != send_header)
        return CURLE_WRITE_ERROR;
    if (send_body && body_callback((char *)payload, 1, send_body, body_data) != send_body)
        return CURLE_WRITE_ERROR;
    return transfer_result;
}
static gh_input input(const void *p, int64_t n) { return (gh_input){p, n}; }
static void reset(void) {
    fail_easy = fail_url = fail_append = append_count = fail_option = option_count = 0;
    performed = security_options = allow_http = fail_url_set = fail_url_get = 0;
    body_callback = header_callback = NULL; body_data = header_data = NULL;
    transfer_result = CURLE_COULDNT_CONNECT; send_body = send_header = 0;
}
static int64_t request(unsigned char *body, int64_t body_len, unsigned char *head,
                       int64_t head_len, int64_t meta[4]) {
    static const char url[] = "https://example.invalid/test";
    static const char fields[] = "X-Test: a\0X-Other: b\0";
    static const unsigned char upload[] = {'a', 0, 'b'};
    for (size_t i = 0; i < 4; ++i) meta[i] = 123;
    return gh_request(input(url, sizeof url - 1), 1, input(fields, sizeof fields - 1),
        input(upload, sizeof upload), 1234, allow_http, (gh_output){body, body_len},
        (gh_output){head, head_len}, (gh_metadata){meta, 4});
}
static void empty_lengths(const int64_t meta[4]) { CHECK(meta[2] == 0 && meta[3] == 0); }
int main(void) {
    unsigned char body[66], head[66]; int64_t meta[4];
    memset(payload, 'x', sizeof payload);
    reset(); CHECK(request(body, 64, head, 64, meta) == TRANSPORT);
    CHECK(performed == 1 && meta[0] == CURLE_COULDNT_CONNECT); empty_lengths(meta);
    int options = option_count, allocations = append_count;
    CHECK(options > 0 && allocations > 0);
    /* Fail every configured option/append, independent of their implementation order. */
    for (int i = 1; i <= options; ++i) {
        reset(); fail_option = i;
        CHECK(request(body, 64, head, 64, meta) == SETUP);
        CHECK(!performed && meta[0] == CURLE_UNKNOWN_OPTION); empty_lengths(meta);
    }
    for (int i = 1; i <= allocations; ++i) {
        reset(); fail_append = i;
        CHECK(request(body, 64, head, 64, meta) == SETUP);
        CHECK(!performed); empty_lengths(meta);
    }
    reset(); fail_easy = 1; CHECK(request(body, 64, head, 64, meta) == SETUP); empty_lengths(meta);
    reset(); fail_url = 1; CHECK(request(body, 64, head, 64, meta) == SETUP); empty_lengths(meta);
    reset(); fail_url_set = 1; CHECK(request(body, 64, head, 64, meta) == SETUP);
    CHECK(!performed); empty_lengths(meta);
    reset(); fail_url_get = 1; CHECK(request(body, 64, head, 64, meta) == SETUP);
    CHECK(!performed); empty_lengths(meta);
    reset(); transfer_result = CURLE_OPERATION_TIMEDOUT; send_body = 3; send_header = 5;
    CHECK(request(body, 64, head, 64, meta) == TIMEOUT);
    CHECK(meta[0] == CURLE_OPERATION_TIMEDOUT); empty_lengths(meta);
    reset(); transfer_result = CURLE_RECV_ERROR; send_body = 3; send_header = 5;
    CHECK(request(body, 64, head, 64, meta) == TRANSPORT); empty_lengths(meta);
    reset(); transfer_result = CURLE_OK; send_body = send_header = 64;
    memset(body, 0xa5, sizeof body); memset(head, 0xa5, sizeof head);
    CHECK(request(body + 1, 64, head + 1, 64, meta) == OK);
    CHECK(meta[2] == 64 && meta[3] == 64 && !memcmp(body + 1, payload, 64));
    CHECK(body[0] == 0xa5 && body[65] == 0xa5 && head[0] == 0xa5 && head[65] == 0xa5);
    reset(); send_body = 65;
    CHECK(request(body + 1, 64, head + 1, 64, meta) == BODY); empty_lengths(meta);
    CHECK(body[0] == 0xa5 && body[65] == 0xa5);
    reset(); send_header = 65;
    CHECK(request(body + 1, 64, head + 1, 64, meta) == HEADERS); empty_lengths(meta);
    CHECK(head[0] == 0xa5 && head[65] == 0xa5);
    reset(); allow_http = 1; CHECK(request(body, 64, head, 64, meta) == TRANSPORT);
    /* Invalid ABI arguments must be rejected before any transport execution. */
    reset(); CHECK(request(body, -1, head, 64, meta) == INVALID && !performed);
    reset(); CHECK(request(NULL, 1, head, 64, meta) == INVALID && !performed);
    reset(); CHECK(request(body, 64, body + 1, 64, meta) == INVALID && !performed);
    static const char url[] = "https://example.invalid/";
    gh_input none = input(NULL, 0), address = input(url, sizeof url - 1);
    CHECK(gh_request(address, 0, none, none, 1234, 0, (gh_output){body, 64},
          (gh_output){head, 64}, (gh_metadata){meta, 3}) == INVALID && !performed);
    CHECK(gh_request(address, 0, none, none, 1234, 0, (gh_output){(uint8_t *)meta, 1},
          (gh_output){head, 64}, (gh_metadata){meta, 4}) == INVALID && !performed);
    CHECK(gh_request(address, 0, none, none, 1234, 0, (gh_output){(uint8_t *)url, 1},
          (gh_output){head, 64}, (gh_metadata){meta, 4}) == INVALID && !performed);
    CHECK(gh_request(input(NULL, 1), 0, none, none, 1234, 0, (gh_output){body, 64},
          (gh_output){head, 64}, (gh_metadata){meta, 4}) == INVALID && !performed);
    puts("native HTTP fault injection passed");
    return 0;
}
